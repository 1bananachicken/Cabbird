/* Implementation of the resource ledger and the per-plugin callback barrier.
 *
 * The barrier is a simple in-flight counter plus a condition variable, and the two
 * entry points differ in one way that matters:
 *
 *   AcquireCallback()        ordinary path.  Refuses once sources are frozen.
 *   AcquireLifecycleLease()  shutdown path.  Still granted after freeze, because
 *                            otherwise on_stop could never run and the plugin could
 *                            never be unloaded cleanly.  That asymmetry is the whole
 *                            trick: freeze the floodgates, but leave one door open for
 *                            the person turning off the lights.
 *
 * Both count as in-flight, so the host's final "is anything still running?" check sees
 * the lifecycle callback too.
 */
#include "cabbird/plugin_scope.hpp"

#include <algorithm>
#include <utility>

namespace cabbird {

const char* PluginResourceKindName(PluginResourceKind kind) noexcept {
    switch (kind) {
        case PluginResourceKind::Config: return "config";
        case PluginResourceKind::Subscription: return "subscription";
        case PluginResourceKind::Task: return "task";
        case PluginResourceKind::Ui: return "ui";
        case PluginResourceKind::Hook: return "hook";
        case PluginResourceKind::Patch: return "patch";
        case PluginResourceKind::Texture: return "texture";
        case PluginResourceKind::Ipc: return "ipc";
        case PluginResourceKind::Command: return "command";
        case PluginResourceKind::Window: return "window";
        case PluginResourceKind::Font: return "font";
        case PluginResourceKind::Input: return "input";
        case PluginResourceKind::Notification: return "notification";
        case PluginResourceKind::Diagnostics: return "diagnostics";
        case PluginResourceKind::Json: return "json";
        case PluginResourceKind::UnityScene: return "unity_scene";
        case PluginResourceKind::Il2cppSubscription: return "il2cpp_subscription";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// ResourceLedger
// ---------------------------------------------------------------------------

std::uint64_t ResourceLedger::Register(
    std::string owner, std::uint64_t generation,
    PluginResourceKind kind, std::string label, Revoker revoker) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t token = next_token_++;
    Entry entry;
    entry.record.token = token;
    entry.record.owner = std::move(owner);
    entry.record.generation = generation;
    entry.record.kind = kind;
    entry.record.label = std::move(label);
    entry.revoker = std::move(revoker);
    entries_.emplace(token, std::move(entry));
    return token;
}

bool ResourceLedger::Release(std::uint64_t token) noexcept {
    Entry entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(token);
        if (it == entries_.end()) {
            return false;
        }
        entry = std::move(it->second);
        entries_.erase(it);
    }
    // The revoker runs OUTSIDE the lock on purpose: a revoker typically unregisters a
    // hook or frees a texture, and doing that while holding the ledger mutex would
    // make any re-entrant registration (a revoker that cleans up two things) deadlock.
    if (entry.revoker) {
        entry.revoker();
    }
    return true;
}

std::size_t ResourceLedger::Revoke(std::string_view owner, std::uint64_t generation) noexcept {
    std::vector<Entry> revoked;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.record.owner == owner && it->second.record.generation == generation) {
                revoked.push_back(std::move(it->second));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (Entry& entry : revoked) {
        if (entry.revoker) {
            entry.revoker();
        }
    }
    return revoked.size();
}

std::size_t ResourceLedger::RevokeExcept(
    std::string_view owner, std::uint64_t generation,
    PluginResourceKind preserved_kind) noexcept {
    std::vector<Entry> revoked;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            const bool matches = it->second.record.owner == owner &&
                                 it->second.record.generation == generation;
            if (matches && it->second.record.kind != preserved_kind) {
                revoked.push_back(std::move(it->second));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (Entry& entry : revoked) {
        if (entry.revoker) {
            entry.revoker();
        }
    }
    return revoked.size();
}

std::vector<PluginResourceRecord> ResourceLedger::Snapshot(
    std::optional<std::string_view> owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PluginResourceRecord> out;
    out.reserve(entries_.size());
    for (const auto& [token, entry] : entries_) {
        (void)token;
        if (owner.has_value() && entry.record.owner != *owner) {
            continue;
        }
        out.push_back(entry.record);
    }
    // Deterministic order: without this, "what does this plugin still hold?" differs
    // between runs and a leak report is not reproducible.
    std::sort(out.begin(), out.end(), [](const PluginResourceRecord& a,
                                         const PluginResourceRecord& b) {
        return a.token < b.token;
    });
    return out;
}

// ---------------------------------------------------------------------------
// PluginScope
// ---------------------------------------------------------------------------

struct PluginScope::BarrierState {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t in_flight{0};
    bool accepting{true};
};

PluginScope::CallbackLease::~CallbackLease() { Reset(); }

PluginScope::CallbackLease::CallbackLease(CallbackLease&& other) noexcept
    : state_(std::move(other.state_)) {}

PluginScope::CallbackLease& PluginScope::CallbackLease::operator=(
    CallbackLease&& other) noexcept {
    if (this != &other) {
        Reset();
        state_ = std::move(other.state_);
    }
    return *this;
}

void PluginScope::CallbackLease::Reset() noexcept {
    if (!state_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->in_flight > 0) {
            --state_->in_flight;
        }
    }
    // Notify outside the lock: a waiter woken inside the critical section would
    // immediately block again on the same mutex, which is legal but adds latency to
    // exactly the path (unload) where latency is the thing being measured.
    state_->cv.notify_all();
    state_.reset();
}

PluginScope::PluginScope(
    std::shared_ptr<ResourceLedger> ledger, std::string owner, std::uint64_t generation)
    : ledger_(std::move(ledger)),
      owner_(std::move(owner)),
      generation_(generation),
      barrier_(std::make_shared<BarrierState>()) {}

std::uint64_t PluginScope::Register(
    PluginResourceKind kind, std::string label, ResourceLedger::Revoker revoker) {
    if (!ledger_) {
        return 0;
    }
    return ledger_->Register(owner_, generation_, kind, std::move(label), std::move(revoker));
}

bool PluginScope::Release(std::uint64_t token) noexcept {
    if (!ledger_ || token == 0) {
        return false;
    }
    return ledger_->Release(token);
}

PluginScope::CallbackLease PluginScope::AcquireCallback(std::uint64_t generation) noexcept {
    if (!barrier_ || generation != generation_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(barrier_->mutex);
    if (!barrier_->accepting) {
        return {};
    }
    ++barrier_->in_flight;
    return CallbackLease(barrier_);
}

bool PluginScope::FreezeCallbackSources() noexcept {
    if (!barrier_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(barrier_->mutex);
    barrier_->accepting = false;
    return true;
}

PluginScope::CallbackLease PluginScope::AcquireLifecycleLease(std::uint64_t generation) noexcept {
    if (!barrier_ || generation != generation_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(barrier_->mutex);
    // Deliberately NOT gated on `accepting`: the lifecycle path has to remain usable
    // after FreezeCallbackSources, or on_stop could never be called.
    ++barrier_->in_flight;
    return CallbackLease(barrier_);
}

bool PluginScope::BeginStop(std::chrono::milliseconds timeout) noexcept {
    if (!barrier_) {
        return false;
    }
    std::unique_lock<std::mutex> lock(barrier_->mutex);
    barrier_->accepting = false;
    // A zero-timeout wait is still a wait: it gives any callback that is releasing its
    // lease right now a chance to finish before we declare failure.
    barrier_->cv.wait_for(lock, timeout, [this] { return barrier_->in_flight == 0; });
    return barrier_->in_flight == 0;
}

std::size_t PluginScope::RevokeAllExcept(PluginResourceKind preserved_kind) noexcept {
    if (!ledger_) {
        return 0;
    }
    return ledger_->RevokeExcept(owner_, generation_, preserved_kind);
}

std::size_t PluginScope::RevokeAll() noexcept {
    if (!ledger_) {
        return 0;
    }
    return ledger_->Revoke(owner_, generation_);
}

std::vector<PluginResourceRecord> PluginScope::Resources() const {
    if (!ledger_) {
        return {};
    }
    return ledger_->Snapshot(owner_);
}

PluginScopeSnapshot PluginScope::Snapshot() const {
    PluginScopeSnapshot out;
    out.owner = owner_;
    out.generation = generation_;
    if (barrier_) {
        std::lock_guard<std::mutex> lock(barrier_->mutex);
        out.accepting_callbacks = barrier_->accepting;
        out.in_flight_callbacks = barrier_->in_flight;
        // A lifecycle lease is legal once ordinary sources are frozen, which is how a
        // stopping plugin still gets its on_stop callback.
        out.lifecycle_allowed = !barrier_->accepting;
    }
    if (ledger_) {
        out.resources = ledger_->Snapshot(owner_).size();
    }
    return out;
}

std::size_t PluginScope::InFlightCallbacks() const noexcept {
    if (!barrier_) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(barrier_->mutex);
    return barrier_->in_flight;
}

bool PluginScope::SourcesFrozen() const noexcept {
    if (!barrier_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(barrier_->mutex);
    return !barrier_->accepting;
}

}  // namespace cabbird
