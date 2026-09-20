#include "cabbird/plugin_manager.hpp"

#include "cabbird/json.hpp"
#include "cabbird/adapter_service_registry.hpp"
#include "cabbird/i18n.hpp"
#include "cabbird/plugin_capability_policy.hpp"
#include "cabbird/plugin_dependency_resolver.hpp"
#include "cabbird/plugin_native_dependency.hpp"
#include "cabbird/plugin_package.hpp"
#include "cabbird/scoped_platform_services.hpp"
#include "cabbird/structured_logger.hpp"
#include "cabbird/thread_local_value.hpp"
#include "cabbird/ui_resource_decoder.hpp"
#include "cabbird/unitymem_compat.hpp"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <malloc.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cabbird {

struct PluginCacheOwnerLease final {
    explicit PluginCacheOwnerLease(HANDLE value) noexcept : value(value) {}
    ~PluginCacheOwnerLease() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }

    PluginCacheOwnerLease(const PluginCacheOwnerLease&) = delete;
    PluginCacheOwnerLease& operator=(const PluginCacheOwnerLease&) = delete;

    HANDLE value{INVALID_HANDLE_VALUE};
};


struct OverlayProxyState;

struct OverlayProxyRegistration final {
    std::shared_ptr<cabbird::PluginScope> scope;
    std::weak_ptr<OverlayProxyState> state;
    PluginManager* manager{};
    std::uint64_t generation{};
    std::uint64_t ledger_token{};
    CabbirdGenerationHandleV1 proxy_handle{};
    const CabbirdUnityOverlayServiceV1* service{};
    CabbirdGenerationHandleV1 service_handle{};
    CabbirdUnityOverlayDrawCallbackV1 callback{};
    void* callback_user{};
    std::atomic_bool active{};
    std::atomic_bool revoked{};
    std::atomic<std::uint32_t> revoke_status{CABBIRD_STATUS_V1_OK};
};

struct OverlayProxyState final {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<OverlayProxyRegistration>> registrations;
    std::uint64_t next_handle_id{1};
};

namespace {

// PUMP LIVENESS COUNTERS.
//
// `per-plugin` callback metrics answer "did THIS plugin's callback run", but they cannot
// answer "was the tick that drives every plugin called at all" -- and those are different
// failures with completely different fixes.  A window that shows only its initial values
// looks identical in both cases, so for several rounds the two were indistinguishable and I
// kept "fixing" the per-plugin side of a host-side problem.
//
// `g_game_update_calls` is incremented on EVERY entry to `PluginManager::GameUpdate`, before
// any per-plugin gate (started / callback lease / faulted).  It is therefore the direct
// answer to "is the plugin host's Game-domain pump running", which is the first question to
// ask rather than the last.
//
// `g_game_update_ticks` counts calls that arrived with a usable delta.  Exposed next to the
// call count because "called, but every call carried nonsense" and "never called" are again
// different bugs.
std::atomic<std::uint64_t> g_game_update_calls{0};
std::atomic<std::uint64_t> g_game_update_ticks{0};
// Same idea for the render domain, which is a SECOND, independent pump: a plugin whose Draw
// never runs draws nothing regardless of how healthy Update is.
std::atomic<std::uint64_t> g_draw_calls{0};

PluginManager* g_manager{};
bool g_process_quarantined{};
cabbird::ThreadLocalObject<std::string> g_loading_plugin_id;
cabbird::ThreadLocalObject<std::filesystem::path> g_loading_package_directory;
cabbird::ThreadLocalScalar<cabbird::LogThreadDomain> g_log_thread_domain;
cabbird::ThreadLocalScalar<cabbird::PluginScope*> g_callback_scope;
cabbird::ThreadLocalScalar<std::uint64_t> g_callback_generation;
cabbird::ThreadLocalScalar<bool> g_lifecycle_callback;
cabbird::ThreadLocalScalar<std::unique_lock<std::mutex>*> g_lifecycle_plugin_lock;

constexpr std::wstring_view kPluginCacheOwnerFile{L".owner.lock"};
constexpr std::array<std::uint8_t, 8> kPngSignature{
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};

std::optional<DWORD> CacheDirectoryProcessId(const std::filesystem::path& directory) noexcept {
    const std::string name = directory.filename().string();
    if (name.empty()) return std::nullopt;
    std::uint32_t value{};
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || value == 0) {
        return std::nullopt;
    }
    return static_cast<DWORD>(value);
}

bool CacheDirectoryHasOwner(const std::filesystem::path& directory) noexcept {
    const std::filesystem::path owner_file = directory / kPluginCacheOwnerFile;
    const HANDLE owner = CreateFileW(
        owner_file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (owner != INVALID_HANDLE_VALUE) {
        CloseHandle(owner);
        return false;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) return true;
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    return true;
}

void PruneStalePluginCaches(const std::filesystem::path& cache_root) noexcept {
    std::vector<std::filesystem::path> stale;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(cache_root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto process_id = CacheDirectoryProcessId(iterator->path());
        if (!process_id || !iterator->is_directory(error)) {
            error.clear();
            continue;
        }
        if (!CacheDirectoryHasOwner(iterator->path())) {
            stale.push_back(iterator->path());
        }
    }
    for (const std::filesystem::path& directory : stale) {
        error.clear();
        std::filesystem::remove_all(directory, error);
    }
}

std::unique_ptr<PluginCacheOwnerLease> AcquirePluginCacheOwner(
    const std::filesystem::path& cache_root,
    const std::filesystem::path& process_cache) {
    PruneStalePluginCaches(cache_root);
    std::error_code error;
    std::filesystem::remove_all(process_cache, error);
    error.clear();
    std::filesystem::create_directories(process_cache, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "plugin cache directory could not be created", process_cache, error);
    }
    const std::filesystem::path owner_file = process_cache / kPluginCacheOwnerFile;
    const HANDLE owner = CreateFileW(
        owner_file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (owner == INVALID_HANDLE_VALUE) {
        const std::error_code owner_error(
            static_cast<int>(GetLastError()), std::system_category());
        std::filesystem::remove_all(process_cache, error);
        throw std::filesystem::filesystem_error(
            "plugin cache ownership could not be acquired", owner_file, owner_error);
    }
    return std::make_unique<PluginCacheOwnerLease>(owner);
}

void RemoveEmptyPluginCacheRoot(const std::filesystem::path& cache_root) noexcept {
    std::error_code error;
    static_cast<void>(std::filesystem::remove(cache_root, error));
}

class ScopedLogThreadDomain final {
public:
    explicit ScopedLogThreadDomain(cabbird::LogThreadDomain domain) noexcept
        : previous_(g_log_thread_domain.Get()) {
        g_log_thread_domain.Set(domain);
    }

    ~ScopedLogThreadDomain() { g_log_thread_domain.Set(previous_); }

private:
    cabbird::LogThreadDomain previous_;
};

class ScopedPluginCallback final {
public:
    ScopedPluginCallback(
        std::shared_ptr<cabbird::PluginScope> scope,
        std::uint64_t generation,
        bool lifecycle) noexcept
        : scope_(std::move(scope)),
          previous_scope_(g_callback_scope.Get()),
          previous_generation_(g_callback_generation.Get()),
          previous_lifecycle_(g_lifecycle_callback.Get()) {
        g_callback_scope.Set(scope_.get());
        g_callback_generation.Set(generation);
        g_lifecycle_callback.Set(lifecycle);
    }

    ~ScopedPluginCallback() {
        g_callback_scope.Set(previous_scope_);
        g_callback_generation.Set(previous_generation_);
        g_lifecycle_callback.Set(previous_lifecycle_);
    }

    ScopedPluginCallback(const ScopedPluginCallback&) = delete;
    ScopedPluginCallback& operator=(const ScopedPluginCallback&) = delete;

private:
    std::shared_ptr<cabbird::PluginScope> scope_;
    cabbird::PluginScope* previous_scope_{};
    std::uint64_t previous_generation_{};
    bool previous_lifecycle_{};
};

cabbird::PluginScope::CallbackLease AcquireCurrentCallbackLease() noexcept {
    cabbird::PluginScope* scope = g_callback_scope.Get();
    if (scope == nullptr) return {};
    return g_lifecycle_callback.Get()
        ? scope->AcquireLifecycleLease(g_callback_generation.Get())
        : scope->AcquireCallback(g_callback_generation.Get());
}

std::chrono::steady_clock::time_point StopDeadlineAfter(
    std::chrono::milliseconds timeout) noexcept {
    const auto bounded = (std::max)(timeout, std::chrono::milliseconds::zero());
    if (bounded == std::chrono::milliseconds::max()) {
        return std::chrono::steady_clock::time_point::max();
    }
    const auto now = std::chrono::steady_clock::now();
    const auto available = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - now);
    return bounded >= available
        ? std::chrono::steady_clock::time_point::max()
        : now + bounded;
}

std::chrono::milliseconds StopRemaining(
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return std::chrono::milliseconds::max();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return std::chrono::milliseconds::zero();
    return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

std::wstring WideUtf8(const char* text) {
    if (text == nullptr || *text == '\0') return {};
    const int length = static_cast<int>(std::strlen(text));
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, result.data(), size);
    return result;
}

std::string Utf8(const std::filesystem::path& path) {
    const auto value = path.wstring();
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

constexpr std::uintmax_t kMaximumUiWindowStateFileBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaximumUiPersistentWindows = 10000U;
constexpr std::size_t kMaximumPersistentPluginWindows = 10000U;
constexpr std::size_t kMaximumPluginIdBytes = 255U;
constexpr auto kUiWindowStateSaveInterval = std::chrono::milliseconds(500);

struct PersistentUiState final {
    std::vector<cabbird::UiWindowPersistentState> windows;
    std::unordered_map<std::string, bool> plugin_windows;
};

std::string SerializeUiWindowState(
    const std::vector<cabbird::UiWindowPersistentState>& windows,
    const std::unordered_map<std::string, bool>& plugin_windows) {
    nlohmann::json document{{"schemaVersion", 1}, {"windows", nlohmann::json::array()},
        {"pluginWindows", nlohmann::json::array()}};
    for (const cabbird::UiWindowPersistentState& window : windows) {
        document["windows"].push_back({
            {"stableId", window.stable_id},
            {"open", window.open},
            {"width", window.width},
            {"height", window.height},
            {"constraints", {
                {"minimumWidth", window.constraints.minimum_width},
                {"minimumHeight", window.constraints.minimum_height},
                {"maximumWidth", window.constraints.maximum_width},
                {"maximumHeight", window.constraints.maximum_height}}}});
    }
    std::vector<std::pair<std::string, bool>> sorted_plugin_windows(
        plugin_windows.begin(), plugin_windows.end());
    std::ranges::sort(sorted_plugin_windows, {}, &std::pair<std::string, bool>::first);
    for (const auto& [plugin_id, visible] : sorted_plugin_windows) {
        document["pluginWindows"].push_back({
            {"pluginId", plugin_id},
            {"visible", visible}});
    }
    return document.dump(2) + '\n';
}

PersistentUiState ParseUiWindowState(
    const nlohmann::json& document) {
    if (!document.is_object() || document.value("schemaVersion", 0U) != 1U ||
        !document.contains("windows") || !document["windows"].is_array() ||
        document["windows"].size() > kMaximumUiPersistentWindows) {
        throw std::runtime_error("UI window state document is invalid");
    }

    PersistentUiState result;
    result.windows.reserve(document["windows"].size());
    for (const nlohmann::json& item : document["windows"]) {
        if (!item.is_object() || !item.contains("constraints") ||
            !item["constraints"].is_object()) {
            throw std::runtime_error("UI window state record is invalid");
        }
        const nlohmann::json& constraints = item["constraints"];
        cabbird::UiWindowPersistentState state;
        state.stable_id = item.at("stableId").get<std::string>();
        state.open = item.at("open").get<bool>();
        state.width = item.at("width").get<float>();
        state.height = item.at("height").get<float>();
        state.constraints = {
            constraints.at("minimumWidth").get<float>(),
            constraints.at("minimumHeight").get<float>(),
            constraints.at("maximumWidth").get<float>(),
            constraints.at("maximumHeight").get<float>()};
        result.windows.push_back(std::move(state));
    }

    if (!document.contains("pluginWindows")) return result;
    const nlohmann::json& plugin_windows = document["pluginWindows"];
    if (!plugin_windows.is_array() ||
        plugin_windows.size() > kMaximumPersistentPluginWindows) {
        throw std::runtime_error("plugin window state collection is invalid");
    }
    result.plugin_windows.reserve(plugin_windows.size());
    for (const nlohmann::json& item : plugin_windows) {
        if (!item.is_object()) {
            throw std::runtime_error("plugin window state record is invalid");
        }
        std::string plugin_id = item.at("pluginId").get<std::string>();
        const bool visible = item.at("visible").get<bool>();
        if (plugin_id.empty() || plugin_id.size() > kMaximumPluginIdBytes ||
            !result.plugin_windows.emplace(std::move(plugin_id), visible).second) {
            throw std::runtime_error("plugin window state value is invalid");
        }
    }
    return result;
}

struct CallbackMetrics {
    std::uint64_t calls{};
    std::uint64_t faults{};
    std::uint64_t slow_calls{};
    std::deque<double> milliseconds;

    void Record(double elapsed, bool fault, double slow_threshold) {
        ++calls;
        if (fault) ++faults;
        if (elapsed >= slow_threshold) ++slow_calls;
        milliseconds.push_back(elapsed);
        if (milliseconds.size() > 512) milliseconds.pop_front();
    }

    [[nodiscard]] CallbackMetricsView View() const {
        CallbackMetricsView result{calls, faults, slow_calls};
        if (milliseconds.empty()) return result;
        std::vector<double> sorted(milliseconds.begin(), milliseconds.end());
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&](double value) {
            const auto index = static_cast<std::size_t>(value * static_cast<double>(sorted.size() - 1));
            return sorted[index];
        };
        result.p50_milliseconds = percentile(0.50);
        result.p95_milliseconds = percentile(0.95);
        result.p99_milliseconds = percentile(0.99);
        return result;
    }
};

enum class UiStackEntryKind : std::uint8_t {
    Window,
    ScopedWindow,
    Child,
    Table,
    TabBar,
    TabItem,
    Menu,
    Popup,
    Font,
};

struct UiStackEntry final {
    UiStackEntryKind kind{UiStackEntryKind::Window};
    cabbird::UiResourceHandle resource{};
};

struct UiStackTracker final {
    std::vector<UiStackEntry> entries;
    bool mismatch{};

    [[nodiscard]] bool ReserveNext() noexcept {
        if (entries.size() == entries.max_size()) return false;
        try {
            entries.reserve(entries.size() + 1U);
        } catch (...) {
            return false;
        }
        return true;
    }

    void PushReserved(const UiStackEntry entry) noexcept { entries.push_back(entry); }

    [[nodiscard]] bool HasTop(
        const UiStackEntryKind kind,
        const cabbird::UiResourceHandle resource = {}) const noexcept {
        return !entries.empty() && entries.back().kind == kind &&
            (resource.id == 0 || entries.back().resource == resource);
    }

    [[nodiscard]] bool Consume(
        const UiStackEntryKind kind,
        const cabbird::UiResourceHandle resource = {}) noexcept {
        if (!HasTop(kind, resource)) {
            mismatch = true;
            return false;
        }
        entries.pop_back();
        return true;
    }

    void MarkMismatch() noexcept { mismatch = true; }

    void Reset() noexcept {
        entries.clear();
        mismatch = false;
    }
};

struct PluginUiProxyContext {
    const CabbirdUiServiceV1* service{};
    UiStackTracker* ui_stack{};
    std::shared_ptr<cabbird::PluginScope> scope;
    std::string plugin_id;
    std::uint64_t generation{};
    bool close_requested{};
    bool reopen_requested{};
    UiStackEntryKind window_begin_kind{UiStackEntryKind::Window};
    UiStackEntryKind window_end_kind{UiStackEntryKind::Window};
    cabbird::UiResourceHandle window_resource{};
};

void AppendHexIdentifier(std::string& destination, const std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    destination.reserve(destination.size() + value.size() * 2U);
    for (const unsigned char byte : value) {
        destination.push_back(digits[byte >> 4U]);
        destination.push_back(digits[byte & 0x0fU]);
    }
}

std::string NamespacedPluginWindowTitle(
    const PluginUiProxyContext& context, const CabbirdStringViewV1 title) {
    const std::string_view raw = title.data == nullptr
        ? std::string_view{}
        : std::string_view(title.data, title.size);
    if (context.plugin_id.empty()) return std::string(raw);

    const std::size_t marker = raw.find("###");
    const std::string_view visible = marker == std::string_view::npos
        ? raw : raw.substr(0U, marker);
    const std::string_view local_id = marker == std::string_view::npos
        ? raw : raw.substr(marker + 3U);
    std::string result(visible);
    result += "###cabbird-plugin:";
    AppendHexIdentifier(result, context.plugin_id);
    result.push_back(':');
    AppendHexIdentifier(result, local_id);
    return result;
}

class ScopedUiProxyWindowKind final {
public:
    ScopedUiProxyWindowKind(
        PluginUiProxyContext& context, const UiStackEntryKind kind,
        const cabbird::UiResourceHandle resource = {}) noexcept
        : context_(context),
          previous_begin_kind_(context.window_begin_kind),
          previous_end_kind_(context.window_end_kind),
          previous_resource_(context.window_resource) {
        context_.window_begin_kind = kind;
        context_.window_end_kind = kind;
        context_.window_resource = resource;
    }

    ~ScopedUiProxyWindowKind() {
        context_.window_begin_kind = previous_begin_kind_;
        context_.window_end_kind = previous_end_kind_;
        context_.window_resource = previous_resource_;
    }

    ScopedUiProxyWindowKind(const ScopedUiProxyWindowKind&) = delete;
    ScopedUiProxyWindowKind& operator=(const ScopedUiProxyWindowKind&) = delete;

private:
    PluginUiProxyContext& context_;
    UiStackEntryKind previous_begin_kind_;
    UiStackEntryKind previous_end_kind_;
    cabbird::UiResourceHandle previous_resource_;
};

PluginUiProxyContext* UiProxyContext(void* user) noexcept {
    return static_cast<PluginUiProxyContext*>(user);
}

cabbird::PluginScope::CallbackLease AcquireUiCallback(
    const PluginUiProxyContext* context) noexcept {
    if (context == nullptr || context->scope == nullptr ||
        g_callback_scope.Get() != context->scope.get() ||
        g_callback_generation.Get() != context->generation ||
        g_lifecycle_callback.Get() ||
        g_log_thread_domain.Get() != cabbird::LogThreadDomain::Render) {
        return {};
    }
    return context->scope->AcquireCallback(context->generation);
}

cabbird::PluginScope::CallbackLease AcquireUiStateCallback(
    const PluginUiProxyContext* context) noexcept {
    if (context == nullptr || context->scope == nullptr ||
        g_callback_scope.Get() != context->scope.get() ||
        g_callback_generation.Get() != context->generation ||
        g_lifecycle_callback.Get() ||
        (g_log_thread_domain.Get() != cabbird::LogThreadDomain::Render &&
         g_log_thread_domain.Get() != cabbird::LogThreadDomain::Game)) {
        return {};
    }
    return context->scope->AcquireCallback(context->generation);
}

template <typename Field>
bool HasUiField(const CabbirdUiServiceV1* service, std::size_t offset) noexcept {
    return service != nullptr && service->struct_size >= offset + sizeof(Field);
}

bool ReserveUiStackEntry(PluginUiProxyContext* context) noexcept {
    if (context == nullptr || context->ui_stack == nullptr) return false;
    if (context->ui_stack->ReserveNext()) return true;
    context->ui_stack->MarkMismatch();
    return false;
}

bool ConsumeUiStackEntry(
    PluginUiProxyContext* context, const UiStackEntryKind kind) noexcept {
    return context != nullptr && context->ui_stack != nullptr &&
        context->ui_stack->Consume(kind);
}

void CABBIRD_CALL ProxySetNextWindowSize(
    void* user, float width, float height, std::uint32_t condition) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr && context->service != nullptr &&
        context->service->set_next_window_size != nullptr) {
        context->service->set_next_window_size(
            context->service->user, width, height, condition);
    }
}

int CABBIRD_CALL ProxyBeginWindow(
    void* user, CabbirdStringViewV1 title, int* open, std::uint32_t flags) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr || context->service == nullptr ||
        context->service->begin_window == nullptr || context->service->end_window == nullptr ||
        context->ui_stack == nullptr) {
        return 0;
    }
    if (!context->ui_stack->ReserveNext()) {
        context->ui_stack->MarkMismatch();
        return 0;
    }
    if (open != nullptr && context->reopen_requested) {
        *open = 1;
        context->reopen_requested = false;
    }
    const std::string scoped_title = NamespacedPluginWindowTitle(*context, title);
    const int result = context->service->begin_window(
        context->service->user,
        {scoped_title.data(), scoped_title.size()}, open, flags);
    context->ui_stack->PushReserved({context->window_begin_kind, context->window_resource});
    if (open != nullptr && *open == 0 &&
        context->window_begin_kind != UiStackEntryKind::ScopedWindow) {
        context->close_requested = true;
    }
    return result;
}

void CABBIRD_CALL ProxyEndWindow(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr && context->service != nullptr &&
        context->service->end_window != nullptr && context->ui_stack != nullptr &&
        context->ui_stack->Consume(context->window_end_kind, context->window_resource)) {
        context->service->end_window(context->service->user);
    }
}

void CABBIRD_CALL ProxyText(void* user, CabbirdStringViewV1 text) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr && context->service != nullptr && context->service->text != nullptr) {
        context->service->text(context->service->user, text);
    }
}

int CABBIRD_CALL ProxyButton(
    void* user, CabbirdStringViewV1 label, float width, float height) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr && context->service != nullptr &&
            context->service->button != nullptr
        ? context->service->button(context->service->user, label, width, height)
        : 0;
}

int CABBIRD_CALL ProxyDrawEntityBbox(
    void* user, const CabbirdEspCameraV1* camera,
    const CabbirdEspEntityBoundsV1* bounds, const CabbirdEspBoxStyleV1* style) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr && context->service != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::draw_entity_bbox)>(
                context->service, offsetof(CabbirdUiServiceV1, draw_entity_bbox)) &&
            context->service->draw_entity_bbox != nullptr
        ? context->service->draw_entity_bbox(context->service->user, camera, bounds, style)
        : 0;
}

int CABBIRD_CALL ProxyCheckbox(void* user, CabbirdStringViewV1 label, int* value) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::checkbox)>(
                context->service, offsetof(CabbirdUiServiceV1, checkbox)) &&
            context->service->checkbox != nullptr
        ? context->service->checkbox(context->service->user, label, value) : 0;
}

int CABBIRD_CALL ProxySliderFloat(
    void* user, CabbirdStringViewV1 label, float* value, float minimum, float maximum) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::slider_float)>(
                context->service, offsetof(CabbirdUiServiceV1, slider_float)) &&
            context->service->slider_float != nullptr
        ? context->service->slider_float(context->service->user, label, value, minimum, maximum)
        : 0;
}

int CABBIRD_CALL ProxyInputUInt32(
    void* user, CabbirdStringViewV1 label, std::uint32_t* value,
    std::uint32_t step, std::uint32_t step_fast) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::input_uint32)>(
                context->service, offsetof(CabbirdUiServiceV1, input_uint32)) &&
            context->service->input_uint32 != nullptr
        ? context->service->input_uint32(
              context->service->user, label, value, step, step_fast)
        : 0;
}

int CABBIRD_CALL ProxyInputDouble(
    void* user, CabbirdStringViewV1 label, double* value, double step, double step_fast) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::input_double)>(
                context->service, offsetof(CabbirdUiServiceV1, input_double)) &&
            context->service->input_double != nullptr
        ? context->service->input_double(
              context->service->user, label, value, step, step_fast)
        : 0;
}

int CABBIRD_CALL ProxyDeveloperModeEnabled(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiStateCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::developer_mode_enabled)>(
                context->service, offsetof(CabbirdUiServiceV1, developer_mode_enabled)) &&
            context->service->developer_mode_enabled != nullptr
        ? context->service->developer_mode_enabled(context->service->user)
        : 0;
}

int CABBIRD_CALL ProxyInputText(
    void* user, CabbirdStringViewV1 label, char* buffer,
    std::size_t buffer_capacity, std::uint32_t flags) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::input_text)>(
                context->service, offsetof(CabbirdUiServiceV1, input_text)) &&
            context->service->input_text != nullptr
        ? context->service->input_text(
              context->service->user, label, buffer, buffer_capacity, flags)
        : 0;
}

int CABBIRD_CALL ProxyButtonEnabled(
    void* user, CabbirdStringViewV1 label, float width, float height, int enabled) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::button_enabled)>(
                context->service, offsetof(CabbirdUiServiceV1, button_enabled)) &&
            context->service->button_enabled != nullptr
        ? context->service->button_enabled(
              context->service->user, label, width, height, enabled)
        : 0;
}

int CABBIRD_CALL ProxyColorEdit4(
    void* user, CabbirdStringViewV1 label, float rgba[4]) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::color_edit4)>(
                context->service, offsetof(CabbirdUiServiceV1, color_edit4)) &&
            context->service->color_edit4 != nullptr
        ? context->service->color_edit4(context->service->user, label, rgba) : 0;
}

int CABBIRD_CALL ProxyDrawEntityBox3d(
    void* user, const CabbirdEspCameraV1* camera,
    const CabbirdEspEntityBoundsV1* bounds, const CabbirdEspBoxStyleV1* style) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::draw_entity_box3d)>(
                context->service, offsetof(CabbirdUiServiceV1, draw_entity_box3d)) &&
            context->service->draw_entity_box3d != nullptr
        ? context->service->draw_entity_box3d(context->service->user, camera, bounds, style)
        : 0;
}

int CABBIRD_CALL ProxyDrawEntityLabel(
    void* user, const CabbirdEspCameraV1* camera,
    const CabbirdEspEntityBoundsV1* bounds, CabbirdStringViewV1 text,
    std::uint32_t color) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::draw_entity_label)>(
                context->service, offsetof(CabbirdUiServiceV1, draw_entity_label)) &&
            context->service->draw_entity_label != nullptr
        ? context->service->draw_entity_label(
              context->service->user, camera, bounds, text, color)
        : 0;
}

void CABBIRD_CALL ProxySeparator(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::separator)>(
            context->service, offsetof(CabbirdUiServiceV1, separator)) &&
        context->service->separator != nullptr) {
        context->service->separator(context->service->user);
    }
}

int CABBIRD_CALL ProxyBeginChild(
    void* user, CabbirdStringViewV1 id, float width, float height, std::uint32_t flags) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr ||
        !HasUiField<decltype(CabbirdUiServiceV1::begin_child)>(
            context->service, offsetof(CabbirdUiServiceV1, begin_child)) ||
        !HasUiField<decltype(CabbirdUiServiceV1::end_child)>(
            context->service, offsetof(CabbirdUiServiceV1, end_child)) ||
        context->service->begin_child == nullptr || context->service->end_child == nullptr ||
        !ReserveUiStackEntry(context)) {
        return 0;
    }
    const int result = context->service->begin_child(
        context->service->user, id, width, height, flags);
    // ImGui child regions require EndChild even when BeginChild returns false.
    context->ui_stack->PushReserved({UiStackEntryKind::Child});
    return result;
}

void CABBIRD_CALL ProxyEndChild(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::end_child)>(
            context->service, offsetof(CabbirdUiServiceV1, end_child)) &&
        context->service->end_child != nullptr &&
        ConsumeUiStackEntry(context, UiStackEntryKind::Child)) {
        context->service->end_child(context->service->user);
    }
}

int CABBIRD_CALL ProxyBeginTable(
    void* user, CabbirdStringViewV1 id, std::int32_t columns,
    std::uint32_t flags, float width, float height) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr ||
        !HasUiField<decltype(CabbirdUiServiceV1::begin_table)>(
            context->service, offsetof(CabbirdUiServiceV1, begin_table)) ||
        !HasUiField<decltype(CabbirdUiServiceV1::end_table)>(
            context->service, offsetof(CabbirdUiServiceV1, end_table)) ||
        context->service->begin_table == nullptr || context->service->end_table == nullptr ||
        !ReserveUiStackEntry(context)) {
        return 0;
    }
    const int result = context->service->begin_table(
        context->service->user, id, columns, flags, width, height);
    if (result != 0) context->ui_stack->PushReserved({UiStackEntryKind::Table});
    return result;
}

void CABBIRD_CALL ProxyTableNextRow(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::table_next_row)>(
            context->service, offsetof(CabbirdUiServiceV1, table_next_row)) &&
        context->service->table_next_row != nullptr) {
        context->service->table_next_row(context->service->user);
    }
}

int CABBIRD_CALL ProxyTableNextColumn(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::table_next_column)>(
                context->service, offsetof(CabbirdUiServiceV1, table_next_column)) &&
            context->service->table_next_column != nullptr
        ? context->service->table_next_column(context->service->user)
        : 0;
}

void CABBIRD_CALL ProxyEndTable(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::end_table)>(
            context->service, offsetof(CabbirdUiServiceV1, end_table)) &&
        context->service->end_table != nullptr &&
        ConsumeUiStackEntry(context, UiStackEntryKind::Table)) {
        context->service->end_table(context->service->user);
    }
}

int CABBIRD_CALL ProxyBeginTabBar(
    void* user, CabbirdStringViewV1 id, const std::uint32_t flags) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr ||
        !HasUiField<decltype(CabbirdUiServiceV1::begin_tab_bar)>(
            context->service, offsetof(CabbirdUiServiceV1, begin_tab_bar)) ||
        !HasUiField<decltype(CabbirdUiServiceV1::end_tab_bar)>(
            context->service, offsetof(CabbirdUiServiceV1, end_tab_bar)) ||
        context->service->begin_tab_bar == nullptr || context->service->end_tab_bar == nullptr ||
        !ReserveUiStackEntry(context)) {
        return 0;
    }
    const int result = context->service->begin_tab_bar(
        context->service->user, id, flags);
    if (result != 0) context->ui_stack->PushReserved({UiStackEntryKind::TabBar});
    return result;
}

int CABBIRD_CALL ProxyBeginTabItem(
    void* user, CabbirdStringViewV1 label, int* open, const std::uint32_t flags,
    const int enabled) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr ||
        !HasUiField<decltype(CabbirdUiServiceV1::begin_tab_item)>(
            context->service, offsetof(CabbirdUiServiceV1, begin_tab_item)) ||
        !HasUiField<decltype(CabbirdUiServiceV1::end_tab_item)>(
            context->service, offsetof(CabbirdUiServiceV1, end_tab_item)) ||
        context->service->begin_tab_item == nullptr || context->service->end_tab_item == nullptr ||
        !ReserveUiStackEntry(context)) {
        return 0;
    }
    const int result = context->service->begin_tab_item(
        context->service->user, label, open, flags, enabled);
    if (result != 0) context->ui_stack->PushReserved({UiStackEntryKind::TabItem});
    return result;
}

void CABBIRD_CALL ProxyEndTabItem(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::end_tab_item)>(
            context->service, offsetof(CabbirdUiServiceV1, end_tab_item)) &&
        context->service->end_tab_item != nullptr &&
        ConsumeUiStackEntry(context, UiStackEntryKind::TabItem)) {
        context->service->end_tab_item(context->service->user);
    }
}

void CABBIRD_CALL ProxyEndTabBar(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::end_tab_bar)>(
            context->service, offsetof(CabbirdUiServiceV1, end_tab_bar)) &&
        context->service->end_tab_bar != nullptr &&
        ConsumeUiStackEntry(context, UiStackEntryKind::TabBar)) {
        context->service->end_tab_bar(context->service->user);
    }
}

int CABBIRD_CALL ProxyBeginMenu(void* user, CabbirdStringViewV1 label, int enabled) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr ||
        !HasUiField<decltype(CabbirdUiServiceV1::begin_menu)>(
            context->service, offsetof(CabbirdUiServiceV1, begin_menu)) ||
        !HasUiField<decltype(CabbirdUiServiceV1::end_menu)>(
            context->service, offsetof(CabbirdUiServiceV1, end_menu)) ||
        context->service->begin_menu == nullptr || context->service->end_menu == nullptr ||
        !ReserveUiStackEntry(context)) {
        return 0;
    }
    const int result = context->service->begin_menu(
        context->service->user, label, enabled);
    if (result != 0) context->ui_stack->PushReserved({UiStackEntryKind::Menu});
    return result;
}

void CABBIRD_CALL ProxyEndMenu(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::end_menu)>(
            context->service, offsetof(CabbirdUiServiceV1, end_menu)) &&
        context->service->end_menu != nullptr &&
        ConsumeUiStackEntry(context, UiStackEntryKind::Menu)) {
        context->service->end_menu(context->service->user);
    }
}

void CABBIRD_CALL ProxyOpenPopup(void* user, CabbirdStringViewV1 id) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::open_popup)>(
            context->service, offsetof(CabbirdUiServiceV1, open_popup)) &&
        context->service->open_popup != nullptr) {
        context->service->open_popup(context->service->user, id);
    }
}

int CABBIRD_CALL ProxyBeginPopupModal(
    void* user, CabbirdStringViewV1 id, int* open, std::uint32_t flags) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr ||
        !HasUiField<decltype(CabbirdUiServiceV1::begin_popup_modal)>(
            context->service, offsetof(CabbirdUiServiceV1, begin_popup_modal)) ||
        !HasUiField<decltype(CabbirdUiServiceV1::end_popup)>(
            context->service, offsetof(CabbirdUiServiceV1, end_popup)) ||
        context->service->begin_popup_modal == nullptr || context->service->end_popup == nullptr ||
        !ReserveUiStackEntry(context)) {
        return 0;
    }
    const int result = context->service->begin_popup_modal(
        context->service->user, id, open, flags);
    if (result != 0) context->ui_stack->PushReserved({UiStackEntryKind::Popup});
    return result;
}

void CABBIRD_CALL ProxyEndPopup(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::end_popup)>(
            context->service, offsetof(CabbirdUiServiceV1, end_popup)) &&
        context->service->end_popup != nullptr &&
        ConsumeUiStackEntry(context, UiStackEntryKind::Popup)) {
        context->service->end_popup(context->service->user);
    }
}

void CABBIRD_CALL ProxyCloseCurrentPopup(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::close_current_popup)>(
            context->service, offsetof(CabbirdUiServiceV1, close_current_popup)) &&
        context->service->close_current_popup != nullptr) {
        context->service->close_current_popup(context->service->user);
    }
}

int CABBIRD_CALL ProxyFilterMatch(
    void* user, CabbirdStringViewV1 filter, CabbirdStringViewV1 value) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::filter_match)>(
                context->service, offsetof(CabbirdUiServiceV1, filter_match)) &&
            context->service->filter_match != nullptr
        ? context->service->filter_match(context->service->user, filter, value)
        : 0;
}

std::uint32_t CABBIRD_CALL ProxyFrameState(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::frame_state)>(
                context->service, offsetof(CabbirdUiServiceV1, frame_state)) &&
            context->service->frame_state != nullptr
        ? context->service->frame_state(context->service->user)
        : 0;
}

void CABBIRD_CALL ProxySetNextWindowSizeConstraints(
    void* user, const float minimum_width, const float minimum_height,
    const float maximum_width, const float maximum_height) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::set_next_window_size_constraints)>(
            context->service,
            offsetof(CabbirdUiServiceV1, set_next_window_size_constraints)) &&
        context->service->set_next_window_size_constraints != nullptr) {
        context->service->set_next_window_size_constraints(
            context->service->user, minimum_width, minimum_height,
            maximum_width, maximum_height);
    }
}

void CABBIRD_CALL ProxyGetWindowSize(void* user, float* width, float* height) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::get_window_size)>(
            context->service, offsetof(CabbirdUiServiceV1, get_window_size)) &&
        context->service->get_window_size != nullptr) {
        context->service->get_window_size(context->service->user, width, height);
    }
}

void CABBIRD_CALL ProxySameLine(
    void* user, const float offset_from_start_x, const float spacing) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::same_line)>(
            context->service, offsetof(CabbirdUiServiceV1, same_line)) &&
        context->service->same_line != nullptr) {
        context->service->same_line(
            context->service->user, offset_from_start_x, spacing);
    }
}

void CABBIRD_CALL ProxySetCursorPosX(void* user, const float local_x) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(CabbirdUiServiceV1::set_cursor_pos_x)>(
            context->service, offsetof(CabbirdUiServiceV1, set_cursor_pos_x)) &&
        context->service->set_cursor_pos_x != nullptr) {
        context->service->set_cursor_pos_x(context->service->user, local_x);
    }
}

int CABBIRD_CALL ProxyTextLink(
    void* user, CabbirdStringViewV1 label, CabbirdStringViewV1 url) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(CabbirdUiServiceV1::text_link)>(
                context->service, offsetof(CabbirdUiServiceV1, text_link)) &&
            context->service->text_link != nullptr
        ? context->service->text_link(context->service->user, label, url)
        : 0;
}

CabbirdUiServiceV1 MakeUiProxy(PluginUiProxyContext* context) noexcept {
    CabbirdUiServiceV1 proxy{
        sizeof(CabbirdUiServiceV1), CABBIRD_UI_SERVICE_V1_VERSION,
        context,
        ProxySetNextWindowSize, ProxyBeginWindow, ProxyEndWindow, ProxyText, ProxyButton,
        ProxyDrawEntityBbox, ProxyCheckbox, ProxySliderFloat, ProxyColorEdit4,
        ProxyDrawEntityBox3d, ProxyDrawEntityLabel,
        ProxySeparator, ProxyBeginChild, ProxyEndChild, ProxyBeginTable,
        ProxyTableNextRow, ProxyTableNextColumn, ProxyEndTable, ProxyBeginMenu,
        ProxyEndMenu, ProxyOpenPopup, ProxyBeginPopupModal, ProxyEndPopup,
        ProxyCloseCurrentPopup, ProxyFilterMatch, ProxyFrameState,
        ProxySetNextWindowSizeConstraints, ProxyGetWindowSize, ProxyInputUInt32, ProxyInputDouble,
        ProxyDeveloperModeEnabled, ProxyInputText, ProxyButtonEnabled,
        ProxySameLine, ProxySetCursorPosX, ProxyTextLink,
        ProxyBeginTabBar, ProxyBeginTabItem, ProxyEndTabItem, ProxyEndTabBar};
    if (context == nullptr || context->service == nullptr) {
        proxy.struct_size = offsetof(CabbirdUiServiceV1, user) + sizeof(proxy.user);
        return proxy;
    }

    std::size_t advertised_size = (std::min)(
        static_cast<std::size_t>(context->service->struct_size), sizeof(proxy));
    if (!HasUiField<decltype(CabbirdUiServiceV1::same_line)>(
            context->service, offsetof(CabbirdUiServiceV1, same_line)) ||
        context->service->same_line == nullptr) {
        advertised_size = (std::min)(
            advertised_size, offsetof(CabbirdUiServiceV1, same_line));
    }
    if (!HasUiField<decltype(CabbirdUiServiceV1::set_cursor_pos_x)>(
            context->service, offsetof(CabbirdUiServiceV1, set_cursor_pos_x)) ||
        context->service->set_cursor_pos_x == nullptr) {
        advertised_size = (std::min)(
            advertised_size, offsetof(CabbirdUiServiceV1, set_cursor_pos_x));
    }
    if (!HasUiField<decltype(CabbirdUiServiceV1::text_link)>(
            context->service, offsetof(CabbirdUiServiceV1, text_link)) ||
        context->service->text_link == nullptr) {
        advertised_size = (std::min)(
            advertised_size, offsetof(CabbirdUiServiceV1, text_link));
    }
    if (!HasUiField<decltype(CabbirdUiServiceV1::begin_tab_bar)>(
            context->service, offsetof(CabbirdUiServiceV1, begin_tab_bar)) ||
        context->service->begin_tab_bar == nullptr) {
        advertised_size = (std::min)(
            advertised_size, offsetof(CabbirdUiServiceV1, begin_tab_bar));
    }
    if (!HasUiField<decltype(CabbirdUiServiceV1::begin_tab_item)>(
            context->service, offsetof(CabbirdUiServiceV1, begin_tab_item)) ||
        context->service->begin_tab_item == nullptr) {
        advertised_size = (std::min)(
            advertised_size, offsetof(CabbirdUiServiceV1, begin_tab_item));
    }
    if (!HasUiField<decltype(CabbirdUiServiceV1::end_tab_item)>(
            context->service, offsetof(CabbirdUiServiceV1, end_tab_item)) ||
        context->service->end_tab_item == nullptr) {
        advertised_size = (std::min)(
            advertised_size, offsetof(CabbirdUiServiceV1, end_tab_item));
    }
    if (!HasUiField<decltype(CabbirdUiServiceV1::end_tab_bar)>(
            context->service, offsetof(CabbirdUiServiceV1, end_tab_bar)) ||
        context->service->end_tab_bar == nullptr) {
        advertised_size = (std::min)(
            advertised_size, offsetof(CabbirdUiServiceV1, end_tab_bar));
    }
    proxy.struct_size = static_cast<std::uint32_t>(advertised_size);
    return proxy;
}

const char* LevelName(CabbirdCoreLogLevelV1 level) {
    switch (level) {
    case CABBIRD_CORE_LOG_LEVEL_V1_TRACE: return "trace";
    case CABBIRD_CORE_LOG_LEVEL_V1_INFO: return "info";
    case CABBIRD_CORE_LOG_LEVEL_V1_WARNING: return "warning";
    case CABBIRD_CORE_LOG_LEVEL_V1_ERROR: return "error";
    default: return "unknown";
    }
}

cabbird::LogLevel StructuredLevel(CabbirdCoreLogLevelV1 level) noexcept {
    switch (level) {
    case CABBIRD_CORE_LOG_LEVEL_V1_TRACE: return cabbird::LogLevel::Trace;
    case CABBIRD_CORE_LOG_LEVEL_V1_INFO: return cabbird::LogLevel::Info;
    case CABBIRD_CORE_LOG_LEVEL_V1_WARNING: return cabbird::LogLevel::Warning;
    case CABBIRD_CORE_LOG_LEVEL_V1_ERROR: return cabbird::LogLevel::Error;
    default: return cabbird::LogLevel::Info;
    }
}

std::string StatusMessage(std::string_view stage, const CabbirdStatusV1& status) {
    std::string detail = std::string(stage) + " failed: code=" + std::to_string(status.code);
    const char* code_name = "unknown";
    switch (status.code) {
    case CABBIRD_STATUS_V1_OK: code_name = "ok"; break;
    case CABBIRD_STATUS_V1_INVALID_ARGUMENT: code_name = "invalid-argument"; break;
    case CABBIRD_STATUS_V1_UNAVAILABLE: code_name = "unavailable"; break;
    case CABBIRD_STATUS_V1_NOT_FOUND: code_name = "not-found"; break;
    case CABBIRD_STATUS_V1_BUFFER_TOO_SMALL: code_name = "buffer-too-small"; break;
    case CABBIRD_STATUS_V1_FAILED: code_name = "failed"; break;
    case CABBIRD_STATUS_V1_TIMEOUT: code_name = "timeout"; break;
    case CABBIRD_STATUS_V1_PERMISSION_DENIED: code_name = "permission-denied"; break;
    case CABBIRD_STATUS_V1_CONFLICT: code_name = "conflict"; break;
    case CABBIRD_STATUS_V1_CANCELLED: code_name = "cancelled"; break;
    default: break;
    }
    detail += " (" + std::string(code_name) + ")";
    if (status.message.data != nullptr && status.message.size != 0) {
        detail += ", message=" + std::string(status.message.data, status.message.size);
    }
    return detail;
}

void HostLog(CabbirdCoreLogLevelV1 level, const char* message) {
    auto callback = AcquireCurrentCallbackLease();
    if (g_callback_scope.Get() != nullptr && !callback) return;
    if (g_manager != nullptr) g_manager->Log(level, message == nullptr ? "" : message);
}

const cabbird::CoreMemoryServices* HostMemoryServices() noexcept {
    return g_manager == nullptr ? nullptr : &g_manager->MemoryServices();
}

bool ReadHostMemory(
    const std::uintptr_t address, void* const destination, const std::size_t size) {
    const auto* services = HostMemoryServices();
    return services != nullptr && services->memory != nullptr &&
        services->memory->ReadMemoryInto(address, destination, size);
}

bool WriteHostMemory(
    const std::uintptr_t address, const void* const source, const std::size_t size) {
    const auto* services = HostMemoryServices();
    return services != nullptr && services->memory != nullptr &&
        services->memory->WriteMemory(address, source, size);
}

CabbirdStatusV1 StatusV1(std::uint32_t code, const char* message = nullptr) {
    return {code, 0, {message, message == nullptr ? 0 : std::strlen(message)}};
}

bool ValidPluginStateId(const std::string_view id) noexcept {
    if (id.size() < 3 || id.size() > 255) return false;
    bool saw_separator{};
    std::size_t segment_size{};
    bool previous_hyphen{};
    for (const char character : id) {
        if (character == '.') {
            if (segment_size == 0 || previous_hyphen) return false;
            saw_separator = true;
            segment_size = 0;
            previous_hyphen = false;
            continue;
        }
        const bool alpha_numeric =
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
        if (!alpha_numeric && character != '-') return false;
        if (segment_size == 0 && character == '-') return false;
        if (++segment_size > 63) return false;
        previous_hyphen = character == '-';
    }
    return saw_separator && segment_size != 0 && !previous_hyphen;
}

struct JsonNode final {
    std::shared_ptr<nlohmann::json> root;
    nlohmann::json* value{};
};

struct PluginServiceContext {
    PluginManager* manager{};
    PluginUiProxyContext* ui_proxy_context{};
    UiStackTracker* ui_stack{};
    std::string plugin_id;
    std::uint64_t generation{};
    std::filesystem::path package_directory;
    std::filesystem::path state_directory;
    std::filesystem::path configuration_directory;
    std::shared_ptr<cabbird::PluginScope> scope;
    cabbird::PluginCapabilityGrant capabilities;
    cabbird::ScopedPlatformServices* platform{};
    cabbird::IpcRegistry* ipc{};
    std::vector<std::string> ipc_dependencies;
    cabbird::Locale localization_locale{cabbird::Locale::EnUs};
    std::shared_ptr<const cabbird::PluginCatalog> localization_catalog;
    std::atomic_bool localization_fallback_logged{};
    CabbirdCoreServiceV1 core{};
    CabbirdPluginStateServiceV1 plugin_state{};
    CabbirdConfigServiceV1 config{};
    CabbirdStorageServiceV1 storage{};
    CabbirdJsonServiceV1 json{};
    CabbirdRuntimeInfoServiceV1 runtime_info{};
    CabbirdLocalizationServiceV1 localization{};
    CabbirdDiagnosticsServiceV1 diagnostics{};
    CabbirdSchedulerServiceV1 scheduler{};
    CabbirdIpcServiceV1 ipc_service{};
    CabbirdCommandsServiceV1 commands{};
    CabbirdNotificationsServiceV1 notifications{};
    CabbirdSignatureServiceV1 signature{};
    CabbirdHookServiceV1 hook{};
    CabbirdPatchServiceV1 patch{};
    const CabbirdUiServiceV1* ui{};
    cabbird::UiResourceRegistry* ui_resources{};
    cabbird::InputService* input{};
    CabbirdWindowServiceV1 window{};
    CabbirdFontServiceV1 font{};
    CabbirdTextureServiceV1 texture{};
    CabbirdInputServiceV1 input_service{};
    std::shared_ptr<OverlayProxyState> overlay_state;
    CabbirdUnityOverlayServiceV1 overlay{};
    /* The per-plugin `cabbird.unity.player` table, and the adapter table it forwards to.
     *
     * The adapter's player table is shared by every plugin and carries `write_position` /
     * `write_state` -- entries the ABI cannot remove (append-only) even though the write half was
     * split into `cabbird.unity.player-teleport`.  Handing that table out unchanged would make the
     * split a comment rather than a boundary: a manifest that declared only `unity-player-snapshot`
     * (a read) could still move the player, because service capability enforcement happens at QUERY
     * time and cannot tell which entry a plugin later calls.
     *
     * WHY THE COPY EXISTS AT ALL, when the sibling project needs no such thing: its player table
     * never contained a write (its `AnomalyNtePlayerServiceV1` is three snapshot entries), so its
     * `nte-player-snapshot` grant IS the whole answer.  This table's v1 shape cannot be changed, so
     * the entry-level decision has to happen somewhere -- and it needs the CALLER's identity, which
     * the shared table does not carry.  The copy supplies exactly that: `user` is this context.
     *
     * The DECISION is not made here (see `RequirePlayerTeleportCapability`): it is
     * `PluginCapabilityGrant::AuthorizePlayerWrite`, the sibling of `AuthorizeRawMemory`.
     * `snapshot` is forwarded unchanged -- the read needs no second grant, because
     * `unity-player-snapshot` already gated the query itself. */
    const CabbirdUnityPlayerServiceV1* adapter_player{};
    CabbirdUnityPlayerServiceV1 player_service{};
    std::mutex json_mutex;
    std::unordered_map<std::uint64_t, JsonNode> json_nodes;
    std::vector<cabbird::UiResourceHandle> open_windows;
    std::vector<cabbird::UiResourceHandle> pushed_fonts;
};

cabbird::PluginScope::CallbackLease AcquireServiceCallback(
    const PluginServiceContext* context) noexcept {
    if (context == nullptr || context->scope == nullptr) return {};
    const bool is_active_lifecycle_callback = g_lifecycle_callback.Get() &&
        g_callback_scope.Get() == context->scope.get() &&
        g_callback_generation.Get() == context->generation;
    return is_active_lifecycle_callback
        ? context->scope->AcquireLifecycleLease(context->generation)
        : context->scope->AcquireCallback(context->generation);
}

bool ValidServiceContext(const PluginServiceContext* context) noexcept {
    return context != nullptr && context->manager == g_manager &&
        context->scope != nullptr &&
        context->generation == context->scope->Generation() &&
        context->plugin_id == context->scope->Owner();
}

constexpr std::size_t kOverlayServiceV1Size =
    offsetof(CabbirdUnityOverlayServiceV1, unsubscribe) +
    sizeof(CabbirdUnityOverlayServiceV1::unsubscribe);

bool ValidOverlayServiceV1(const CabbirdUnityOverlayServiceV1* service) noexcept {
    return service != nullptr && service->struct_size >= kOverlayServiceV1Size &&
        service->service_version >= CABBIRD_UNITY_OVERLAY_SERVICE_V1_VERSION &&
        service->subscribe != nullptr && service->unsubscribe != nullptr;
}

const CabbirdUnityOverlayServiceV1* QueryOverlayServiceV1() noexcept {
    const void* service = cabbird::ProcessAdapterServices().Query(
        CABBIRD_UNITY_OVERLAY_SERVICE_V1_ID,
        CABBIRD_UNITY_OVERLAY_SERVICE_V1_VERSION);
    const auto* overlay = static_cast<const CabbirdUnityOverlayServiceV1*>(service);
    return ValidOverlayServiceV1(overlay) ? overlay : nullptr;
}

void RevokeOverlayRegistration(
    const std::shared_ptr<OverlayProxyRegistration>& registration) noexcept {
    if (registration == nullptr || registration->revoked.exchange(true)) return;
    registration->active.store(false, std::memory_order_release);
    if (ValidOverlayServiceV1(registration->service)) {
        try {
            const CabbirdStatusV1 status = registration->service->unsubscribe(
                registration->service->user, registration->service_handle);
            registration->revoke_status.store(status.code, std::memory_order_release);
        } catch (...) {
            registration->revoke_status.store(
                CABBIRD_STATUS_V1_FAILED, std::memory_order_release);
        }
    }
    if (const auto state = registration->state.lock()) {
        std::scoped_lock lock(state->mutex);
        const auto found = state->registrations.find(registration->proxy_handle.id);
        if (found != state->registrations.end() && found->second == registration) {
            state->registrations.erase(found);
        }
    }
}

void CABBIRD_CALL InvokeOverlayDrawCallbackV1(
    void* user, const CabbirdUnityOverlayFrameV1* frame) noexcept {
    auto* const candidate = static_cast<OverlayProxyRegistration*>(user);
    if (candidate == nullptr) return;
    const auto state = candidate->state.lock();
    if (state == nullptr) return;
    std::shared_ptr<OverlayProxyRegistration> registration;
    {
        std::scoped_lock lock(state->mutex);
        const auto found = state->registrations.find(candidate->proxy_handle.id);
        if (found == state->registrations.end() || found->second.get() != candidate) {
            return;
        }
        registration = found->second;
    }
    if (!registration->active.load(std::memory_order_acquire) ||
        registration->scope == nullptr || registration->callback == nullptr) {
        return;
    }
    auto callback = registration->scope->AcquireCallback(registration->generation);
    if (!callback || !registration->active.load(std::memory_order_acquire)) return;
    const ScopedLogThreadDomain log_domain(cabbird::LogThreadDomain::Game);
    ScopedPluginCallback callback_scope(
        registration->scope, registration->generation, false);
    try {
        registration->callback(registration->callback_user, frame);
    } catch (...) {
        if (registration->manager != nullptr) {
            try {
                registration->manager->LogPlugin(
                    CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
                    "exception in ABI v1 overlay callback",
                    registration->scope->Owner(), registration->generation);
            } catch (...) {
            }
        }
    }
}

CabbirdStatusV1 CABBIRD_CALL SubscribeOverlayV1(
    void* user, CabbirdUnityOverlayDrawCallbackV1 callback, void* callback_user,
    CabbirdGenerationHandleV1* handle) noexcept {
    if (handle == nullptr || callback == nullptr) {
        return StatusV1(
            CABBIRD_STATUS_V1_INVALID_ARGUMENT,
            "overlay subscription arguments are invalid");
    }
    *handle = {};
    auto* const context = static_cast<PluginServiceContext*>(user);
    if (!ValidServiceContext(context) || context->overlay_state == nullptr) {
        return StatusV1(
            CABBIRD_STATUS_V1_INVALID_ARGUMENT,
            "plugin overlay service context is invalid");
    }
    auto service_callback = AcquireServiceCallback(context);
    if (!service_callback) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    const CabbirdUnityOverlayServiceV1* const service = QueryOverlayServiceV1();
    if (service == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "overlay service is not ready");
    }
    std::shared_ptr<OverlayProxyRegistration> registration;
    bool subscribed{};
    try {
        registration = std::make_shared<OverlayProxyRegistration>();
        registration->scope = context->scope;
        registration->state = context->overlay_state;
        registration->manager = context->manager;
        registration->generation = context->generation;
        registration->service = service;
        registration->callback = callback;
        registration->callback_user = callback_user;

        CabbirdStatusV1 status = service->subscribe(
            service->user, InvokeOverlayDrawCallbackV1, registration.get(),
            &registration->service_handle);
        if (status.code != CABBIRD_STATUS_V1_OK) return status;
        subscribed = true;
        if (registration->service_handle.id == 0) {
            RevokeOverlayRegistration(registration);
            return StatusV1(
                CABBIRD_STATUS_V1_FAILED,
                "overlay service returned an invalid subscription handle");
        }

        {
            std::scoped_lock lock(context->overlay_state->mutex);
            std::uint64_t id = context->overlay_state->next_handle_id++;
            if (id == 0) id = context->overlay_state->next_handle_id++;
            registration->proxy_handle = {id, context->generation};
            context->overlay_state->registrations.emplace(id, registration);
        }
        registration->active.store(true, std::memory_order_release);
        registration->ledger_token = context->scope->Register(
            cabbird::PluginResourceKind::Subscription,
            "cabbird.unity.overlay.draw",
            [registration] { RevokeOverlayRegistration(registration); });
        if (registration->ledger_token == 0) {
            RevokeOverlayRegistration(registration);
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
        }
        if (registration->revoked.load(std::memory_order_acquire)) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
        }
        *handle = registration->proxy_handle;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    } catch (...) {
        if (subscribed) RevokeOverlayRegistration(registration);
        return StatusV1(CABBIRD_STATUS_V1_FAILED, "overlay subscription failed");
    }
}

CabbirdStatusV1 CABBIRD_CALL UnsubscribeOverlayV1(
    void* user, const CabbirdGenerationHandleV1 handle) noexcept {
    auto* const context = static_cast<PluginServiceContext*>(user);
    if (!ValidServiceContext(context) || context->overlay_state == nullptr ||
        handle.id == 0 || handle.generation != context->generation) {
        return StatusV1(
            CABBIRD_STATUS_V1_INVALID_ARGUMENT,
            "overlay subscription handle is invalid");
    }
    auto service_callback = AcquireServiceCallback(context);
    if (!service_callback) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    std::shared_ptr<OverlayProxyRegistration> registration;
    {
        std::scoped_lock lock(context->overlay_state->mutex);
        const auto found = context->overlay_state->registrations.find(handle.id);
        if (found == context->overlay_state->registrations.end() ||
            found->second->proxy_handle.generation != handle.generation) {
            return StatusV1(
                CABBIRD_STATUS_V1_NOT_FOUND,
                "overlay subscription is not registered");
        }
        registration = found->second;
    }
    if (!context->scope->Release(registration->ledger_token)) {
        return StatusV1(
            CABBIRD_STATUS_V1_NOT_FOUND,
            "overlay subscription is not registered");
    }
    const std::uint32_t revoke_status =
        registration->revoke_status.load(std::memory_order_acquire);
    return revoke_status == CABBIRD_STATUS_V1_OK
        ? StatusV1(CABBIRD_STATUS_V1_OK)
        : StatusV1(revoke_status, "overlay provider rejected unsubscription");
}

bool IsActiveDrawResourceCallback(const PluginServiceContext& context) noexcept {
    return context.scope != nullptr &&
        g_callback_scope.Get() == context.scope.get() &&
        g_callback_generation.Get() == context.generation &&
        !g_lifecycle_callback.Get() &&
        g_log_thread_domain.Get() == cabbird::LogThreadDomain::Render;
}

bool IsSynchronousStateIoForbidden() noexcept {
    return g_log_thread_domain.Get() == cabbird::LogThreadDomain::Game ||
        g_log_thread_domain.Get() == cabbird::LogThreadDomain::Render;
}

cabbird::ScopedPluginServiceOwner ScopedPlatformOwner(
    const PluginServiceContext& context) {
    return {context.scope, context.state_directory, context.configuration_directory};
}

cabbird::IpcPluginOwner IpcOwner(const PluginServiceContext& context) {
    return {context.scope, context.ipc_dependencies};
}

cabbird::IpcCallingDomain CurrentIpcDomain() noexcept {
    switch (g_log_thread_domain.Get()) {
    case cabbird::LogThreadDomain::Lifecycle: return cabbird::IpcCallingDomain::Lifecycle;
    case cabbird::LogThreadDomain::Worker: return cabbird::IpcCallingDomain::Worker;
    case cabbird::LogThreadDomain::Game: return cabbird::IpcCallingDomain::Game;
    case cabbird::LogThreadDomain::Render: return cabbird::IpcCallingDomain::Render;
    default: return cabbird::IpcCallingDomain::Unknown;
    }
}

template <typename Callback>
CabbirdStatusV1 InvokeScopedPlatform(void* user, Callback&& callback) noexcept {
    auto* context = static_cast<PluginServiceContext*>(user);
    if (!ValidServiceContext(context) || context->platform == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    auto lease = AcquireServiceCallback(context);
    if (!lease) return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    try {
        return callback(*context);
    } catch (...) {
        return StatusV1(CABBIRD_STATUS_V1_FAILED, "scoped platform service failed");
    }
}

template <typename Callback>
CabbirdStatusV1 InvokeStateIoService(void* user, Callback&& callback) noexcept {
    if (IsSynchronousStateIoForbidden()) {
        return StatusV1(
            CABBIRD_STATUS_V1_UNAVAILABLE,
            "plugin state I/O is unavailable from game or render callbacks");
    }
    return InvokeScopedPlatform(user, std::forward<Callback>(callback));
}

template <typename Callback>
CabbirdStatusV1 InvokeUiResourceService(void* user, Callback&& callback) noexcept {
    auto* context = static_cast<PluginServiceContext*>(user);
    if (!ValidServiceContext(context) || context->ui_resources == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin resource context is invalid");
    }
    auto lease = AcquireServiceCallback(context);
    if (!lease) return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    try {
        return callback(*context);
    } catch (...) {
        return StatusV1(CABBIRD_STATUS_V1_FAILED, "plugin resource service failed");
    }
}

std::string_view ServiceString(const CabbirdStringViewV1 value) noexcept;

bool ValidGenerationHandle(
    const PluginServiceContext& context, const CabbirdGenerationHandleV1 handle) noexcept {
    return handle.id != 0 && handle.generation == context.generation;
}

cabbird::UiResourceHandle UiHandle(const CabbirdGenerationHandleV1 handle) noexcept {
    return {handle.id};
}

CabbirdGenerationHandleV1 GenerationHandle(
    const PluginServiceContext& context, const cabbird::UiResourceHandle handle) noexcept {
    return {handle.id, context.generation};
}

constexpr std::uint32_t kHostWindowNoCollapse = 1U << 5U;
constexpr std::uint32_t kHostWindowNoSavedSettings = 1U << 8U;
constexpr std::uint32_t kHostWindowFirstUse = 4U;

std::uint32_t ToHostUiWindowFlags(const std::uint32_t flags) noexcept {
    std::uint32_t result{};
    if ((flags & CABBIRD_WINDOW_V1_NO_COLLAPSE) != 0) result |= kHostWindowNoCollapse;
    if ((flags & CABBIRD_WINDOW_V1_NO_SAVED_SETTINGS) != 0) {
        result |= kHostWindowNoSavedSettings;
    }
    return result;
}

std::uint32_t ToInputCaptureFlags(const cabbird::InputCaptureFlags flags) noexcept {
    std::uint32_t result{};
    if (cabbird::HasCaptureFlag(flags, cabbird::InputCaptureFlag::Mouse)) {
        result |= CABBIRD_INPUT_CAPTURE_V1_MOUSE;
    }
    if (cabbird::HasCaptureFlag(flags, cabbird::InputCaptureFlag::Keyboard)) {
        result |= CABBIRD_INPUT_CAPTURE_V1_KEYBOARD;
    }
    if (cabbird::HasCaptureFlag(flags, cabbird::InputCaptureFlag::Text)) {
        result |= CABBIRD_INPUT_CAPTURE_V1_TEXT;
    }
    return result;
}

void PopulateInputSnapshot(
    const cabbird::InputSnapshot& source, CabbirdInputSnapshotV1& destination) noexcept {
    destination = {};
    destination.struct_size = sizeof(destination);
    destination.modifiers = source.modifiers;
    destination.sequence = source.sequence;
    destination.timestamp_milliseconds = source.timestamp_milliseconds;
    destination.mouse_x = source.mouse_x;
    destination.mouse_y = source.mouse_y;
    destination.mouse_wheel = source.mouse_wheel_delta;
    for (std::size_t index = 1; index < source.keys.size(); ++index) {
        if (source.keys[index]) destination.keys[index / 8U] |= static_cast<std::uint8_t>(1U << (index % 8U));
    }
    for (std::size_t index{}; index < source.mouse_buttons.size(); ++index) {
        if (source.mouse_buttons[index]) {
            destination.mouse_buttons |= static_cast<std::uint8_t>(1U << index);
        }
    }
}

struct PackageResourcePath final {
    std::filesystem::path package_directory;
    std::string relative_path;
};

std::optional<PackageResourcePath> ResolvePackageResourcePath(
    const PluginServiceContext& context, const CabbirdStringViewV1 value) {
    const std::string_view raw = ServiceString(value);
    if (raw.empty() || context.package_directory.empty()) return std::nullopt;
    const cabbird::PluginPackagePathResult relative =
        cabbird::ValidatePluginPackageRelativePath(raw, false);
    if (!relative.Ok()) return std::nullopt;
    return PackageResourcePath{context.package_directory, Utf8(relative.path)};
}

class ScopedFileHandle final {
public:
    explicit ScopedFileHandle(const HANDLE value) noexcept : value_(value) {}

    ~ScopedFileHandle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }

    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return value_; }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

template <typename Request>
cabbird::UiResourceReadResult ReadPackageResourceBytes(
    const Request& request, const std::size_t maximum_bytes,
    const cabbird::UiResourceAllocationAdmission admission) {
    if (request.package_directory.empty()) {
        return cabbird::ReadUiResourceBytes(
            std::filesystem::path(request.relative_path), maximum_bytes, admission);
    }

    HANDLE raw_file = INVALID_HANDLE_VALUE;
    const cabbird::PluginPackagePathResult opened = cabbird::OpenConfinedPluginPackageFile(
        request.package_directory, request.relative_path, false, &raw_file);
    if (!opened.Ok() || raw_file == INVALID_HANDLE_VALUE) {
        cabbird::UiResourceReadResult failed;
        failed.error = opened.error == cabbird::PluginPackageError::ReparsePoint
            ? cabbird::UiResourceDecodeError::ReparsePoint
            : cabbird::UiResourceDecodeError::PathUnavailable;
        return failed;
    }
    const ScopedFileHandle file(raw_file);
    return cabbird::ReadUiResourceBytesFromFileHandle(file.Get(), maximum_bytes, admission);
}

std::string_view ServiceString(const CabbirdStringViewV1 value) noexcept {
    return value.data == nullptr ? std::string_view{} : std::string_view(value.data, value.size);
}

CabbirdStatusV1 CopyLocalizationString(
    const std::string_view value, char* const destination, std::size_t* const inout_size) noexcept {
    if (inout_size == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "localization size is null");
    }
    const std::size_t required = value.size() + 1U;
    if (destination == nullptr) {
        *inout_size = required;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (*inout_size < required) {
        *inout_size = required;
        return StatusV1(
            CABBIRD_STATUS_V1_BUFFER_TOO_SMALL, "localization destination is too small");
    }
    std::memcpy(destination, value.data(), value.size());
    destination[value.size()] = '\0';
    *inout_size = required;
    return StatusV1(CABBIRD_STATUS_V1_OK);
}

void ReportLocalizationFallback(
    PluginServiceContext& context, std::string detail) noexcept {
    if (context.localization_locale == cabbird::Locale::EnUs ||
        context.localization_fallback_logged.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    try {
        context.manager->LogPlugin(
            CABBIRD_CORE_LOG_LEVEL_V1_WARNING,
            "plugin localization fallback: " + std::move(detail),
            context.plugin_id, context.generation);
    } catch (...) {
    }
}

template <typename Callback>
CabbirdStatusV1 InvokeLocalizationService(void* const user, Callback&& callback) noexcept {
    auto* const context = static_cast<PluginServiceContext*>(user);
    if (!ValidServiceContext(context) || context->localization_catalog == nullptr) {
        return StatusV1(
            CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin localization context is invalid");
    }
    auto lease = AcquireServiceCallback(context);
    if (!lease) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    try {
        return callback(*context);
    } catch (...) {
        return StatusV1(CABBIRD_STATUS_V1_FAILED, "plugin localization service failed");
    }
}

CabbirdStatusV1 CABBIRD_CALL LocaleV1(
    void* const user, char* const destination, std::size_t* const inout_size) {
    return InvokeLocalizationService(user, [&](const PluginServiceContext& context) {
        return CopyLocalizationString(
            cabbird::LocaleName(context.localization_locale), destination, inout_size);
    });
}

CabbirdStatusV1 CABBIRD_CALL TranslateV1(
    void* const user,
    const CabbirdStringViewV1 key,
    const CabbirdStringViewV1 english_fallback,
    const CabbirdStringViewV1* const arguments,
    const std::size_t argument_count,
    char* const destination,
    std::size_t* const inout_size) {
    if (key.data == nullptr || key.size == 0 || english_fallback.data == nullptr ||
        inout_size == nullptr || argument_count > 8U ||
        (argument_count != 0 && arguments == nullptr)) {
        return StatusV1(
            CABBIRD_STATUS_V1_INVALID_ARGUMENT, "localization request is invalid");
    }
    std::array<std::string_view, 8> argument_views{};
    for (std::size_t index = 0; index < argument_count; ++index) {
        if (arguments[index].data == nullptr && arguments[index].size != 0) {
            return StatusV1(
                CABBIRD_STATUS_V1_INVALID_ARGUMENT, "localization argument is invalid");
        }
        argument_views[index] = ServiceString(arguments[index]);
    }
    return InvokeLocalizationService(user, [&](PluginServiceContext& context) {
        const cabbird::PluginTranslation translation = context.localization_catalog->Translate(
            ServiceString(key), ServiceString(english_fallback),
            std::span<const std::string_view>(argument_views.data(), argument_count));
        if (translation.used_english_fallback) {
            ReportLocalizationFallback(
                context,
                "key=" + std::string(ServiceString(key)) + " reason=" +
                    (translation.argument_mismatch
                        ? "argument-mismatch"
                        : "localized-message-unavailable"));
        }
        return CopyLocalizationString(translation.text, destination, inout_size);
    });
}

void* CABBIRD_CALL AllocateV1(void* user, std::size_t size, std::size_t alignment) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return nullptr;
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    return _aligned_malloc(size, alignment);
}

void* CABBIRD_CALL ReallocateV1(
    void* user, void* memory, std::size_t size, std::size_t alignment) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return nullptr;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    return _aligned_realloc(memory, size, alignment);
}

void CABBIRD_CALL ReleaseV1(void* user, void* memory, std::size_t) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    _aligned_free(memory);
}

void CABBIRD_CALL LogV1(void* user, std::uint32_t level, CabbirdStringViewV1 message) {
    std::string copy = message.data == nullptr ? std::string{} : std::string(message.data, message.size);
    auto* context = static_cast<PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr && context->manager != nullptr && g_manager == context->manager) {
        context->manager->LogPlugin(
            static_cast<CabbirdCoreLogLevelV1>(level), std::move(copy),
            context->plugin_id, context->generation);
    } else {
        HostLog(static_cast<CabbirdCoreLogLevelV1>(level), copy.c_str());
    }
}

CabbirdStatusV1 RequireRawMemoryCapability(
    const PluginServiceContext* context,
    const std::string_view capability) noexcept {
    if (context == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    const cabbird::PluginServiceAuthorization authorization =
        context->capabilities.AuthorizeRawMemory(capability);
    if (authorization.allowed) return StatusV1(CABBIRD_STATUS_V1_OK);
    return StatusV1(
        CABBIRD_STATUS_V1_PERMISSION_DENIED,
        capability == "memory-read"
            ? "manifest capability memory-read is required"
            : "manifest capability memory-write is required");
}

/* The write half of the player bridge, enforced per call.
 *
 * `cabbird.unity.player` is granted by `unity-player-snapshot` and `cabbird.unity.player-teleport`
 * by `unity-player-teleport`; the first table still carries `write_position` / `write_state`
 * because the ABI is append-only and `include/cabbird/unity_services.hpp` keeps that compatibility
 * path working on purpose.  A read-only manifest must therefore be refused HERE, at the entry,
 * rather than at the query.
 *
 * The decision itself is NOT made here: it is `PluginCapabilityGrant::AuthorizePlayerWrite`, the
 * sibling of `AuthorizeRawMemory`.  That is deliberate -- `cabbird.core` and `cabbird.unity.player`
 * are the two services whose v1 table mixes a read with a write, and the framework answers both
 * the same way, with a named capability pair resolved in `plugin_capability_policy.cpp`.  A check
 * written inline here would be a second, private policy that the mapping table does not mention.
 *
 * The message names the missing capability, because "the teleport did nothing" and "you were not
 * granted the teleport" are different problems that a user cannot tell apart from a silent
 * failure. */
CabbirdStatusV1 RequirePlayerTeleportCapability(const PluginServiceContext* context) noexcept {
    if (context == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    if (context->capabilities.AuthorizePlayerWrite().allowed) {
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    return StatusV1(
        CABBIRD_STATUS_V1_PERMISSION_DENIED,
        "manifest capability unity-player-teleport is required to write the player");
}

const CabbirdUnityPlayerServiceV1* AdapterPlayer(const PluginServiceContext* context) noexcept {
    const CabbirdUnityPlayerServiceV1* const player =
        context == nullptr ? nullptr : context->adapter_player;
    return player != nullptr && player->snapshot != nullptr ? player : nullptr;
}

CabbirdStatusV1 CABBIRD_CALL PlayerSnapshotV1(
    void* user, CabbirdUnityPlayerSnapshotV1* snapshot) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    if (!ValidServiceContext(context)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    const CabbirdUnityPlayerServiceV1* const player = AdapterPlayer(context);
    if (player == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "player service is not ready");
    }
    auto callback = AcquireServiceCallback(context);
    if (!callback) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    return player->snapshot(player->user, snapshot);
}

CabbirdStatusV1 CABBIRD_CALL PlayerWritePositionV1(
    void* user, const CabbirdUnityPlayerWriteRequestV1* request) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    if (!ValidServiceContext(context)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    const CabbirdStatusV1 authorization = RequirePlayerTeleportCapability(context);
    if (authorization.code != CABBIRD_STATUS_V1_OK) return authorization;
    const CabbirdUnityPlayerServiceV1* const player = AdapterPlayer(context);
    if (player == nullptr || player->write_position == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "player service is not ready");
    }
    auto callback = AcquireServiceCallback(context);
    if (!callback) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    return player->write_position(player->user, request);
}

CabbirdStatusV1 CABBIRD_CALL PlayerWriteStateV1(
    void* user, CabbirdUnityPlayerWriteResultV1* result) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    if (!ValidServiceContext(context)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    const CabbirdStatusV1 authorization = RequirePlayerTeleportCapability(context);
    if (authorization.code != CABBIRD_STATUS_V1_OK) return authorization;
    const CabbirdUnityPlayerServiceV1* const player = AdapterPlayer(context);
    if (player == nullptr || player->write_state == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "player service is not ready");
    }
    auto callback = AcquireServiceCallback(context);
    if (!callback) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    return player->write_state(player->user, result);
}

CabbirdStatusV1 CABBIRD_CALL ReadV1(
    void* user, std::uintptr_t address, CabbirdMutableByteSpanV1 destination) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    if (!ValidServiceContext(context)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    auto callback = AcquireServiceCallback(context);
    if (!callback) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    const CabbirdStatusV1 authorization = RequireRawMemoryCapability(context, "memory-read");
    if (authorization.code != CABBIRD_STATUS_V1_OK) return authorization;
    return destination.data != nullptr && ReadHostMemory(address, destination.data, destination.size)
        ? StatusV1(CABBIRD_STATUS_V1_OK)
        : StatusV1(CABBIRD_STATUS_V1_FAILED, "memory read failed");
}

CabbirdStatusV1 CABBIRD_CALL WriteV1(
    void* user, std::uintptr_t address, CabbirdByteSpanV1 source) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    if (!ValidServiceContext(context)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    auto callback = AcquireServiceCallback(context);
    if (!callback) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    const CabbirdStatusV1 authorization = RequireRawMemoryCapability(context, "memory-write");
    if (authorization.code != CABBIRD_STATUS_V1_OK) return authorization;
    return source.data != nullptr && WriteHostMemory(address, source.data, source.size)
        ? StatusV1(CABBIRD_STATUS_V1_OK)
        : StatusV1(CABBIRD_STATUS_V1_FAILED, "memory write failed");
}

/* Host implementations for the two Core service fields Cabbird added beyond Anomaly's table.
 *
 * Both are thin wrappers over the cabbird::mem pipeline that already validates on the live target,
 * so neither introduces new memory semantics -- they only expose an
 * existing, tested path through the C ABI.
 *
 * patch_memory reuses the memory-write capability rather than inventing a new one: PluginCapabilityGrant
 * has no separate raw-memory-patch capability, and a plugin permitted to write a data value is the
 * same plugin permitted to patch one. */
CabbirdStatusV1 CABBIRD_CALL PatchMemoryV1(
    void* user, std::uintptr_t address, CabbirdByteSpanV1 source) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    if (!ValidServiceContext(context)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    auto callback = AcquireServiceCallback(context);
    if (!callback) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    const CabbirdStatusV1 authorization = RequireRawMemoryCapability(context, "memory-write");
    if (authorization.code != CABBIRD_STATUS_V1_OK) return authorization;
    // PatchMemory changes page protection as needed, then writes, then restores -- the distinction
    // core.h draws between WriteMemory and PatchMemory.
    return source.data != nullptr && cabbird::mem::PatchMemory(address, source.data, source.size)
        ? StatusV1(CABBIRD_STATUS_V1_OK)
        : StatusV1(CABBIRD_STATUS_V1_FAILED, "memory patch failed");
}

/* Base address of a loaded module by name.  core.h documents 0 as the not-loaded result rather
 * than an error status, so this returns a bare uintptr_t and reports nothing through StatusV1. */
uintptr_t CABBIRD_CALL ModuleBaseV1(void* user, CabbirdStringViewV1 module_name) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    if (!ValidServiceContext(context)) return 0;
    auto callback = AcquireServiceCallback(context);
    if (!callback) return 0;
    if (module_name.data == nullptr || module_name.size == 0) return 0;
    // WideUtf8 takes a NUL-terminated string, and CabbirdStringViewV1 does not guarantee one, so
    // the view is materialised first.
    const std::wstring wide = WideUtf8(std::string(module_name.data, module_name.size).c_str());
    if (wide.empty()) return 0;
    const auto module = cabbird::mem::FindModule(wide);
    return module ? module->base : 0;
}
CabbirdStatusV1 CABBIRD_CALL PluginDirectoryV1(
    void* user, char* destination, std::size_t* inout_size) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    if (context == nullptr || inout_size == nullptr || context->package_directory.empty()) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin scope is unavailable");
    }
    const std::string encoded = Utf8(context->package_directory);
    const std::size_t required = encoded.size() + 1;
    if (destination == nullptr) {
        *inout_size = required;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (*inout_size < required) {
        *inout_size = required;
        return StatusV1(CABBIRD_STATUS_V1_BUFFER_TOO_SMALL, "destination is too small");
    }
    std::memcpy(destination, encoded.c_str(), required);
    *inout_size = required;
    return StatusV1(CABBIRD_STATUS_V1_OK);
}

CabbirdStatusV1 CABBIRD_CALL PluginStateDirectoryV1(
    void* user, char* destination, std::size_t* inout_size) {
    if (IsSynchronousStateIoForbidden()) {
        return StatusV1(
            CABBIRD_STATUS_V1_UNAVAILABLE,
            "plugin state I/O is unavailable from game or render callbacks");
    }
    const auto* context = static_cast<const PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    if (context == nullptr || inout_size == nullptr || context->state_directory.empty()) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin state scope is unavailable");
    }
    std::error_code error;
    std::filesystem::create_directories(context->state_directory, error);
    if (error || !std::filesystem::is_directory(context->state_directory, error) || error) {
        return StatusV1(CABBIRD_STATUS_V1_FAILED, "plugin state directory is unavailable");
    }
    const std::string encoded = Utf8(context->state_directory);
    const std::size_t required = encoded.size() + 1;
    if (destination == nullptr) {
        *inout_size = required;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (*inout_size < required) {
        *inout_size = required;
        return StatusV1(CABBIRD_STATUS_V1_BUFFER_TOO_SMALL, "destination is too small");
    }
    std::memcpy(destination, encoded.c_str(), required);
    *inout_size = required;
    return StatusV1(CABBIRD_STATUS_V1_OK);
}

CabbirdStatusV1 CABBIRD_CALL RegisterConfigSchemaV1(
    void* user, CabbirdStringViewV1 schema_id, std::uint32_t schema_version,
    CabbirdByteSpanV1 schema_json, CabbirdGenerationHandleV1* handle) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->RegisterConfigSchema(
            ScopedPlatformOwner(context), ServiceString(schema_id), schema_version, schema_json, handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL UnregisterConfigSchemaV1(
    void* user, CabbirdGenerationHandleV1 handle) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->UnregisterConfigSchema(ScopedPlatformOwner(context), handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL ReadConfigV1(
    void* user, CabbirdStringViewV1 schema_id, std::uint32_t* schema_version,
    CabbirdMutableByteSpanV1 destination, std::size_t* inout_size) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->ReadConfig(
            ScopedPlatformOwner(context), ServiceString(schema_id), schema_version, destination, inout_size);
    });
}

CabbirdStatusV1 CABBIRD_CALL WriteConfigV1(
    void* user, CabbirdStringViewV1 schema_id, std::uint32_t schema_version,
    CabbirdByteSpanV1 document) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->WriteConfig(
            ScopedPlatformOwner(context), ServiceString(schema_id), schema_version, document);
    });
}

CabbirdStatusV1 CABBIRD_CALL MigrateConfigV1(
    void* user, CabbirdStringViewV1 schema_id, CabbirdConfigMigrationV1 migration,
    void* migration_user) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->MigrateConfig(
            ScopedPlatformOwner(context), ServiceString(schema_id), migration, migration_user);
    });
}

CabbirdStatusV1 CABBIRD_CALL ReadStorageV1(
    void* user, CabbirdStringViewV1 relative_path, CabbirdMutableByteSpanV1 destination,
    std::size_t* inout_size) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->ReadStorage(
            ScopedPlatformOwner(context), ServiceString(relative_path), destination, inout_size);
    });
}

CabbirdStatusV1 CABBIRD_CALL WriteStorageV1(
    void* user, CabbirdStringViewV1 relative_path, CabbirdByteSpanV1 source) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->WriteStorage(
            ScopedPlatformOwner(context), ServiceString(relative_path), source);
    });
}

CabbirdStatusV1 CABBIRD_CALL RemoveStorageV1(
    void* user, CabbirdStringViewV1 relative_path) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->RemoveStorage(
            ScopedPlatformOwner(context), ServiceString(relative_path));
    });
}

CabbirdStatusV1 CABBIRD_CALL RuntimeInfoV1(void* user, CabbirdRuntimeInfoV1* snapshot) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->RuntimeInfo(ScopedPlatformOwner(context), snapshot);
    });
}

CabbirdStatusV1 CABBIRD_CALL RuntimeVersionV1(
    void* user, char* destination, std::size_t* inout_size) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        static_cast<void>(context);
        return context.platform->RuntimeVersion(destination, inout_size);
    });
}

CabbirdStatusV1 CABBIRD_CALL RegisterSelfTestV1(
    void* user, CabbirdStringViewV1 id, CabbirdDiagnosticSelfTestV1 callback,
    void* callback_user, CabbirdGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->RegisterSelfTest(
            ScopedPlatformOwner(context), ServiceString(id), callback, callback_user, handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL UnregisterSelfTestV1(
    void* user, CabbirdGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->UnregisterSelfTest(ScopedPlatformOwner(context), handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL RunSelfTestV1(
    void* user, CabbirdStringViewV1 id, CabbirdMutableByteSpanV1 destination,
    std::size_t* inout_size) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->RunSelfTest(
            ScopedPlatformOwner(context), ServiceString(id), destination, inout_size);
    });
}

CabbirdStatusV1 CABBIRD_CALL DiagnosticsSnapshotV1(
    void* user, CabbirdMutableByteSpanV1 destination, std::size_t* inout_size) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->DiagnosticsSnapshot(
            ScopedPlatformOwner(context), destination, inout_size);
    });
}

CabbirdStatusV1 CABBIRD_CALL ScheduleV1(
    void* user, std::uint32_t delay_milliseconds, CabbirdTaskCallbackV1 callback,
    void* callback_user, CabbirdGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->Schedule(
            ScopedPlatformOwner(context), delay_milliseconds, callback, callback_user, handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL CancelTaskV1(
    void* user, CabbirdGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->CancelTask(ScopedPlatformOwner(context), handle);
    });
}

template <typename Callback>
CabbirdStatusV1 InvokeScopedIpc(void* user, Callback&& callback) noexcept {
    auto* context = static_cast<PluginServiceContext*>(user);
    if (!ValidServiceContext(context) || context->ipc == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin IPC context is invalid");
    }
    auto lease = AcquireServiceCallback(context);
    if (!lease) return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    try { return callback(*context); }
    catch (...) { return StatusV1(CABBIRD_STATUS_V1_FAILED, "IPC service failed"); }
}

CabbirdStatusV1 CABBIRD_CALL RegisterIpcEndpointV1(
    void* user, const CabbirdIpcEndpointDescriptorV1* descriptor,
    CabbirdIpcRequestHandlerV1 handler, void* callback_user,
    CabbirdGenerationHandleV1* endpoint) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->RegisterEndpoint(
            IpcOwner(context), descriptor, handler, callback_user, endpoint);
    });
}

CabbirdStatusV1 CABBIRD_CALL UnregisterIpcEndpointV1(
    void* user, CabbirdGenerationHandleV1 endpoint) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->UnregisterEndpoint(IpcOwner(context), endpoint);
    });
}

CabbirdStatusV1 CABBIRD_CALL InvokeIpcV1(
    void* user, const CabbirdIpcEndpointSelectorV1* selector,
    CabbirdByteSpanV1 request, CabbirdMutableByteSpanV1 response,
    std::size_t* response_size) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->Invoke(
            IpcOwner(context), CurrentIpcDomain(), selector, request, response, response_size);
    });
}

CabbirdStatusV1 CABBIRD_CALL InvokeIpcAsyncV1(
    void* user, const CabbirdIpcEndpointSelectorV1* selector,
    CabbirdByteSpanV1 request, CabbirdIpcCompletionCallbackV1 completion,
    void* completion_user, CabbirdGenerationHandleV1* pending_call) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->InvokeAsync(
            IpcOwner(context), selector, request, completion, completion_user, pending_call);
    });
}

CabbirdStatusV1 CABBIRD_CALL CancelIpcV1(
    void* user, CabbirdGenerationHandleV1 pending_call) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->Cancel(IpcOwner(context), pending_call);
    });
}

CabbirdStatusV1 CABBIRD_CALL SubscribeIpcV1(
    void* user, const CabbirdIpcEndpointSelectorV1* selector,
    CabbirdIpcEventCallbackV1 callback, void* callback_user,
    CabbirdGenerationHandleV1* subscription) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->Subscribe(
            IpcOwner(context), selector, callback, callback_user, subscription);
    });
}

CabbirdStatusV1 CABBIRD_CALL UnsubscribeIpcV1(
    void* user, CabbirdGenerationHandleV1 subscription) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->Unsubscribe(IpcOwner(context), subscription);
    });
}

CabbirdStatusV1 CABBIRD_CALL PublishIpcV1(
    void* user, CabbirdGenerationHandleV1 endpoint, CabbirdByteSpanV1 event) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->Publish(IpcOwner(context), endpoint, event);
    });
}

CabbirdStatusV1 CABBIRD_CALL RegisterCommandV1(
    void* user, CabbirdStringViewV1 name, CabbirdStringViewV1 description,
    CabbirdCommandCallbackV1 callback, void* callback_user, CabbirdGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->RegisterCommand(
            ScopedPlatformOwner(context), ServiceString(name), ServiceString(description),
            callback, callback_user, handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL UnregisterCommandV1(
    void* user, CabbirdGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->UnregisterCommand(ScopedPlatformOwner(context), handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL InvokeCommandV1(
    void* user, CabbirdStringViewV1 name, CabbirdStringViewV1 arguments,
    CabbirdMutableByteSpanV1 destination, std::size_t* inout_size) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->InvokeCommand(
            ScopedPlatformOwner(context), ServiceString(name), ServiceString(arguments),
            destination, inout_size);
    });
}

CabbirdStatusV1 CABBIRD_CALL PostNotificationV1(
    void* user, CabbirdNotificationSeverityV1 severity, CabbirdStringViewV1 title,
    CabbirdStringViewV1 body, std::uint32_t timeout_milliseconds,
    CabbirdGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->PostNotification(
            ScopedPlatformOwner(context), severity, ServiceString(title), ServiceString(body),
            timeout_milliseconds, handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL DismissNotificationV1(
    void* user, CabbirdGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->DismissNotification(ScopedPlatformOwner(context), handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL ResolveSignatureV1(
    void* user, CabbirdStringViewV1 module_name, CabbirdStringViewV1 section_name,
    CabbirdStringViewV1 pattern, std::uintptr_t* address) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->ResolveSignature(
            ScopedPlatformOwner(context), ServiceString(module_name), ServiceString(section_name),
            ServiceString(pattern), address);
    });
}

CabbirdStatusV1 CABBIRD_CALL CreateHookV1(
    void* user, const CabbirdHookRequestV1* request, std::uintptr_t* original,
    CabbirdGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->CreateHook(ScopedPlatformOwner(context), request, original, handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL ReleaseHookV1(
    void* user, CabbirdGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->ReleaseHook(ScopedPlatformOwner(context), handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL BeginHookCallbackV1(
    void* user, CabbirdGenerationHandleV1 hook, CabbirdGenerationHandleV1* callback_lease) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->BeginHookCallback(
            ScopedPlatformOwner(context), hook, callback_lease);
    });
}

CabbirdStatusV1 CABBIRD_CALL EndHookCallbackV1(
    void* user, CabbirdGenerationHandleV1 callback_lease) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->EndHookCallback(ScopedPlatformOwner(context), callback_lease);
    });
}

CabbirdStatusV1 CABBIRD_CALL ApplyPatchV1(
    void* user, std::uintptr_t address, CabbirdByteSpanV1 replacement,
    CabbirdStringViewV1 label, CabbirdGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->ApplyPatch(
            ScopedPlatformOwner(context), address, replacement, ServiceString(label), handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL ReleasePatchV1(
    void* user, CabbirdGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->ReleasePatch(ScopedPlatformOwner(context), handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL RegisterWindowV1(
    void* user, const CabbirdWindowSpecV1* spec, CabbirdGenerationHandleV1* handle) {
    if (handle == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "window handle is null");
    *handle = {};
    if (spec == nullptr || spec->struct_size < sizeof(*spec)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "window spec is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        cabbird::UiWindowRequest request;
        request.id = std::string(ServiceString(spec->id));
        request.title = std::string(ServiceString(spec->title));
        request.flags = spec->flags;
        request.persist_settings = (spec->flags & CABBIRD_WINDOW_V1_NO_SAVED_SETTINGS) == 0;
        request.initial_width = spec->initial_width;
        request.initial_height = spec->initial_height;
        request.constraints = {
            spec->minimum_width, spec->minimum_height, spec->maximum_width, spec->maximum_height};
        request.default_open = spec->default_open != 0;
        const auto resource = context.ui_resources->RegisterWindow(context.scope, std::move(request));
        if (!resource) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "window spec was rejected");
        *handle = GenerationHandle(context, resource);
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL ReleaseWindowV1(
    void* user, const CabbirdGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle) ||
            !context.ui_resources->Release(context.scope, UiHandle(handle))) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "window handle is not live");
        }
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL SetWindowOpenV1(
    void* user, const CabbirdGenerationHandleV1 handle, const std::int32_t open) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle) ||
            !(open == 0
                ? context.ui_resources->CloseWindow(context.scope, UiHandle(handle))
                : context.ui_resources->OpenWindow(context.scope, UiHandle(handle)))) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "window handle is not live");
        }
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL ToggleWindowV1(
    void* user, const CabbirdGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle) ||
            !context.ui_resources->ToggleWindow(context.scope, UiHandle(handle))) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "window handle is not live");
        }
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL WindowStateV1(
    void* user, const CabbirdGenerationHandleV1 handle, CabbirdWindowStateV1* state) {
    if (state == nullptr || state->struct_size < sizeof(*state)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "window state is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "window handle is not live");
        }
        const auto window = context.ui_resources->WindowState(context.scope, UiHandle(handle));
        if (!window) return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "window handle is not live");
        *state = {sizeof(*state), window->flags, window->width, window->height,
            context.ui_resources->DeviceGeneration(), window->open ? 1 : 0, 0};
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL BeginWindowV1(
    void* user, const CabbirdGenerationHandleV1 handle, const std::uint32_t flags,
    std::int32_t* visible) {
    if (visible == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "visible is null");
    *visible = 0;
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "window rendering is not active");
        }
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "window handle is not live");
        }
        const auto window = context.ui_resources->WindowState(context.scope, UiHandle(handle));
        if (!window) return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "window handle is not live");
        if (!window->open) return StatusV1(CABBIRD_STATUS_V1_OK);
        if (context.ui_proxy_context == nullptr || context.ui_stack == nullptr ||
            context.ui_proxy_context->service == nullptr ||
            context.ui_proxy_context->service->begin_window == nullptr) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "window stack is not ready");
        }
        const CabbirdUiServiceV1* ui = reinterpret_cast<const CabbirdUiServiceV1*>(context.ui);
        if (ui == nullptr || ui->set_next_window_size == nullptr || ui->begin_window == nullptr) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "window rendering is not active");
        }
        if (HasUiField<decltype(CabbirdUiServiceV1::set_next_window_size_constraints)>(
                ui, offsetof(CabbirdUiServiceV1, set_next_window_size_constraints)) &&
            ui->set_next_window_size_constraints != nullptr) {
            ui->set_next_window_size_constraints(
                ui->user, window->constraints.minimum_width,
                window->constraints.minimum_height, window->constraints.maximum_width,
                window->constraints.maximum_height);
        }
        if (window->width > 0.0F && window->height > 0.0F) {
            ui->set_next_window_size(ui->user, window->width, window->height, kHostWindowFirstUse);
        }
        const std::string title = window->title + "###" + window->stable_id;
        // Reserve before acquiring the host UI stack. Once begin_window succeeds,
        // recording the matching end must not allocate and fail independently.
        if (context.open_windows.size() == context.open_windows.max_size()) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "window stack is exhausted");
        }
        context.open_windows.reserve(context.open_windows.size() + 1U);
        int open = 1;
        const std::size_t stack_size = context.ui_stack->entries.size();
        int result{};
        {
            ScopedUiProxyWindowKind scoped_kind(
                *context.ui_proxy_context, UiStackEntryKind::ScopedWindow, UiHandle(handle));
            result = ui->begin_window(
                ui->user, {title.data(), title.size()}, &open,
                ToHostUiWindowFlags(window->flags | flags));
        }
        if (context.ui_stack->entries.size() != stack_size + 1U ||
            !context.ui_stack->HasTop(UiStackEntryKind::ScopedWindow, UiHandle(handle))) {
            context.ui_stack->MarkMismatch();
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "window stack is unavailable");
        }
        if (open == 0) static_cast<void>(context.ui_resources->CloseWindow(context.scope, UiHandle(handle)));
        context.open_windows.push_back(UiHandle(handle));
        *visible = result != 0 ? 1 : 0;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL EndWindowV1(
    void* user, const CabbirdGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "window rendering is not active");
        }
        if (!ValidGenerationHandle(context, handle) || context.open_windows.empty() ||
            context.open_windows.back() != UiHandle(handle)) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "window end is unbalanced");
        }
        if (context.ui_proxy_context == nullptr || context.ui_stack == nullptr ||
            context.ui_proxy_context->service == nullptr ||
            context.ui_proxy_context->service->end_window == nullptr ||
            !context.ui_stack->HasTop(UiStackEntryKind::ScopedWindow, UiHandle(handle))) {
            if (context.ui_stack != nullptr) context.ui_stack->MarkMismatch();
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "window end is unbalanced");
        }
        const CabbirdUiServiceV1* ui = reinterpret_cast<const CabbirdUiServiceV1*>(context.ui);
        if (ui == nullptr || ui->end_window == nullptr) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "window rendering is not active");
        }
        if (HasUiField<decltype(CabbirdUiServiceV1::get_window_size)>(
                ui, offsetof(CabbirdUiServiceV1, get_window_size)) &&
            ui->get_window_size != nullptr) {
            float width{};
            float height{};
            ui->get_window_size(ui->user, &width, &height);
            static_cast<void>(context.ui_resources->SetWindowSize(
                context.scope, UiHandle(handle), width, height));
        }
        {
            ScopedUiProxyWindowKind scoped_kind(
                *context.ui_proxy_context, UiStackEntryKind::ScopedWindow, UiHandle(handle));
            ui->end_window(ui->user);
        }
        context.open_windows.pop_back();
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL RequestFontV1(
    void* user, const CabbirdFontRequestV1* request, CabbirdGenerationHandleV1* handle) {
    if (handle == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "font handle is null");
    *handle = {};
    if (request == nullptr || request->struct_size < sizeof(*request)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "font request is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        const auto path = ResolvePackageResourcePath(context, request->relative_path);
        if (!path) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "font path escapes package");
        cabbird::UiFontRequest descriptor;
        descriptor.package_directory = path->package_directory;
        descriptor.relative_path = path->relative_path;
        descriptor.flags = request->flags;
        descriptor.size_pixels = request->size_pixels;
        descriptor.glyph_range = static_cast<cabbird::UiGlyphRange>(request->glyph_range);
        const auto resource = context.ui_resources->RequestFont(context.scope, std::move(descriptor));
        if (!resource) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "font request was rejected");
        if (!context.manager->QueueUiFontLoad(context.scope, resource)) {
            static_cast<void>(context.ui_resources->Release(context.scope, resource));
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "font worker is not available");
        }
        *handle = GenerationHandle(context, resource);
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL ReleaseFontV1(
    void* user, const CabbirdGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle) ||
            !context.ui_resources->Release(context.scope, UiHandle(handle))) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "font handle is not live");
        }
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

std::uint32_t ToFontStateFlags(const cabbird::UiResourceState state) noexcept {
    switch (state) {
    case cabbird::UiResourceState::Queued: return CABBIRD_FONT_STATE_V1_QUEUED;
    case cabbird::UiResourceState::Ready: return CABBIRD_FONT_STATE_V1_READY;
    case cabbird::UiResourceState::Failed: return CABBIRD_FONT_STATE_V1_FAILED;
    case cabbird::UiResourceState::StaleDevice: return CABBIRD_FONT_STATE_V1_STALE_DEVICE;
    default: return CABBIRD_FONT_STATE_V1_NONE;
    }
}

CabbirdStatusV1 CABBIRD_CALL FontStateV1(
    void* user, const CabbirdGenerationHandleV1 handle, CabbirdFontStateV1* state) {
    if (state == nullptr || state->struct_size < sizeof(*state)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "font state is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "font handle is not live");
        }
        const auto font = context.ui_resources->ResourceState(context.scope, UiHandle(handle));
        if (!font) return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "font handle is not live");
        *state = {sizeof(*state), ToFontStateFlags(font->state), font->effective_font_size_pixels,
            font->font_scale, font->device_generation,
            font->state == cabbird::UiResourceState::Ready ? 1 : 0, 0};
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL PushFontV1(
    void* user, const CabbirdGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "font rendering is not active");
        }
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "font handle is not live");
        }
        const auto font = context.ui_resources->ResourceState(context.scope, UiHandle(handle));
        if (!font) return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "font handle is not live");
        if (font->state == cabbird::UiResourceState::Failed) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "font atlas build failed");
        }
        if (context.ui_stack == nullptr || !context.ui_stack->ReserveNext()) {
            if (context.ui_stack != nullptr) context.ui_stack->MarkMismatch();
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "font stack is exhausted");
        }
        // Match BeginWindowV1: grow bookkeeping before the backend changes its
        // font stack so a later allocation failure cannot leave it unbalanced.
        if (context.pushed_fonts.size() == context.pushed_fonts.max_size()) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "font stack is exhausted");
        }
        context.pushed_fonts.reserve(context.pushed_fonts.size() + 1U);
        if (!context.manager->PushUiFont(context.scope, UiHandle(handle))) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "font render backend is not ready");
        }
        context.ui_stack->PushReserved({UiStackEntryKind::Font, UiHandle(handle)});
        context.pushed_fonts.push_back(UiHandle(handle));
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL PopFontV1(void* user) {
    return InvokeUiResourceService(user, [](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "font rendering is not active");
        }
        if (context.pushed_fonts.empty() || context.ui_stack == nullptr ||
            !context.ui_stack->HasTop(UiStackEntryKind::Font, context.pushed_fonts.back())) {
            if (context.ui_stack != nullptr) context.ui_stack->MarkMismatch();
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "font pop is unbalanced");
        }
        if (!context.manager->PopUiFont()) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "font render backend is not ready");
        }
        static_cast<void>(context.ui_stack->Consume(
            UiStackEntryKind::Font, context.pushed_fonts.back()));
        context.pushed_fonts.pop_back();
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL RequestTextureV1(
    void* user, const CabbirdTextureRequestV1* request, CabbirdGenerationHandleV1* handle) {
    if (handle == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "texture handle is null");
    *handle = {};
    if (request == nullptr || request->struct_size < sizeof(*request) ||
        (request->encoded_bytes.size != 0 && request->encoded_bytes.data == nullptr)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "texture request is invalid");
    }
    if (request->format == CABBIRD_TEXTURE_FORMAT_V1_RGBA8) {
        if (request->encoded_bytes.size > cabbird::kDefaultUiResourceDecodedByteLimit) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "raw RGBA texture exceeds byte limit");
        }
    } else if (request->encoded_bytes.size > cabbird::kDefaultUiResourceEncodedByteLimit) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "encoded texture exceeds byte limit");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        cabbird::UiTextureRequest descriptor;
        if (request->relative_path.size != 0) {
            const auto path = ResolvePackageResourcePath(context, request->relative_path);
            if (!path) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "texture path escapes package");
            descriptor.package_directory = path->package_directory;
            descriptor.relative_path = path->relative_path;
        }
        descriptor.flags = request->flags;
        descriptor.format = static_cast<cabbird::UiTextureFormat>(request->format);
        descriptor.width = request->width;
        descriptor.height = request->height;
        if (descriptor.format == cabbird::UiTextureFormat::Rgba8) {
            constexpr std::uint64_t bytes_per_pixel = 4U;
            const std::uint64_t maximum_bytes = cabbird::kDefaultUiResourceDecodedByteLimit;
            if (request->width == 0 || request->height == 0 ||
                request->width > cabbird::kDefaultUiResourceImageDimensionLimit ||
                request->height > cabbird::kDefaultUiResourceImageDimensionLimit ||
                static_cast<std::uint64_t>(request->width) >
                    maximum_bytes / bytes_per_pixel / request->height) {
                return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "raw RGBA texture dimensions are invalid");
            }
            const std::uint64_t expected = static_cast<std::uint64_t>(request->width) *
                static_cast<std::uint64_t>(request->height) * bytes_per_pixel;
            if (expected != request->encoded_bytes.size) {
                return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "raw RGBA texture dimensions are invalid");
            }
        } else if (request->width != 0 || request->height != 0) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "encoded texture dimensions must be zero");
        }

        const cabbird::UiResourceStagingReservation reservation =
            context.ui_resources->ReserveStaging(request->encoded_bytes.size);
        if (request->encoded_bytes.size != 0 && !reservation) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "texture staging budget is exhausted");
        }

        cabbird::UiResourceHandle resource;
        try {
            if (request->encoded_bytes.size != 0) {
                descriptor.encoded_bytes.assign(
                    request->encoded_bytes.data,
                    request->encoded_bytes.data + request->encoded_bytes.size);
            }
            resource = context.ui_resources->RequestTexture(
                context.scope, std::move(descriptor), reservation);
        } catch (...) {
            static_cast<void>(context.ui_resources->ReleaseStaging(reservation));
            throw;
        }
        static_cast<void>(context.ui_resources->ReleaseStaging(reservation));
        if (!resource) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "texture request was rejected");
        if (!context.manager->QueueUiTextureLoad(context.scope, resource)) {
            static_cast<void>(context.ui_resources->Release(context.scope, resource));
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "texture worker is not available");
        }
        *handle = GenerationHandle(context, resource);
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL ReleaseTextureV1(
    void* user, const CabbirdGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle) ||
            !context.ui_resources->Release(context.scope, UiHandle(handle))) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "texture handle is not live");
        }
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

std::uint32_t ToTextureStateFlags(const cabbird::UiResourceState state) noexcept {
    switch (state) {
    case cabbird::UiResourceState::Queued: return CABBIRD_TEXTURE_STATE_V1_QUEUED;
    case cabbird::UiResourceState::Ready: return CABBIRD_TEXTURE_STATE_V1_READY;
    case cabbird::UiResourceState::Failed: return CABBIRD_TEXTURE_STATE_V1_FAILED;
    case cabbird::UiResourceState::StaleDevice: return CABBIRD_TEXTURE_STATE_V1_STALE_DEVICE;
    default: return CABBIRD_TEXTURE_STATE_V1_NONE;
    }
}

CabbirdStatusV1 CABBIRD_CALL TextureStateV1(
    void* user, const CabbirdGenerationHandleV1 handle, CabbirdTextureStateV1* state) {
    if (state == nullptr || state->struct_size < sizeof(*state)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "texture state is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "texture handle is not live");
        }
        const auto texture = context.ui_resources->ResourceState(context.scope, UiHandle(handle));
        if (!texture) return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "texture handle is not live");
        *state = {sizeof(*state), ToTextureStateFlags(texture->state), texture->texture_width,
            texture->texture_height, texture->device_generation, texture->staged_bytes};
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL DrawTextureV1(
    void* user, const CabbirdGenerationHandleV1 handle, const float width, const float height,
    std::uint32_t tint_rgba) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "texture rendering is not active");
        }
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "texture handle is not live");
        }
        const auto texture = context.ui_resources->ResourceState(context.scope, UiHandle(handle));
        if (!texture) return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "texture handle is not live");
        if (texture->state == cabbird::UiResourceState::Failed) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "texture decode or upload failed");
        }
        if (!context.manager->DrawUiTexture(
                context.scope, UiHandle(handle), width, height, tint_rgba)) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "texture render backend is not ready");
        }
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL InputSnapshotV1(void* user, CabbirdInputSnapshotV1* snapshot) {
    if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "input snapshot is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (context.input == nullptr) return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "input is not ready");
        PopulateInputSnapshot(context.input->Snapshot(), *snapshot);
        if (const auto capture = context.input->UiCapture()) {
            snapshot->capture_flags = ToInputCaptureFlags(capture->state.flags);
        }
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL WasPressedV1(
    void* user, const std::uint32_t virtual_key, std::int32_t* pressed) {
    if (pressed == nullptr || virtual_key == 0 || virtual_key >= cabbird::kInputKeyCount) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "input key is invalid");
    }
    *pressed = 0;
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (context.input == nullptr) return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "input is not ready");
        *pressed = context.input->WasPressed(
            cabbird::InputControl::ForKey(static_cast<cabbird::InputKey>(virtual_key))) ? 1 : 0;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL RegisterHotkeyV1(
    void* user, const CabbirdHotkeySpecV1* spec, CabbirdHotkeyCallbackV1 callback,
    void* callback_user, CabbirdGenerationHandleV1* handle) {
    if (handle == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "hotkey handle is null");
    *handle = {};
    constexpr std::uint32_t known_flags = CABBIRD_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS |
        CABBIRD_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED |
        CABBIRD_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED;
    if (spec == nullptr || spec->struct_size < sizeof(*spec) || callback == nullptr ||
        spec->virtual_key == 0 || spec->virtual_key >= cabbird::kInputKeyCount ||
        (spec->modifiers & ~static_cast<std::uint32_t>(cabbird::ToMask(cabbird::InputModifier::Shift) |
            cabbird::ToMask(cabbird::InputModifier::Control) |
            cabbird::ToMask(cabbird::InputModifier::Alt) |
            cabbird::ToMask(cabbird::InputModifier::Super))) != 0 ||
        (spec->flags & ~known_flags) != 0 ||
        ((spec->flags & CABBIRD_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED) != 0 &&
         (spec->flags & CABBIRD_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED) != 0)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "hotkey spec is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (context.input == nullptr) return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "input is not ready");
        cabbird::HotkeySpec request;
        request.id = std::string(ServiceString(spec->id));
        request.trigger = cabbird::InputControl::ForKey(static_cast<cabbird::InputKey>(spec->virtual_key));
        request.modifiers = spec->modifiers;
        request.exact_modifiers = (spec->flags & CABBIRD_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS) == 0;
        request.capture_policy = (spec->flags & CABBIRD_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED) != 0
            ? cabbird::HotkeyCapturePolicy::AllowWhileUiCaptured
            : (spec->flags & CABBIRD_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED) != 0
                ? cabbird::HotkeyCapturePolicy::OnlyWhileUiCaptured
                : cabbird::HotkeyCapturePolicy::RespectUiCapture;
        const std::shared_ptr<cabbird::PluginScope> scope = context.scope;
        const std::uint64_t generation = context.generation;
        const auto registration = context.input->RegisterHotkey(
            scope, std::move(request),
            [scope, generation, callback, callback_user](const cabbird::HotkeyEvent& event) {
                auto callback_scope = scope->AcquireCallback(generation);
                if (!callback_scope) return;
                ScopedPluginCallback activation(scope, generation, false);
                CabbirdInputSnapshotV1 snapshot{};
                PopulateInputSnapshot(event.snapshot, snapshot);
                snapshot.capture_flags = ToInputCaptureFlags(event.ui_capture.state.flags);
                callback(callback_user, {event.handle.value, generation}, &snapshot);
            });
        if (!registration) {
            return StatusV1(
                registration.status == cabbird::HotkeyRegistrationStatus::Conflict
                    ? CABBIRD_STATUS_V1_CONFLICT
                    : registration.status == cabbird::HotkeyRegistrationStatus::DispatcherUnavailable
                        ? CABBIRD_STATUS_V1_UNAVAILABLE
                        : CABBIRD_STATUS_V1_INVALID_ARGUMENT,
                registration.status == cabbird::HotkeyRegistrationStatus::Conflict
                    ? "hotkey conflicts with an existing binding"
                    : "hotkey registration failed");
        }
        *handle = {registration.handle.value, context.generation};
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL ReleaseHotkeyV1(
    void* user, const CabbirdGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (context.input == nullptr || !ValidGenerationHandle(context, handle) ||
            !context.input->ReleaseHotkey(context.scope, {handle.id})) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "hotkey handle is not live");
        }
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL InputCaptureStateV1(void* user, std::uint32_t* flags) {
    if (flags == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "capture flags are null");
    *flags = CABBIRD_INPUT_CAPTURE_V1_NONE;
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(
                CABBIRD_STATUS_V1_UNAVAILABLE,
                "input capture state is only available during draw");
        }
        if (context.input == nullptr) return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "input is not ready");
        if (const auto capture = context.input->UiCapture()) {
            *flags = ToInputCaptureFlags(capture->state.flags);
        }
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 JsonCopyString(
    const std::string_view source, char* destination, std::size_t* inout_size) noexcept {
    if (inout_size == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "result size is required");
    }
    const std::size_t required = source.size() + 1U;
    if (destination == nullptr) {
        *inout_size = required;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (*inout_size < required) {
        *inout_size = required;
        return StatusV1(CABBIRD_STATUS_V1_BUFFER_TOO_SMALL, "destination is too small");
    }
    if (!source.empty()) std::memcpy(destination, source.data(), source.size());
    destination[source.size()] = '\0';
    *inout_size = required;
    return StatusV1(CABBIRD_STATUS_V1_OK);
}

void EraseJsonNode(PluginServiceContext* context, const std::uint64_t id) noexcept {
    if (context == nullptr) return;
    try {
        std::scoped_lock lock(context->json_mutex);
        context->json_nodes.erase(id);
    } catch (...) {
    }
}

CabbirdStatusV1 RegisterJsonNode(
    PluginServiceContext& context, std::shared_ptr<nlohmann::json> root,
    nlohmann::json* value, CabbirdGenerationHandleV1* handle) noexcept {
    auto token = std::make_shared<std::uint64_t>(0);
    const std::uint64_t resource =
        context.scope->Register(
            cabbird::PluginResourceKind::Json, "cabbird.json.value",
            [&context, token] { EraseJsonNode(&context, *token); });
    if (resource == 0) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    *token = resource;
    {
        std::scoped_lock lock(context.json_mutex);
        context.json_nodes.insert_or_assign(
            resource, JsonNode{std::move(root), value});
    }
    *handle = {resource, context.generation};
    return StatusV1(CABBIRD_STATUS_V1_OK);
}

template <typename Callback>
CabbirdStatusV1 InvokeJsonService(void* user, Callback&& callback) noexcept {
    auto* context = static_cast<PluginServiceContext*>(user);
    if (!ValidServiceContext(context)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin JSON context is invalid");
    }
    auto lease = AcquireServiceCallback(context);
    if (!lease) return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    try {
        return callback(*context);
    } catch (...) {
        return StatusV1(CABBIRD_STATUS_V1_FAILED, "JSON service failed");
    }
}

bool FindJsonNode(
    PluginServiceContext& context, const CabbirdGenerationHandleV1 handle,
    JsonNode& node) noexcept {
    if (handle.id == 0 || handle.generation != context.generation) return false;
    std::scoped_lock lock(context.json_mutex);
    const auto found = context.json_nodes.find(handle.id);
    if (found == context.json_nodes.end() || found->second.value == nullptr) return false;
    node = found->second;
    return true;
}

CabbirdStatusV1 CABBIRD_CALL JsonParseV1(
    void* user, const CabbirdStringViewV1 document,
    CabbirdGenerationHandleV1* handle) {
    if (handle == nullptr || (document.data == nullptr && document.size != 0)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON document is invalid");
    }
    *handle = {};
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        if (document.size == 0 || document.data == nullptr) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON document is empty");
        }
        auto root = std::make_shared<nlohmann::json>(
            nlohmann::json::parse(document.data, document.data + document.size));
        return RegisterJsonNode(context, std::move(root), root.get(), handle);
    });
}

CabbirdStatusV1 CABBIRD_CALL JsonReleaseV1(
    void* user, const CabbirdGenerationHandleV1 handle) {
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        if (handle.id == 0 || handle.generation != context.generation) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON handle is invalid");
        }
        return context.scope->Release(handle.id)
            ? StatusV1(CABBIRD_STATUS_V1_OK)
            : StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON handle is not live");
    });
}

CabbirdStatusV1 CABBIRD_CALL JsonKindV1(
    void* user, const CabbirdGenerationHandleV1 handle, std::uint32_t* kind) {
    if (kind == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "kind is null");
    *kind = CABBIRD_JSON_V1_NULL;
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        JsonNode node;
        if (!FindJsonNode(context, handle, node)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON handle is not live");
        }
        switch (node.value->type()) {
        case nlohmann::json::value_t::null: *kind = CABBIRD_JSON_V1_NULL; break;
        case nlohmann::json::value_t::boolean: *kind = CABBIRD_JSON_V1_BOOLEAN; break;
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
        case nlohmann::json::value_t::number_float: *kind = CABBIRD_JSON_V1_NUMBER; break;
        case nlohmann::json::value_t::string: *kind = CABBIRD_JSON_V1_STRING; break;
        case nlohmann::json::value_t::array: *kind = CABBIRD_JSON_V1_ARRAY; break;
        case nlohmann::json::value_t::object: *kind = CABBIRD_JSON_V1_OBJECT; break;
        default: *kind = CABBIRD_JSON_V1_NULL; break;
        }
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL JsonBooleanV1(
    void* user, const CabbirdGenerationHandleV1 handle, std::int32_t* value) {
    if (value == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "value is null");
    *value = 0;
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        JsonNode node;
        if (!FindJsonNode(context, handle, node)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON handle is not live");
        }
        if (!node.value->is_boolean()) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON value is not boolean");
        }
        *value = node.value->get<bool>() ? 1 : 0;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL JsonNumberV1(
    void* user, const CabbirdGenerationHandleV1 handle, double* value) {
    if (value == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "value is null");
    *value = 0.0;
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        JsonNode node;
        if (!FindJsonNode(context, handle, node)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON handle is not live");
        }
        if (!node.value->is_number()) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON value is not a number");
        }
        *value = node.value->get<double>();
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL JsonStringV1(
    void* user, const CabbirdGenerationHandleV1 handle,
    char* destination, std::size_t* inout_size) {
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        JsonNode node;
        if (!FindJsonNode(context, handle, node)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON handle is not live");
        }
        if (!node.value->is_string()) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON value is not a string");
        }
        return JsonCopyString(node.value->get_ref<const std::string&>(), destination, inout_size);
    });
}

CabbirdStatusV1 CABBIRD_CALL JsonArraySizeV1(
    void* user, const CabbirdGenerationHandleV1 handle, std::size_t* size) {
    if (size == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "size is null");
    *size = 0;
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        JsonNode node;
        if (!FindJsonNode(context, handle, node)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON handle is not live");
        }
        if (!node.value->is_array()) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON value is not an array");
        }
        *size = node.value->size();
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL JsonArrayItemV1(
    void* user, const CabbirdGenerationHandleV1 handle, const std::size_t index,
    CabbirdGenerationHandleV1* child) {
    if (child == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "child is null");
    *child = {};
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        JsonNode node;
        if (!FindJsonNode(context, handle, node)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON handle is not live");
        }
        if (!node.value->is_array()) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON value is not an array");
        }
        if (index >= node.value->size()) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON array index is out of range");
        }
        return RegisterJsonNode(context, node.root, &(*node.value)[index], child);
    });
}

CabbirdStatusV1 CABBIRD_CALL JsonObjectSizeV1(
    void* user, const CabbirdGenerationHandleV1 handle, std::size_t* size) {
    if (size == nullptr) return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "size is null");
    *size = 0;
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        JsonNode node;
        if (!FindJsonNode(context, handle, node)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON handle is not live");
        }
        if (!node.value->is_object()) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON value is not an object");
        }
        *size = node.value->size();
        return StatusV1(CABBIRD_STATUS_V1_OK);
    });
}

CabbirdStatusV1 CABBIRD_CALL JsonObjectKeyAtV1(
    void* user, const CabbirdGenerationHandleV1 handle, const std::size_t index,
    char* destination, std::size_t* inout_size) {
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        JsonNode node;
        if (!FindJsonNode(context, handle, node)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON handle is not live");
        }
        if (!node.value->is_object()) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON value is not an object");
        }
        std::size_t current = 0;
        for (auto iterator = node.value->begin(); iterator != node.value->end(); ++iterator) {
            if (current++ == index) {
                return JsonCopyString(iterator.key(), destination, inout_size);
            }
        }
        return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON object index is out of range");
    });
}

CabbirdStatusV1 CABBIRD_CALL JsonObjectFindV1(
    void* user, const CabbirdGenerationHandleV1 handle,
    const CabbirdStringViewV1 key, CabbirdGenerationHandleV1* child) {
    if (child == nullptr || key.data == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON object lookup is invalid");
    }
    *child = {};
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        JsonNode node;
        if (!FindJsonNode(context, handle, node)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON handle is not live");
        }
        if (!node.value->is_object()) {
            return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "JSON value is not an object");
        }
        auto found = node.value->find(std::string(key.data, key.size));
        if (found == node.value->end()) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON object key is missing");
        }
        return RegisterJsonNode(context, node.root, &found.value(), child);
    });
}

CabbirdStatusV1 CABBIRD_CALL JsonSerializeV1(
    void* user, const CabbirdGenerationHandleV1 handle,
    char* destination, std::size_t* inout_size) {
    return InvokeJsonService(user, [&](PluginServiceContext& context) {
        JsonNode node;
        if (!FindJsonNode(context, handle, node)) {
            return StatusV1(CABBIRD_STATUS_V1_NOT_FOUND, "JSON handle is not live");
        }
        return JsonCopyString(node.value->dump(), destination, inout_size);
    });
}

CabbirdStatusV1 CABBIRD_CALL QueryServiceV1(
    void* host_context, CabbirdStringViewV1 service_id,
    std::uint32_t minimum_version, const void** service) {
    if (service == nullptr || service_id.data == nullptr) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    }
    *service = nullptr;
    const std::string_view id(service_id.data, service_id.size);
    auto* context = static_cast<PluginServiceContext*>(host_context);
    if (!ValidServiceContext(context)) {
        return StatusV1(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    auto callback = AcquireServiceCallback(context);
    if (!callback) {
        return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    if (!context->capabilities.AuthorizeService(id).allowed) {
        return StatusV1(
            CABBIRD_STATUS_V1_PERMISSION_DENIED,
            "service capability is not granted");
    }
    if (id == CABBIRD_CORE_SERVICE_V1_ID &&
        minimum_version <= context->core.service_version) {
        *service = &context->core;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_PLUGIN_STATE_SERVICE_V1_ID &&
        minimum_version <= context->plugin_state.service_version) {
        *service = &context->plugin_state;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_CONFIG_SERVICE_V1_ID &&
        minimum_version <= context->config.service_version) {
        *service = &context->config;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_STORAGE_SERVICE_V1_ID &&
        minimum_version <= context->storage.service_version) {
        *service = &context->storage;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_JSON_SERVICE_V1_ID &&
        minimum_version <= context->json.service_version) {
        *service = &context->json;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_RUNTIME_INFO_SERVICE_V1_ID &&
        minimum_version <= context->runtime_info.service_version) {
        *service = &context->runtime_info;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_LOCALIZATION_SERVICE_V1_ID &&
        minimum_version <= context->localization.service_version) {
        *service = &context->localization;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_DIAGNOSTICS_SERVICE_V1_ID &&
        minimum_version <= context->diagnostics.service_version) {
        *service = &context->diagnostics;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_SCHEDULER_SERVICE_V1_ID &&
        minimum_version <= context->scheduler.service_version) {
        *service = &context->scheduler;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_IPC_SERVICE_V1_ID &&
        minimum_version <= context->ipc_service.service_version) {
        *service = &context->ipc_service;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_COMMANDS_SERVICE_V1_ID &&
        minimum_version <= context->commands.service_version) {
        *service = &context->commands;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_NOTIFICATIONS_SERVICE_V1_ID &&
        minimum_version <= context->notifications.service_version) {
        *service = &context->notifications;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_SIGNATURE_SERVICE_V1_ID &&
        minimum_version <= context->signature.service_version) {
        *service = &context->signature;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_HOOK_SERVICE_V1_ID &&
        minimum_version <= context->hook.service_version) {
        *service = &context->hook;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_PATCH_SERVICE_V1_ID &&
        minimum_version <= context->patch.service_version) {
        *service = &context->patch;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_WINDOW_SERVICE_V1_ID &&
        minimum_version <= context->window.service_version) {
        *service = &context->window;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_FONT_SERVICE_V1_ID &&
        minimum_version <= context->font.service_version) {
        *service = &context->font;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_TEXTURE_SERVICE_V1_ID &&
        minimum_version <= context->texture.service_version) {
        *service = &context->texture;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_INPUT_SERVICE_V1_ID &&
        minimum_version <= context->input_service.service_version) {
        *service = &context->input_service;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_UNITY_OVERLAY_SERVICE_V1_ID) {
        if (minimum_version > CABBIRD_UNITY_OVERLAY_SERVICE_V1_VERSION) {
            return StatusV1(
                CABBIRD_STATUS_V1_UNAVAILABLE,
                "requested overlay version is not available");
        }
        if (QueryOverlayServiceV1() == nullptr) {
            return StatusV1(
                CABBIRD_STATUS_V1_UNAVAILABLE,
                "overlay service is not ready");
        }
        *service = &context->overlay;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (id == CABBIRD_UI_SERVICE_V1_ID) {
        const CabbirdUiServiceV1* ui = context->manager->UiService();
        if (ui != nullptr && ui->service_version >= minimum_version) {
            if (context->ui == nullptr || context->ui->service_version < minimum_version) {
                return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "requested UI version is not ready");
            }
            *service = context != nullptr && context->ui != nullptr
                ? static_cast<const void*>(context->ui)
                : static_cast<const void*>(ui);
            return StatusV1(CABBIRD_STATUS_V1_OK);
        }
    }
    if (id == CABBIRD_UNITY_PLAYER_SERVICE_V1_ID) {
        /* Per-plugin table, not the shared adapter one: the two write entries it still carries are
         * authorized per call through the policy (`AuthorizePlayerWrite`), which needs the caller's
         * identity -- the shared table has none.  See PluginServiceContext::player_service. */
        const auto* adapter = static_cast<const CabbirdUnityPlayerServiceV1*>(
            cabbird::ProcessAdapterServices().Query(id, minimum_version));
        if (adapter == nullptr || adapter->snapshot == nullptr) {
            return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "player service is not ready");
        }
        context->adapter_player = adapter;
        context->player_service = CabbirdUnityPlayerServiceV1{
            sizeof(CabbirdUnityPlayerServiceV1),
            adapter->service_version,
            context,
            &PlayerSnapshotV1,
            &PlayerWritePositionV1,
            &PlayerWriteStateV1};
        *service = &context->player_service;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    if (const void* adapter = cabbird::ProcessAdapterServices().Query(id, minimum_version)) {
        *service = adapter;
        return StatusV1(CABBIRD_STATUS_V1_OK);
    }
    return StatusV1(CABBIRD_STATUS_V1_UNAVAILABLE, "service is not ready");
}

bool RecoverPluginUiStack(
    PluginUiProxyContext& proxy, PluginServiceContext& services) noexcept {
    UiStackTracker* const stack = proxy.ui_stack;
    bool unbalanced = stack == nullptr || stack->mismatch ||
        (stack != nullptr && !stack->entries.empty());

    const auto end_window = [&]() noexcept {
        if (proxy.service == nullptr || proxy.service->end_window == nullptr) return;
        try {
            proxy.service->end_window(proxy.service->user);
        } catch (...) {
        }
    };
    const auto end_child = [&]() noexcept {
        if (!HasUiField<decltype(CabbirdUiServiceV1::end_child)>(
                proxy.service, offsetof(CabbirdUiServiceV1, end_child)) ||
            proxy.service->end_child == nullptr) {
            return;
        }
        try {
            proxy.service->end_child(proxy.service->user);
        } catch (...) {
        }
    };
    const auto end_table = [&]() noexcept {
        if (!HasUiField<decltype(CabbirdUiServiceV1::end_table)>(
                proxy.service, offsetof(CabbirdUiServiceV1, end_table)) ||
            proxy.service->end_table == nullptr) {
            return;
        }
        try {
            proxy.service->end_table(proxy.service->user);
        } catch (...) {
        }
    };
    const auto end_menu = [&]() noexcept {
        if (!HasUiField<decltype(CabbirdUiServiceV1::end_menu)>(
                proxy.service, offsetof(CabbirdUiServiceV1, end_menu)) ||
            proxy.service->end_menu == nullptr) {
            return;
        }
        try {
            proxy.service->end_menu(proxy.service->user);
        } catch (...) {
        }
    };
    const auto end_popup = [&]() noexcept {
        if (!HasUiField<decltype(CabbirdUiServiceV1::end_popup)>(
                proxy.service, offsetof(CabbirdUiServiceV1, end_popup)) ||
            proxy.service->end_popup == nullptr) {
            return;
        }
        try {
            proxy.service->end_popup(proxy.service->user);
        } catch (...) {
        }
    };

    if (stack != nullptr) {
        while (!stack->entries.empty()) {
            const UiStackEntry entry = stack->entries.back();
            stack->entries.pop_back();
            switch (entry.kind) {
            case UiStackEntryKind::Window:
                end_window();
                break;
            case UiStackEntryKind::ScopedWindow:
                end_window();
                if (!services.open_windows.empty() &&
                    services.open_windows.back() == entry.resource) {
                    services.open_windows.pop_back();
                } else {
                    stack->MarkMismatch();
                }
                break;
            case UiStackEntryKind::Child:
                end_child();
                break;
            case UiStackEntryKind::Table:
                end_table();
                break;
            case UiStackEntryKind::Menu:
                end_menu();
                break;
            case UiStackEntryKind::Popup:
                end_popup();
                break;
            case UiStackEntryKind::Font:
                if (services.manager != nullptr) {
                    try {
                        static_cast<void>(services.manager->PopUiFont());
                    } catch (...) {
                    }
                }
                if (!services.pushed_fonts.empty() &&
                    services.pushed_fonts.back() == entry.resource) {
                    services.pushed_fonts.pop_back();
                } else {
                    stack->MarkMismatch();
                }
                break;
            }
        }
        unbalanced = unbalanced || stack->mismatch;
        stack->Reset();
    }

    // The tracker is authoritative for normal operation.  These fallbacks keep
    // a generation from contaminating the next draw if a host table was swapped
    // while a scoped resource was live.
    while (!services.open_windows.empty()) {
        end_window();
        services.open_windows.pop_back();
        unbalanced = true;
    }
    while (!services.pushed_fonts.empty()) {
        if (services.manager != nullptr) {
            try {
                static_cast<void>(services.manager->PopUiFont());
            } catch (...) {
            }
        }
        services.pushed_fonts.pop_back();
        unbalanced = true;
    }
    return unbalanced;
}

}  // namespace

struct PluginManager::LoadedPlugin {
    HMODULE module{};
    CabbirdPluginDescriptorV1 descriptor_v1{};
    void* plugin_context{};
    bool waiting_for_service{};
    bool started{};
    bool faulted{};
    bool capability_audit_logged{};
    bool localization_catalog_loaded{};
    PluginView view;
    std::filesystem::path shadow;
    cabbird::PluginShadowGeneration shadow_generation;
    CallbackMetrics update_metrics;
    CallbackMetrics draw_metrics;
    UiStackTracker ui_stack;
    PluginServiceContext service_context;
    CabbirdHostApiV1 host_api{};
    PluginUiProxyContext ui_proxy_context;
    CabbirdUiServiceV1 ui_proxy{};
    std::shared_ptr<cabbird::PluginScope> scope;
};

struct UiResourceWorkerGate final {
    struct Pending final {
        std::shared_ptr<cabbird::PluginScope> scope;
        cabbird::UiResourceHandle handle;
        cabbird::UiResourceKind kind{cabbird::UiResourceKind::Font};
    };

    std::mutex mutex;
    // A canonical resource can have several scope leases while one Worker job
    // is queued. Keep every candidate so releasing the most recently queued
    // duplicate cannot strand an earlier live lease in Queued state.
    std::unordered_map<std::uint64_t, std::vector<Pending>> pending_resources;
};

void AddUiResourceWorkerCandidate(
    std::vector<UiResourceWorkerGate::Pending>& candidates,
    UiResourceWorkerGate::Pending candidate) {
    const auto existing = std::find_if(
        candidates.begin(), candidates.end(),
        [&candidate](const UiResourceWorkerGate::Pending& current) {
            return current.handle == candidate.handle;
        });
    if (existing != candidates.end()) {
        *existing = std::move(candidate);
    } else {
        candidates.push_back(std::move(candidate));
    }
}

// The lease is captured when a Worker callback is posted, rather than being
// constructed inside that callback. Dispatcher cancellation destroys queued
// callbacks without invoking them, so this is what releases both the gate and
// any resource-scoped staging reservation on every terminal path.
class UiResourceWorkerGateLease final {
public:
    UiResourceWorkerGateLease(
        std::shared_ptr<cabbird::UiResourceRegistry> registry,
        std::shared_ptr<UiResourceWorkerGate> gate, const std::uint64_t resource_id,
        const cabbird::UiResourceKind kind) noexcept
        : registry_(std::move(registry)), gate_(std::move(gate)), resource_id_(resource_id),
          kind_(kind) {}

    ~UiResourceWorkerGateLease() { Finalize(true); }

    UiResourceWorkerGateLease(const UiResourceWorkerGateLease&) = delete;
    UiResourceWorkerGateLease& operator=(const UiResourceWorkerGateLease&) = delete;

    void Activate() noexcept { active_ = true; }
    void Complete() noexcept { Finalize(false); }
    void Fail() noexcept { Finalize(true); }

    [[nodiscard]] std::optional<UiResourceWorkerGate::Pending> CurrentLive() const noexcept {
        std::vector<UiResourceWorkerGate::Pending> candidates;
        try {
            if (!active_ || gate_ == nullptr || registry_ == nullptr) return std::nullopt;
            {
                std::scoped_lock lock(gate_->mutex);
                const auto found = gate_->pending_resources.find(resource_id_);
                if (found == gate_->pending_resources.end()) return std::nullopt;
                candidates = found->second;
            }
            for (auto candidate = candidates.rbegin(); candidate != candidates.rend(); ++candidate) {
                if (candidate->scope == nullptr || !candidate->handle) continue;
                const auto state = registry_->ResourceState(candidate->scope, candidate->handle);
                if (state && state->kind == kind_ && state->resource_id == resource_id_ &&
                    state->state != cabbird::UiResourceState::Revoked) {
                    return *candidate;
                }
            }
        } catch (...) {
        }
        return std::nullopt;
    }

private:
    void Finalize(const bool mark_failed) noexcept {
        std::vector<UiResourceWorkerGate::Pending> pending;
        try {
            if (!active_ || gate_ == nullptr) return;
            active_ = false;
            std::scoped_lock lock(gate_->mutex);
            const auto found = gate_->pending_resources.find(resource_id_);
            if (found == gate_->pending_resources.end()) return;
            pending = std::move(found->second);
            gate_->pending_resources.erase(found);
        } catch (...) {
            return;
        }
        if (!mark_failed || registry_ == nullptr) return;
        for (auto candidate = pending.rbegin(); candidate != pending.rend(); ++candidate) {
            if (candidate->scope == nullptr || !candidate->handle) continue;
            if (kind_ == cabbird::UiResourceKind::Font &&
                registry_->MarkFontFailed(candidate->scope, candidate->handle)) {
                return;
            }
            if (kind_ == cabbird::UiResourceKind::Texture &&
                registry_->MarkTextureFailed(candidate->scope, candidate->handle)) {
                return;
            }
        }
    }

    std::shared_ptr<cabbird::UiResourceRegistry> registry_;
    std::shared_ptr<UiResourceWorkerGate> gate_;
    std::uint64_t resource_id_{};
    cabbird::UiResourceKind kind_{cabbird::UiResourceKind::Font};
    bool active_{};
};

struct UiResourceWorkerStagingAdmission final {
    std::shared_ptr<cabbird::UiResourceRegistry> registry;
    std::shared_ptr<cabbird::PluginScope> scope;
    cabbird::UiResourceHandle handle;
    std::size_t base_bytes{};

    [[nodiscard]] static bool Reserve(void* user, const std::size_t byte_count) noexcept {
        const auto* admission = static_cast<const UiResourceWorkerStagingAdmission*>(user);
        if (admission == nullptr || admission->registry == nullptr || admission->scope == nullptr ||
            !admission->handle ||
            byte_count > (std::numeric_limits<std::size_t>::max)() - admission->base_bytes) {
            return false;
        }
        return admission->registry->ReserveResourceStaging(
            admission->scope, admission->handle, admission->base_bytes + byte_count);
    }

    [[nodiscard]] cabbird::UiResourceAllocationAdmission Callback() noexcept {
        return {this, &Reserve};
    }
};

PluginManager::PluginManager(
    std::filesystem::path root,
    std::filesystem::path plugin_directory,
    cabbird::CoreMemoryServices memory_services,
    PluginCallbackBudgets callback_budgets,
    std::shared_ptr<cabbird::StructuredLogger> logger,
    cabbird::HotkeyDispatcher input_dispatcher,
    UiResourceWorkerDispatcher ui_resource_worker_dispatcher,
    cabbird::IpcPost ipc_post,
    PluginLoadPredicate load_predicate,
    PluginActivationObserver activation_observer)
    : root_(std::filesystem::absolute(root)),
      plugin_directory_(plugin_directory.is_absolute() ? std::move(plugin_directory)
                                                        : root_ / plugin_directory),
      cache_directory_(plugin_directory_ / L".cache" / std::to_wstring(GetCurrentProcessId())),
      memory_services_(cabbird::NormalizeCoreMemoryServices(std::move(memory_services))),
      callback_budgets_(callback_budgets),
      logger_(std::move(logger)),
      shadow_store_(cache_directory_ / L"packages"),
       file_watcher_(plugin_directory_),
      enablement_file_watcher_(root_ / L"config", L"plugin-enablement.json"),
      enablement_store_(root_ / L"config" / L"plugin-enablement.json"),
       ui_window_state_file_(root_ / L"state" / L"ui-window-state.json"),
       input_service_(std::move(input_dispatcher)),
       ui_resource_worker_dispatcher_(std::move(ui_resource_worker_dispatcher)),
       ui_resource_worker_gate_(std::make_shared<UiResourceWorkerGate>()),
       load_predicate_(std::move(load_predicate)),
       activation_observer_(std::move(activation_observer)),
       lifecycle_ledger_(std::make_shared<cabbird::ResourceLedger>()),
      platform_services_(std::make_unique<cabbird::ScopedPlatformServices>(
          memory_services_, lifecycle_ledger_)),
      ipc_registry_(std::make_unique<cabbird::IpcRegistry>(std::move(ipc_post))) {
    if (g_manager != nullptr || g_process_quarantined) {
        throw std::logic_error("only one PluginManager may be active at a time");
    }
    cache_owner_ = AcquirePluginCacheOwner(plugin_directory_ / L".cache", cache_directory_);
    g_manager = this;
    LoadPersistentUiWindowState();
    std::string state_error;
    if (!enablement_store_.Load(&state_error)) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR, "plugin enablement load failed: " + state_error);
    }
}

PluginManager::~PluginManager() {
    file_watcher_.Stop();
    enablement_file_watcher_.Stop();
    SavePersistentUiWindowState(true);
    UnloadAll();
    // A quarantined generation can still be executing inside its DLL.  Retain
    // the broker too, so that a late callback cannot dereference a stale
    // service context while the process is winding down.
    if (quarantined_plugins_.empty()) {
        platform_services_.reset();
    } else {
        static_cast<void>(platform_services_.release());
    }
    std::error_code error;
    std::filesystem::remove_all(cache_directory_, error);
    // A quarantined plugin may still own a thread executing inside its DLL. Keep
    // both its service context and module mapping alive until process teardown.
    const bool process_quarantined = !quarantined_plugins_.empty();
    if (process_quarantined) g_process_quarantined = true;
    for (auto& plugin : quarantined_plugins_) static_cast<void>(plugin.release());
    quarantined_plugins_.clear();
    if (process_quarantined) {
        // A quarantined callback may still read its shadow package. Keep the
        // ownership lock alive with the deliberately retained module mapping.
        static_cast<void>(cache_owner_.release());
    } else {
        cache_owner_.reset();
        error.clear();
        std::filesystem::remove_all(cache_directory_, error);
        RemoveEmptyPluginCacheRoot(plugin_directory_ / L".cache");
    }
    if (g_manager == this) g_manager = nullptr;
}

void PluginManager::RunLifecycleOperation(
    std::mutex& execution_mutex, std::function<void()> operation) {
    std::unique_lock lock(execution_mutex);
    while (plugin_load_step_in_progress_.load(std::memory_order_acquire)) {
        lock.unlock();
        plugin_load_step_in_progress_.wait(true, std::memory_order_acquire);
        lock.lock();
    }
    g_lifecycle_plugin_lock.Set(&lock);
    try {
        operation();
    } catch (...) {
        g_lifecycle_plugin_lock.Set(nullptr);
        throw;
    }
    g_lifecycle_plugin_lock.Set(nullptr);
}

bool PluginManager::PluginLoadStepInProgress() const noexcept {
    return plugin_load_step_in_progress_.load(std::memory_order_acquire);
}

void PluginManager::RunPluginLoadStep(std::function<void()> operation) {
    auto* const lock = g_lifecycle_plugin_lock.Get();
    if (lock == nullptr) {
        operation();
        return;
    }
    plugin_load_step_in_progress_.store(true, std::memory_order_release);
    lock->unlock();
    try {
        operation();
    } catch (...) {
        lock->lock();
        plugin_load_step_in_progress_.store(false, std::memory_order_release);
        plugin_load_step_in_progress_.notify_all();
        throw;
    }
    lock->lock();
    plugin_load_step_in_progress_.store(false, std::memory_order_release);
    plugin_load_step_in_progress_.notify_all();
}

void PluginManager::Log(CabbirdCoreLogLevelV1 level, std::string message) {
    LogImpl(level, std::move(message), {}, 0);
}

void PluginManager::LogPlugin(
    CabbirdCoreLogLevelV1 level,
    std::string message,
    std::string_view plugin_id,
    std::uint64_t generation) {
    LogImpl(level, std::move(message), std::string(plugin_id), generation);
}

void PluginManager::SetTranslator(
    std::shared_ptr<const cabbird::Translator> translator) noexcept {
    if (!plugins_.empty() || !quarantined_plugins_.empty()) return;
    translator_ = std::move(translator);
}

void PluginManager::LogImpl(
    CabbirdCoreLogLevelV1 level,
    std::string message,
    std::string plugin_id,
    std::uint64_t generation) {
    std::ostringstream line;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    line << std::setfill('0') << std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute
         << ':' << std::setw(2) << time.wSecond << " [pid " << GetCurrentProcessId() << "] ["
         << LevelName(level) << "] " << message;
    const std::string rendered = line.str();
    {
        std::scoped_lock events_lock(events_mutex_);
        events_.push_back(rendered);
        if (events_.size() > 256) events_.erase(events_.begin(), events_.begin() + 64);
    }
    if (logger_ != nullptr) {
        cabbird::LogDetails details;
        details.thread_domain = g_log_thread_domain.Get();
        details.event_id = plugin_id.empty() ? "plugin.manager" : "plugin.host";
        if (!plugin_id.empty()) {
            details.plugin = cabbird::PluginLogOwner{
                std::move(plugin_id), generation};
        }
        static_cast<void>(logger_->Log(
            StructuredLevel(level), "plugin-manager", std::move(message),
            std::move(details)));
    }
}

std::vector<std::string> PluginManager::Events() const {
    std::scoped_lock events_lock(events_mutex_);
    return events_;
}

cabbird::PluginScope::CallbackLease PluginManager::AcquireCallback(
    std::string_view plugin_id, std::uint64_t generation) noexcept {
    const auto found = std::find_if(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
        return plugin->view.id == plugin_id && plugin->view.generation == generation;
    });
    if (found == plugins_.end() || (*found)->scope == nullptr) return {};
    return (*found)->scope->AcquireCallback(generation);
}

void PluginManager::SetQueuedCallbackCanceller(
    std::function<void(std::string_view, std::uint64_t)> canceller) {
    queued_callback_canceller_ = std::move(canceller);
}

void PluginManager::SetCallbackEvidenceObserver(
    std::function<void(const PluginCallbackEvidence&)> observer) {
    callback_evidence_observer_ = std::move(observer);
}

std::vector<PluginStopDiagnostic> PluginManager::StopDiagnostics() const {
    std::scoped_lock lock(stop_diagnostics_mutex_);
    return stop_diagnostics_;
}

bool PluginManager::LoadAllowed(const cabbird::PluginCatalogEntry& entry) const {
    return entry.manifest && (!load_predicate_ || load_predicate_(*entry.manifest));
}

void PluginManager::PublishSuspended(const cabbird::PluginCatalogEntry& entry) {
    if (!entry.manifest) return;
    PluginView view;
    view.id = entry.manifest->id;
    view.name = entry.manifest->name;
    view.author = entry.manifest->author;
    view.version = entry.manifest->version.ToString();
    view.source = entry.entry_file;
    view.package_directory = entry.package_root;
    view.visible = false;
    view.enabled = false;
    view.state = "suspended";
    view.status_reason = "suspended by Runtime recovery policy";
    disabled_plugins_[view.id] = std::move(view);
}

bool PluginManager::LoadCatalogEntry(const cabbird::PluginCatalogEntry& entry) {
    if (!LoadAllowed(entry)) {
        PublishSuspended(entry);
        return false;
    }
    cabbird::PluginShadowResult staged;
    RunPluginLoadStep([&] { staged = shadow_store_.Stage(entry); });
    if (!staged.Ok()) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
            "package shadow failed for " + std::string(entry.Id()) + ": " + staged.error);
        return false;
    }
    const std::filesystem::path source = entry.entry_file;
    const std::filesystem::path binary = staged.shadow->entry_file;
    const std::filesystem::path package = entry.package_root;
    const std::string plugin_id(staged.shadow->manifest.id);
    const std::uint64_t generation = staged.shadow->generation;
    const auto publish_activation = [&](bool entering) noexcept {
        try {
            if (activation_observer_) {
                activation_observer_(plugin_id, generation, entering);
            }
        } catch (...) {
        }
    };
    publish_activation(true);
    try {
        const bool loaded = LoadBinary(
            source, binary, package, std::move(*staged.shadow));
        publish_activation(false);
        return loaded;
    } catch (...) {
        publish_activation(false);
        throw;
    }
}

bool PluginManager::Activate(LoadedPlugin& plugin) {
    if (plugin.scope == nullptr) return false;
    plugin.plugin_context = nullptr;
    plugin.ui_stack.Reset();
    plugin.service_context.manager = this;
    plugin.service_context.ui_proxy_context = &plugin.ui_proxy_context;
    plugin.service_context.ui_stack = &plugin.ui_stack;
    plugin.service_context.plugin_id = plugin.view.id;
    plugin.service_context.generation = plugin.view.generation;
    plugin.service_context.package_directory = plugin.view.package_directory;
    plugin.service_context.state_directory = ValidPluginStateId(plugin.view.id)
        ? (root_ / L"state" / L"plugins" /
              WideUtf8(plugin.view.id.c_str())).lexically_normal()
        : std::filesystem::path{};
    plugin.service_context.configuration_directory = ValidPluginStateId(plugin.view.id)
        ? (root_ / L"config" / L"plugins" /
              WideUtf8(plugin.view.id.c_str())).lexically_normal()
        : std::filesystem::path{};
    plugin.service_context.scope = plugin.scope;
    plugin.service_context.platform = platform_services_.get();
    plugin.service_context.ipc = ipc_registry_.get();
    plugin.service_context.ipc_dependencies.clear();
    for (const auto& dependency : plugin.shadow_generation.manifest.dependencies) {
        plugin.service_context.ipc_dependencies.push_back(dependency.id);
    }
    plugin.service_context.ui_resources = ui_resources_.get();
    plugin.service_context.input = &input_service_;
    plugin.service_context.open_windows.clear();
    plugin.service_context.pushed_fonts.clear();
    plugin.service_context.capabilities =
        cabbird::ResolvePluginCapabilityGrant(&plugin.shadow_generation.manifest);
    if (!plugin.localization_catalog_loaded) {
        plugin.service_context.localization_locale = translator_ == nullptr
            ? cabbird::Locale::EnUs
            : translator_->locale();
        cabbird::PluginCatalogLoadResult localization = cabbird::LoadPluginCatalog(
            plugin.service_context.localization_locale,
            plugin.shadow_generation.package_root);
        plugin.service_context.localization_catalog = std::move(localization.catalog);
        plugin.service_context.localization_fallback_logged.store(
            false, std::memory_order_relaxed);
        plugin.localization_catalog_loaded = true;
        if (!localization.diagnostics.empty()) {
            const cabbird::CatalogDiagnostic& diagnostic = localization.diagnostics.front();
            std::string detail = diagnostic.message;
            if (!diagnostic.path.empty()) detail += " path=" + diagnostic.path;
            ReportLocalizationFallback(plugin.service_context, std::move(detail));
        }
    }
    if (!plugin.capability_audit_logged) {
        for (const auto& audit : plugin.service_context.capabilities.Audits()) {
            if (audit.code == cabbird::PluginCapabilityAuditCode::UnknownCapability) {
                Log(
                    CABBIRD_CORE_LOG_LEVEL_V1_WARNING,
                    "plugin capability audit: unknown capability: plugin=" +
                        plugin.view.id + " capability=" + audit.capability);
            } else {
                Log(
                    CABBIRD_CORE_LOG_LEVEL_V1_WARNING,
                    "plugin capability audit: required service missing capability: plugin=" +
                        plugin.view.id + " service=" + audit.service +
                        " capability=" + audit.capability);
            }
        }
        plugin.capability_audit_logged = true;
    }
    /* Designated initializers, deliberately, and naming all nine fields.
     *
     * CabbirdCoreServiceV1 has nine fields (abi/cabbird-sdk-v1-windows-x64.json records size 72
     * with offsets), while the caller copied from Anomaly names seven.  With positional
     * initialization the seventh argument -- PluginDirectoryV1, signature (void*, char*, size_t*)
     * -- landed on patch_memory, signature (void*, uintptr_t, CabbirdByteSpanV1), producing
     * C2679 "no operator= accepts an initializer list".  Naming every field means a future
     * addition to the struct cannot silently shift an argument again.
     *
     * last_status stays null on purpose.  core.h describes it as "the last status returned on
     * this thread", but nothing in Cabbird records such a value: StatusV1 (defined above) builds
     * and returns a status without writing it anywhere, so there is no thread-local store to read
     * back.  A plugin calling last_status receives null and must treat that as "not supported".
     * Implementing it means adding thread-local state to every host call path -- not something to
     * introduce without a decision, so it is left visibly unimplemented rather than approximated. */
    plugin.service_context.core = {
        .struct_size = sizeof(CabbirdCoreServiceV1),
        .service_version = CABBIRD_CORE_SERVICE_V1_VERSION,
        .user = &plugin.service_context,
        .log = LogV1,
        .read_memory = ReadV1,
        .write_memory = WriteV1,
        .patch_memory = PatchMemoryV1,
        .plugin_directory = PluginDirectoryV1,
        .module_base = ModuleBaseV1,
        .last_status = nullptr};
    plugin.service_context.plugin_state = {
        sizeof(CabbirdPluginStateServiceV1), CABBIRD_PLUGIN_STATE_SERVICE_V1_VERSION,
        &plugin.service_context, PluginStateDirectoryV1};
    plugin.service_context.config = {
        sizeof(CabbirdConfigServiceV1), CABBIRD_CONFIG_SERVICE_V1_VERSION,
        &plugin.service_context,
        RegisterConfigSchemaV1, UnregisterConfigSchemaV1, ReadConfigV1, WriteConfigV1,
        MigrateConfigV1};
    plugin.service_context.storage = {
        sizeof(CabbirdStorageServiceV1), CABBIRD_STORAGE_SERVICE_V1_VERSION,
        &plugin.service_context, ReadStorageV1, WriteStorageV1, RemoveStorageV1};
    plugin.service_context.json = {
        sizeof(CabbirdJsonServiceV1), CABBIRD_JSON_SERVICE_V1_VERSION,
        &plugin.service_context,
        JsonParseV1, JsonReleaseV1, JsonKindV1, JsonBooleanV1, JsonNumberV1,
        JsonStringV1, JsonArraySizeV1, JsonArrayItemV1, JsonObjectSizeV1,
        JsonObjectKeyAtV1, JsonObjectFindV1, JsonSerializeV1};
    plugin.service_context.runtime_info = {
        sizeof(CabbirdRuntimeInfoServiceV1), CABBIRD_RUNTIME_INFO_SERVICE_V1_VERSION,
        &plugin.service_context, RuntimeInfoV1, RuntimeVersionV1};
    plugin.service_context.localization = {
        sizeof(CabbirdLocalizationServiceV1), CABBIRD_LOCALIZATION_SERVICE_V1_VERSION,
        &plugin.service_context, LocaleV1, TranslateV1};
    plugin.service_context.diagnostics = {
        sizeof(CabbirdDiagnosticsServiceV1), CABBIRD_DIAGNOSTICS_SERVICE_V1_VERSION,
        &plugin.service_context,
        RegisterSelfTestV1, UnregisterSelfTestV1, RunSelfTestV1, DiagnosticsSnapshotV1};
    plugin.service_context.scheduler = {
        sizeof(CabbirdSchedulerServiceV1), CABBIRD_SCHEDULER_SERVICE_V1_VERSION,
        &plugin.service_context, ScheduleV1, CancelTaskV1};
    plugin.service_context.ipc_service = {
        sizeof(CabbirdIpcServiceV1), CABBIRD_IPC_SERVICE_V1_VERSION,
        &plugin.service_context, RegisterIpcEndpointV1, UnregisterIpcEndpointV1,
        InvokeIpcV1, InvokeIpcAsyncV1, CancelIpcV1, SubscribeIpcV1,
        UnsubscribeIpcV1, PublishIpcV1};
    plugin.service_context.commands = {
        sizeof(CabbirdCommandsServiceV1), CABBIRD_COMMANDS_SERVICE_V1_VERSION,
        &plugin.service_context, RegisterCommandV1, UnregisterCommandV1, InvokeCommandV1};
    plugin.service_context.notifications = {
        sizeof(CabbirdNotificationsServiceV1), CABBIRD_NOTIFICATIONS_SERVICE_V1_VERSION,
        &plugin.service_context, PostNotificationV1, DismissNotificationV1};
    plugin.service_context.signature = {
        sizeof(CabbirdSignatureServiceV1), CABBIRD_SIGNATURE_SERVICE_V1_VERSION,
        &plugin.service_context, ResolveSignatureV1};
    plugin.service_context.hook = {
        sizeof(CabbirdHookServiceV1), CABBIRD_HOOK_SERVICE_V1_VERSION,
        &plugin.service_context,
        CreateHookV1, ReleaseHookV1, BeginHookCallbackV1, EndHookCallbackV1};
    plugin.service_context.patch = {
        sizeof(CabbirdPatchServiceV1), CABBIRD_PATCH_SERVICE_V1_VERSION,
        &plugin.service_context, ApplyPatchV1, ReleasePatchV1};
    plugin.service_context.window = {
        sizeof(CabbirdWindowServiceV1), CABBIRD_WINDOW_SERVICE_V1_VERSION,
        &plugin.service_context,
        RegisterWindowV1, ReleaseWindowV1, SetWindowOpenV1, ToggleWindowV1,
        WindowStateV1, BeginWindowV1, EndWindowV1};
    plugin.service_context.font = {
        sizeof(CabbirdFontServiceV1), CABBIRD_FONT_SERVICE_V1_VERSION,
        &plugin.service_context, RequestFontV1, ReleaseFontV1, FontStateV1, PushFontV1, PopFontV1};
    plugin.service_context.texture = {
        sizeof(CabbirdTextureServiceV1), CABBIRD_TEXTURE_SERVICE_V1_VERSION,
        &plugin.service_context,
        RequestTextureV1, ReleaseTextureV1, TextureStateV1, DrawTextureV1};
    plugin.service_context.input_service = {
        sizeof(CabbirdInputServiceV1), CABBIRD_INPUT_SERVICE_V1_VERSION,
        &plugin.service_context,
        InputSnapshotV1, WasPressedV1, RegisterHotkeyV1, ReleaseHotkeyV1, InputCaptureStateV1};
    if (plugin.service_context.overlay_state == nullptr) {
        plugin.service_context.overlay_state = std::make_shared<OverlayProxyState>();
    }
    plugin.service_context.overlay = {
        sizeof(CabbirdUnityOverlayServiceV1), CABBIRD_UNITY_OVERLAY_SERVICE_V1_VERSION,
        &plugin.service_context, SubscribeOverlayV1, UnsubscribeOverlayV1};
    plugin.service_context.ui = &plugin.ui_proxy;
    plugin.host_api = {
        sizeof(CabbirdHostApiV1), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        &plugin.service_context,
        {sizeof(CabbirdAllocatorV1), 0, &plugin.service_context,
            AllocateV1, ReallocateV1, ReleaseV1},
        QueryServiceV1};
    plugin.ui_proxy_context.scope = plugin.scope;
    plugin.ui_proxy_context.plugin_id = plugin.view.id;
    plugin.ui_proxy_context.generation = plugin.view.generation;
    plugin.ui_proxy_context.ui_stack = &plugin.ui_stack;
    plugin.ui_proxy_context.service = ui_service_;
    plugin.ui_proxy = MakeUiProxy(&plugin.ui_proxy_context);
    g_loading_plugin_id.Get() = plugin.view.id;
    g_loading_package_directory.Get() = plugin.view.package_directory;
    CabbirdStatusV1 load_status = StatusV1(CABBIRD_STATUS_V1_FAILED);
    {
        auto callback = plugin.scope->AcquireCallback(plugin.view.generation);
        if (callback) {
            ScopedPluginCallback callback_scope(plugin.scope, plugin.view.generation, false);
            RunPluginLoadStep([&] {
                try {
                    load_status = plugin.descriptor_v1.on_load(
                        &plugin.host_api, &plugin.plugin_context);
                } catch (...) {
                    Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
                        "exception while loading ABI v1 plugin " + plugin.view.id);
                }
            });
        } else {
            Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR, "plugin scope rejected ABI v1 load: " + plugin.view.id);
        }
    }
    g_loading_plugin_id.Get().clear();
    g_loading_package_directory.Get().clear();
    if (load_status.code == CABBIRD_STATUS_V1_UNAVAILABLE) {
        plugin.plugin_context = nullptr;
        plugin.waiting_for_service = true;
        plugin.started = false;
        Log(CABBIRD_CORE_LOG_LEVEL_V1_INFO, "ABI v1 plugin waiting for service: " + plugin.view.id);
        return true;
    }
    if (load_status.code != CABBIRD_STATUS_V1_OK) {
        plugin.plugin_context = nullptr;
        plugin.waiting_for_service = false;
        Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
            "ABI v1 plugin rejected activation: plugin=" + plugin.view.id +
                " " + StatusMessage("on_load", load_status));
        return false;
    }
    if (plugin.descriptor_v1.on_start != nullptr) {
        CabbirdStatusV1 start_status = StatusV1(CABBIRD_STATUS_V1_FAILED);
        auto callback = plugin.scope->AcquireCallback(plugin.view.generation);
        if (callback) {
            ScopedPluginCallback callback_scope(plugin.scope, plugin.view.generation, false);
            RunPluginLoadStep([&] {
                try { start_status = plugin.descriptor_v1.on_start(plugin.plugin_context); }
                catch (...) {
                    Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
                        "exception while starting ABI v1 plugin " + plugin.view.id);
                }
            });
        }
        if (start_status.code != CABBIRD_STATUS_V1_OK) {
            auto unload_callback = plugin.scope->AcquireCallback(plugin.view.generation);
            if (unload_callback) {
                ScopedPluginCallback callback_scope(plugin.scope, plugin.view.generation, false);
                RunPluginLoadStep([&] {
                    try { plugin.descriptor_v1.on_unload(plugin.plugin_context); } catch (...) {}
                });
            }
            plugin.plugin_context = nullptr;
            if (start_status.code == CABBIRD_STATUS_V1_UNAVAILABLE) {
                plugin.waiting_for_service = true;
                plugin.started = false;
                Log(CABBIRD_CORE_LOG_LEVEL_V1_INFO,
                    "ABI v1 plugin waiting for service: " + plugin.view.id);
                return true;
            }
            plugin.waiting_for_service = false;
            Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
                "ABI v1 plugin rejected activation: plugin=" + plugin.view.id +
                    " " + StatusMessage("on_start", start_status));
            return false;
        }
    }
    plugin.waiting_for_service = false;
    plugin.started = true;
    ReconcileWindowVisibility(plugin);
    Log(CABBIRD_CORE_LOG_LEVEL_V1_INFO, "ABI v1 plugin started: " + plugin.view.id);
    return true;
}

bool PluginManager::LoadBinary(
    const std::filesystem::path& source,
    const std::filesystem::path& binary,
    const std::filesystem::path& package_directory,
    cabbird::PluginShadowGeneration shadow_generation) {
    const auto discard_shadow = [&] { shadow_store_.Retire(shadow_generation); };

    const cabbird::PluginCapabilityGrant grant =
        cabbird::ResolvePluginCapabilityGrant(&shadow_generation.manifest);
    if (!grant.IsEnforceable()) {
        for (const auto& audit : grant.Audits()) {
            const char* code = audit.code ==
                    cabbird::PluginCapabilityAuditCode::UnknownCapability
                ? "unknown-capability"
                : audit.code ==
                        cabbird::PluginCapabilityAuditCode::RequiredServiceMissingCapability
                    ? "required-service-capability-missing"
                    : "required-service-mapping-missing";
            Log(
                CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
                "plugin manifest capability denied: plugin=" +
                    shadow_generation.plugin_id + " code=" + code +
                    " service=" + audit.service +
                    " capability=" + audit.capability);
        }
        discard_shadow();
        return false;
    }

    cabbird::PluginNativeDependencyPreflightResult dependency_preflight;
    RunPluginLoadStep([&] {
        dependency_preflight = cabbird::PreflightPluginNativeDependencies(binary);
    });
    if (!dependency_preflight.Ok()) {
        for (const auto& diagnostic : dependency_preflight.diagnostics) {
            std::string message = "plugin native dependency denied: package=" +
                Utf8(package_directory) + " module=" +
                Utf8(std::filesystem::path(diagnostic.module_name)) + " code=" +
                std::string(cabbird::PluginNativeDependencyDiagnosticCodeName(diagnostic.code)) +
                " requester=" + Utf8(diagnostic.requester) +
                " detail=" + diagnostic.message;
            if (!diagnostic.expected_path.empty()) {
                message += " expected=" + Utf8(diagnostic.expected_path);
            }
            if (!diagnostic.loaded_path.empty()) {
                message += " loaded=" + Utf8(diagnostic.loaded_path);
            }
            Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR, std::move(message));
        }
        discard_shadow();
        return false;
    }

    HMODULE module{};
    DWORD load_error{ERROR_SUCCESS};
    RunPluginLoadStep([&] {
        const DLL_DIRECTORY_COOKIE search_cookie = AddDllDirectory(binary.parent_path().c_str());
        module = LoadLibraryExW(
            binary.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module == nullptr) load_error = GetLastError();
        if (search_cookie != nullptr) RemoveDllDirectory(search_cookie);
    });
    if (module == nullptr) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
            "load failed for " + source.string() + ": " + std::to_string(load_error));
        discard_shadow();
        return false;
    }
    const auto unload_module = [&] {
        RunPluginLoadStep([&] { FreeLibrary(module); });
    };
    const auto entry_v1 = reinterpret_cast<CabbirdPluginEntryV1Fn>(
        GetProcAddress(module, CABBIRD_PLUGIN_V1_ENTRY_NAME));
    if (entry_v1 != nullptr) {
        CabbirdPluginDescriptorV1 descriptor{};
        // DIVERGENCE FROM UPSTREAM (a bug fix).  Upstream -- and this port,
        // until now -- set only `struct_size` here, then required
        // `descriptor.api_major == CABBIRD_PLUGIN_API_V1_MAJOR` a few lines below.
        //
        // Those two facts cannot both hold.  include/cabbird/sdk/plugin.h marks struct_size,
        // api_major and api_minor as "Filled in by the host before the entry point is called",
        // and plugins are written to that contract, so a plugin author has every reason to
        // leave them alone.  A compliant plugin therefore leaves
        // api_major as the zero it was initialised to, the comparison against 1 never succeeds,
        // and EVERY plugin is rejected -- discovered, enablement-resolved, and then thrown away
        // with "invalid ABI v1 plugin metadata ... api=0.0".  That is exactly the shape of an
        // empty plugin page.
        //
        // The same file's sibling loader, PluginHost::Load (src/plugin/plugin_host.cpp:136-138),
        // already fills all three, so the two loaders disagreed: PluginHost worked while the
        // manager's own path silently rejected everything.  Filling the two missing fields makes
        // this probe agree with both the header and the other loader.
        //
        // Kept as a real version check rather than deleted: with the prefix correctly seeded,
        // api_major == 1 is now a meaningful assertion about the plugin, not a tautology.
        descriptor.struct_size = sizeof(descriptor);
        descriptor.api_major = static_cast<std::uint16_t>(CABBIRD_PLUGIN_API_V1_MAJOR);
        descriptor.api_minor = static_cast<std::uint16_t>(CABBIRD_PLUGIN_API_V1_MINOR);
        CabbirdStatusV1 entry_status = StatusV1(CABBIRD_STATUS_V1_FAILED);
        RunPluginLoadStep([&] {
            try { entry_status = entry_v1(&descriptor); } catch (...) {
                Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
                    "exception in ABI v1 plugin entry: " + source.string());
            }
        });
        constexpr std::size_t minimum_size = offsetof(CabbirdPluginDescriptorV1, on_draw) +
            sizeof(descriptor.on_draw);
        const bool valid = entry_status.code == CABBIRD_STATUS_V1_OK &&
            descriptor.struct_size >= minimum_size &&
            descriptor.api_major == CABBIRD_PLUGIN_API_V1_MAJOR &&
            descriptor.api_minor <= CABBIRD_PLUGIN_API_V1_MINOR &&
            descriptor.id.data != nullptr && descriptor.id.size != 0 &&
            descriptor.name.data != nullptr && descriptor.name.size != 0 &&
            descriptor.version.data != nullptr && descriptor.version.size != 0 &&
            descriptor.on_load != nullptr && descriptor.on_unload != nullptr;
        if (!valid) {
            Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
                "invalid ABI v1 plugin metadata: " + source.string() +
                    " entry_code=" + std::to_string(entry_status.code) +
                    " struct_size=" + std::to_string(descriptor.struct_size) +
                    " api=" + std::to_string(descriptor.api_major) + "." +
                    std::to_string(descriptor.api_minor));
            unload_module();
            discard_shadow();
            return false;
        }
        auto copy_view = [](CabbirdStringViewV1 value) {
            return value.data == nullptr ? std::string{} : std::string(value.data, value.size);
        };
        auto plugin = std::make_unique<LoadedPlugin>();
        plugin->module = module;
        plugin->descriptor_v1 = descriptor;
        plugin->view.id = copy_view(descriptor.id);
        plugin->view.name = copy_view(descriptor.name);
        plugin->view.author = copy_view(descriptor.author);
        plugin->view.version = copy_view(descriptor.version);
        plugin->view.source = source;
        plugin->view.package_directory = package_directory;
        plugin->view.visibility_control = descriptor.on_draw != nullptr;
        plugin->view.generation = shadow_generation.generation;
        if (plugin->view.id != shadow_generation.plugin_id) {
            Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
                "plugin identity mismatch: manifest=" + shadow_generation.plugin_id +
                    " descriptor=" + plugin->view.id);
            unload_module();
            discard_shadow();
            return false;
        }
        plugin->scope = std::make_shared<cabbird::PluginScope>(
            lifecycle_ledger_, plugin->view.id, plugin->view.generation);
        static_cast<void>(plugin->scope->Register(
            cabbird::PluginResourceKind::Task, "plugin.update"));
        if (plugin->view.visibility_control) {
            static_cast<void>(plugin->scope->Register(
                cabbird::PluginResourceKind::Ui, "plugin.draw"));
        }
        plugin->ui_proxy_context.service = ui_service_;
        plugin->ui_proxy_context.ui_stack = &plugin->ui_stack;
        plugin->ui_proxy_context.scope = plugin->scope;
        plugin->ui_proxy_context.plugin_id = plugin->view.id;
        plugin->ui_proxy_context.generation = plugin->view.generation;
        plugin->ui_proxy = MakeUiProxy(&plugin->ui_proxy_context);
        plugin->shadow = binary;
        plugin->shadow_generation = std::move(shadow_generation);
        if (std::any_of(plugins_.begin(), plugins_.end(), [&](const auto& loaded) {
                return loaded->view.id == plugin->view.id;
        }) || std::any_of(quarantined_plugins_.begin(), quarantined_plugins_.end(),
                [&](const auto& loaded) { return loaded->view.id == plugin->view.id; })) {
            Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR, "duplicate plugin id: " + plugin->view.id);
            static_cast<void>(plugin->scope->RevokeAll());
            unload_module();
            discard_shadow();
            return false;
        }
        if (!Activate(*plugin)) {
            Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR, "ABI v1 plugin rejected activation: " + plugin->view.id);
            static_cast<void>(plugin->scope->RevokeAll());
            unload_module();
            discard_shadow();
            return false;
        }
        Log(CABBIRD_CORE_LOG_LEVEL_V1_INFO,
            "loaded ABI v1 " + plugin->view.name + " " + plugin->view.version);
        plugins_.push_back(std::move(plugin));
        return true;
    }
    Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR, "plugin does not export the current ABI entry: " + source.string());
    unload_module();
    discard_shadow();
    return false;
}

void PluginManager::LoadAll() {
    file_watcher_.Stop();
    enablement_file_watcher_.Stop();
    {
        std::scoped_lock lock(pending_package_changes_mutex_);
        pending_package_changes_.clear();
    }
    cabbird::PluginCatalogSnapshot catalog;
    cabbird::PluginDependencyPlan plan;
    std::map<std::string, cabbird::PluginEnablementDecision, std::less<>> enablement;
    RunPluginLoadStep([&] {
        std::error_code error;
        std::filesystem::create_directories(plugin_directory_, error);
        catalog = cabbird::DiscoverPluginCatalog(plugin_directory_);
        plan = cabbird::ResolvePluginDependencies(catalog);
        enablement = enablement_store_.Resolve(catalog);
    });
    disabled_plugins_.clear();
    const auto disabled_view = [&](const cabbird::PluginCatalogEntry& entry,
                                   std::string state, std::string reason) {
        if (!entry.manifest) return;
        PluginView view;
        view.id = entry.manifest->id;
        view.name = entry.manifest->name;
        view.author = entry.manifest->author;
        view.version = entry.manifest->version.ToString();
        view.source = entry.entry_file;
        view.package_directory = entry.package_root;
        view.visible = false;
        view.enabled = false;
        view.state = std::move(state);
        view.status_reason = std::move(reason);
        disabled_plugins_[view.id] = std::move(view);
    };
    bool loaded = true;
    for (const cabbird::PluginCatalogEntry& entry : catalog.Entries()) {
        if (entry.LoadCandidate()) continue;
        const std::string id = entry.manifest ? entry.manifest->id : entry.package_root.filename().string();
        for (const cabbird::PluginCatalogIssue& issue : entry.issues) {
            Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
                "catalog rejected " + id + " [" + issue.code + "] " + issue.message);
        }
    }
    for (const cabbird::PluginDependencyNode& node : plan.nodes) {
        if (node.state == cabbird::PluginDependencyState::Ready) continue;
        for (const std::string& diagnostic : node.diagnostics) {
            Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR, "dependency blocked " + node.id + ": " + diagnostic);
        }
    }
    for (const std::string& id : plan.load_order) {
        const cabbird::PluginCatalogEntry* entry = catalog.Find(id);
        if (entry != nullptr && !LoadAllowed(*entry)) {
            PublishSuspended(*entry);
            continue;
        }
        const auto decision = enablement.find(id);
        if (entry != nullptr && decision != enablement.end() && !decision->second.enabled) {
            disabled_view(*entry, "disabled", decision->second.reason);
            continue;
        }
        loaded = entry != nullptr && LoadCatalogEntry(*entry) && loaded;
    }

    if (plan.load_order.empty()) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_INFO, "plugin directory is ready: " + plugin_directory_.string());
    }
    if (!file_watcher_.Start([this](std::vector<std::string> package_names) {
            QueuePackageChanges(std::move(package_names));
        })) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_WARNING, "plugin file watcher failed to start");
    }
    if (!enablement_file_watcher_.Start([this] { QueueEnablementReload(); })) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_WARNING, "plugin enablement file watcher failed to start");
    }
    if (!loaded) Log(CABBIRD_CORE_LOG_LEVEL_V1_WARNING, "one or more plugin packages were not activated");
}

void PluginManager::ReloadAll() {
    UnloadAll();
    LoadAll();
}

bool PluginManager::Reload(std::string_view plugin_id) {
    const auto loaded = std::find_if(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
        return plugin->view.id == plugin_id;
    });
    std::filesystem::path package_directory;
    if (loaded != plugins_.end()) {
        package_directory = (*loaded)->view.package_directory;
    } else {
        const auto disabled = disabled_plugins_.find(std::string(plugin_id));
        if (disabled == disabled_plugins_.end()) return false;
        package_directory = disabled->second.package_directory;
    }
    const std::string package_name = Utf8(package_directory.filename());
    if (package_name.empty()) return false;
    if (!ReloadPackages({package_name})) return false;
    const auto views = Plugins();
    return std::any_of(views.begin(), views.end(), [&](const auto& plugin) {
        return plugin.id == plugin_id;
    });
}

bool PluginManager::SetEnabled(std::string_view plugin_id, bool enabled) {
    cabbird::PluginCatalogSnapshot catalog;
    std::string error;
    bool updated{};
    RunPluginLoadStep([&] {
        catalog = cabbird::DiscoverPluginCatalog(plugin_directory_);
        updated = enablement_store_.SetPluginEnabled(
            catalog, plugin_id, enabled, &error);
    });
    if (!updated) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
            "plugin enablement update failed: id=" + std::string(plugin_id) +
                " enabled=" + (enabled ? "true" : "false") + " " + error);
        return false;
    }
    const bool reconciled = ReconcileEnablement(catalog);
    const auto views = Plugins();
    const auto found = std::find_if(views.begin(), views.end(), [&](const auto& plugin) {
        return plugin.id == plugin_id;
    });
    const bool target_quarantined = std::any_of(
        quarantined_plugins_.begin(), quarantined_plugins_.end(),
        [&](const auto& plugin) { return plugin->view.id == plugin_id; });
    const bool succeeded = !target_quarantined && found != views.end() && found->enabled == enabled;
    if (succeeded) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_INFO,
            "plugin enablement applied: id=" + std::string(plugin_id) +
                " enabled=" + (enabled ? "true" : "false"));
    } else {
        std::string reason = "reconciliation=" + std::string(reconciled ? "succeeded" : "failed");
        if (target_quarantined) {
            reason += ", state=quarantined";
        } else if (found == views.end()) {
            reason += ", reason=plugin-not-present-after-reconciliation";
        } else {
            reason += ", state=" + found->state +
                " enabled=" + (found->enabled ? "true" : "false");
            if (!found->status_reason.empty()) reason += " reason=" + found->status_reason;
        }
        Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
            "plugin enablement rejected: id=" + std::string(plugin_id) +
                " enabled=" + (enabled ? "true" : "false") + " " + reason);
    }
    return succeeded;
}

bool PluginManager::ReconcileEnablement(
    const cabbird::PluginCatalogSnapshot& catalog) {
    const auto decisions = enablement_store_.Resolve(catalog);
    const auto publish_view = [&](const cabbird::PluginCatalogEntry& entry,
                                  std::string state, std::string reason) {
        if (!entry.manifest) return;
        PluginView view;
        view.id = entry.manifest->id;
        view.name = entry.manifest->name;
        view.author = entry.manifest->author;
        view.version = entry.manifest->version.ToString();
        view.source = entry.entry_file;
        view.package_directory = entry.package_root;
        view.visible = false;
        view.enabled = false;
        view.state = std::move(state);
        view.status_reason = std::move(reason);
        disabled_plugins_[view.id] = std::move(view);
    };
    const auto active = [&](std::string_view id) {
        return std::any_of(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
            return plugin->view.id == id;
        });
    };
    const auto quarantined = [&](std::string_view id) {
        return std::any_of(
            quarantined_plugins_.begin(), quarantined_plugins_.end(),
            [&](const auto& plugin) { return plugin->view.id == id; });
    };

    // Reconcile desired state against the generations that actually exist. This
    // intentionally does not rely on a before/after boolean delta: repeating an
    // enable request must retry a generation whose previous activation failed.
    const cabbird::PluginDependencyPlan plan = cabbird::ResolvePluginDependencies(catalog);
    std::unordered_set<std::string> retained;
    for (const auto& plugin : plugins_) {
        const auto decision = decisions.find(plugin->view.id);
        const auto* node = plan.Find(plugin->view.id);
        const auto* entry = catalog.Find(plugin->view.id);
        if (decision != decisions.end() && decision->second.enabled &&
            node != nullptr && node->state == cabbird::PluginDependencyState::Ready &&
            entry != nullptr && LoadAllowed(*entry)) {
            retained.insert(plugin->view.id);
        }
    }
    bool narrowed = true;
    while (narrowed) {
        narrowed = false;
        const std::vector<std::string> candidates(retained.begin(), retained.end());
        for (const std::string& id : candidates) {
            const auto* entry = catalog.Find(id);
            const bool missing_dependency = entry == nullptr || !entry->manifest ||
                std::any_of(
                    entry->manifest->dependencies.begin(),
                    entry->manifest->dependencies.end(),
                    [&](const cabbird::PluginDependencyManifest& dependency) {
                        return !dependency.optional && !retained.contains(dependency.id);
                    });
            if (missing_dependency && retained.erase(id) != 0) narrowed = true;
        }
    }
    std::unordered_set<std::string> unload_ids;
    std::vector<std::string> fallback_stop_order;
    for (auto plugin = plugins_.rbegin(); plugin != plugins_.rend(); ++plugin) {
        if (!retained.contains((*plugin)->view.id)) {
            unload_ids.insert((*plugin)->view.id);
            fallback_stop_order.push_back((*plugin)->view.id);
        }
    }

    const auto manifest_entry = [&](std::string_view id) {
        const auto found = std::find_if(
            catalog.Entries().begin(), catalog.Entries().end(),
            [&](const cabbird::PluginCatalogEntry& entry) {
                return entry.manifest && entry.manifest->id == id;
            });
        return found == catalog.Entries().end() ? nullptr : &*found;
    };
    std::unordered_set<std::string> protected_dependencies;
    std::function<void(std::string_view)> protect_required_dependencies =
        [&](std::string_view consumer_id) {
            const auto* entry = manifest_entry(consumer_id);
            if (entry == nullptr || !entry->manifest) return;
            for (const auto& dependency : entry->manifest->dependencies) {
                if (dependency.optional ||
                    !protected_dependencies.insert(dependency.id).second) {
                    continue;
                }
                protect_required_dependencies(dependency.id);
            }
        };
    for (const auto& plugin : quarantined_plugins_) {
        protect_required_dependencies(plugin->view.id);
    }

    bool stopped = true;
    const auto stop_one = [&](const std::string& id) {
        if (unload_ids.erase(id) == 0) return;
        if (protected_dependencies.contains(id)) {
            stopped = false;
            return;
        }
        const auto found = std::find_if(
            plugins_.begin(), plugins_.end(),
            [&](const auto& plugin) { return plugin->view.id == id; });
        if (found == plugins_.end()) return;
        const std::size_t index = static_cast<std::size_t>(
            std::distance(plugins_.begin(), found));
        if (!UnloadIndicesWithDeadline(
                {index}, true, std::chrono::milliseconds(1000))) {
            stopped = false;
            protect_required_dependencies(id);
        }
    };
    for (const std::string& id : plan.stop_order) stop_one(id);
    for (const std::string& id : fallback_stop_order) stop_one(id);

    // Refresh every catalog view, including entries whose resolved boolean did
    // not change but whose explanation changed from a default to an override or
    // to a dependency-derived reason.
    for (const auto& [id, decision] : decisions) {
        const auto* entry = catalog.Find(id);
        if (entry == nullptr) continue;
        if (!LoadAllowed(*entry)) {
            PublishSuspended(*entry);
        } else if (quarantined(id) || active(id)) {
            disabled_plugins_.erase(id);
        } else if (decision.enabled) {
            disabled_plugins_.erase(id);
        } else {
            publish_view(*entry, "disabled", decision.reason);
        }
    }

    bool loaded = true;
    for (const std::string& id : plan.load_order) {
        const auto decision = decisions.find(id);
        if (decision == decisions.end() || !decision->second.enabled || active(id)) continue;
        const cabbird::PluginCatalogEntry* entry = catalog.Find(id);
        if (entry != nullptr && !LoadAllowed(*entry)) {
            PublishSuspended(*entry);
            continue;
        }
        if (entry == nullptr || !entry->manifest || quarantined(id)) {
            loaded = false;
            continue;
        }

        std::string missing_dependency;
        for (const auto& dependency : entry->manifest->dependencies) {
            const auto dependency_decision = decisions.find(dependency.id);
            if (!dependency.optional &&
                (dependency_decision == decisions.end() ||
                 !dependency_decision->second.enabled || !active(dependency.id))) {
                missing_dependency = dependency.id;
                break;
            }
        }
        if (!missing_dependency.empty()) {
            publish_view(
                *entry, "dependency-blocked",
                "required dependency is not active: " + missing_dependency);
            loaded = false;
            continue;
        }

        if (!LoadCatalogEntry(*entry) || !active(id)) {
            publish_view(*entry, "faulted", "plugin activation failed");
            loaded = false;
        } else {
            disabled_plugins_.erase(id);
        }
    }

    // Nodes omitted from load_order have a statically invalid dependency plan.
    // Publish a deterministic blocked view rather than silently leaving the
    // configured-enabled plugin absent from the runtime snapshot.
    for (const auto& [id, decision] : decisions) {
        if (!decision.enabled || active(id)) continue;
        if (quarantined(id)) {
            loaded = false;
            continue;
        }
        const auto* entry = catalog.Find(id);
        if (entry == nullptr) continue;
        if (!disabled_plugins_.contains(id)) {
            const auto* node = plan.Find(id);
            const std::string reason = node != nullptr && !node->diagnostics.empty()
                ? node->diagnostics.front()
                : "plugin did not publish an active generation";
            publish_view(*entry, "dependency-blocked", reason);
        }
        loaded = false;
    }

    return stopped && loaded;
}

void PluginManager::UnloadAll() {
    static_cast<void>(StopForRuntime());
}

bool PluginManager::StopForRuntime(std::chrono::milliseconds timeout) {
    std::vector<std::size_t> indices;
    indices.reserve(plugins_.size());
    for (std::size_t index = 0; index < plugins_.size(); ++index) indices.push_back(index);
    {
        std::scoped_lock lock(stop_diagnostics_mutex_);
        stop_diagnostics_.clear();
    }
    return UnloadIndicesWithDeadline(indices, true, timeout);
}

void PluginManager::UnloadIndices(
    const std::vector<std::size_t>& indices, const bool retire_shadow_generations) {
    static_cast<void>(UnloadIndicesWithDeadline(
        indices, retire_shadow_generations, std::chrono::milliseconds(5000)));
}

bool PluginManager::UnloadIndicesWithDeadline(
    const std::vector<std::size_t>& indices,
    const bool retire_shadow_generations,
    const std::chrono::milliseconds timeout) {
    std::vector<std::size_t> ordered = indices;
    std::sort(ordered.begin(), ordered.end(), std::greater<>());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
    const auto deadline = StopDeadlineAfter(timeout);
    bool all_stopped = true;
    for (const std::size_t index : ordered) {
        if (index >= plugins_.size()) continue;
        auto& plugin = *plugins_[index];
        const std::shared_ptr<cabbird::PluginScope> scope = plugin.scope;
        const std::uint64_t generation = plugin.view.generation;
        const std::size_t resources_before = scope == nullptr ? 0 : scope->Resources().size();
        bool quarantine{};
        bool timed_out{};
        bool drained = scope == nullptr;
        std::string quarantine_reason;

        // Freeze every generation before inspecting the remaining deadline.
        // Even a generation reached after the global deadline must have its
        // queued dispatcher work cancelled before it is quarantined.
        if (scope != nullptr) {
            scope->FreezeCallbackSources();
            if (queued_callback_canceller_) {
                try { queued_callback_canceller_(plugin.view.id, generation); } catch (...) {}
            }
            const bool platform_revoked = platform_services_ == nullptr ||
                platform_services_->RevokeScope(
                    ScopedPlatformOwner(plugin.service_context), deadline,
                    cabbird::ScopedPlatformRevokePhase::PreStop);
            if (!platform_revoked) {
                quarantine = true;
                timed_out = StopRemaining(deadline) == std::chrono::milliseconds::zero();
                quarantine_reason = "platform hook revocation failed";
            }
        }

        const auto remaining_before_stop = StopRemaining(deadline);
        if (!quarantine && remaining_before_stop == std::chrono::milliseconds::zero()) {
            quarantine = true;
            timed_out = true;
            drained = false;
            quarantine_reason = "host stop deadline exceeded";
        }

        // Freeze publication sources before cancelling queued work and draining
        // ordinary generation leases.  The optional dispatcher hook lets the
        // production composition root cancel owner/generation queues here.
        if (!quarantine && scope != nullptr) {
            drained = scope->BeginStop(StopRemaining(deadline));
            if (!drained) {
                quarantine = true;
                timed_out = true;
                quarantine_reason = "callback barrier timed out";
            }
        }

        if (!quarantine && scope != nullptr) {
            static_cast<void>(scope->RevokeAllExcept(cabbird::PluginResourceKind::Config));
        }

        if (!quarantine && plugin.started &&
            plugin.descriptor_v1.on_stop != nullptr) {
            auto stop_lease = scope == nullptr
                ? cabbird::PluginScope::CallbackLease{}
                : scope->AcquireLifecycleLease(generation);
            if (scope != nullptr && !stop_lease) {
                quarantine = true;
                quarantine_reason = "lifecycle lease unavailable";
            } else {
                std::promise<CabbirdStatusV1> completed;
                std::future<CabbirdStatusV1> completion = completed.get_future();
                const auto stop = plugin.descriptor_v1.on_stop;
                void* context = plugin.plugin_context;
                const auto remaining = StopRemaining(deadline);
                const std::uint32_t stop_deadline = remaining.count() <= 0
                    ? 0u
                    : static_cast<std::uint32_t>(
                          (std::min)(remaining.count(),
                              static_cast<std::int64_t>(UINT32_MAX)));
                std::thread stop_thread([
                    scope, generation, stop, context, stop_deadline,
                    lease = std::move(stop_lease), promise = std::move(completed)]() mutable {
                    CabbirdStatusV1 result = StatusV1(CABBIRD_STATUS_V1_FAILED);
                    ScopedPluginCallback callback_scope(scope, generation, true);
                    try { result = stop(context, stop_deadline); } catch (...) {}
                    try { promise.set_value(result); } catch (...) {}
                });
                if (completion.wait_for(remaining) != std::future_status::ready) {
                    stop_thread.detach();
                    quarantine = true;
                    timed_out = true;
                    quarantine_reason = "on_stop timed out";
                } else {
                    const CabbirdStatusV1 status = completion.get();
                    stop_thread.join();
                    if (status.code != CABBIRD_STATUS_V1_OK) {
                        quarantine = true;
                        quarantine_reason =
                            "on_stop status=" + std::to_string(status.code);
                    }
                }
            }
        }

        // Config schemas remain scoped through on_stop so plugins can use the
        // durable Config ABI for their final save. All scoped resources are
        // revoked before on_unload and before the module can be released.
        if (!quarantine && scope != nullptr) {
            const bool platform_revoked = platform_services_ == nullptr ||
                platform_services_->RevokeScope(
                    ScopedPlatformOwner(plugin.service_context), deadline);
            if (!platform_revoked) {
                quarantine = true;
                timed_out = StopRemaining(deadline) == std::chrono::milliseconds::zero();
                quarantine_reason = "platform hook revocation failed";
            } else {
                static_cast<void>(scope->RevokeAll());
            }
        }

        if (quarantine) {
            all_stopped = false;
            plugin.faulted = true;
            plugin.view.state = "quarantined";
            plugin.view.status_reason = quarantine_reason;
            const std::size_t in_flight = scope == nullptr ? 0 : scope->InFlightCallbacks();
            {
                std::scoped_lock lock(stop_diagnostics_mutex_);
                stop_diagnostics_.push_back({
                    plugin.view.id, generation, drained, timed_out,
                    in_flight, resources_before, quarantine_reason});
            }
            Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
                "plugin quarantined after stop failure: " + plugin.view.id +
                " " + quarantine_reason);
            quarantined_plugins_.push_back(std::move(plugins_[index]));
            plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }

        // A lifecycle lease protects the final unload callback.  It is acquired
        // only after ordinary callbacks have drained and remains held through
        // on_unload, so the module cannot be unmapped while host calls run.
        auto unload_lease = scope == nullptr
            ? cabbird::PluginScope::CallbackLease{}
            : scope->AcquireLifecycleLease(generation);
        if (plugin.started) {
            ScopedPluginCallback callback_scope(scope, generation, true);
            if (unload_lease || scope == nullptr) {
                try { plugin.descriptor_v1.on_unload(plugin.plugin_context); } catch (...) {
                    Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR, "exception while unloading " + plugin.view.id);
                }
            }
        }
        plugin.started = false;
        plugin.plugin_context = nullptr;
        {
            std::scoped_lock lock(stop_diagnostics_mutex_);
            stop_diagnostics_.push_back({
                plugin.view.id, generation, drained, false, 0, resources_before, {}});
        }
        FreeLibrary(plugin.module);
        if (retire_shadow_generations) shadow_store_.Retire(plugin.shadow_generation);
        plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return all_stopped;
}

void PluginManager::LoadPersistentUiWindowState() {
    try {
        std::error_code error;
        if (!std::filesystem::exists(ui_window_state_file_, error)) {
            if (error) throw std::filesystem::filesystem_error(
                "UI window state existence check failed", ui_window_state_file_, error);
            return;
        }
        const std::uintmax_t byte_count = std::filesystem::file_size(ui_window_state_file_, error);
        if (error || byte_count > kMaximumUiWindowStateFileBytes) {
            throw std::runtime_error("UI window state file is unavailable or too large");
        }

        std::ifstream input(ui_window_state_file_, std::ios::binary);
        if (!input) throw std::runtime_error("UI window state file cannot be opened");
        const nlohmann::json document = nlohmann::json::parse(input);
        PersistentUiState state = ParseUiWindowState(document);
        if (!ui_resources_->ImportPersistentWindowState(state.windows)) {
            throw std::runtime_error("UI window state values are invalid");
        }
        ui_window_state_last_document_ = SerializeUiWindowState(
            state.windows, state.plugin_windows);
        {
            std::scoped_lock lock(plugin_window_visibility_mutex_);
            plugin_window_visibility_ = std::move(state.plugin_windows);
        }
    } catch (const std::exception& exception) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_WARNING, "UI window state load failed: " + std::string(exception.what()));
    } catch (...) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_WARNING, "UI window state load failed");
    }
}

void PluginManager::SavePersistentUiWindowState(const bool force) noexcept {
    try {
        std::scoped_lock lock(ui_window_state_mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!force && ui_window_state_last_save_ != std::chrono::steady_clock::time_point{} &&
            now - ui_window_state_last_save_ < kUiWindowStateSaveInterval) {
            return;
        }
        std::unordered_map<std::string, bool> plugin_windows;
        {
            std::scoped_lock visibility_lock(plugin_window_visibility_mutex_);
            plugin_windows = plugin_window_visibility_;
        }
        const std::string document = SerializeUiWindowState(
            ui_resources_->ExportPersistentWindowState(), plugin_windows);
        if (document == ui_window_state_last_document_) {
            ui_window_state_last_save_ = now;
            return;
        }

        std::error_code error;
        std::filesystem::create_directories(ui_window_state_file_.parent_path(), error);
        if (error) throw std::filesystem::filesystem_error(
            "UI window state directory create failed", ui_window_state_file_.parent_path(), error);
        const std::filesystem::path temporary = ui_window_state_file_.wstring() +
            L".tmp-" + std::to_wstring(GetCurrentProcessId());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("UI window state temporary file cannot be opened");
        output << document;
        output.flush();
        output.close();
        if (!output) throw std::runtime_error("UI window state write failed");
        if (!MoveFileExW(
                temporary.c_str(), ui_window_state_file_.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error_code = GetLastError();
            std::filesystem::remove(temporary, error);
            throw std::system_error(
                static_cast<int>(error_code), std::system_category(),
                "UI window state publish failed");
        }
        ui_window_state_last_document_ = document;
        ui_window_state_last_save_ = now;
    } catch (const std::exception& exception) {
        try {
            Log(CABBIRD_CORE_LOG_LEVEL_V1_WARNING, "UI window state save failed: " + std::string(exception.what()));
        } catch (...) {
        }
    } catch (...) {
        try {
            Log(CABBIRD_CORE_LOG_LEVEL_V1_WARNING, "UI window state save failed");
        } catch (...) {
        }
    }
}

void PluginManager::Maintenance() {
    static_cast<void>(MaintenancePluginState());
    PersistUiWindowState();
}

void PluginManager::SetPerformanceDiagnosticsEnabled(const bool enabled) noexcept {
    performance_diagnostics_enabled_.store(enabled, std::memory_order_relaxed);
    file_watcher_.SetDiagnosticsEnabled(enabled);
}

bool PluginManager::PerformanceDiagnosticsEnabled() const noexcept {
    return performance_diagnostics_enabled_.load(std::memory_order_relaxed);
}

PluginMaintenanceTiming PluginManager::MaintenancePluginState() {
    const ScopedLogThreadDomain log_domain(cabbird::LogThreadDomain::Worker);
    if (!PerformanceDiagnosticsEnabled()) {
        RetryWaitingForAdapterServices();
        PollForChanges();
        return {};
    }
    const auto retry_started = std::chrono::steady_clock::now();
    RetryWaitingForAdapterServices();
    const auto poll_started = std::chrono::steady_clock::now();
    PollForChanges();
    const auto completed = std::chrono::steady_clock::now();
    return {poll_started - retry_started, completed - poll_started};
}

void PluginManager::RetryWaitingForAdapterServices() {
    const std::uint64_t revision = cabbird::ProcessAdapterServices().Revision();
    if (revision == observed_adapter_service_revision_) return;
    observed_adapter_service_revision_ = revision;
    if (ui_service_ == nullptr) return;

    RetryWaitingPlugins();
}

void PluginManager::RetryWaitingPlugins() {
    for (const auto& plugin : plugins_) {
        if (!plugin->waiting_for_service || Activate(*plugin)) continue;
        plugin->waiting_for_service = false;
        Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
            "ABI v1 deferred activation failed: " + plugin->view.id);
    }
}

void PluginManager::PersistUiWindowState() {
    const ScopedLogThreadDomain log_domain(cabbird::LogThreadDomain::Worker);
    SavePersistentUiWindowState();
}

void PluginManager::ReconcileWindowVisibility(LoadedPlugin& plugin) noexcept {
    if (plugin.scope == nullptr || ui_resources_ == nullptr) return;
    const cabbird::UiWindowGroupState windows =
        ui_resources_->WindowGroupState(plugin.scope);
    if (windows.window_count != 0) {
        plugin.view.visible = windows.open_window_count != 0;
        std::scoped_lock lock(plugin_window_visibility_mutex_);
        plugin_window_visibility_.erase(plugin.view.id);
        return;
    }
    std::scoped_lock lock(plugin_window_visibility_mutex_);
    const auto persisted = plugin_window_visibility_.find(plugin.view.id);
    if (persisted != plugin_window_visibility_.end()) {
        plugin.view.visible = persisted->second;
    }
}

void PluginManager::SetPersistentPluginWindowVisibility(
    const std::string_view plugin_id, const bool visible) noexcept {
    try {
        std::scoped_lock lock(plugin_window_visibility_mutex_);
        plugin_window_visibility_.insert_or_assign(std::string(plugin_id), visible);
    } catch (...) {
    }
}

void PluginManager::GameUpdate(double delta_seconds) {
    // Counted before every gate below, so this is the pump's own liveness and not any one
    // plugin's.  See the comment on `g_game_update_calls`.
    g_game_update_calls.fetch_add(1, std::memory_order_relaxed);
    if (delta_seconds > 0.0) g_game_update_ticks.fetch_add(1, std::memory_order_relaxed);
    const ScopedLogThreadDomain log_domain(cabbird::LogThreadDomain::Game);

    for (const auto& plugin : plugins_) {
        if (plugin->faulted) continue;
        auto callback = plugin->scope != nullptr
            ? plugin->scope->AcquireCallback(plugin->view.generation)
            : cabbird::PluginScope::CallbackLease{};
        if (plugin->scope != nullptr && !callback) continue;
        ScopedPluginCallback callback_scope(
            plugin->scope, plugin->view.generation, false);
        const auto started = std::chrono::steady_clock::now();
        bool invoked{};
        bool fault{};
        if (plugin->started && plugin->descriptor_v1.on_update != nullptr) {
            invoked = true;
            try { plugin->descriptor_v1.on_update(plugin->plugin_context, delta_seconds); } catch (...) {
                fault = true;
                plugin->faulted = true;
                plugin->view.status_reason = "Update callback raised an exception";
                Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR, "exception in ABI v1 update: " + plugin->view.id);
            }
        }
        if (invoked) {
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            plugin->update_metrics.Record(
                elapsed, fault, callback_budgets_.update_slow_milliseconds);
            if (callback_evidence_observer_) {
                try {
                    callback_evidence_observer_({
                        PluginCallbackEvidenceKind::Update,
                        plugin->view.id,
                        plugin->view.generation,
                        GetCurrentThreadId(),
                        elapsed * 1000.0,
                        fault});
                } catch (...) {
                }
            }
        }
        ReconcileWindowVisibility(*plugin);
    }
}

bool PluginManager::ReloadPackages(const std::vector<std::string>& package_names) {
    std::string summary;
    for (const std::string& package_name : package_names) {
        if (!summary.empty()) summary += ',';
        summary += package_name;
    }
    std::unordered_set<std::string> changed(package_names.begin(), package_names.end());
    std::unordered_set<std::string> affected;
    for (const auto& plugin : plugins_) {
        if (changed.contains(Utf8(plugin->view.package_directory.filename()))) {
            affected.insert(plugin->view.id);
        }
    }
    for (const auto& [id, plugin] : disabled_plugins_) {
        if (changed.contains(Utf8(plugin.package_directory.filename()))) {
            affected.insert(id);
        }
    }

    cabbird::PluginCatalogSnapshot catalog;
    RunPluginLoadStep([&] {
        catalog = cabbird::DiscoverPluginCatalog(plugin_directory_);
    });
    for (const cabbird::PluginCatalogEntry& entry : catalog.Entries()) {
        if (entry.manifest && changed.contains(Utf8(entry.package_root.filename()))) {
            affected.insert(entry.manifest->id);
        }
    }
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const cabbird::PluginCatalogEntry& entry : catalog.Entries()) {
            if (!entry.manifest || affected.contains(entry.manifest->id)) continue;
            const bool depends_on_affected = std::any_of(
                entry.manifest->dependencies.begin(), entry.manifest->dependencies.end(),
                [&](const cabbird::PluginDependencyManifest& dependency) {
                    return affected.contains(dependency.id);
                });
            if (depends_on_affected) {
                affected.insert(entry.manifest->id);
                expanded = true;
            }
        }
    }

    struct RollbackCandidate {
        std::string id;
        std::filesystem::path source;
        std::filesystem::path shadow;
        std::filesystem::path package_directory;
        cabbird::PluginShadowGeneration shadow_generation;
    };
    std::vector<RollbackCandidate> rollback_candidates;
    std::vector<std::size_t> unload_indices;
    for (std::size_t index = 0; index < plugins_.size(); ++index) {
        if (!affected.contains(plugins_[index]->view.id)) continue;
        const auto& plugin = *plugins_[index];
        rollback_candidates.push_back({
            plugin.view.id, plugin.view.source, plugin.shadow,
            plugin.view.package_directory, plugin.shadow_generation});
        unload_indices.push_back(index);
    }
    UnloadIndices(unload_indices, false);
    if (std::any_of(
            quarantined_plugins_.begin(), quarantined_plugins_.end(),
            [&](const auto& plugin) { return affected.contains(plugin->view.id); })) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
            "plugin package reload stopped by quarantined generation: " + summary);
        return false;
    }

    const auto decisions = enablement_store_.Resolve(catalog);
    const auto publish_disabled = [&](const cabbird::PluginCatalogEntry& entry,
                                      std::string reason) {
        if (!entry.manifest) return;
        PluginView view;
        view.id = entry.manifest->id;
        view.name = entry.manifest->name;
        view.author = entry.manifest->author;
        view.version = entry.manifest->version.ToString();
        view.source = entry.entry_file;
        view.package_directory = entry.package_root;
        view.visible = false;
        view.enabled = false;
        view.state = "disabled";
        view.status_reason = std::move(reason);
        disabled_plugins_[view.id] = std::move(view);
    };
    for (const std::string& id : affected) disabled_plugins_.erase(id);

    const cabbird::PluginDependencyPlan plan = cabbird::ResolvePluginDependencies(catalog);
    bool replacements_ok = true;
    for (const std::string& id : plan.load_order) {
        if (!affected.contains(id)) continue;
        const cabbird::PluginCatalogEntry* entry = catalog.Find(id);
        if (entry != nullptr && !LoadAllowed(*entry)) {
            PublishSuspended(*entry);
            continue;
        }
        const auto decision = decisions.find(id);
        if (entry != nullptr && decision != decisions.end() && !decision->second.enabled) {
            publish_disabled(*entry, decision->second.reason);
            continue;
        }
        if (entry != nullptr) replacements_ok = LoadCatalogEntry(*entry) && replacements_ok;
    }

    for (const RollbackCandidate& candidate : rollback_candidates) {
        std::error_code error;
        if (!std::filesystem::exists(candidate.package_directory, error) ||
            disabled_plugins_.contains(candidate.id)) {
            continue;
        }
        const bool active = std::any_of(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
            return plugin->view.id == candidate.id;
        });
        replacements_ok = active && replacements_ok;
    }

    if (!replacements_ok) {
        std::vector<std::size_t> replacement_indices;
        for (std::size_t index = 0; index < plugins_.size(); ++index) {
            if (affected.contains(plugins_[index]->view.id)) replacement_indices.push_back(index);
        }
        UnloadIndices(replacement_indices);
        bool rollback_ok = true;
        for (const RollbackCandidate& candidate : rollback_candidates) {
            std::error_code error;
            if (!std::filesystem::exists(candidate.package_directory, error) ||
                disabled_plugins_.contains(candidate.id)) {
                shadow_store_.Retire(candidate.shadow_generation);
                continue;
            }
            const bool restored = LoadBinary(
                candidate.source, candidate.shadow_generation.entry_file,
                candidate.package_directory, candidate.shadow_generation);
            rollback_ok = restored && rollback_ok;
        }
        Log(rollback_ok ? CABBIRD_CORE_LOG_LEVEL_V1_WARNING : CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
            std::string(rollback_ok ? "plugin package reload rolled back: "
                                    : "plugin package rollback failed: ") + summary);
        return false;
    }

    for (const RollbackCandidate& candidate : rollback_candidates) {
        shadow_store_.Retire(candidate.shadow_generation);
    }
    Log(CABBIRD_CORE_LOG_LEVEL_V1_INFO, "plugin package change applied: " + summary);
    return true;
}

void PluginManager::PollForChanges() {
    std::vector<std::string> changed;
    {
        std::scoped_lock lock(pending_package_changes_mutex_);
        changed.swap(pending_package_changes_);
    }
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    if (!changed.empty()) static_cast<void>(ReloadPackages(changed));
    ApplyQueuedEnablementReload();
}

void PluginManager::QueuePackageChanges(
    std::vector<std::string> package_names) noexcept {
    try {
        std::scoped_lock lock(pending_package_changes_mutex_);
        for (std::string& package_name : package_names) {
            pending_package_changes_.push_back(std::move(package_name));
        }
    } catch (...) {
    }
}

void PluginManager::QueueEnablementReload() noexcept {
    pending_enablement_reload_.store(true, std::memory_order_release);
}

void PluginManager::ApplyQueuedEnablementReload() {
    if (!pending_enablement_reload_.exchange(false, std::memory_order_acq_rel)) return;
    std::string error;
    if (!enablement_store_.Load(&error)) {
        Log(CABBIRD_CORE_LOG_LEVEL_V1_ERROR, "plugin enablement reload failed: " + error);
        return;
    }
    const cabbird::PluginCatalogSnapshot catalog =
        cabbird::DiscoverPluginCatalog(plugin_directory_);
    const bool reconciled = ReconcileEnablement(catalog);
    Log(reconciled ? CABBIRD_CORE_LOG_LEVEL_V1_INFO : CABBIRD_CORE_LOG_LEVEL_V1_WARNING,
        std::string("plugin enablement config applied: reconciliation=") +
            (reconciled ? "succeeded" : "failed"));
}

void PluginManager::Draw(void* imgui_context) {
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
    const ScopedLogThreadDomain log_domain(cabbird::LogThreadDomain::Render);
    for (const auto& plugin : plugins_) {
        ReconcileWindowVisibility(*plugin);
        if (!plugin->view.visible || plugin->faulted) continue;
        auto callback = plugin->scope != nullptr
            ? plugin->scope->AcquireCallback(plugin->view.generation)
            : cabbird::PluginScope::CallbackLease{};
        if (plugin->scope != nullptr && !callback) continue;
        ScopedPluginCallback callback_scope(
            plugin->scope, plugin->view.generation, false);
        const auto started = std::chrono::steady_clock::now();
        bool invoked{};
        bool fault{};
        if (plugin->started && plugin->descriptor_v1.on_draw != nullptr &&
            ui_service_ != nullptr) {
            invoked = true;
            plugin->ui_proxy_context.close_requested = false;
            try {
                plugin->descriptor_v1.on_draw(
                    plugin->plugin_context,
                    reinterpret_cast<const CabbirdUiServiceV1*>(&plugin->ui_proxy));
                if (plugin->ui_proxy_context.close_requested) {
                    plugin->view.visible = false;
                    if (ui_resources_->WindowGroupState(plugin->scope).window_count == 0) {
                        SetPersistentPluginWindowVisibility(plugin->view.id, false);
                    }
                }
            } catch (...) {
                fault = true;
                plugin->faulted = true;
                plugin->view.status_reason = "Draw callback raised an exception";
            }
            const bool ui_stack_fault = RecoverPluginUiStack(
                plugin->ui_proxy_context, plugin->service_context);
            if (ui_stack_fault) {
                fault = true;
                plugin->faulted = true;
                plugin->view.status_reason = "Draw callback left the UI stack unbalanced";
            }
            if (fault) {
                Log(
                    CABBIRD_CORE_LOG_LEVEL_V1_ERROR,
                    std::string(ui_stack_fault ? "unbalanced UI stack: "
                                               : "exception in ABI v1 draw: ") +
                        plugin->view.id);
            }
        }
        if (invoked) {
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            plugin->draw_metrics.Record(
                elapsed, fault, callback_budgets_.draw_slow_milliseconds);
            if (callback_evidence_observer_) {
                try {
                    callback_evidence_observer_({
                        PluginCallbackEvidenceKind::Draw,
                        plugin->view.id,
                        plugin->view.generation,
                        GetCurrentThreadId(),
                        elapsed * 1000.0,
                        fault});
                } catch (...) {
                }
            }
        }
        ReconcileWindowVisibility(*plugin);
    }
}

void PluginManager::SetImGuiContext(void* imgui_context) noexcept {
    imgui_context_ = imgui_context;
}

void PluginManager::SetUiService(const CabbirdUiServiceV1* service) {
    ui_service_ = service != nullptr &&
            service->service_version == CABBIRD_UI_SERVICE_V1_VERSION &&
            service->struct_size >= sizeof(CabbirdUiServiceV1)
        ? service
        : nullptr;
    for (const auto& plugin : plugins_) {
        plugin->ui_proxy_context.service = ui_service_;
        plugin->ui_proxy = MakeUiProxy(&plugin->ui_proxy_context);
    }
    if (ui_service_ == nullptr) return;
    observed_adapter_service_revision_ = cabbird::ProcessAdapterServices().Revision();
    RetryWaitingPlugins();
}

void PluginManager::PublishInputFrame(
    const cabbird::InputFrameState& frame, const cabbird::InputUiCaptureState capture) {
    static_cast<void>(input_service_.AdvanceFrame(frame, capture));
}

void PluginManager::PublishUiCapture(const cabbird::InputUiCaptureState capture) {
    static_cast<void>(input_service_.RecordUiCapture(capture));
}

void PluginManager::ResetInput(const cabbird::InputResetReason reason) noexcept {
    try {
        static_cast<void>(input_service_.Reset(reason));
    } catch (...) {
    }
}

void PluginManager::OnUiDeviceLost() noexcept {
    try {
        std::shared_ptr<cabbird::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        if (backend != nullptr) backend->OnDeviceLost();
        static_cast<void>(ui_resources_->InvalidateDeviceResources());
        static_cast<void>(input_service_.OnDeviceReset());
    } catch (...) {
    }
}

bool PluginManager::OnUiDeviceRebuilt() noexcept {
    try {
        const std::uint64_t generation = ui_resources_->DeviceGeneration();
        if (!ui_resources_->RebuildDeviceResources(generation)) return false;
        std::shared_ptr<cabbird::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        return backend == nullptr || backend->OnDeviceRebuilt(generation);
    } catch (...) {
        return false;
    }
}

void PluginManager::SetUiResourceRenderBackend(
    std::shared_ptr<cabbird::UiResourceRenderBackend> backend) noexcept {
    try {
        std::scoped_lock lock(ui_resource_backend_mutex_);
        ui_resource_render_backend_ = std::move(backend);
    } catch (...) {
    }
}

void PluginManager::PrepareUiResources() noexcept {
    try {
        std::shared_ptr<cabbird::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        if (backend == nullptr) return;
        backend->CollectGarbage(*ui_resources_);
        for (const auto& plugin : plugins_) {
            if (plugin == nullptr || plugin->scope == nullptr) continue;
            for (const cabbird::UiResourceSnapshot& resource :
                 ui_resources_->Resources(plugin->scope)) {
                if (resource.kind == cabbird::UiResourceKind::Font &&
                    resource.state != cabbird::UiResourceState::Failed) {
                    backend->PrepareFont(*ui_resources_, plugin->scope, resource.handle);
                } else if (resource.kind == cabbird::UiResourceKind::Texture &&
                           resource.state != cabbird::UiResourceState::Failed) {
                    backend->PrepareTexture(*ui_resources_, plugin->scope, resource.handle);
                }
            }
        }
    } catch (...) {
    }
}

void PluginManager::PrepareUiTexture(
    const std::shared_ptr<cabbird::PluginScope>& scope,
    const cabbird::UiResourceHandle handle) noexcept {
    if (scope == nullptr || !handle) return;
    try {
        std::shared_ptr<cabbird::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        if (backend == nullptr) return;
        const auto resource = ui_resources_->ResourceState(scope, handle);
        if (!resource || resource->kind != cabbird::UiResourceKind::Texture ||
            resource->state == cabbird::UiResourceState::Failed ||
            resource->state == cabbird::UiResourceState::Revoked) {
            return;
        }
        backend->PrepareTexture(*ui_resources_, scope, handle);
    } catch (...) {
    }
}

bool PluginManager::QueueUiFontLoad(
    const std::shared_ptr<cabbird::PluginScope>& scope,
    const cabbird::UiResourceHandle handle) noexcept {
    if (scope == nullptr || !handle || !ui_resource_worker_dispatcher_) return false;

    const std::shared_ptr<cabbird::UiResourceRegistry> registry = ui_resources_;
    const auto resource = registry->ResourceState(scope, handle);
    if (!resource || resource->kind != cabbird::UiResourceKind::Font || resource->resource_id == 0 ||
        resource->state == cabbird::UiResourceState::Failed ||
        resource->state == cabbird::UiResourceState::Revoked) {
        return false;
    }

    const std::shared_ptr<UiResourceWorkerGate> gate = ui_resource_worker_gate_;
    if (gate == nullptr) return false;
    const std::uint64_t resource_id = resource->resource_id;
    try {
        std::scoped_lock lock(gate->mutex);
        const auto pending = gate->pending_resources.find(resource_id);
        if (pending != gate->pending_resources.end()) {
            AddUiResourceWorkerCandidate(
                pending->second, {scope, handle, cabbird::UiResourceKind::Font});
            return true;
        }
    } catch (...) {
        return false;
    }
    // A source payload is already staged, or the resource has reached a state
    // owned by the render backend. Neither case needs another Worker read.
    if (resource->state != cabbird::UiResourceState::Queued || resource->staged_bytes != 0 ||
        resource->reserved_staging_bytes != 0) {
        return true;
    }

    std::shared_ptr<UiResourceWorkerGateLease> worker_gate;
    try {
        worker_gate = std::make_shared<UiResourceWorkerGateLease>(
            registry, gate, resource_id, cabbird::UiResourceKind::Font);
        std::scoped_lock lock(gate->mutex);
        auto [pending, inserted] = gate->pending_resources.try_emplace(
            resource_id,
            std::vector<UiResourceWorkerGate::Pending>{
                {scope, handle, cabbird::UiResourceKind::Font}});
        if (!inserted) {
            AddUiResourceWorkerCandidate(
                pending->second, {scope, handle, cabbird::UiResourceKind::Font});
            return true;
        }
        worker_gate->Activate();
    } catch (...) {
        return false;
    }

    try {
        const std::string owner = scope->Owner();
        const std::uint64_t generation = scope->Generation();
        if (!ui_resource_worker_dispatcher_(
            owner, generation,
            [registry, worker_gate, resource_id] {
                try {
                    const auto pending = worker_gate->CurrentLive();
                    if (!pending || pending->kind != cabbird::UiResourceKind::Font) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto lease = pending->scope->AcquireCallback(pending->scope->Generation());
                    if (!lease) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto state = registry->ResourceState(pending->scope, pending->handle);
                    if (!state || state->kind != cabbird::UiResourceKind::Font ||
                        state->resource_id != resource_id ||
                        state->state != cabbird::UiResourceState::Queued || state->staged_bytes != 0) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto request = registry->FontRequest(pending->scope, pending->handle);
                    if (!request) {
                        worker_gate->Fail();
                        return;
                    }

                    const auto read_target = worker_gate->CurrentLive();
                    if (!read_target) {
                        worker_gate->Complete();
                        return;
                    }
                    UiResourceWorkerStagingAdmission read_admission{
                        registry, read_target->scope, read_target->handle};
                    cabbird::UiResourceReadResult read = ReadPackageResourceBytes(
                        *request,
                        cabbird::kDefaultUiResourceEncodedByteLimit, read_admission.Callback());
                    if (!read) {
                        worker_gate->Fail();
                        return;
                    }
                    const auto current = worker_gate->CurrentLive();
                    if (!current || !registry->SetFontData(
                            current->scope, current->handle, std::move(read.bytes))) {
                        worker_gate->Fail();
                        return;
                    }
                    worker_gate->Complete();
                } catch (...) {
                    worker_gate->Fail();
                }
            })) {
            worker_gate->Fail();
            return false;
        }
    } catch (...) {
        worker_gate->Fail();
        return false;
    }
    return true;
}

bool PluginManager::QueueUiTextureLoad(
    const std::shared_ptr<cabbird::PluginScope>& scope,
    const cabbird::UiResourceHandle handle) noexcept {
    if (scope == nullptr || !handle || !ui_resource_worker_dispatcher_) return false;

    const std::shared_ptr<cabbird::UiResourceRegistry> registry = ui_resources_;
    const auto resource = registry->ResourceState(scope, handle);
    if (!resource || resource->kind != cabbird::UiResourceKind::Texture || resource->resource_id == 0 ||
        resource->state == cabbird::UiResourceState::Failed ||
        resource->state == cabbird::UiResourceState::Revoked) {
        return false;
    }

    const std::shared_ptr<UiResourceWorkerGate> gate = ui_resource_worker_gate_;
    if (gate == nullptr) return false;
    const std::uint64_t resource_id = resource->resource_id;
    try {
        std::scoped_lock lock(gate->mutex);
        const auto pending = gate->pending_resources.find(resource_id);
        if (pending != gate->pending_resources.end()) {
            AddUiResourceWorkerCandidate(
                pending->second, {scope, handle, cabbird::UiResourceKind::Texture});
            return true;
        }
    } catch (...) {
        return false;
    }
    // Raw RGBA input and a completed decode are already render-ready payloads.
    // Device recovery uploads the cached pixels again without a Worker decode.
    if (resource->state != cabbird::UiResourceState::Queued ||
        (resource->texture_format == cabbird::UiTextureFormat::Rgba8 &&
         resource->staged_bytes != 0) ||
        resource->reserved_staging_bytes != 0) {
        return true;
    }

    std::shared_ptr<UiResourceWorkerGateLease> worker_gate;
    try {
        worker_gate = std::make_shared<UiResourceWorkerGateLease>(
            registry, gate, resource_id, cabbird::UiResourceKind::Texture);
        std::scoped_lock lock(gate->mutex);
        auto [pending, inserted] = gate->pending_resources.try_emplace(
            resource_id,
            std::vector<UiResourceWorkerGate::Pending>{
                {scope, handle, cabbird::UiResourceKind::Texture}});
        if (!inserted) {
            AddUiResourceWorkerCandidate(
                pending->second, {scope, handle, cabbird::UiResourceKind::Texture});
            return true;
        }
        worker_gate->Activate();
    } catch (...) {
        return false;
    }

    try {
        const std::string owner = scope->Owner();
        const std::uint64_t generation = scope->Generation();
        if (!ui_resource_worker_dispatcher_(
            owner, generation,
            [registry, worker_gate, resource_id] {
                try {
                    const auto pending = worker_gate->CurrentLive();
                    if (!pending || pending->kind != cabbird::UiResourceKind::Texture) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto lease = pending->scope->AcquireCallback(pending->scope->Generation());
                    if (!lease) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto state = registry->ResourceState(pending->scope, pending->handle);
                    if (!state || state->kind != cabbird::UiResourceKind::Texture ||
                        state->resource_id != resource_id ||
                        state->state != cabbird::UiResourceState::Queued ||
                        (state->texture_format == cabbird::UiTextureFormat::Rgba8 &&
                         state->staged_bytes != 0)) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto reservation_target = worker_gate->CurrentLive();
                    if (!reservation_target) {
                        worker_gate->Complete();
                        return;
                    }
                    if (state->staged_bytes != 0 && !registry->ReserveResourceStaging(
                            reservation_target->scope, reservation_target->handle,
                            state->staged_bytes)) {
                        worker_gate->Fail();
                        return;
                    }
                    auto request = registry->TextureRequest(
                        reservation_target->scope, reservation_target->handle);
                    if (!request || request->format != cabbird::UiTextureFormat::Auto) {
                        worker_gate->Fail();
                        return;
                    }

                    std::vector<std::uint8_t> encoded = std::move(request->encoded_bytes);
                    if (encoded.empty()) {
                        const auto read_target = worker_gate->CurrentLive();
                        if (!read_target) {
                            worker_gate->Complete();
                            return;
                        }
                        UiResourceWorkerStagingAdmission read_admission{
                            registry, read_target->scope, read_target->handle};
                        cabbird::UiResourceReadResult read = ReadPackageResourceBytes(
                            *request,
                            cabbird::kDefaultUiResourceEncodedByteLimit, read_admission.Callback());
                        if (!read) {
                            worker_gate->Fail();
                            return;
                        }
                        encoded = std::move(read.bytes);
                    }

                    const auto decode_target = worker_gate->CurrentLive();
                    if (!decode_target) {
                        worker_gate->Complete();
                        return;
                    }
                    UiResourceWorkerStagingAdmission decode_admission{
                        registry, decode_target->scope, decode_target->handle, encoded.size()};
                    cabbird::UiImageDecodeResult decoded = cabbird::DecodeUiImageRgba8(
                        encoded, cabbird::UiImageDecodeLimits{}, decode_admission.Callback());
                    if (!decoded) {
                        worker_gate->Fail();
                        return;
                    }
                    const auto current = worker_gate->CurrentLive();
                    if (!current || !registry->SetTextureData(
                            current->scope, current->handle, std::move(decoded.image.pixels),
                            cabbird::UiTextureFormat::Rgba8,
                            decoded.image.width, decoded.image.height)) {
                        worker_gate->Fail();
                        return;
                    }
                    worker_gate->Complete();
                } catch (...) {
                    worker_gate->Fail();
                }
            })) {
            worker_gate->Fail();
            return false;
        }
    } catch (...) {
        worker_gate->Fail();
        return false;
    }
    return true;
}

bool PluginManager::PushUiFont(
    const std::shared_ptr<cabbird::PluginScope>& scope,
    const cabbird::UiResourceHandle handle) noexcept {
    try {
        std::shared_ptr<cabbird::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        return backend != nullptr && backend->PushFont(*ui_resources_, scope, handle);
    } catch (...) {
        return false;
    }
}

bool PluginManager::PopUiFont() noexcept {
    try {
        std::shared_ptr<cabbird::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        return backend != nullptr && backend->PopFont();
    } catch (...) {
        return false;
    }
}

bool PluginManager::DrawUiTexture(
    const std::shared_ptr<cabbird::PluginScope>& scope,
    const cabbird::UiResourceHandle handle, const float width, const float height,
    const std::uint32_t tint_rgba) noexcept {
    try {
        std::shared_ptr<cabbird::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        return backend != nullptr &&
            backend->DrawTexture(*ui_resources_, scope, handle, width, height, tint_rgba);
    } catch (...) {
        return false;
    }
}

PluginRuntimeDiagnosticsSnapshot PluginManager::DiagnosticsSnapshot() const {
    struct ServiceCandidate final {
        std::string_view id;
        std::uint32_t version{};
        bool published{};
    };

    PluginRuntimeDiagnosticsSnapshot snapshot;
    snapshot.plugins.reserve(
        plugins_.size() + quarantined_plugins_.size() + disabled_plugins_.size());
    const cabbird::IpcDiagnostics ipc_snapshot = ipc_registry_ == nullptr
        ? cabbird::IpcDiagnostics{} : ipc_registry_->Snapshot();

    const auto populate_platform_diagnostics =
        [this, &ipc_snapshot](const LoadedPlugin& plugin, PluginView& view) {
            PluginPlatformDiagnosticsView diagnostics;
            const cabbird::PluginCapabilityGrant& grant = plugin.service_context.capabilities;
            diagnostics.capability_enforced = grant.EnforcesRawMemoryCapabilities();
            diagnostics.capabilities = grant.Capabilities();

            const auto deny = [&](const std::string_view service,
                                  const cabbird::PluginServiceAuthorization authorization) {
                if (authorization.allowed) return;
                const std::string reason = authorization.required_capability.empty()
                    ? std::string(service) + ": no capability mapping is available"
                    : std::string(service) + ": required capability " +
                        std::string(authorization.required_capability) + " is not granted";
                if (std::find(
                        diagnostics.deny_reasons.begin(), diagnostics.deny_reasons.end(), reason) ==
                    diagnostics.deny_reasons.end()) {
                    diagnostics.deny_reasons.push_back(reason);
                }
            };
            const auto add_service = [&](const ServiceCandidate candidate) {
                const cabbird::PluginServiceAuthorization authorization =
                    grant.AuthorizeService(candidate.id);
                if (!authorization.allowed) {
                    deny(candidate.id, authorization);
                    return;
                }
                if (candidate.published && candidate.version != 0) {
                    diagnostics.services.push_back({std::string(candidate.id), candidate.version});
                }
            };

            const std::array platform_services{
                ServiceCandidate{CABBIRD_CORE_SERVICE_V1_ID,
                    plugin.service_context.core.service_version, true},
                ServiceCandidate{CABBIRD_PLUGIN_STATE_SERVICE_V1_ID,
                    plugin.service_context.plugin_state.service_version, true},
                ServiceCandidate{CABBIRD_CONFIG_SERVICE_V1_ID,
                    plugin.service_context.config.service_version, true},
                ServiceCandidate{CABBIRD_STORAGE_SERVICE_V1_ID,
                    plugin.service_context.storage.service_version, true},
                ServiceCandidate{CABBIRD_JSON_SERVICE_V1_ID,
                    plugin.service_context.json.service_version, true},
                ServiceCandidate{CABBIRD_RUNTIME_INFO_SERVICE_V1_ID,
                    plugin.service_context.runtime_info.service_version, true},
                ServiceCandidate{CABBIRD_LOCALIZATION_SERVICE_V1_ID,
                    plugin.service_context.localization.service_version, true},
                ServiceCandidate{CABBIRD_DIAGNOSTICS_SERVICE_V1_ID,
                    plugin.service_context.diagnostics.service_version, true},
                ServiceCandidate{CABBIRD_SCHEDULER_SERVICE_V1_ID,
                    plugin.service_context.scheduler.service_version, true},
                ServiceCandidate{CABBIRD_IPC_SERVICE_V1_ID,
                    plugin.service_context.ipc_service.service_version, true},
                ServiceCandidate{CABBIRD_COMMANDS_SERVICE_V1_ID,
                    plugin.service_context.commands.service_version, true},
                ServiceCandidate{CABBIRD_NOTIFICATIONS_SERVICE_V1_ID,
                    plugin.service_context.notifications.service_version, true},
                ServiceCandidate{CABBIRD_SIGNATURE_SERVICE_V1_ID,
                    plugin.service_context.signature.service_version, true},
                ServiceCandidate{CABBIRD_HOOK_SERVICE_V1_ID,
                    plugin.service_context.hook.service_version, true},
                ServiceCandidate{CABBIRD_PATCH_SERVICE_V1_ID,
                    plugin.service_context.patch.service_version, true},
                ServiceCandidate{CABBIRD_UI_SERVICE_V1_ID,
                    plugin.service_context.ui == nullptr ? 0U : plugin.service_context.ui->service_version,
                    UiService() != nullptr},
                ServiceCandidate{CABBIRD_WINDOW_SERVICE_V1_ID,
                    plugin.service_context.window.service_version, true},
                ServiceCandidate{CABBIRD_FONT_SERVICE_V1_ID,
                    plugin.service_context.font.service_version, true},
                ServiceCandidate{CABBIRD_TEXTURE_SERVICE_V1_ID,
                    plugin.service_context.texture.service_version, true},
                ServiceCandidate{CABBIRD_INPUT_SERVICE_V1_ID,
                    plugin.service_context.input_service.service_version, true},
            };
            for (const ServiceCandidate candidate : platform_services) add_service(candidate);
            for (const auto& service : cabbird::ProcessAdapterServices().Snapshot()) {
                add_service({service.id, service.version, service.table != nullptr});
            }
            std::sort(
                diagnostics.services.begin(), diagnostics.services.end(),
                [](const PluginServiceVersionView& left, const PluginServiceVersionView& right) {
                    return left.id < right.id;
                });
            diagnostics.services.erase(
                std::unique(
                    diagnostics.services.begin(), diagnostics.services.end(),
                    [](const PluginServiceVersionView& left, const PluginServiceVersionView& right) {
                        return left.id == right.id;
                    }),
                diagnostics.services.end());

            if (plugin.scope != nullptr) {
                const std::vector<cabbird::PluginResourceRecord> resources =
                    plugin.scope->Resources();
                diagnostics.resources.ledger_resources = resources.size();
                for (const cabbird::PluginResourceRecord& resource : resources) {
                    switch (resource.kind) {
                    case cabbird::PluginResourceKind::Window:
                        ++diagnostics.resources.windows;
                        break;
                    case cabbird::PluginResourceKind::Font:
                        ++diagnostics.resources.fonts;
                        break;
                    case cabbird::PluginResourceKind::Texture:
                        ++diagnostics.resources.textures;
                        break;
                    case cabbird::PluginResourceKind::Input:
                        ++diagnostics.resources.hotkeys;
                        break;
                    case cabbird::PluginResourceKind::Ipc:
                        ++diagnostics.resources.ipc_resources;
                        break;
                    default:
                        break;
                    }
                }
            }

            if (plugin.scope != nullptr && platform_services_ != nullptr) {
                const cabbird::ScopedPlatformDiagnosticsView scoped = platform_services_->Snapshot(
                    ScopedPlatformOwner(plugin.service_context));
                diagnostics.resources.configs = scoped.resources.configs;
                diagnostics.resources.self_tests = scoped.resources.self_tests;
                diagnostics.resources.tasks = scoped.resources.tasks;
                diagnostics.resources.commands = scoped.resources.commands;
                diagnostics.resources.notifications = scoped.resources.notifications;
                diagnostics.resources.hooks = scoped.resources.hooks;
                diagnostics.resources.patches = scoped.resources.patches;
                diagnostics.queued_tasks = scoped.queued_tasks;
                diagnostics.scoped_callbacks = {
                    scoped.callback_calls, scoped.callback_faults, scoped.slow_callbacks};
            }
            for (const cabbird::IpcEndpointDiagnostics& endpoint : ipc_snapshot.endpoints) {
                if (endpoint.provider == plugin.view.id || std::ranges::find(
                        endpoint.consumers, plugin.view.id) != endpoint.consumers.end()) {
                    diagnostics.ipc_endpoints.push_back(endpoint);
                }
            }
            view.platform_diagnostics = std::move(diagnostics);
        };

    const auto append_loaded = [&](const LoadedPlugin& plugin, const bool quarantined) {
        PluginView view = plugin.view;
        view.enabled = !quarantined;
        view.state = quarantined ? "quarantined"
            : plugin.faulted ? "faulted"
            : plugin.waiting_for_service ? "waiting-for-service"
            : (plugin.started ? "active" : "loaded");
        if (quarantined) {
            view.visible = false;
        } else if (plugin.waiting_for_service) {
            view.status_reason = "required service is not ready";
        } else if (!plugin.faulted) {
            view.status_reason.clear();
        }
        view.update_metrics = plugin.update_metrics.View();
        view.draw_metrics = plugin.draw_metrics.View();
        populate_platform_diagnostics(plugin, view);
        snapshot.plugins.push_back(std::move(view));
    };

    for (const auto& plugin : plugins_) append_loaded(*plugin, false);
    for (const auto& plugin : quarantined_plugins_) append_loaded(*plugin, true);
    for (const auto& [id, disabled] : disabled_plugins_) snapshot.plugins.push_back(disabled);
    std::sort(
        snapshot.plugins.begin(), snapshot.plugins.end(),
        [](const PluginView& left, const PluginView& right) { return left.id < right.id; });
    return snapshot;
}

std::vector<PluginView> PluginManager::Plugins() const {
    return DiagnosticsSnapshot().plugins;
}

std::string PluginManager::DiagnosticsJson() const {
    const PluginRuntimeDiagnosticsSnapshot snapshot = DiagnosticsSnapshot();
    std::string output{"{\"schemaVersion\":" + std::to_string(snapshot.schema_version) +
        ",\"pump\":{\"gameUpdateCalls\":" + std::to_string(g_game_update_calls.load(std::memory_order_relaxed)) +
        ",\"gameUpdateTicks\":" + std::to_string(g_game_update_ticks.load(std::memory_order_relaxed)) +
        ",\"drawCalls\":" + std::to_string(g_draw_calls.load(std::memory_order_relaxed)) + "}" +
        ",\"plugins\":["};
    const auto append_strings = [&output](const std::vector<std::string>& values) {
        output.push_back('[');
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0) output.push_back(',');
            output += cabbird::json::Quote(values[index]);
        }
        output.push_back(']');
    };
    for (std::size_t index = 0; index < snapshot.plugins.size(); ++index) {
        if (index != 0) output.push_back(',');
        const PluginView& plugin = snapshot.plugins[index];
        const PluginPlatformDiagnosticsView& platform = plugin.platform_diagnostics;
        output += "{\"id\":" + cabbird::json::Quote(plugin.id) +
            ",\"generation\":" + std::to_string(plugin.generation) +
            ",\"state\":" + cabbird::json::Quote(plugin.state) +
            ",\"statusReason\":" + cabbird::json::Quote(plugin.status_reason) +
            ",\"capabilityEnforced\":" +
                (platform.capability_enforced ? "true" : "false") +
            ",\"capabilities\":";
        append_strings(platform.capabilities);
        output += ",\"services\":[";
        for (std::size_t service_index = 0; service_index < platform.services.size(); ++service_index) {
            if (service_index != 0) output.push_back(',');
            const PluginServiceVersionView& service = platform.services[service_index];
            output += "{\"id\":" + cabbird::json::Quote(service.id) +
                ",\"version\":" + std::to_string(service.version) + '}';
        }
        const PluginResourceCountsView& resources = platform.resources;
        output += "],\"resources\":{\"ledger\":" +
            std::to_string(resources.ledger_resources) +
            ",\"configs\":" + std::to_string(resources.configs) +
            ",\"selfTests\":" + std::to_string(resources.self_tests) +
            ",\"tasks\":" + std::to_string(resources.tasks) +
            ",\"ipcResources\":" + std::to_string(resources.ipc_resources) +
            ",\"commands\":" + std::to_string(resources.commands) +
            ",\"notifications\":" + std::to_string(resources.notifications) +
            ",\"hooks\":" + std::to_string(resources.hooks) +
            ",\"patches\":" + std::to_string(resources.patches) +
            ",\"windows\":" + std::to_string(resources.windows) +
            ",\"fonts\":" + std::to_string(resources.fonts) +
            ",\"textures\":" + std::to_string(resources.textures) +
            ",\"hotkeys\":" + std::to_string(resources.hotkeys) +
            "},\"queuedTasks\":" + std::to_string(platform.queued_tasks) +
            ",\"callbacks\":{\"updateSlow\":" +
                std::to_string(plugin.update_metrics.slow_calls) +
            ",\"drawSlow\":" + std::to_string(plugin.draw_metrics.slow_calls) +
            ",\"scoped\":{\"calls\":" +
                std::to_string(platform.scoped_callbacks.calls) +
            ",\"faults\":" + std::to_string(platform.scoped_callbacks.faults) +
            ",\"slowCalls\":" + std::to_string(platform.scoped_callbacks.slow_calls) +
            "}},\"ipcEndpoints\":[";
        for (std::size_t ipc_index = 0; ipc_index < platform.ipc_endpoints.size(); ++ipc_index) {
            if (ipc_index != 0) output.push_back(',');
            const cabbird::IpcEndpointDiagnostics& endpoint = platform.ipc_endpoints[ipc_index];
            output += "{\"id\":" + cabbird::json::Quote(endpoint.id) +
                ",\"provider\":" + cabbird::json::Quote(endpoint.provider) +
                ",\"consumers\":";
            append_strings(endpoint.consumers);
            output += ",\"generation\":" + std::to_string(endpoint.generation) +
                ",\"major\":" + std::to_string(endpoint.major_version) +
                ",\"minor\":" + std::to_string(endpoint.minor_version) +
                ",\"requestSchema\":" + cabbird::json::Quote(endpoint.request_schema_hash) +
                ",\"responseSchema\":" + cabbird::json::Quote(endpoint.response_schema_hash) +
                ",\"eventSchema\":" + cabbird::json::Quote(endpoint.event_schema_hash) +
                ",\"modes\":" + std::to_string(endpoint.modes) +
                ",\"affinity\":" + std::to_string(endpoint.affinity) +
                ",\"calls\":" + std::to_string(endpoint.calls) +
                ",\"failures\":" + std::to_string(endpoint.failures) +
                ",\"timeouts\":" + std::to_string(endpoint.timeouts) +
                ",\"events\":" + std::to_string(endpoint.events) +
                ",\"subscriptions\":" + std::to_string(endpoint.subscriptions) +
                ",\"pendingCalls\":" + std::to_string(endpoint.pending_calls) +
                ",\"p95Milliseconds\":" + std::to_string(endpoint.p95_milliseconds) + '}';
        }
        output += "],\"denyReasons\":";
        append_strings(platform.deny_reasons);
        output.push_back('}');
    }
    output += "]}";
    return output;
}

std::filesystem::path PluginManager::PackageDirectory(std::string_view plugin_id) const {
    const auto found = std::find_if(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
        return plugin->view.id == plugin_id;
    });
    return found == plugins_.end() ? std::filesystem::path{} : (*found)->view.package_directory;
}

bool PluginManager::SetVisible(std::string_view plugin_id, bool visible) {
    const auto found = std::find_if(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
        return plugin->view.id == plugin_id;
    });
    if (found == plugins_.end()) return false;
    auto callback = (*found)->scope != nullptr
        ? (*found)->scope->AcquireCallback((*found)->view.generation)
        : cabbird::PluginScope::CallbackLease{};
    if ((*found)->scope != nullptr && !callback) return false;
    ScopedPluginCallback callback_scope(
        (*found)->scope, (*found)->view.generation, false);
    const cabbird::UiWindowGroupState windows =
        ui_resources_->WindowGroupState((*found)->scope);
    if (windows.window_count != 0 &&
        !ui_resources_->SetWindowGroupOpen((*found)->scope, visible)) {
        return false;
    }
    if (windows.window_count == 0) {
        SetPersistentPluginWindowVisibility((*found)->view.id, visible);
    }
    (*found)->view.visible = visible;
    (*found)->ui_proxy_context.reopen_requested = visible;
    ReconcileWindowVisibility(*(*found));
    return (*found)->view.visibility_control;
}

}  // namespace cabbird
