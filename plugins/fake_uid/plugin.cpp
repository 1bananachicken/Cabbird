#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/services/platform.h"
#include "cabbird/sdk/services/ui.h"
#include "uid_runtime.hpp"
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
using cabbird::sdk::StringView;
using cabbird::sdk::Ok;
constexpr const char* kSchemaId = "fake-uid-settings-v1";
constexpr const char* kSchema = R"({
  "type":"object",
  "required":["enabled","hidePrefix","displayUid"],
  "properties":{
    "enabled":{"type":"boolean"},
    "hidePrefix":{"type":"boolean"},
    "displayUid":{"type":"string","minLength":1,"maxLength":256}
  },
  "additionalProperties":false
})";

const CabbirdUiServiceV1* g_ui{};
const CabbirdConfigServiceV1* g_config{};
const CabbirdIl2CppServiceV1* g_il2cpp{};
const CabbirdSchedulerServiceV1* g_scheduler{};
std::mutex g_mutex;
fake_uid::Settings g_active{};
bool g_running{};
int g_open{1};
int g_hide_prefix{0};
char g_display[1025]{"12345678"};
char g_notice[256]{};

void CABBIRD_CALL PersistConfig(void*, CabbirdGenerationHandleV1) {
    std::scoped_lock lock(g_mutex);
    if (!g_config) return;
    const std::string document = nlohmann::json{
        {"enabled", g_active.enabled != 0},
        {"hidePrefix", g_active.hide_prefix != 0},
        {"displayUid", g_active.display_uid}
    }.dump();
    static_cast<void>(g_config->write_atomic(g_config->user, StringView(kSchemaId), 1,
        {reinterpret_cast<const std::uint8_t*>(document.data()), document.size()}));
}

void LoadConfig() {
    if (!g_config) return;
    CabbirdGenerationHandleV1 handle{};
    const auto registration = g_config->register_schema(g_config->user, StringView(kSchemaId), 1,
        {reinterpret_cast<const std::uint8_t*>(kSchema), std::strlen(kSchema)}, &handle);
    if (registration.code != CABBIRD_STATUS_V1_OK) { g_config = nullptr; return; }
    std::size_t size{};
    std::uint32_t version{};
    const auto probe = g_config->read(g_config->user, StringView(kSchemaId), &version, {nullptr, 0}, &size);
    if (probe.code != CABBIRD_STATUS_V1_OK || size == 0 || size > 8192 || version != 1) return;
    std::vector<std::uint8_t> bytes(size);
    if (g_config->read(g_config->user, StringView(kSchemaId), &version, {bytes.data(), bytes.size()}, &size).code !=
        CABBIRD_STATUS_V1_OK || size > bytes.size()) return;
    try {
        const auto json = nlohmann::json::parse(bytes.begin(), bytes.begin() + size);
        const auto display = json.at("displayUid").get<std::string>();
        // Legacy originalUid is informational only, never an editable identity.
        if (display.empty() || display.size() >= sizeof(g_display) ||
            display.find('\0') != std::string::npos) return;
        fake_uid::Settings loaded{};
        std::snprintf(loaded.display_uid, sizeof(loaded.display_uid), "%s", display.c_str());
        loaded.hide_prefix = json.value("hidePrefix", false);
        loaded.enabled = json.value("enabled", false);
        // Parse all fields before committing; a malformed field must not leave
        // a partially loaded editor. Load validates the final runtime request.
        g_active = loaded;
        std::memcpy(g_display, loaded.display_uid, sizeof(g_display));
        g_hide_prefix = loaded.hide_prefix;
    } catch (...) {
        std::snprintf(g_notice, sizeof(g_notice), "Stored configuration was invalid; defaults retained.");
    }
}
void SaveConfig() {
    if (!g_config) {
        return;
    }
    // Config I/O is forbidden from render/UI callbacks. Run it on the host
    // scheduler worker instead of reporting a misleading save failure.
    if (g_scheduler && g_scheduler->schedule) {
        CabbirdGenerationHandleV1 ignored{};
        static_cast<void>(g_scheduler->schedule(g_scheduler->user, 0, PersistConfig, nullptr, &ignored));
    }
}
void CopyEditor(fake_uid::Settings& request) {
    request.hide_prefix = g_hide_prefix;
    std::memcpy(request.display_uid, g_display, sizeof(g_display));
}
void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1*) {
    std::scoped_lock lock(g_mutex);
    if (!g_ui) return;
    cabbird::sdk::UiWindow window(g_ui, "FakeUID", &g_open);
    if (!window) return;
    auto* ui = g_ui;
    const fake_uid::Status state = fake_uid::Snapshot();
    char original_line[96]{};
    std::snprintf(original_line, sizeof(original_line), "Original UID: %s",
        state.original_uid[0] ? state.original_uid :
        (state.original_text[0] ? state.original_text : "waiting for game"));
    ui->text(ui->user, StringView(original_line));
    ui->input_text(ui->user, StringView("Display UID"), g_display, sizeof(g_display), 0);
    ui->checkbox(ui->user, StringView("Hide UID prefix"), &g_hide_prefix);
    const int usable = g_running;
    if (ui->button_enabled(ui->user, StringView("Apply"), 0, 0, usable)) {
        fake_uid::Settings request{};
        CopyEditor(request);
        request.enabled = 1;
        std::string error;
        if (fake_uid::Configure(request, error)) {
            g_active = request;
            SaveConfig();
        } else {
            std::snprintf(g_notice, sizeof(g_notice), "%s", error.c_str());
        }
    }
    if (ui->button_enabled(ui->user, StringView("Revert to original"), 0, 0, usable)) {
        g_active.enabled = 0;
        std::string error;
        if (fake_uid::Configure(g_active, error)) SaveConfig();
        else std::snprintf(g_notice, sizeof(g_notice), "Restore request could not be queued.");
    }
}
CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (!host || !context) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;
    const cabbird::sdk::Host services(host);
    g_ui = services.Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID, CABBIRD_UI_SERVICE_V1_VERSION).get();
    if (!g_ui || !g_ui->input_text || !g_ui->button_enabled)
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    g_config = services.Query<CabbirdConfigServiceV1>(
        CABBIRD_CONFIG_SERVICE_V1_ID, CABBIRD_CONFIG_SERVICE_V1_VERSION).get();
    if (g_config && (!g_config->register_schema || !g_config->read || !g_config->write_atomic)) g_config = nullptr;
    g_il2cpp = services.Query<CabbirdIl2CppServiceV1>(
        CABBIRD_IL2CPP_SERVICE_V1_ID, CABBIRD_IL2CPP_SERVICE_V1_VERSION).get();
    g_scheduler = services.Query<CabbirdSchedulerServiceV1>(
        CABBIRD_SCHEDULER_SERVICE_V1_ID, CABBIRD_SCHEDULER_SERVICE_V1_VERSION).get();
    g_active = {};
    g_hide_prefix = g_active.hide_prefix;
    std::memcpy(g_display, g_active.display_uid, sizeof(g_display));
    LoadConfig();
    CopyEditor(g_active);
    std::string error;
    if (!fake_uid::Configure(g_active, error)) {
        g_active.enabled = false;
        static_cast<void>(fake_uid::Configure(g_active, error));
        std::snprintf(g_notice, sizeof(g_notice), "Stored settings rejected; override disabled.");
    }
    return Ok();
}
CabbirdStatusV1 CABBIRD_CALL Start(void*) {
    std::scoped_lock lock(g_mutex);
    g_running = true;
    return Ok();
}
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) {
    std::scoped_lock lock(g_mutex);
    g_running = false;
    return fake_uid::Shutdown(g_il2cpp) ? Ok() : CabbirdStatusV1{CABBIRD_STATUS_V1_FAILED, 0, {}};
}
void CABBIRD_CALL Unload(void*) {
    std::scoped_lock lock(g_mutex);
    g_ui = nullptr; g_config = nullptr; g_il2cpp = nullptr; g_scheduler = nullptr;
}
void CABBIRD_CALL Update(void*, double) {
    {
        std::scoped_lock lock(g_mutex);
        if (!g_running) return;
    }
    // Tick owns all managed objects. Do not hold the UI mutex over engine calls.
    // Keep ticking after Revert so original text can be restored before unload.
    fake_uid::Tick(g_il2cpp);
}
} // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    descriptor->id = StringView("cabbird.fake-uid");
    descriptor->name = StringView("FakeUID");
    descriptor->author = StringView("Cabbird");
    descriptor->version = StringView("0.1.0");
    descriptor->on_load = Load;
    descriptor->on_start = Start;
    descriptor->on_stop = Stop;
    descriptor->on_unload = Unload;
    descriptor->on_update = Update;
    descriptor->on_draw = Draw;
    return Ok();
}
