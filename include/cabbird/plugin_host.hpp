/* cabbird/plugin_host.hpp -- loads, starts, stops and unloads plugin DLLs.
 *
 * The upstream equivalent (anomaly/plugin_manager.cpp) is 289 KB and covers manifest
 * validation, dependency resolution, directory watching, shadow stores and hot reload.
 * This is deliberately far smaller: it does the part that must be right before any of
 * that is worth building -- resolve the entry point, check the ABI version, drive the
 * lifecycle, and never unload a DLL that still has a callback in flight.
 *
 * The invariants it enforces:
 *
 *   1. The descriptor is host-allocated.  The plugin writes identity and callbacks in;
 *      it never keeps the pointer.
 *   2. api_major must match, or the load is refused before on_load runs.  A plugin
 *      built against a different major has a different struct layout, so calling into
 *      it is already too late to fail safely.
 *   3. Every stop is bounded.  on_stop gets a deadline and the host enforces it by
 *      revoking resources whether or not the plugin cooperated.
 *   4. Unload is refused while any callback is in flight.  Refusing is correct:
 *      FreeLibrary with a live callback is a crash, and "the plugin would not stop"
 *      is a reportable condition, not a reason to corrupt the process.
 *
 * NOT here yet: manifest files, dependency resolution between plugins, hot reload /
 * generation bump, directory watching.
 */
#pragma once

#include "cabbird/plugin_scope.hpp"
#include "cabbird/sdk/plugin.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cabbird {

enum class PluginState {
    Discovered,
    Loaded,
    Started,
    Stopping,
    Stopped,
    Failed,
};

const char* PluginStateName(PluginState state) noexcept;

struct PluginRecord {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::filesystem::path path;
    std::uint64_t generation{};
    PluginState state{PluginState::Discovered};
    std::string last_error;
};

struct PluginStopReport {
    bool stopped{};
    bool drained{};
    std::size_t revoked{};
    std::size_t in_flight_at_timeout{};
};

/* Owns every loaded plugin.  One host per process; the ledger is shared with the rest
 * of the runtime so there is a single answer to "what is still alive?".
 */
class PluginHost final {
public:
    PluginHost();
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    /* Supplies the API table handed to every plugin.  Must be called before Load.
     * The allocator inside it is what plugins use for cross-boundary allocations. */
    void SetHostApi(const CabbirdHostApiV1& api);
    [[nodiscard]] const CabbirdHostApiV1& HostApi() const noexcept { return api_; }

    /* LoadLibrary + resolve CabbirdPluginEntryV1 + validate api major + on_load.
     * Returns the plugin id, or an empty string on failure (see LastError()). */
    [[nodiscard]] std::string Load(const std::filesystem::path& dll_path);

    bool Start(std::string_view id);
    /* Freezes callbacks, drains with `deadline`, revokes resources, calls on_stop,
     * then revokes anything the plugin registered during on_stop. */
    PluginStopReport Stop(
        std::string_view id,
        std::chrono::milliseconds deadline = std::chrono::seconds(5));
    /* Stop (if needed) and FreeLibrary.  Refuses -- returning false -- while callbacks
     * are still in flight, because unloading then would be a use-after-free. */
    bool Unload(
        std::string_view id,
        std::chrono::milliseconds deadline = std::chrono::seconds(5));

    /* Broadcast to the Game domain.  A plugin that is not Started is skipped. */
    void Update(double delta_seconds);
    /* Broadcast to the Render domain, inside Present. */
    void Draw(const CabbirdUiServiceV1* ui);

    [[nodiscard]] std::vector<PluginRecord> Plugins() const;
    [[nodiscard]] const PluginRecord* Find(std::string_view id) const;
    [[nodiscard]] const std::string& LastError() const noexcept { return last_error_; }
    [[nodiscard]] std::shared_ptr<ResourceLedger> Ledger() const noexcept { return ledger_; }

private:
    struct Loaded;
    [[nodiscard]] Loaded* Get(std::string_view id);
    [[nodiscard]] const Loaded* Get(std::string_view id) const;

    std::shared_ptr<ResourceLedger> ledger_;
    CabbirdHostApiV1 api_{};
    std::vector<std::unique_ptr<Loaded>> plugins_;
    std::string last_error_;
    std::uint64_t next_generation_{1};
};

}  // namespace cabbird
