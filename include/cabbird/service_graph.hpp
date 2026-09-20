/* cabbird/service_graph.hpp -- dependency-ordered service startup and shutdown.
 *
 * Copied from the sibling Anomaly project (include/anomaly/service_graph.hpp) as part of
 * the UE -> Unity port.  Every field, enum value and method is kept: the graph is the piece
 * that decides the ORDER in which the runtime's services come up and go down, and that
 * order is not an implementation detail here -- it is the difference between a service
 * finding `GameAssembly.dll` already resolved and a service crashing on a null export.
 *
 * WHAT IT IS FOR, CONCRETELY: Cabbird's services have hard dependencies on each other --
 * the memory layer before the IL2CPP facade, the IL2CPP facade before the Game-domain
 * probe, the profiles before anything that reads a version-specific layout.  Before this
 * class, that order lived in the call sequence of one startup function.  Here it is data
 * (`required_dependencies`), it is checked before anything starts (`Build()` rejects a
 * missing or cyclic dependency), and it is reversible (`StopAll` runs in reverse order and
 * publishes a snapshot of whatever failed).
 *
 * THREE CONCEPTS WORTH READING TWICE:
 *
 *   * `ServiceState::Degraded` -- not an error.  Our target is a live shipping
 *     build whose layout is discovered at runtime, so "this service is up but running with
 *     a reduced capability set" is a normal outcome, and the graph must be able to express
 *     it without failing the whole startup.  The per-feature half of the same idea is
 *     reported by the Unity compatibility page, whose rows come from the profile document.
 *
 *   * `ServiceAffinity` -- which thread domain a service's start/stop runs on.  It is
 *     enforced through RuntimeDispatchers::Invoke, so a service that declares `Game`
 *     affinity is started by the game's own thread and not by the lifecycle thread.  That
 *     is the contract that keeps managed-object work off threads the GC does not know.
 *
 *   * `ServiceLifetime::PluginScoped` -- the service is created per plugin and torn down
 *     with it, which is what makes unloading a plugin actually release the things it used.
 *
 * `start` returns DWORD/Windows error codes, same as everything else in this runtime: the
 * value travels into ServiceSnapshot::error and from there into the diagnostics JSON, so a
 * failure is reported rather than swallowed.
 */
#pragma once

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cabbird {

enum class ServiceLifetime {
    /* The service is a caller-supplied object; the graph does not own it. */
    Provided,
    /* One instance for the whole process. */
    Singleton,
    /* One instance per plugin, destroyed with the plugin. */
    PluginScoped,
};

enum class ServiceStartup {
    /* StartAll does not return until this service is up.  Anything a later service needs
     * to exist at startup belongs here. */
    Blocking,
    /* Started in the background; WaitForAsync() is the barrier. */
    Async,
    /* Started on first use. */
    Lazy,
};

enum class ServiceAffinity {
    Lifecycle,
    Game,
    Render,
    Worker,
    /* The graph may run it wherever the caller already is.  Used by services with no
     * thread-sensitive work; it is an assertion, so do not reach for it to silence a
     * deadlock. */
    Any,
};

enum class ServiceState {
    Registered,
    Starting,
    Ready,
    Degraded,
    Failed,
    Stopping,
    Stopped,
};

struct ServiceDependency {
    std::string id;
    std::uint32_t minimum_version{1};
};

struct ServiceDescriptor {
    std::string id;
    std::uint32_t version{1};
    ServiceLifetime lifetime{ServiceLifetime::Singleton};
    ServiceStartup startup{ServiceStartup::Blocking};
    ServiceAffinity affinity{ServiceAffinity::Lifecycle};
    std::vector<ServiceDependency> required_dependencies;
    /* Optional dependencies do not order startup and do not fail it.  Their only effect is
     * on the snapshot: `resolved` says whether the service happened to be there. */
    std::vector<ServiceDependency> optional_dependencies;
    std::function<DWORD(std::stop_token)> start;
    std::function<void()> stop;
};

struct ServiceDependencySnapshot {
    std::string id;
    std::uint32_t minimum_version{1};
    bool optional{};
    bool resolved{};
    std::uint32_t resolved_version{};
    ServiceState state{ServiceState::Registered};
};

struct ServiceSnapshot {
    std::string id;
    std::uint32_t version{};
    ServiceLifetime lifetime{ServiceLifetime::Singleton};
    ServiceStartup startup{ServiceStartup::Blocking};
    ServiceAffinity affinity{ServiceAffinity::Lifecycle};
    ServiceState state{ServiceState::Registered};
    DWORD error{ERROR_SUCCESS};
    std::chrono::microseconds startup_duration{};
    /* Execution evidence captured by the composition root.  A zero thread id means the
     * callback has not run yet -- which is exactly the question you want answered when a
     * service is suspected of having run on the wrong thread. */
    std::uint64_t start_thread_id{};
    std::uint64_t stop_thread_id{};
    std::chrono::microseconds start_queue_delay{};
    std::chrono::microseconds stop_queue_delay{};
    /* True when the affinity could not be honoured and the callback ran inline anyway.
     * Recorded instead of logged because it is the difference between "the domains work"
     * and "the domains were bypassed and nothing said so". */
    bool affinity_bypassed{};
    std::vector<ServiceDependencySnapshot> dependencies;
    std::vector<std::string> failure_chain;
};

struct ServiceAffinityExecutors {
    /* The executor owns the dispatch and waits for completion.  Implementations must
     * return ERROR_NOT_READY when the requested domain is not available, rather than
     * running the work on the wrong thread. */
    std::function<DWORD(
        ServiceAffinity, std::function<DWORD(std::stop_token)>, std::stop_token)> start;
    std::function<void(ServiceAffinity, std::function<void()>)> stop;
};

struct ServiceFailureSnapshot {
    std::string service_id;
    DWORD error{ERROR_SUCCESS};
    /* The dependency whose failure caused this one, empty when the service failed on its
     * own.  Kept as a name so the JSON artifact reads as a chain rather than a flat list. */
    std::string caused_by;
};

struct ServiceGraphSnapshot {
    bool built{};
    bool stop_requested{};
    bool startup_active{};
    bool blocking_startup_complete{};
    bool async_startup_complete{};
    DWORD error{ERROR_SUCCESS};
    DWORD async_startup_error{ERROR_SUCCESS};
    std::vector<ServiceFailureSnapshot> failures;
    std::vector<ServiceSnapshot> services;
};

class ServiceGraph final {
public:
    ServiceGraph();
    ~ServiceGraph();

    ServiceGraph(const ServiceGraph&) = delete;
    ServiceGraph& operator=(const ServiceGraph&) = delete;
    ServiceGraph(ServiceGraph&&) = delete;
    ServiceGraph& operator=(ServiceGraph&&) = delete;

    [[nodiscard]] DWORD Register(ServiceDescriptor descriptor) noexcept;
    /* Must be set before Build.  Leaving it unset keeps the standalone/direct execution
     * behaviour that the offline fixtures rely on -- which is why the tests can exercise
     * ordering without dragging RuntimeDispatchers in. */
    void SetAffinityExecutors(ServiceAffinityExecutors executors) noexcept;
    [[nodiscard]] DWORD Build() noexcept;
    [[nodiscard]] DWORD StartAll(std::stop_token stop_token = {}) noexcept;
    [[nodiscard]] DWORD StartService(
        std::string_view service_id, std::stop_token stop_token = {}) noexcept;
    [[nodiscard]] DWORD WaitForAsync(std::stop_token stop_token = {}) noexcept;
    /* Cancels incomplete starts but RETAINS Ready services so the reverse-order StopAll
     * still has something to stop.  A startup that was aborted halfway is exactly when
     * skipping the teardown hurts most. */
    void CancelStartup() noexcept;
    void StopAll() noexcept;

    [[nodiscard]] ServiceGraphSnapshot Snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabbird
