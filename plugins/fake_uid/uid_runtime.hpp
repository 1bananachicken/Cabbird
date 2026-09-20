#pragma once
#include <cstdint>
#include "cabbird/sdk/services/il2cpp.h"
#include <string>

namespace fake_uid {
struct Settings {
    bool enabled{};
    bool hide_prefix{false};
    char original_uid[32]{};
    char display_uid[1025]{"12345678"};
};
struct Status {
    bool enabled{};
    std::uint32_t tracked{};
    std::uint32_t applied{};
    std::uint64_t writes{};
    char original_uid[32]{};
    char original_text[1153]{};
    char message[256]{};
};
// Native copies only; callable from Draw. No managed entry point here.
bool Configure(const Settings& settings, std::string& error);
Status Snapshot();
// All Unity calls are made from the plugin's existing on_update callback.
void Tick(const CabbirdIl2CppServiceV1* service);
// Ordinary callbacks are drained by the host before on_stop. Releases only GC
// roots; never invokes a Unity UI method from the lifecycle worker.
bool Shutdown(const CabbirdIl2CppServiceV1* service);
}
