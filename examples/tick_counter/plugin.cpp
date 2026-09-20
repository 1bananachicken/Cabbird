#include "cabbird/sdk/cpp.hpp"

#include <cstdio>

namespace {

const CabbirdUiServiceV1* g_ui{};
unsigned long long g_ticks{};

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    const auto ui = cabbird::sdk::Host(host).Query<CabbirdUiServiceV1>(
        CABBIRD_UI_SERVICE_V1_ID, CABBIRD_UI_SERVICE_V1_VERSION);
    if (!ui) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    g_ui = ui.get();
    *context = &g_ticks;
    return cabbird::sdk::Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) { return cabbird::sdk::Ok(); }
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) { return cabbird::sdk::Ok(); }

void CABBIRD_CALL Unload(void*) {
    g_ui = nullptr;
    g_ticks = 0;
}

void CABBIRD_CALL Update(void*, double) { ++g_ticks; }

void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1* ui) {
    if (ui == nullptr) ui = g_ui;
    int open = 1;
    cabbird::sdk::UiWindow window(ui, "Game Tick Counter", &open);
    if (!window) return;
    char text[96]{};
    std::snprintf(text, sizeof(text), "Validated game-thread ticks: %llu", g_ticks);
    ui->text(ui->user, cabbird::sdk::StringView(text));
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        cabbird::sdk::StringView("cabbird.example.tick-counter"),
        cabbird::sdk::StringView("Game Tick Counter"),
        cabbird::sdk::StringView("Cabbird"), cabbird::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return cabbird::sdk::Ok();
}
