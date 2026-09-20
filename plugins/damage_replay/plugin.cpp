#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/services/core.h"
#include "cabbird/sdk/services/interop.h"
#include "cabbird/sdk/services/localization.h"
#include "cabbird/sdk/services/platform.h"
#include "cabbird/sdk/services/ui.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

using cabbird::sdk::Host;
using cabbird::sdk::Ok;
using cabbird::sdk::StringView;

namespace {

constexpr std::uintptr_t kOnExecuteDamageElementRva = 0x59E4510;
constexpr std::uintptr_t kSourcePlayerEntityIdOffset = 0x54;

constexpr int kMinimumReplayCount = 1;
constexpr int kMaximumReplayCount = 1024;
constexpr int kDefaultReplayCount = 10;

const CabbirdCoreServiceV1* g_core = nullptr;
const CabbirdUiServiceV1* g_ui = nullptr;
const CabbirdHookServiceV1* g_hook = nullptr;
const CabbirdLocalizationServiceV1* g_localization = nullptr;
const CabbirdStorageServiceV1* g_storage = nullptr;

std::atomic<std::uintptr_t> g_trampoline{0};
std::atomic<bool> g_installed{false};
CabbirdGenerationHandleV1 g_handle{};

std::atomic<int> g_replay_count{kDefaultReplayCount};

std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_replayed{0};
std::atomic<std::uint64_t> g_refused{0};

enum StatusId : int {
    kStatusNone = 0,
    kStatusInstalled,
    kStatusRemoved,
    kStatusNoHookService,
    kStatusAlreadyInstalled,
    kStatusNoGameAssembly,
    kStatusCreateRefused,
    kStatusHookServiceUnavailable,
    kStatusCount,
};

struct StatusText final {
    const char* key;
    const char* english;
};

constexpr StatusText kStatusTexts[kStatusCount] = {
    {nullptr, nullptr},
    {"cabbird.damage-replay.status.installed", "installed."},
    {"cabbird.damage-replay.status.removed", "removed."},
    {"cabbird.damage-replay.status.no-hook-service", "the host publishes no hook service"},
    {"cabbird.damage-replay.status.already-installed", "already installed"},
    {"cabbird.damage-replay.status.no-game-assembly", "GameAssembly.dll is not loaded"},
    {"cabbird.damage-replay.status.create-refused", "create refused (status {0})"},
    {"cabbird.damage-replay.status.hook-unavailable",
     "cabbird.interop.hook is not available in this host build"},
};

std::atomic<int> g_status_id{kStatusNone};
std::atomic<int> g_status_code{0};

void SetStatus(const StatusId id, const int code = 0) noexcept {
    g_status_code.store(code, std::memory_order_relaxed);
    g_status_id.store(static_cast<int>(id), std::memory_order_release);
}

constexpr std::size_t kTranslationBytes = 256;

const char* Tr(
    const char* const key,
    const char* const english,
    const char* const argument0 = nullptr,
    const char* const argument1 = nullptr,
    const char* const argument2 = nullptr) noexcept {
    if (english == nullptr) return "";
    const CabbirdLocalizationServiceV1* const service = g_localization;
    if (service == nullptr || service->translate == nullptr || key == nullptr) return english;

    const CabbirdStringViewV1 arguments[3] = {
        {argument0, argument0 == nullptr ? 0U : std::strlen(argument0)},
        {argument1, argument1 == nullptr ? 0U : std::strlen(argument1)},
        {argument2, argument2 == nullptr ? 0U : std::strlen(argument2)}};
    std::size_t argument_count = 0;
    if (argument2 != nullptr) {
        argument_count = 3;
    } else if (argument1 != nullptr) {
        argument_count = 2;
    } else if (argument0 != nullptr) {
        argument_count = 1;
    }

    thread_local char buffer[kTranslationBytes];
    std::size_t size = sizeof(buffer);
    const CabbirdStatusV1 status = service->translate(
        service->user, StringView(key), StringView(english), arguments, argument_count,
        buffer, &size);
    if (status.code != CABBIRD_STATUS_V1_OK || size == 0 || buffer[0] == '\0') return english;
    return buffer;
}

constexpr const char* kStorageKey = "damage-replay.txt";
constexpr const char* kStorageCountField = "replay_count=";
constexpr std::size_t kMaximumStoredBytes = 256;

std::atomic<int> g_stored_count{kDefaultReplayCount};

int ClampReplayCount(const long long value) noexcept {
    if (value < kMinimumReplayCount) return kMinimumReplayCount;
    if (value > kMaximumReplayCount) return kMaximumReplayCount;
    return static_cast<int>(value);
}

bool ReadStoredCount(int* const count) noexcept {
    if (count == nullptr) return false;
    const CabbirdStorageServiceV1* const storage = g_storage;
    if (storage == nullptr || storage->read == nullptr) return false;

    std::size_t size = 0;
    CabbirdStatusV1 status = storage->read(
        storage->user, StringView(kStorageKey), {nullptr, 0}, &size);
    if (status.code != CABBIRD_STATUS_V1_OK || size == 0 || size > kMaximumStoredBytes) {
        return false;
    }

    char document[kMaximumStoredBytes + 1]{};
    std::size_t capacity = kMaximumStoredBytes;
    status = storage->read(
        storage->user, StringView(kStorageKey),
        {reinterpret_cast<std::uint8_t*>(document), capacity}, &capacity);
    if (status.code != CABBIRD_STATUS_V1_OK || capacity > kMaximumStoredBytes) return false;
    document[capacity] = '\0';

    const char* field = std::strstr(document, kStorageCountField);
    if (field == nullptr) return false;
    field += std::strlen(kStorageCountField);
    char* end = nullptr;
    const long parsed = std::strtol(field, &end, 10);
    if (end == field) return false;

    *count = ClampReplayCount(parsed);
    return true;
}

bool WriteStoredCount(const int count) noexcept {
    const CabbirdStorageServiceV1* const storage = g_storage;
    if (storage == nullptr || storage->write_atomic == nullptr) return false;

    char document[64]{};
    const int length = std::snprintf(
        document, sizeof(document), "%s%d\n", kStorageCountField, count);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(document)) return false;

    const CabbirdStatusV1 status = storage->write_atomic(
        storage->user, StringView(kStorageKey),
        {reinterpret_cast<const std::uint8_t*>(document), static_cast<std::size_t>(length)});
    return status.code == CABBIRD_STATUS_V1_OK;
}

void FlushStoredCount() noexcept {
    const int current = g_replay_count.load(std::memory_order_relaxed);
    if (current == g_stored_count.load(std::memory_order_relaxed)) return;
    if (WriteStoredCount(current)) {
        g_stored_count.store(current, std::memory_order_relaxed);
    }
}

bool HookUsable() noexcept {
    return g_hook != nullptr && g_hook->create != nullptr && g_hook->release != nullptr &&
           g_hook->begin_callback != nullptr && g_hook->end_callback != nullptr;
}

bool ReadBytes(std::uintptr_t address, void* destination, std::size_t size) noexcept {
    if (address == 0 || destination == nullptr) return false;
    const CabbirdCoreServiceV1* const core = g_core;
    if (core == nullptr || core->read_memory == nullptr) return false;
    const CabbirdStatusV1 status = core->read_memory(
        core->user, address,
        {reinterpret_cast<std::uint8_t*>(destination), size});
    return status.code == CABBIRD_STATUS_V1_OK;
}

bool IsPlayerSourced(std::uintptr_t element) noexcept {
    if (element == 0) return false;
    std::int32_t source_player = 0;
    if (!ReadBytes(element + kSourcePlayerEntityIdOffset, &source_player, sizeof(source_player))) {
        return false;
    }
    return source_player != 0;
}

class CallbackLease {
public:
    CallbackLease() noexcept {
        if (!HookUsable()) return;
        CabbirdGenerationHandleV1 lease{};
        if (g_hook->begin_callback(g_hook->user, g_handle, &lease).code == CABBIRD_STATUS_V1_OK) {
            lease_ = lease;
            held_ = true;
        }
    }

    ~CallbackLease() noexcept {
        if (held_ && g_hook != nullptr && g_hook->end_callback != nullptr) {
            static_cast<void>(g_hook->end_callback(g_hook->user, lease_));
        }
    }

    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;

private:
    CabbirdGenerationHandleV1 lease_{};
    bool held_{false};
};

using OnExecuteDamageElementFn = void(CABBIRD_CALL*)(std::uintptr_t self, std::uintptr_t element);

void CABBIRD_CALL DetourElementExecute(std::uintptr_t self, std::uintptr_t element) {
    static_cast<void>(self);

    const CallbackLease lease;

    OnExecuteDamageElementFn trampoline = nullptr;
    for (int spin = 0; spin < 200000; ++spin) {
        trampoline = reinterpret_cast<OnExecuteDamageElementFn>(
            g_trampoline.load(std::memory_order_acquire));
        if (trampoline != nullptr) break;
        YieldProcessor();
    }
    if (trampoline == nullptr) {
        return;
    }

    g_calls.fetch_add(1, std::memory_order_relaxed);

    const int repeats = g_replay_count.load(std::memory_order_relaxed);
    const bool outgoing = IsPlayerSourced(element);

    const int effective = (repeats > 1 && !outgoing) ? 1 : repeats;
    if (repeats > 1 && !outgoing) {
        g_refused.fetch_add(1, std::memory_order_relaxed);
    }
    if (effective > 1) {
        g_replayed.fetch_add(static_cast<std::uint64_t>(effective - 1),
                             std::memory_order_relaxed);
    }

    for (int repeat = 0; repeat < effective; ++repeat) {
        trampoline(self, element);
    }
}

bool InstallHook() {
    if (!HookUsable()) {
        SetStatus(kStatusNoHookService);
        return false;
    }
    if (g_installed.load(std::memory_order_relaxed)) {
        SetStatus(kStatusAlreadyInstalled);
        return true;
    }
    if (g_core == nullptr || g_core->module_base == nullptr) {
        SetStatus(kStatusNoGameAssembly);
        return false;
    }

    const std::uintptr_t base =
        g_core->module_base(g_core->user, StringView("GameAssembly.dll"));
    if (base == 0) {
        SetStatus(kStatusNoGameAssembly);
        return false;
    }
    const std::uintptr_t target = base + kOnExecuteDamageElementRva;

    CabbirdHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = CABBIRD_HOOK_V1_FUNCTION;
    request.target = target;
    request.detour = reinterpret_cast<void*>(&DetourElementExecute);
    request.label = StringView("cabbird.damage-replay");

    std::uintptr_t original = 0;
    CabbirdGenerationHandleV1 handle{};
    const CabbirdStatusV1 status = g_hook->create(g_hook->user, &request, &original, &handle);
    if (status.code != CABBIRD_STATUS_V1_OK || original == 0) {
        SetStatus(kStatusCreateRefused, static_cast<int>(status.code));
        return false;
    }

    g_handle = handle;
    g_trampoline.store(original, std::memory_order_release);
    g_installed.store(true, std::memory_order_relaxed);

    SetStatus(kStatusInstalled);
    return true;
}

void RemoveHook() {
    g_installed.store(false, std::memory_order_relaxed);
    g_trampoline.store(0, std::memory_order_release);
    if (g_hook != nullptr && g_hook->release != nullptr) {
        static_cast<void>(g_hook->release(g_hook->user, g_handle));
    }
    g_handle = CabbirdGenerationHandleV1{};
    SetStatus(kStatusRemoved);
}

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *context = nullptr;
    const Host services(host);

    g_core = services.Query<CabbirdCoreServiceV1>(CABBIRD_CORE_SERVICE_V1_ID,
                                                 CABBIRD_CORE_SERVICE_V1_VERSION).get();
    if (g_core == nullptr || g_core->read_memory == nullptr || g_core->module_base == nullptr) {
        g_core = nullptr;
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }

    g_ui = services.Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID,
                                             CABBIRD_UI_SERVICE_V1_VERSION).get();
    if (g_ui == nullptr || g_ui->text == nullptr || g_ui->begin_window == nullptr ||
        g_ui->end_window == nullptr) {
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_hook = services.Query<CabbirdHookServiceV1>(CABBIRD_HOOK_SERVICE_V1_ID,
                                                 CABBIRD_HOOK_SERVICE_V1_VERSION).get();

    g_localization = services.Query<CabbirdLocalizationServiceV1>(
        CABBIRD_LOCALIZATION_SERVICE_V1_ID, CABBIRD_LOCALIZATION_SERVICE_V1_VERSION).get();
    if (g_localization != nullptr &&
        (g_localization->translate == nullptr || g_localization->locale == nullptr)) {
        g_localization = nullptr;
    }
    g_storage = services.Query<CabbirdStorageServiceV1>(
        CABBIRD_STORAGE_SERVICE_V1_ID, CABBIRD_STORAGE_SERVICE_V1_VERSION).get();
    if (g_storage != nullptr &&
        (g_storage->read == nullptr || g_storage->write_atomic == nullptr)) {
        g_storage = nullptr;
    }

    int stored = kDefaultReplayCount;
    if (ReadStoredCount(&stored)) {
        g_replay_count.store(stored, std::memory_order_relaxed);
        g_stored_count.store(stored, std::memory_order_relaxed);
    }

    if (!HookUsable()) {
        SetStatus(kStatusHookServiceUnavailable);
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) {
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) {
    FlushStoredCount();
    return Ok();
}

void CABBIRD_CALL Unload(void*) {
    g_ui = nullptr;
    g_core = nullptr;
    g_hook = nullptr;
    g_localization = nullptr;
    g_storage = nullptr;
}

std::atomic<int> g_pending{0};
int g_open = 1;

void CABBIRD_CALL Update(void*, double) {
    const int pending = g_pending.exchange(0, std::memory_order_acq_rel);
    if (pending == 1) {
        InstallHook();
    } else if (pending == 2) {
        RemoveHook();
    }
}

void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1* ui) {
    if (ui == nullptr) return;

    const int visible = ui->begin_window(ui->user, StringView("Damage Replay"), &g_open, 0);
    if (visible == 0) {
        ui->end_window(ui->user);
        return;
    }

    const bool installed = g_installed.load(std::memory_order_relaxed);
    const bool busy = g_pending.load(std::memory_order_relaxed) != 0;

    static int enabled_ui = 0;
    if (!busy) {
        enabled_ui = installed ? 1 : 0;
    }
    if (ui->checkbox != nullptr) {
        if (ui->checkbox(ui->user,
                         StringView(Tr("cabbird.damage-replay.enabled", "enabled")),
                         &enabled_ui) != 0) {
            g_pending.store(enabled_ui != 0 ? 1 : 2, std::memory_order_release);
        }
    } else {
        ui->text(ui->user, StringView(Tr("cabbird.damage-replay.no-checkbox",
                                         "this host publishes no checkbox control")));
    }

    if (ui->separator != nullptr) ui->separator(ui->user);

    ui->text(ui->user,
             StringView(Tr("cabbird.damage-replay.replay-count", "replay count")));
    if (ui->input_uint32 != nullptr) {
        std::uint32_t value = static_cast<std::uint32_t>(g_replay_count.load(std::memory_order_relaxed));
        if (ui->input_uint32(ui->user, StringView("##replay"), &value, 1, 10) != 0) {
            if (value < static_cast<std::uint32_t>(kMinimumReplayCount)) {
                value = static_cast<std::uint32_t>(kMinimumReplayCount);
            }
            if (value > static_cast<std::uint32_t>(kMaximumReplayCount)) {
                value = static_cast<std::uint32_t>(kMaximumReplayCount);
            }
            g_replay_count.store(static_cast<int>(value), std::memory_order_relaxed);
        }
    }

    if (ui->separator != nullptr) ui->separator(ui->user);

    char calls[24]{};
    char replayed[24]{};
    char refused[24]{};
    std::snprintf(calls, sizeof(calls), "%llu",
                  static_cast<unsigned long long>(g_calls.load(std::memory_order_relaxed)));
    std::snprintf(replayed, sizeof(replayed), "%llu",
                  static_cast<unsigned long long>(g_replayed.load(std::memory_order_relaxed)));
    std::snprintf(refused, sizeof(refused), "%llu",
                  static_cast<unsigned long long>(g_refused.load(std::memory_order_relaxed)));
    ui->text(ui->user, StringView(Tr(
        "cabbird.damage-replay.counters",
        "calls {0}   replayed {1}   incoming refused {2}",
        calls, replayed, refused)));

    const int status_id = g_status_id.load(std::memory_order_acquire);
    const bool has_status = status_id > kStatusNone && status_id < kStatusCount;
    if (has_status || busy) {
        if (ui->separator != nullptr) ui->separator(ui->user);
    }
    if (has_status) {
        const StatusText& entry = kStatusTexts[status_id];
        char code[16]{};
        std::snprintf(code, sizeof(code), "%d", g_status_code.load(std::memory_order_relaxed));
        ui->text(ui->user, StringView(Tr(entry.key, entry.english, code)));
    }
    if (busy) {
        ui->text(ui->user, StringView(Tr("cabbird.damage-replay.applying",
                                         "applying on the game thread...")));
    }

    ui->end_window(ui->user);
}

}

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    descriptor->id = StringView("cabbird.damage-replay");
    descriptor->name = StringView("Damage Replay");
    descriptor->author = StringView("Cabbird");
    descriptor->version = StringView("0.3.0");
    descriptor->on_load = Load;
    descriptor->on_start = Start;
    descriptor->on_stop = Stop;
    descriptor->on_unload = Unload;
    descriptor->on_update = Update;
    descriptor->on_draw = Draw;
    return Ok();
}
