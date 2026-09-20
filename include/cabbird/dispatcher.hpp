/* cabbird/dispatcher.hpp -- one execution lane with owner/generation tracking.
 *
 * Ported from anomaly/dispatcher.hpp.  A Dispatcher is a queue plus a pump; it owns no
 * thread.  Whether it gets a thread is the caller's decision, and that separation is
 * the whole reason this design survives inside a game process:
 *
 *   Game   domain: NO thread.  The game's own tick calls Pump().
 *   Render domain: NO thread.  Present calls Pump().
 *   Lifecycle / Worker: a real thread, created by the runtime and nowhere else.
 *
 * Creating a thread inside a Unity/IL2CPP process at the wrong moment has already cost
 * this project a crash (Boehm GC "Collecting from unknown thread"), so the domains that
 * touch game state must borrow a thread that the engine already owns and trusts.
 *
 * Every task carries `owner` and `generation`:
 *   * owner      -- which plugin or service queued it
 *   * generation -- which incarnation of that owner it belongs to
 *
 * The generation is what makes hot reload safe.  When a plugin is reloaded its
 * generation increments, and CancelOwnerGeneration(owner, old) removes exactly the work
 * queued by the previous incarnation -- not the new one's, and not some other plugin's.
 * Without it, "cancel the old work" degrades into "cancel everything and hope".
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cabbird {

struct TaskHandle {
    std::uint64_t value{};

    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(TaskHandle, TaskHandle) = default;
};

enum class TaskState : std::uint8_t {
    Queued,
    Running,
    Completed,
    Cancelled,
    Failed,
};

const char* TaskStateName(TaskState state) noexcept;

struct TaskSnapshot {
    TaskHandle handle;
    std::string owner;
    std::uint64_t generation{};
    TaskState state{TaskState::Queued};
    std::string failure_message;
};

/* A task that threw.  This is reported rather than propagated: a plugin callback that
 * throws must not be able to unwind through the host's frame pump, because that would
 * unwind into Unity's own call stack and take the process with it. */
struct TaskFailure {
    TaskHandle handle;
    std::string owner;
    std::uint64_t generation{};
    std::string message;
    std::exception_ptr exception;
};

struct DispatcherOptions {
    /* How many finished/cancelled/failed tasks to remember for GetTask().  Bounded on
     * purpose: an unbounded record of every task ever run is a leak that only shows up
     * in a long game session. */
    std::size_t terminal_history_capacity{1024};
};

class Dispatcher final {
public:
    using Callback = std::function<void()>;

    explicit Dispatcher(DispatcherOptions options = {});
    ~Dispatcher();

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    Dispatcher(Dispatcher&&) = delete;
    Dispatcher& operator=(Dispatcher&&) = delete;

    [[nodiscard]] TaskHandle Post(
        std::string owner, std::uint64_t generation, Callback callback);
    [[nodiscard]] bool Cancel(TaskHandle handle);
    [[nodiscard]] std::size_t CancelPending() noexcept;
    [[nodiscard]] bool DrainAll(std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t CancelOwnerGeneration(
        std::string_view owner, std::uint64_t generation);

    /* Records the current generation for an owner.  Called on (re)load so that a
     * queued task can tell whether it is stale by the time it runs. */
    void SetGeneration(std::string owner, std::uint64_t generation);
    [[nodiscard]] bool Drain(
        std::string_view owner,
        std::uint64_t generation,
        std::chrono::milliseconds timeout);

    /* Claim this thread as the one the domain runs on.  Idempotent, and deliberately
     * separate from construction: the pump thread already exists (it is the game's),
     * so the dispatcher declares an affinity rather than creating one. */
    void BindToCurrentThread() noexcept;
    [[nodiscard]] bool IsCurrentThread() const noexcept;
    [[nodiscard]] std::thread::id BoundThread() const noexcept;

    /* Runs queued callbacks.  Returns how many were invoked; cancelled entries are
     * removed and not counted.  `max_callbacks` bounds how long a single pump can take,
     * which matters when the pump runs inside Present. */
    [[nodiscard]] std::size_t Pump(
        std::size_t max_callbacks = (std::numeric_limits<std::size_t>::max)()) noexcept;

    /* Binds to the current thread and pumps until stop is requested.  Used only by the
     * Lifecycle and Worker domains, in addition to DrainAll. */
    void Run(std::stop_token stop_token) noexcept;

    [[nodiscard]] std::optional<TaskSnapshot> GetTask(TaskHandle handle) const;
    [[nodiscard]] std::optional<TaskState> GetState(TaskHandle handle) const;
    [[nodiscard]] std::vector<TaskFailure> Failures() const;
    [[nodiscard]] std::size_t TrackedTaskCount() const noexcept;
    [[nodiscard]] std::size_t PendingCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabbird
