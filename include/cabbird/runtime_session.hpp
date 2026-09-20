/* cabbird/runtime_session.hpp -- one owner for the whole in-process lifetime.
 *
 * Copied from the sibling Anomaly project (include/anomaly/runtime_session.hpp) as part of
 * the UE -> Unity port.  The structure, the option fields and the stop ordering are kept
 * exactly; the only edits are the bootstrap constants/type names, which come from Cabbird's
 * own core_api.h.
 *
 * WHY A SESSION OBJECT AND NOT A STARTUP FUNCTION: every shutdown bug this project has had
 * is a variant of "who stops this, and in what order".  A thread that outlives the image,
 * a plugin callback that lands after its DLL is gone, an injector that stays resident and
 * locks its own executable.  RuntimeSession makes the phases explicit and single-owner:
 *
 *   Start()        -> services (blocking, then async), then workers, then RUNNING
 *   RequestStop()  -> the stop token, from any thread
 *   ...            -> STOPPING_PLUGINS (plugin generations drain while the dispatchers are
 *                     still alive, which is the only window where a plugin CAN be stopped)
 *   ...            -> STOPPING_SERVICES (reverse dependency order)
 *   Join()         -> the lifecycle thread exits; on_stopped fires exactly once
 *
 * The stop_plugins hook runs BEFORE service shutdown on purpose.  A plugin's teardown may
 * need the services it depends on, so stopping services first would hand every plugin a
 * torn-down dependency -- and the failure would look like a plugin bug.
 *
 * `RuntimeStartContext` mirrors what the injector hands the mapped image (see
 * cabbird/core_api.h).  `external_stop_event` is borrowed on input and DUPLICATED before
 * Start returns: the injector's handle belongs to the injector's process-lifetime view, and
 * a session that waits on a handle it does not own is one CloseHandle away from a hang
 * nobody can explain.
 */
#pragma once

#include "cabbird/core_api.h"
#include "cabbird/runtime_dispatchers.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace cabbird {

class ServiceGraph;

struct RuntimeStartContext {
    std::uint32_t bootstrap_abi_version{CABBIRD_BOOTSTRAP_ABI_VERSION};
    CabbirdBootstrapType bootstrap_type{CABBIRD_BOOTSTRAP_TYPE_UNKNOWN};
    HMODULE bootstrap_module{};
    HMODULE game_module{};
    /* Where the runtime keeps its own state, and where diagnostics go.  Two paths, not one,
     * because a mapped image can write logs next to the injector while reading game data
     * elsewhere -- and after this project's "ini silently not read" incident, the log
     * directory is the first thing anyone checks. */
    std::filesystem::path runtime_root;
    std::filesystem::path log_directory;
    /* Borrowed on input.  RuntimeSession duplicates the handle before Start returns. */
    HANDLE external_stop_event{};
};

struct RuntimeWorker {
    std::string name;
    std::function<DWORD(std::stop_token)> run;
};

struct RuntimeSessionSnapshot {
    CabbirdRuntimeState state{CABBIRD_RUNTIME_STATE_DORMANT};
    DWORD last_error{ERROR_SUCCESS};
    std::uint64_t generation{};
};

struct RuntimeSessionOptions {
    std::shared_ptr<ServiceGraph> services;
    RuntimeDispatchersOptions dispatcher_options{};
    std::function<DWORD(std::stop_token)> initialize;
    /* Invoked after runtime workers have joined and while the dispatcher domains remain
     * alive, so plugin generations can drain before service shutdown. */
    std::function<DWORD(std::chrono::milliseconds)> stop_plugins;
    std::chrono::milliseconds plugin_stop_timeout{std::chrono::seconds(1)};
    std::function<void()> shutdown;
    /* Invoked exactly once after the final Stopped state is published and before the
     * lifecycle thread exits.  Join synchronizes its completion; an observer exception is
     * contained and does not change the final snapshot.  The observer must not call Join or
     * destroy its owning session -- that is a self-join deadlock, not a recoverable error. */
    std::function<void(RuntimeSessionSnapshot)> on_stopped;
    std::vector<RuntimeWorker> workers;
};

class RuntimeSession final {
public:
    /* The session must outlive work submitted through Dispatchers().  Destroying the
     * session from one of its dispatcher callbacks is invalid. */
    RuntimeSession(RuntimeStartContext start_context, RuntimeSessionOptions options);
    ~RuntimeSession();

    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;
    RuntimeSession(RuntimeSession&&) = delete;
    RuntimeSession& operator=(RuntimeSession&&) = delete;

    [[nodiscard]] DWORD Start() noexcept;
    void RequestStop() noexcept;
    [[nodiscard]] bool WaitForStop(std::chrono::milliseconds timeout) const noexcept;
    void Join() noexcept;

    [[nodiscard]] RuntimeSessionSnapshot Snapshot() const noexcept;
    [[nodiscard]] const RuntimeStartContext& StartContext() const noexcept;
    [[nodiscard]] RuntimeDispatchers& Dispatchers() noexcept;
    [[nodiscard]] const RuntimeDispatchers& Dispatchers() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabbird
