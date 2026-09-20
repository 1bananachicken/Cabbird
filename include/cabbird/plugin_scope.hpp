/* cabbird/plugin_scope.hpp -- per-plugin resource ledger and callback barrier.
 *
 * Ported from anomaly/plugin_scope.hpp.  This is the single most important piece of
 * the plugin framework, and the reason is worth restating in our own terms:
 *
 *   A plugin DLL can only be unloaded when (a) none of its code is executing on any
 *   thread, and (b) none of the host objects it registered still hold a callback into
 *   it.  Get either wrong and the process crashes -- not "the plugin misbehaves", but
 *   memory is freed underneath running code.
 *
 * Cabbird has already had one component kill its host process by doing something it
 * was not entitled to do, so this is not hypothetical here.  Every resource a plugin
 * creates -- a hook, a subscription, a queued task, a UI window, a texture, a patch, an
 * IPC endpoint -- is registered here with a revoker, and unloading is:
 *
 *     scope.FreezeCallbackSources()   stop accepting new callbacks
 *     scope.BeginStop(timeout)        drain the ones already in flight
 *     scope.RevokeAllExcept(kind)     (optional) staged shutdown
 *     scope.RevokeAll()               undo every resource, in reverse
 *     -> only now is FreeLibrary safe
 *
 * TWO names differ from upstream on purpose:
 *   * `PluginResourceKind::NteEscMenuButton` is NOT ported.  It existed only to attach a
 *     button to NTE's own ESC menu; the game-specific variants behind it (icons, labels,
 *     expand-vs-close semantics) are NTE content and are excluded.
 *     Inventing a Unity placeholder would be worse than omitting it: when the Unity
 *     profile layer defines its own menu resource it adds its own kind.
 *   * `UnityScene` and `Il2cppSubscription` are new: the two Unity-specific resource
 *     classes we already know we need (scene-load subscriptions, and subscriptions to
 *     IL2CPP type/instance changes).
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cabbird {

enum class PluginResourceKind : std::uint8_t {
    Config,
    Subscription,
    Task,
    Ui,
    Hook,
    Patch,
    Texture,
    Ipc,
    Command,
    Window,
    Font,
    Input,
    Notification,
    Diagnostics,
    Json,
    /* Added for the Unity target. */
    UnityScene,
    Il2cppSubscription,
};

const char* PluginResourceKindName(PluginResourceKind kind) noexcept;

struct PluginResourceRecord {
    std::uint64_t token{};
    std::string owner;
    std::uint64_t generation{};
    PluginResourceKind kind{PluginResourceKind::Task};
    std::string label;
};

struct PluginScopeSnapshot {
    std::string owner;
    std::uint64_t generation{};
    bool accepting_callbacks{};
    bool lifecycle_allowed{};
    std::size_t in_flight_callbacks{};
    std::size_t resources{};
};

/* Owns the tokens for every resource the host holds on a plugin's behalf.
 *
 * Deliberately separate from PluginScope: the ledger is process-wide and keyed by
 * token, so it can answer "what is still registered?" even for a plugin whose scope
 * object is gone -- which is exactly the state after a failed unload. */
class ResourceLedger final {
public:
    using Revoker = std::function<void()>;

    [[nodiscard]] std::uint64_t Register(
        std::string owner, std::uint64_t generation,
        PluginResourceKind kind, std::string label, Revoker revoker = {});
    bool Release(std::uint64_t token) noexcept;
    /* Revoke everything owned by (owner, generation).  Returns how many were revoked. */
    std::size_t Revoke(std::string_view owner, std::uint64_t generation) noexcept;
    /* Staged shutdown: revoke everything except one category, so a resource can
     * survive across on_stop and be torn down afterwards. */
    std::size_t RevokeExcept(
        std::string_view owner, std::uint64_t generation,
        PluginResourceKind preserved_kind) noexcept;
    [[nodiscard]] std::vector<PluginResourceRecord> Snapshot(
        std::optional<std::string_view> owner = std::nullopt) const;

private:
    struct Entry {
        PluginResourceRecord record;
        Revoker revoker;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::uint64_t next_token_{1};
};

class PluginScope final : public std::enable_shared_from_this<PluginScope> {
private:
    struct BarrierState;

public:
    /* RAII token representing "this plugin is executing right now".  While any lease
     * is alive on a generation, BeginStop cannot report the scope drained.  Move-only
     * so the count cannot be silently duplicated. */
    class CallbackLease final {
    public:
        CallbackLease() = default;
        ~CallbackLease();
        CallbackLease(CallbackLease&& other) noexcept;
        CallbackLease& operator=(CallbackLease&& other) noexcept;
        CallbackLease(const CallbackLease&) = delete;
        CallbackLease& operator=(const CallbackLease&) = delete;
        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

    private:
        friend class PluginScope;
        explicit CallbackLease(std::shared_ptr<BarrierState> state) : state_(std::move(state)) {}
        void Reset() noexcept;
        std::shared_ptr<BarrierState> state_;
    };

    PluginScope(
        std::shared_ptr<ResourceLedger> ledger,
        std::string owner,
        std::uint64_t generation);

    [[nodiscard]] const std::string& Owner() const noexcept { return owner_; }
    [[nodiscard]] std::uint64_t Generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t Register(
        PluginResourceKind kind, std::string label, ResourceLedger::Revoker revoker = {});
    bool Release(std::uint64_t token) noexcept;
    /* Returns a lease only if the generation matches and callbacks are still being
     * accepted.  A null lease means "do not call into this plugin now". */
    [[nodiscard]] CallbackLease AcquireCallback(std::uint64_t generation) noexcept;
    /* Freezes ordinary callback sources without waiting for callbacks already in
     * flight.  A lifecycle lease can be acquired after BeginStop has drained those. */
    bool FreezeCallbackSources() noexcept;
    [[nodiscard]] CallbackLease AcquireLifecycleLease(std::uint64_t generation) noexcept;
    /* Drains in-flight callbacks, up to `timeout`.  Returns false on timeout -- which
     * must be treated as "unload anyway would be unsafe", not as a warning. */
    [[nodiscard]] bool BeginStop(std::chrono::milliseconds timeout) noexcept;
    /* Host staged shutdown can preserve one resource category through on_stop.
     * The caller must finish with RevokeAll before on_unload or DLL release. */
    std::size_t RevokeAllExcept(PluginResourceKind preserved_kind) noexcept;
    std::size_t RevokeAll() noexcept;
    [[nodiscard]] std::vector<PluginResourceRecord> Resources() const;
    [[nodiscard]] PluginScopeSnapshot Snapshot() const;
    [[nodiscard]] std::size_t InFlightCallbacks() const noexcept;
    [[nodiscard]] bool SourcesFrozen() const noexcept;

private:
    std::shared_ptr<ResourceLedger> ledger_;
    std::string owner_;
    std::uint64_t generation_{};
    std::shared_ptr<BarrierState> barrier_;
};

}  // namespace cabbird
