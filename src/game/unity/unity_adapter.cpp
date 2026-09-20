// UnityAdapter: one implementation boundary, matching Anomaly's Ue5NteAdapter.
// State owns the lifecycle, frame clock, and internal Unity capability components.
// SDK capabilities remain distinct; none has an independent host lifecycle.

#include "cabbird/unity_adapter.hpp"
#include "cabbird/dispatch_tick_hook.hpp"
#include "cabbird/unity_build_profile.hpp"
#include <filesystem>
#include <string>
#include "cabbird/thread_local_value.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include "cabbird/unity_services.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include "cabbird/adapter_service_registry.hpp"
#include "cabbird/il2cpp.hpp"
#include "cabbird/il2cpp_dump.hpp"
#include "cabbird/sdk/services/unity.h"
#include <cstdint>
#include "cabbird/memory.hpp"
#include "cabbird/sdk/services/il2cpp.h"
#include "cabbird/sdk/il2cpp.hpp"
#include "cabbird/json.hpp"
#include <cfloat>
#include <cmath>
#include "imgui.h"
#include <optional>
#include <cstddef>
#include <string_view>
#include <array>
#include <type_traits>
#include <stdexcept>


namespace cabbird {

// Private component operations; only the adapter coordinates their lifecycle.
static void InvalidateUnityEntityState() noexcept;
static void InvalidateUnityPlayerState() noexcept;
static void InvalidateUnityPlayerTeleportState() noexcept;
static void InvalidateUnityTransformState() noexcept;
static void RefreshUnityEntityEsp();
// The entity walk's sampling divisor, in game ticks, set once from the adapter's
// `UnitySnapshotSamplingOptions`.  Declared here because the walk is a free function in this
// translation unit and the value has to be in place before the first tick.
static void SetUnityEntityRefreshDivisor(std::uint32_t divisor) noexcept;
// Whether the walk may call the compiled `Transform::get_position` body directly.  Set once from
// the same options struct, for the same reason: it has to be in place before the first tick.
static void SetUnityEntityDirectPosition(bool enabled) noexcept;
static bool EntityDirectPositionEnabled() noexcept;
static void RefreshUnityPlayer();
static void RefreshUnityPlayerTeleport();
static void ObserveUnityIl2CppGameThread();
static void ProcessUnityIl2CppDiagnostics();
static void InitializeUnityIl2CppState();
static void CancelUnityIl2CppDiagnostics() noexcept;

namespace {

enum class UnityAdapterComponent : std::uint8_t {
    Dump,
    Overlay,
    Transform,
    Entities,
    Player,
    Teleport,
    Il2Cpp,
    Count,
};

// The Unity adapter owns this subsystem.  The SDK tables remain separate capabilities, but their
// publication, refresh ordering, generation boundary, and revocation are one adapter operation.
// The concrete entity/player/transform/IL2CPP implementations are deliberately private to the
// implementation and never appear in the public adapter header.
struct ServiceEndpoint;
// Allocates opaque identities only; all live generations and caches belong to State.
std::atomic<std::uint64_t> g_adapter_generation_sequence{1};

class UnityAdapterServices final : public std::enable_shared_from_this<UnityAdapterServices> {
public:
    explicit UnityAdapterServices(std::weak_ptr<void> state, std::filesystem::path profile_path)
        : state_lifetime_(std::move(state)), profile_path_(std::move(profile_path)) {}
    ~UnityAdapterServices();
    UnityAdapterServices(const UnityAdapterServices&) = delete;
    UnityAdapterServices& operator=(const UnityAdapterServices&) = delete;

    [[nodiscard]] bool Start();
    [[nodiscard]] bool Stop(
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max()) noexcept;
    [[nodiscard]] bool Close(std::chrono::steady_clock::time_point deadline) noexcept;
    void Tick() noexcept;

    [[nodiscard]] bool Started() const noexcept { return started_; }
    [[nodiscard]] std::uint64_t Generation() const noexcept { return generation_; }
    std::uint64_t AdvanceGeneration() noexcept {
        const auto generation = g_adapter_generation_sequence.fetch_add(1, std::memory_order_relaxed);
        generation_.store(generation, std::memory_order_release);
        return generation;
    }
    [[nodiscard]] std::uint64_t RefreshFailures() const noexcept { return refresh_failures_; }
    void InvalidateScene() noexcept;

    // Component state is allocated by the adapter owner and is destroyed with that owner.  The
    // internal components define their concrete state types; they do not
    // own process-lifetime singleton state.
    template <typename T>
    T& Component(UnityAdapterComponent component) {
        static_assert(std::is_default_constructible_v<T>);
        const auto index = static_cast<std::size_t>(component);
        std::scoped_lock lock(component_mutex_);
        auto& slot = components_[index];
        if (!slot) slot = std::make_shared<T>();
        return *static_cast<T*>(slot.get());
    }

    void ResetComponents() noexcept;
    static UnityAdapterServices* Current() noexcept;
    [[nodiscard]] std::shared_ptr<ServiceEndpoint> Endpoint() const noexcept {
        return endpoint_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool InCall() const noexcept { return Current() == this; }
    [[nodiscard]] std::shared_ptr<void> StateOwner() const noexcept { return state_lifetime_.lock(); }

    // The build-profile document the adapter was configured with.
    //
    // Kept HERE rather than fetched from the adapter, because the entity walk needs one address
    // out of that same per-build file and the adapter's own state type is private to this
    // translation unit.  The services object is created by the adapter and lives as long as it
    // does, so the reference is stable for the life of the walk.
    [[nodiscard]] const std::filesystem::path& ProfilePath() const noexcept {
        return profile_path_;
    }

private:
    std::weak_ptr<void> state_lifetime_;
    std::filesystem::path profile_path_;
    std::atomic<std::shared_ptr<ServiceEndpoint>> endpoint_;
    std::atomic<bool> started_{};
    std::atomic<std::uint64_t> generation_{};
    std::atomic<std::uint64_t> refresh_failures_{};
    std::uint64_t tick_sequence_{};
    std::mutex component_mutex_;
    std::array<std::shared_ptr<void>,
               static_cast<std::size_t>(UnityAdapterComponent::Count)> components_{};
};


}  // namespace (adapter ownership)

struct UnityAdapter::State final {
    struct CallbackEndpoint;
    ~State();

    // Serializes publication of the configured callback against Clear so a setter that overlaps
    // Clear cannot repopulate it after Clear detached it.
    std::timed_mutex lifecycle_mutex;
    std::atomic<bool> stopping{false};

    // -- lifecycle gates -----------------------------------------------------
    std::atomic<bool> started{false};
    // Bumped by every successful `Start` and by every `Stop` that found the adapter started.  A
    // tick records it on entry and re-checks it before delivering, so a frame that spans a
    // restart is dropped rather than delivered into the wrong generation.
    std::atomic<std::uint64_t> lifecycle_epoch{0};
    // The thread the first accepted frame arrived on; 0 until then.
    std::atomic<DWORD> game_thread_id{0};
    std::atomic<std::uint64_t> tick_sequence{0};
    std::atomic<std::uint64_t> rejected_thread_ticks{0};
    std::atomic<double> last_delta_seconds{0.0};

    // -- frame clock ---------------------------------------------------------
    // Owned here rather than by the host: resolving the game's dispatcher is a fact about the
    // game, and the host should not have to know that this game has one.
    std::unique_ptr<DispatchTickHook> dispatch_hook;
    bool dispatch_hook_active{false};

    // The install-retry thread.  See `Start` for why the clock has to be retried at all, and
    // `frame_clock_attempts` for the count that makes a silent failure impossible to miss.
    // Joined and cleared before `Start` returns, so a caller never observes a half-open retry.
    std::thread frame_clock_thread;
    std::atomic<std::uint32_t> frame_clock_attempts{0};
    // How long the install waited, in total, and whether it eventually succeeded.  Reported by
    // the bridge's log line so "the clock installed on attempt 37 after 3.6 s" is visible.
    std::atomic<std::uint64_t> frame_clock_elapsed_ms{0};
    std::atomic<bool> frame_clock_cancelled{false};
    std::shared_ptr<UnityAdapterServices> services;

    std::filesystem::path profile_path;
    std::string profile_path_text;  // stable storage for the const-ref accessor
    std::string build_id;
    std::string resolution_how;
    std::uintptr_t dispatch_address{};
    std::string error;

    // The plugins this adapter's frames drive.  Shared, because the host owns the manager and
    // the adapter borrows it only for the call.
    std::shared_ptr<PluginManager> plugins;
    // Serializes the plugin pump against plugin-window drawing.  Held only for the call.
    std::shared_ptr<std::mutex> plugin_mutex;

    std::mutex filter_mutex;
    FrameFilter frame_filter;

    std::atomic<std::shared_ptr<const TickCallback>> configured_tick_callback;
    std::atomic<std::shared_ptr<CallbackEndpoint>> callback_endpoint;
    // Kept across bounded Stop failures; detaching must not forget an in-flight tick.
    std::shared_ptr<CallbackEndpoint> draining_endpoint;

    void DeliverFrame(double delta_seconds,
                      const std::shared_ptr<CallbackEndpoint>& endpoint) noexcept;
};


}  // namespace cabbird


namespace cabbird {
namespace {

ThreadLocalScalar<const void*> g_active_tick_callback_state;

// WaitOnAddress/WakeByAddressAll are resolved dynamically rather than imported: the adapter is
// also loaded into the manual-mapped image, where the import table is not populated by a loader.
class AddressWaitApi final {
public:
    using WaitOnAddressFunction = BOOL(WINAPI*)(
        volatile VOID*, PVOID, SIZE_T, DWORD);
    using WakeByAddressAllFunction = VOID(WINAPI*)(PVOID);

    [[nodiscard]] static const AddressWaitApi& Instance() noexcept {
        static const AddressWaitApi api;
        return api;
    }

    [[nodiscard]] WaitOnAddressFunction Wait() const noexcept {
        return wait_;
    }

    [[nodiscard]] WakeByAddressAllFunction WakeAll() const noexcept {
        return wake_all_;
    }

private:
    AddressWaitApi() noexcept {
        HMODULE module = GetModuleHandleW(L"kernelbase.dll");
        if (module == nullptr) module = GetModuleHandleW(L"kernel32.dll");
        if (module == nullptr) return;
        wait_ = reinterpret_cast<WaitOnAddressFunction>(
            GetProcAddress(module, "WaitOnAddress"));
        wake_all_ = reinterpret_cast<WakeByAddressAllFunction>(
            GetProcAddress(module, "WakeByAddressAll"));
    }

    WaitOnAddressFunction wait_{};
    WakeByAddressAllFunction wake_all_{};
};

class AdmissionGate final {
public:
    [[nodiscard]] bool TryEnter() noexcept {
        std::uint64_t observed = Load();
        for (;;) {
            if ((observed & kClosedBit) != 0 ||
                (observed & kActiveMask) == kActiveMask) {
                return false;
            }
            const std::uint64_t desired = observed + 1U;
            const auto previous = static_cast<std::uint64_t>(InterlockedCompareExchange64(
                &value_, static_cast<LONG64>(desired), static_cast<LONG64>(observed)));
            if (previous == observed) return true;
            observed = previous;
        }
    }

    void Close() noexcept {
        static_cast<void>(InterlockedOr64(&value_, static_cast<LONG64>(kClosedBit)));
        WakeAll(&value_);
    }

    void Leave() noexcept {
        const auto remaining =
            static_cast<std::uint64_t>(InterlockedDecrement64(&value_)) & kActiveMask;
        if (remaining == 0) {
            WakeAll(&value_);
        }
    }

    [[nodiscard]] bool IsDrained() const noexcept {
        return (Load() & kActiveMask) == 0;
    }

    [[nodiscard]] bool DrainUntil(
        std::chrono::steady_clock::time_point deadline) noexcept {
        for (;;) {
            const std::uint64_t observed = Load();
            if ((observed & kActiveMask) == 0) return true;
            if (deadline != std::chrono::steady_clock::time_point::max() &&
                std::chrono::steady_clock::now() >= deadline) {
                return false;
            }

            const DWORD timeout = RemainingMilliseconds(deadline);
            LONG64 expected = static_cast<LONG64>(observed);
            Wait(&value_, expected, timeout);
        }
    }

private:
    static void Wait(volatile LONG64* address, LONG64 expected, DWORD timeout) noexcept {
        if (const auto wait = AddressWaitApi::Instance().Wait()) {
            static_cast<void>(wait(address, &expected, sizeof(expected), timeout));
            return;
        }
        if (timeout != 0) {
            Sleep(timeout == INFINITE
                ? static_cast<DWORD>(1)
                : (std::min)(timeout, static_cast<DWORD>(1)));
        }
    }

    static void WakeAll(volatile LONG64* address) noexcept {
        if (const auto wake = AddressWaitApi::Instance().WakeAll()) {
            wake(const_cast<void*>(static_cast<const volatile void*>(address)));
        }
    }

    [[nodiscard]] std::uint64_t Load() const noexcept {
        return static_cast<std::uint64_t>(InterlockedCompareExchange64(
            const_cast<volatile LONG64*>(&value_), 0, 0));
    }

    [[nodiscard]] static DWORD RemainingMilliseconds(
        std::chrono::steady_clock::time_point deadline) noexcept {
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            return INFINITE;
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        if (std::chrono::duration_cast<std::chrono::steady_clock::duration>(milliseconds) <
            remaining) {
            ++milliseconds;
        }
        if (milliseconds <= std::chrono::milliseconds::zero()) return 0;
        return static_cast<DWORD>((std::min)(
            milliseconds.count(), static_cast<std::int64_t>(INFINITE - 1U)));
    }

    static constexpr std::uint64_t kClosedBit = std::uint64_t{1} << 63U;
    static constexpr std::uint64_t kActiveMask = ~kClosedBit;
    volatile LONG64 value_{};
};

// One immutable table bundle per publication generation. The registry retains this
// small endpoint after revocation; it never retains mutable component state.
struct ServiceEndpoint final : std::enable_shared_from_this<ServiceEndpoint> {
    explicit ServiceEndpoint(std::weak_ptr<UnityAdapterServices> value) : owner(std::move(value)) {}
    std::weak_ptr<UnityAdapterServices> owner;
    AdmissionGate calls;
    std::atomic<bool> ready{};
    std::uint64_t generation{};
    CabbirdUnityDumpServiceV1 dump{};
    CabbirdUnityOverlayServiceV1 overlay{};
    CabbirdUnityTransformServiceV1 transform{};
    CabbirdUnityEntitiesServiceV1 entities{};
    CabbirdUnityPlayerServiceV1 player{};
    CabbirdUnityPlayerTeleportServiceV1 teleport{};
    CabbirdIl2CppServiceV1 il2cpp{};

    void InitializeTables();
    bool Publish();
    bool RevokeUntil(std::chrono::steady_clock::time_point deadline) noexcept;
};

// Process entry discovery is not state ownership. Cached SDK callers use their own
// endpoint instead; a new publication can never redirect an old callback.
std::atomic<std::shared_ptr<ServiceEndpoint>> g_service_endpoint;
ThreadLocalScalar<UnityAdapterServices*> g_service_call_owner;

class ServiceOwnerScope final {
public:
    explicit ServiceOwnerScope(UnityAdapterServices* owner) noexcept
        : previous_(g_service_call_owner.Get()) { g_service_call_owner.Set(owner); }
    ~ServiceOwnerScope() { g_service_call_owner.Set(previous_); }
    ServiceOwnerScope(const ServiceOwnerScope&) = delete;
    ServiceOwnerScope& operator=(const ServiceOwnerScope&) = delete;
private:
    UnityAdapterServices* previous_;
};

class ServiceLease final {
public:
    ServiceLease() = default;
    explicit ServiceLease(std::shared_ptr<ServiceEndpoint> endpoint) noexcept {
        if (!endpoint || !endpoint->ready.load(std::memory_order_acquire) ||
            !endpoint->calls.TryEnter()) return;
        auto owner = endpoint->owner.lock();
        if (!owner) { endpoint->calls.Leave(); return; }
        auto state = owner->StateOwner();
        if (!state) { endpoint->calls.Leave(); return; }
        endpoint_ = std::move(endpoint);
        owner_ = std::move(owner);
        state_ = std::move(state);
    }
    ServiceLease(ServiceLease&& other) noexcept
        : endpoint_(std::move(other.endpoint_)), owner_(std::move(other.owner_)),
          state_(std::move(other.state_)) {}
    ServiceLease(const ServiceLease&) = delete;
    ServiceLease& operator=(const ServiceLease&) = delete;
    ~ServiceLease() { if (endpoint_) endpoint_->calls.Leave(); }
    explicit operator bool() const noexcept { return owner_ != nullptr; }
    UnityAdapterServices* Owner() const noexcept { return owner_.get(); }
private:
    std::shared_ptr<ServiceEndpoint> endpoint_;
    std::shared_ptr<UnityAdapterServices> owner_;
    std::shared_ptr<void> state_;
};

class ServiceCall final {
public:
    // Internal helpers inherit an already admitted owner. Host entry points acquire
    // the current published endpoint; absence is an ordinary unavailable result.
    ServiceCall() noexcept
        : lease_(UnityAdapterServices::Current() ? nullptr :
                 g_service_endpoint.load(std::memory_order_acquire)),
          owner_(UnityAdapterServices::Current() ? UnityAdapterServices::Current() : lease_.Owner()),
          scope_(owner_) {}
    // SDK entries must always validate the explicit endpoint, even during a nested call.
    explicit ServiceCall(ServiceEndpoint* endpoint) noexcept
        : lease_(endpoint ? endpoint->shared_from_this() : nullptr),
          owner_(lease_.Owner()), scope_(owner_) {}
    explicit operator bool() const noexcept { return owner_ != nullptr; }
private:
    ServiceLease lease_;
    UnityAdapterServices* owner_{};
    ServiceOwnerScope scope_;
};

template <typename Result>
Result ServiceUnavailable() noexcept {
    if constexpr (std::is_same_v<Result, CabbirdStatusV1>)
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    else return Result{};
}

template <auto Function, typename... Args>
auto CABBIRD_CALL ServiceEntry(void* user, Args... args) noexcept
    -> decltype(Function(user, args...)) {
    using Result = decltype(Function(user, args...));
    ServiceCall call(static_cast<ServiceEndpoint*>(user));
    if (!call) return ServiceUnavailable<Result>();
    try { return Function(nullptr, args...); }
    catch (...) {
        if constexpr (std::is_same_v<Result, CabbirdStatusV1>)
            return {CABBIRD_STATUS_V1_FAILED, 0, {}};
        else return Result{};
    }
}

// Borrowed output text is a per-thread presentation copy, not a component cache.
// Its lifetime does not end when an owner stops or a later generation replaces state.
ThreadLocalObject<std::array<std::string, 32>> g_borrowed_service_text;
const char* BorrowedServiceText(std::size_t slot, std::string_view value) {
    auto& text = g_borrowed_service_text.Get()[slot];
    text.assign(value);
    return text.c_str();
}

class RetiredTickCallbackQueue final {
public:
    RetiredTickCallbackQueue() {
        std::thread([this] { Run(); }).detach();
    }

    void Retire(const UnityAdapter::TickCallback* callback) noexcept {
        if (callback == nullptr) return;
        try {
            {
                std::scoped_lock lock(mutex_);
                callbacks_.push_back(callback);
            }
            ready_.notify_one();
        } catch (...) {
            // A callback target can own arbitrary code. Preserve it rather
            // than running that destructor on a bounded lifecycle path.
        }
    }

private:
    void Run() noexcept {
        for (;;) {
            const UnityAdapter::TickCallback* callback{};
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return !callbacks_.empty(); });
                callback = callbacks_.front();
                callbacks_.pop_front();
            }
            delete callback;
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<const UnityAdapter::TickCallback*> callbacks_;
};

RetiredTickCallbackQueue* ProcessRetiredTickCallbacks() noexcept {
    // This queue intentionally outlives Runtime teardown: a user callback
    // destructor may block or reenter after the bounded Adapter stop path.
    static auto* queue = []() noexcept -> RetiredTickCallbackQueue* {
        try {
            return new RetiredTickCallbackQueue();
        } catch (...) {
            return nullptr;
        }
    }();
    return queue;
}

void RetireTickCallback(const UnityAdapter::TickCallback* callback) noexcept {
    if (auto* queue = ProcessRetiredTickCallbacks()) {
        queue->Retire(callback);
    }
    // If the process queue cannot be initialized, intentionally retain the
    // callback object rather than destroying arbitrary code during Stop.
}

std::shared_ptr<const UnityAdapter::TickCallback> MakeTickCallback(
    UnityAdapter::TickCallback callback) {
    if (!callback) return {};
    return std::shared_ptr<const UnityAdapter::TickCallback>(
        new UnityAdapter::TickCallback(std::move(callback)), RetireTickCallback);
}

template <typename Mutex>
[[nodiscard]] bool LockUntil(
    std::unique_lock<Mutex>& lock,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        lock.lock();
        return true;
    }
    return lock.try_lock_until(deadline);
}

}  // namespace

// The callback endpoint shares the adapter State defined above.
struct UnityAdapter::State::CallbackEndpoint final {
    class CallLease final {
    public:
        CallLease() = default;
        CallLease(
            CallbackEndpoint* endpoint,
            std::shared_ptr<const TickCallback> callback) noexcept
            : endpoint_(endpoint), callback_(std::move(callback)) {}
        CallLease(const CallLease&) = delete;
        CallLease& operator=(const CallLease&) = delete;
        CallLease(CallLease&& other) noexcept
            : endpoint_(std::exchange(other.endpoint_, nullptr)),
              callback_(std::move(other.callback_)) {}
        CallLease& operator=(CallLease&& other) noexcept {
            if (this == &other) return *this;
            Release();
            endpoint_ = std::exchange(other.endpoint_, nullptr);
            callback_ = std::move(other.callback_);
            return *this;
        }
        ~CallLease() { Release(); }

        [[nodiscard]] explicit operator bool() const noexcept {
            return endpoint_ != nullptr;
        }

        void Invoke(double delta_seconds) const {
            if (callback_) (*callback_)(delta_seconds);
        }

    private:
        void Release() noexcept {
            if (endpoint_ == nullptr) return;
            callback_.reset();
            endpoint_->ReleaseCall();
            endpoint_ = nullptr;
        }

        CallbackEndpoint* endpoint_{};
        std::shared_ptr<const TickCallback> callback_;
    };

    explicit CallbackEndpoint(std::shared_ptr<const TickCallback> callback) noexcept
        : callback_(std::move(callback)) {}

    [[nodiscard]] CallLease Acquire() noexcept {
        if (!gate_.TryEnter()) return {};
        auto callback = callback_.load(std::memory_order_acquire);
        return CallLease(this, std::move(callback));
    }

    void Close() noexcept {
        gate_.Close();
    }

    [[nodiscard]] std::shared_ptr<const TickCallback> Clear() noexcept {
        return callback_.exchange({}, std::memory_order_acq_rel);
    }

    [[nodiscard]] std::shared_ptr<const TickCallback> Set(
        std::shared_ptr<const TickCallback> callback) noexcept {
        return callback_.exchange(std::move(callback), std::memory_order_acq_rel);
    }

    [[nodiscard]] bool IsDrained() const noexcept {
        return gate_.IsDrained();
    }

    [[nodiscard]] bool DrainUntil(
        std::chrono::steady_clock::time_point deadline) noexcept {
        return gate_.DrainUntil(deadline);
    }

private:
    void ReleaseCall() noexcept {
        gate_.Leave();
    }

    AdmissionGate gate_;
    std::atomic<std::shared_ptr<const TickCallback>> callback_;
};

void UnityAdapter::SetTickCallback(TickCallback callback) {
    const auto state = state_;
    std::shared_ptr<const TickCallback> replacement = MakeTickCallback(std::move(callback));
    std::shared_ptr<const TickCallback> retired_configured;
    std::shared_ptr<const TickCallback> retired_endpoint;
    {
        // Serialize publication with Start/Stop/Clear so a setter that overlaps
        // Stop cannot repopulate the configured callback after Stop detached it.
        std::unique_lock<std::timed_mutex> lifecycle_lock(state->lifecycle_mutex);
        if (state->stopping.load(std::memory_order_acquire)) return;
        retired_configured = state->configured_tick_callback.exchange(
            replacement, std::memory_order_acq_rel);
        const auto endpoint = state->callback_endpoint.load(std::memory_order_acquire);
        if (endpoint) {
            retired_endpoint = endpoint->Set(std::move(replacement));
        }
    }
}

// Every frame source enters the same generation-bound gate before filters, internal
// refreshes or plugin callbacks. A clock captures weak owners, never the public adapter.
void UnityAdapter::State::DeliverFrame(
    double delta_seconds, const std::shared_ptr<CallbackEndpoint>& endpoint) noexcept {
    if (!endpoint || !started.load(std::memory_order_acquire) ||
        callback_endpoint.load(std::memory_order_acquire) != endpoint ||
        g_active_tick_callback_state.Get() == this) return;
    auto lease = endpoint->Acquire();
    if (!lease) return;
    const auto previous = g_active_tick_callback_state.Get();
    struct ActiveTick final {
        const void* previous;
        ~ActiveTick() { g_active_tick_callback_state.Set(previous); }
    } active{previous};
    g_active_tick_callback_state.Set(this);
    try {
        const DWORD current = GetCurrentThreadId();
        DWORD expected = 0;
        if (!game_thread_id.compare_exchange_strong(
                expected, current, std::memory_order_acq_rel) && expected != current) {
            rejected_thread_ticks.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        FrameFilter filter;
        {
            std::scoped_lock lock(filter_mutex);
            filter = frame_filter;
        }
        if (filter) {
            try {
                if (!filter()) return;
            } catch (...) {
                std::scoped_lock lock(filter_mutex);
                frame_filter = {};
                return;
            }
        }
        // A filter may have reentered Stop; that closes this generation immediately.
        if (!started.load(std::memory_order_acquire)) return;
        last_delta_seconds.store(delta_seconds, std::memory_order_relaxed);
        tick_sequence.fetch_add(1, std::memory_order_acq_rel);
        if (services) services->Tick();
        if (started.load(std::memory_order_acquire)) lease.Invoke(delta_seconds);
    } catch (...) {
        // Keep C++ faults inside the adapter; the lease drains even on failure.
    }
}

void UnityAdapter::OnGameTick(double delta_seconds) noexcept {
    const auto state = state_;
    state->DeliverFrame(delta_seconds,
                        state->callback_endpoint.load(std::memory_order_acquire));
}

bool UnityAdapter::ClearTickCallback(std::chrono::milliseconds timeout) noexcept {
    const auto state = state_;
    const bool called_by_active_tick_callback =
        g_active_tick_callback_state.Get() == state.get();
    const auto bounded_timeout =
        (std::max)(timeout, std::chrono::milliseconds::zero());
    const auto deadline = bounded_timeout == std::chrono::milliseconds::max()
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + bounded_timeout;
    std::unique_lock<std::timed_mutex> lifecycle_lock(state->lifecycle_mutex, std::defer_lock);
    const bool lifecycle_locked = called_by_active_tick_callback
        ? lifecycle_lock.try_lock()
        : LockUntil(lifecycle_lock, deadline);
    if (!lifecycle_locked) return false;
    const auto endpoint = state->callback_endpoint.load(std::memory_order_acquire);
    auto retired_endpoint_callback = endpoint ? endpoint->Clear() : nullptr;
    auto retired_configured_callback = state->configured_tick_callback.exchange(
        {}, std::memory_order_acq_rel);
    lifecycle_lock.unlock();
    if (called_by_active_tick_callback) return false;
    return !endpoint || endpoint->DrainUntil(deadline);
}

// -- accessors ------------------------------------------------------------------------------
//
bool UnityAdapter::Started() const noexcept {
    return state_->started.load(std::memory_order_acquire);
}

std::uint64_t UnityAdapter::TickSequence() const noexcept {
    return state_->tick_sequence.load(std::memory_order_acquire);
}

std::uint64_t UnityAdapter::RejectedThreadTicks() const noexcept {
    return state_->rejected_thread_ticks.load(std::memory_order_acquire);
}

DWORD UnityAdapter::GameThreadId() const noexcept {
    return state_->game_thread_id.load(std::memory_order_acquire);
}

double UnityAdapter::LastDeltaSeconds() const noexcept {
    return state_->last_delta_seconds.load(std::memory_order_relaxed);
}

const std::string& UnityAdapter::BuildId() const noexcept { return state_->build_id; }

const std::string& UnityAdapter::ResolutionHow() const noexcept { return state_->resolution_how; }

std::uintptr_t UnityAdapter::DispatchAddress() const noexcept { return state_->dispatch_address; }

const std::string& UnityAdapter::FrameClockProfilePath() const noexcept {
    return state_->profile_path_text;
}

const std::string& UnityAdapter::LastError() const noexcept { return state_->error; }

// -- lifecycle: the frame clock -----------------------------------------------------------------
//
// Lifecycle methods reach into
// `State::CallbackEndpoint` to detach and drain, and this is the translation unit that DEFINES
// that type. Constructors, accessors and components share this implementation.

bool UnityAdapter::Start(TickCallback update) {
    return Start(std::move(update), std::chrono::milliseconds::zero());
}

bool UnityAdapter::Start(TickCallback update, std::chrono::milliseconds frame_clock_budget) {
    return Start(std::move(update), frame_clock_budget, FrameClockFactory{});
}

bool UnityAdapter::Start(TickCallback update, std::chrono::milliseconds frame_clock_budget,
                         const FrameClockFactory& clock_factory, std::stop_token stop_token) {
    const auto state = state_;
    std::unique_lock<std::timed_mutex> lifecycle_lock(state->lifecycle_mutex);
    if (state->started.load(std::memory_order_acquire)) return false;
    if (state->stopping.load(std::memory_order_acquire)) return false;
    if (stop_token.stop_requested()) return false;
    const bool services_were_started = state->services->Started();
    if (!services_were_started && !state->services->Start()) return false;

    if (update) {
        state->configured_tick_callback.store(
            MakeTickCallback(std::move(update)), std::memory_order_release);
    }

    // The delivery endpoint exists BEFORE the clock is installed, so a caller that supplies its
    // own clock factory can exercise the whole delivery path offline, and so an installation
    // failure below cannot leave a live clock with no endpoint to deliver into.
    state->callback_endpoint.store(
        std::make_shared<State::CallbackEndpoint>(
            state->configured_tick_callback.load(std::memory_order_acquire)),
        std::memory_order_release);

    // Bind this clock to this exact endpoint. A saved callback from an earlier
    // clock cannot deliver into a later Start, or dereference a destroyed adapter.
    std::function<void(double)> frame_callback = [
        weak_state = std::weak_ptr<State>(state),
        weak_endpoint = std::weak_ptr<State::CallbackEndpoint>(
            state->callback_endpoint.load(std::memory_order_acquire))](double delta) noexcept {
        const auto owner = weak_state.lock();
        const auto endpoint = weak_endpoint.lock();
        if (owner && endpoint) owner->DeliverFrame(delta, endpoint);
    };

    // Build the clock.  The factory lets a test supply a stub; production uses the real
    // `DispatchTickHook`.
    const auto build_clock = [&]() -> std::unique_ptr<DispatchTickHook> {
        if (clock_factory) return clock_factory(state->profile_path, frame_callback);
        return std::make_unique<DispatchTickHook>(state->profile_path, frame_callback);
    };

    // THE INSTALL IS RETRIED, AND THAT IS THE WHOLE POINT.
    //
    // The host starts the render domain as soon as the proxy DLL runs, which is before Unity has
    // loaded `GameAssembly.dll`.  A single attempt therefore fails with
    // `metadata unavailable (il2cpp runtime not attached)` / `profile: module not loaded`, and
    // the resolution was never wrong -- it was simply asked too early and then never asked again.
    // The profile's prologue does match once the game is up.
    //
    // A clock that can only be installed before the game exists is a clock that never installs,
    // so the install waits for `frame_clock_budget`.  Attempts are 100 ms apart because the two
    // things being waited for (the module load and the IL2CPP runtime attaching) are one-time
    // events on the order of seconds, not milliseconds.
    const auto deadline = frame_clock_budget > std::chrono::milliseconds::zero()
        ? std::chrono::steady_clock::now() + frame_clock_budget
        : std::chrono::steady_clock::now();
    constexpr auto kRetryInterval = std::chrono::milliseconds(100);

    std::unique_ptr<DispatchTickHook> hook;
    std::uint32_t attempts = 0;
    const auto install_started = std::chrono::steady_clock::now();
    for (;;) {
        if (stop_token.stop_requested()) {
            state->error = "frame clock startup cancelled";
            break;
        }
        ++attempts;
        hook = build_clock();
        if (hook != nullptr && hook->Start()) {
            if (stop_token.stop_requested()) {
                hook->Stop();
                hook.reset();
                state->error = "frame clock startup cancelled";
            }
            break;
        }

        // The last failure's reason is kept, because it is what the host logs when the whole
        // budget is spent -- "cannot resolve: metadata: ...; profile: module not loaded" is
        // exactly the text that identified this defect.
        if (hook != nullptr) state->error = hook->LastError();
        hook.reset();

        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(kRetryInterval);
        // A `Stop` that arrived while waiting must not be overtaken by a clock installed after it.
        if (state->stopping.load(std::memory_order_acquire)) break;
    }
    state->frame_clock_attempts.store(attempts, std::memory_order_release);
    state->frame_clock_elapsed_ms.store(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - install_started).count()),
        std::memory_order_release);

    if (hook == nullptr) {
        state->callback_endpoint.store({}, std::memory_order_release);
        if (!services_were_started) {
            static_cast<void>(state->services->Stop());
        }
        if (state->error.empty()) state->error = "frame clock factory returned no clock";
        return false;
    }

    state->build_id = hook->BuildId();
    state->resolution_how = hook->ResolutionHow();
    state->dispatch_address = hook->ResolutionAddress();
    state->dispatch_hook = std::move(hook);
    state->dispatch_hook_active = true;

    state->game_thread_id.store(0, std::memory_order_release);
    state->tick_sequence.store(0, std::memory_order_release);
    state->rejected_thread_ticks.store(0, std::memory_order_release);
    state->last_delta_seconds.store(0.0, std::memory_order_relaxed);
    state->stopping.store(false, std::memory_order_release);
    state->error.clear();

    // Epoch bump AFTER the clock is live and the counters are reset, so a tick arriving
    // immediately is already inside the new generation.
    state->lifecycle_epoch.fetch_add(1, std::memory_order_acq_rel);

    state->started.store(true, std::memory_order_release);
    return true;
}

bool UnityAdapter::Stop(std::chrono::milliseconds timeout) noexcept {
    const auto state = state_;
    const auto bounded = (std::max)(timeout, std::chrono::milliseconds::zero());
    const auto deadline = bounded == std::chrono::milliseconds::max()
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + bounded;

    // A tick that stops its own adapter must not wait behind another stopper that is already
    // draining that same tick; `g_active_tick_callback_state` is how the ported code detected
    // this, and it is set by `OnGameTick` above.
    const bool called_by_active_tick = g_active_tick_callback_state.Get() == state.get() ||
        state->services->InCall();

    std::unique_lock<std::timed_mutex> lifecycle_lock(state->lifecycle_mutex, std::defer_lock);
    if (called_by_active_tick) {
        if (!lifecycle_lock.try_lock()) return false;
    } else if (!LockUntil(lifecycle_lock, deadline)) {
        return false;
    }

    const bool was_started = state->started.exchange(false, std::memory_order_acq_rel);
    state->stopping.store(true, std::memory_order_release);

    // Detach and close the endpoint BEFORE waiting for in-flight ticks.  This ordering is the
    // contract: it is what makes "no later tick will enter the callback" true while the drain
    // below is still running.
    auto detached = state->callback_endpoint.exchange({}, std::memory_order_acq_rel);
    if (detached) {
        detached->Close();
        static_cast<void>(detached->Clear());
        state->draining_endpoint = std::move(detached);
    }
    if (was_started) state->lifecycle_epoch.fetch_add(1, std::memory_order_acq_rel);

    // A reentrant Stop closes admission but cannot wait for its own lease or
    // remove the detour from inside its callback. An external Stop finishes it.
    const bool revoked = state->services->Close(deadline);
    if (called_by_active_tick) return false;
    if (state->dispatch_hook_active) {
        state->dispatch_hook->Stop();
        state->dispatch_hook_active = false;
    }
    if (state->draining_endpoint && !state->draining_endpoint->DrainUntil(deadline)) {
        return false;
    }
    // Components are not revoked/reset while an accepted tick still uses them.
    if (!revoked || !state->services->Stop(deadline)) return false;
    state->draining_endpoint.reset();
    state->dispatch_hook.reset();

    state->stopping.store(false, std::memory_order_release);
    return true;
}

}  // namespace cabbird


/* ---- UnityAdapter lifecycle and owned subsystem definitions ---- */


namespace cabbird {

UnityAdapter::State::~State() {
    // The last accepted tick/SDK/worker lease has released State. This also covers
    // destruction of the public adapter from inside its own callback, where Stop
    // could only close admission and could not remove the active clock yet.
    if (dispatch_hook) dispatch_hook->Stop();
    if (services) static_cast<void>(services->Stop(std::chrono::steady_clock::now()));
}

UnityAdapter::UnityAdapter(std::filesystem::path frame_clock_profile,
                           std::shared_ptr<PluginManager> plugins,
                           UnitySnapshotSamplingOptions snapshot_sampling)
    : state_(std::make_shared<State>()) {
    state_->profile_path = std::move(frame_clock_profile);
    // Captured as text at construction so the accessor can return a reference to stable storage
    // rather than build a string per call.
    state_->profile_path_text = state_->profile_path.string();
    state_->plugins = std::move(plugins);
    // One mutex per manager, created here so that both the game-domain pump and the
    // render-domain pump serialize against the same lock object for this adapter's lifetime.
    state_->plugin_mutex = std::make_shared<std::mutex>();
    state_->services = std::make_shared<UnityAdapterServices>(state_, state_->profile_path);
    // Applied at construction rather than re-read per tick: the walk's cost is a game-thread
    // property, and a divisor that could change under a running walk is a new way for the
    // sampling to be neither the configured value nor 1.  The setter clamps 0 to 1, because a
    // divisor of zero is a division by zero at the gate.
    SetUnityEntityRefreshDivisor(snapshot_sampling.entity_tick_interval);
    SetUnityEntityDirectPosition(snapshot_sampling.entity_direct_position);
}

UnityAdapter::~UnityAdapter() {
    // Zero timeout: the destructor must not block.  Everything a callback may still be touching
    // is shared-owned, so the drain is what `Stop` is for and the caller is expected to have run
    // it; this only guarantees the detour is gone.
    Stop(std::chrono::milliseconds::zero());
}

void UnityAdapter::SetFrameFilter(FrameFilter filter) {
    const auto state = state_;
    std::scoped_lock lock(state->filter_mutex);
    state->frame_filter = std::move(filter);
}

bool UnityAdapter::StartServices() {
    const auto state = state_;
    std::scoped_lock lock(state->lifecycle_mutex);
    if (state->stopping.load(std::memory_order_acquire)) return false;
    if (state->services->Started()) return true;
    return state->services->Start();
}

bool UnityAdapter::ServicesStarted() const noexcept {
    return state_ != nullptr && state_->services != nullptr && state_->services->Started();
}

std::uint64_t UnityAdapter::ServiceGeneration() const noexcept {
    return state_ != nullptr && state_->services != nullptr
        ? state_->services->Generation() : 0;
}

std::uint64_t UnityAdapter::ServiceRefreshFailures() const noexcept {
    return state_->services ? state_->services->RefreshFailures() : 0;
}

void UnityAdapter::InvalidateScene() noexcept {
    if (state_ != nullptr && state_->services != nullptr) state_->services->InvalidateScene();
}

}  // namespace cabbird


/* ---- UnityAdapter service composition ---- */

// UnityAdapter-owned service composition.
//
// All Unity engine-facing components live in this translation unit and are owned by the
// adapter State. The SDK tables remain separate capabilities, but publication, refresh order,
// generation, invalidation, and revocation are one adapter operation.


namespace cabbird {
namespace {

void RefreshAdapterServices() {
    RefreshUnityEntityEsp();
    RefreshUnityPlayer();
    RefreshUnityPlayerTeleport();
    ObserveUnityIl2CppGameThread();
    ProcessUnityIl2CppDiagnostics();
}

}  // namespace

bool UnityAdapterServices::Start() {
    if (started_ || Endpoint()) return false;
    auto endpoint = std::make_shared<ServiceEndpoint>(weak_from_this());
    endpoint->InitializeTables();
    std::shared_ptr<ServiceEndpoint> empty;
    if (!g_service_endpoint.compare_exchange_strong(empty, endpoint)) return false;
    endpoint_.store(endpoint, std::memory_order_release);
    ServiceOwnerScope scope(this);
    try {
        InitializeUnityIl2CppState();
        SetOverlayCameraSource(&ProvideEspCameraMatrix, nullptr);
        if (!endpoint->Publish()) {
            ServiceOwnerScope outside(nullptr);
            static_cast<void>(Stop());
            return false;
        }
    } catch (...) {
        ServiceOwnerScope outside(nullptr);
        static_cast<void>(Stop());
        throw;
    }
    endpoint->generation = AdvanceGeneration();
    tick_sequence_ = 0;
    refresh_failures_ = 0;
    started_ = true;
    endpoint->ready.store(true, std::memory_order_release);
    return true;
}

bool UnityAdapterServices::Close(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto endpoint = Endpoint();
    if (!endpoint) return true;
    endpoint->ready.store(false, std::memory_order_release);
    endpoint->calls.Close();
    if (started_.exchange(false, std::memory_order_acq_rel)) AdvanceGeneration();
    ServiceOwnerScope scope(this);
    CancelUnityIl2CppDiagnostics();
    return endpoint->RevokeUntil(deadline);
}

bool UnityAdapterServices::Stop(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto endpoint = Endpoint();
    if (!endpoint) return true;
    const bool reentrant = InCall();
    if (!Close(deadline)) return false;
    if (reentrant || !endpoint->calls.DrainUntil(deadline)) return false;
    ResetComponents();
    endpoint_.store({}, std::memory_order_release);
    auto expected = endpoint;
    static_cast<void>(g_service_endpoint.compare_exchange_strong(expected, {}));
    return true;
}

void UnityAdapterServices::Tick() noexcept {
    ServiceLease lease(Endpoint());
    if (!lease) return;
    ServiceOwnerScope scope(this);
    ++tick_sequence_;
    try { RefreshAdapterServices(); }
    catch (...) { ++refresh_failures_; }
}

void UnityAdapterServices::InvalidateScene() noexcept {
    ServiceLease lease(Endpoint());
    if (!lease) return;
    ServiceOwnerScope scope(this);
    AdvanceGeneration();
    InvalidateUnityEntityState();
    InvalidateUnityPlayerState();
    InvalidateUnityPlayerTeleportState();
    InvalidateUnityTransformState();
}

void UnityAdapterServices::ResetComponents() noexcept {
    std::scoped_lock lock(component_mutex_);
    for (auto& component : components_) component.reset();
}

UnityAdapterServices::~UnityAdapterServices() {
    static_cast<void>(Stop(std::chrono::steady_clock::now()));
}

UnityAdapterServices* UnityAdapterServices::Current() noexcept {
    return g_service_call_owner.Get();
}

}  // namespace cabbird


/* ---- UnityAdapter component: dump ---- */

// The host backend for `cabbird.unity.dump`.
//
// WHAT THIS IS REPLACING
// ----------------------
// The dump capability already existed and was already SEH-guarded, but it had NO CALLER
// AT ALL: the `il2cpp_dump*` keys in cabbird_overlay.ini were read by the manual-map
// probe, which is no longer part of this build.  So the
// project shipped a dumper that nothing could reach.
//
// The obvious shape would be "turn the ini switches back on", but that is the wrong one:
// Cabbird hot-reloads plugins, so
// the dump should be an ACTION a plugin can take in the current session, not a switch
// that has to be set before the game starts.  That matters concretely, not
// aesthetically:
//
//   * the dump has to be taken with the game in the right STATE -- a dump captured at a
//     login screen contains no battle entities;
//   * changing an ini costs a full relaunch of the game, and the launch itself is the
//     expensive, user-owned step;
//   * with hot reload, one session can dump at the menu, walk into a fight, dump
//     again, and diff the two.
//
// SO WHY IS THIS IN src/plugin/ AND NOT IN src/mem/
// -------------------------------------------------
// The dump primitive belongs to src/mem/ and stays there untouched.  What lives here is
// the SERVICE around it: request validation, the worker thread, the result buffer, and
// the file write.  That is plugin-layer work -- it exists so that a plugin can call it --
// and putting it here keeps `il2cpp_dump.cpp`'s job ("walk the metadata") separate from
// "be reachable from a DLL that was loaded five seconds ago".
//
// WHY ASYNCHRONOUS ONLY
// ---------------------
// The service contract offers a blocking form from the game domain and a queued form
// from the render domain (see `unity.h`).  This implementation queues ALWAYS, including
// when it is called from the game thread, and reports PENDING either way.  The reasons
// are concrete:
//
//   1. A render callback is the only place a plugin is guaranteed to be called every
//      frame, and it must not stall Present.  A ~9 MB metadata walk from there is the
//      definition of stalling it.
//   2. `il2cpp_class_get_methods` forces lazy metadata initialisation and allocates
//      managed memory.  Doing that on a thread the runtime does not know about is what
//      produced "Fatal error in GC: Collecting from unknown thread" on an earlier
//      probe.  `DumpTypeSystem` enters a `ThreadScope` for us, and a thread we created
//      ourselves is exactly the case that needs it.  Reusing the game's own thread
//      would make the attachment a no-op and hide whether the fix works.
//
// The cost is that a caller must poll, which `state()` supports and which is safe to do
// every frame.


namespace cabbird {

namespace { namespace unity_adapter_dump_detail {
struct DumpComponentState {
    std::uint64_t last_heartbeat_seen{};
    std::uint64_t last_heartbeat_ticks{};
    std::mutex mutex;
    CabbirdDumpResultV1 result{};
    std::string dump;
    // The recovered plaintext metadata, held here rather than rebuilt per call.
    //
    // 50 MB that costs ~6.6 s to find, so it is copied ONCE -- into the library's own
    // process-wide cache -- and this is a second copy the service hands out chunk by
    // chunk.  A second copy is deliberate and is not the same trade as the library
    // cache: the library must own its bytes because the runtime's mapping can move, and
    // the service must own ITS bytes because it serves them under `mutex` while the
    // producer may already be running the next request.
    //
    // It is not published through `metadata_data` until a request that asked for it has
    // finished, so a reader can never observe a half-filled buffer.
    std::string metadata;
    std::atomic<bool> running{false};
    std::atomic<std::uint32_t> state{CABBIRD_DUMP_V1_RESULT_IDLE};
    std::atomic<std::uint64_t> progress_classes{};
    std::atomic<std::uint64_t> progress_fields{};
    std::atomic<std::uint64_t> progress_images{};
    std::atomic<std::uint64_t> progress_ticks{};
    std::atomic<std::uint64_t> progress_final_ms{};
    std::atomic<std::uint64_t> claim_ticks{};
    std::atomic<std::uint64_t> progress_heartbeat{};
    std::mutex message_mutex;
    char status_message[256]{};
};

DumpComponentState& ComponentState() {
    return UnityAdapterServices::Current()->Component<DumpComponentState>(
        UnityAdapterComponent::Dump);
}
} }  // namespace (adapter internals)
#define g_last_heartbeat_seen (ComponentState().last_heartbeat_seen)
#define g_last_heartbeat_ticks (ComponentState().last_heartbeat_ticks)
#define g_mutex (ComponentState().mutex)
#define g_result (ComponentState().result)
#define g_dump (ComponentState().dump)
#define g_metadata (ComponentState().metadata)
#define g_running (ComponentState().running)
#define g_state (ComponentState().state)
#define g_progress_classes (ComponentState().progress_classes)
#define g_progress_fields (ComponentState().progress_fields)
#define g_progress_images (ComponentState().progress_images)
#define g_progress_ticks (ComponentState().progress_ticks)
#define g_progress_final_ms (ComponentState().progress_final_ms)
#define g_claim_ticks (ComponentState().claim_ticks)
#define g_progress_heartbeat (ComponentState().progress_heartbeat)
#define g_message_mutex (ComponentState().message_mutex)
#define g_status_message (ComponentState().status_message)

// The stall detector's bookkeeping is part of the adapter-owned dump state. Both
// Read and written only from `DumpState`, which runs on the render-domain poll.  Deliberately NOT
// atomics: this is that reader's private state, not a published fact about the walk, and making it
// atomic would suggest another thread may act on it.
namespace { namespace unity_adapter_dump_detail {
} }  // namespace (adapter internals)
namespace { namespace unity_adapter_dump_detail {

// One request is in flight at a time.  Two concurrent walks of the same metadata is
// the kind of thing that produces a crash with no useful evidence, so the second
// caller is rejected rather than queued -- a rejected request is a fact the caller
// can act on, a silently queued one is not.
// Set for the duration of the worker's run.  The state is published BEFORE this is
// cleared, so a caller that sees anything other than PENDING is guaranteed to be
// looking at a finished result rather than at a half-written one.
// The published state.
//
// THIS LINE IS WHY THE WINDOW KEPT SHOWING ZEROES, and it is a one-word defect: the
// initialiser was `UNAVAILABLE`.  Nothing ever moved this variable off UNAVAILABLE except a
// COMPLETED walk, because `DumpRun` set the PENDING state on its OUTPUT struct and never on
// this one.  So every poll that was not "a walk has finished" reported "the dump service is
// unavailable" -- from a service that was perfectly available and, at that very moment,
// walking 23,000 classes.  That contradiction (a live progress counter published alongside
// `state == UNAVAILABLE`) is what the host diagnostic showed for four rounds while I kept
// looking further downstream instead of at the producer.
//
// PENDING (= 0) is the honest default: the enum has no IDLE, and zero already means "no
// result to report yet", which covers both "nothing requested" and "a walk is in flight".
// The other half of the fix is the `g_state` write in `DumpRun` below, which was missing.
// PENDING means "a walk is in flight" and nothing else, so it must not be the value a fresh
// service starts at.
//
// It used to be `CABBIRD_DUMP_V1_RESULT_PENDING`, which is zero -- and `PENDING` being zero meant
// the service reported "a dump is running" to the very first poll, before any request.  A plugin
// polls from its first frame, so the window showed a running timer and "a dump is already
// running" on a service nobody had asked to do anything.  `IDLE` is the honest initial value and
// is now the zero value; see the enum for why a sensor needs one.

// Live progress, so that "it is running" and "it is wedged" stop looking identical.
//
// WHY THIS EXISTS: a completed 30 MB dump could finish while the window still read
// "running..." -- because the window only ever showed the LAST RESULT, and until the worker
// published its final result there was nothing to show but the word "pending".  A user
// watching that has no way to tell a 40-second walk from a thread that died.
//
// These are published WITHOUT the lock, from the worker's hot loop, and they reuse the
// result's own counters rather than adding fields: `class_count` / `field_count` mean
// "visited so far" while a dump runs and "total" when it finishes.  That keeps the ABI
// unchanged -- a new struct would force every plugin to rebuild against a new header, for
// progress reporting that only ever needs to be approximately right.
//
// `g_progress_started` is a steady_clock tick count, used to report elapsed time; elapsed
// is what makes a heartbeat readable, because a counter that advances and a clock that
// advances are two independent witnesses to the worker being alive.
// The LAST walk's total duration in milliseconds, frozen when it finished.
//
// Separate from `g_progress_ticks` because they answer different questions: `ticks` is
// "when did the current/last walk start" (only meaningful while it is running) and this is
// "how long did the last walk take" (only meaningful when it is not).  Publishing both and
// letting `state` choose is what stops a finished dump from showing a growing timer.

// THE LIVENESS WITNESS.
//
// A walk that is accepted but never reports progress left the service reporting PENDING for as
// long as the process lived, because nothing but the worker itself ever published a terminal
// state -- and a worker that died, or that never got past resolving the runtime, published
// nothing.  The window showed "running" forever.
//
// These two counters are what let a READER decide the worker is gone instead of waiting for the
// worker to say so.  `g_progress_heartbeat` is incremented by the walk's own progress callback,
// so it only advances if the walk is genuinely alive; `g_claim_ticks` is when the slot was
// taken.  A claimed walk whose heartbeat has not moved for longer than the stall limit is a
// dead walk, and `DumpState` says so.

// How long a claimed walk may go without a single progress report before the service calls it
// dead.  Generous on purpose: the first report only arrives after the runtime is attached and
// the first image has been enumerated, and a cold start can spend seconds there.  This is a
// liveness bound, not a performance target -- it exists so that "stuck" eventually becomes a
// result instead of an eternal "running".
constexpr std::uint64_t kStallLimitMs = 180000;

std::uint64_t NowTicks() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Every status this service returns is a code and nothing else -- the SDK's message
// view is a borrowed string owned by the host, and inventing text here would mean
// handing a plugin a pointer into this image for a lifetime it cannot reason about.
CabbirdStatusV1 MakeStatus(CabbirdStatusCodeV1 code) noexcept {
    CabbirdStatusV1 status{};
    status.code = static_cast<std::uint32_t>(code);
    return status;
}

CabbirdDumpResultV1 MakeResult() noexcept {
    CabbirdDumpResultV1 result{};
    result.struct_size = sizeof(result);
    result.state = g_state.load(std::memory_order_acquire);
    return result;
}

void PublishRunning() noexcept {
    std::scoped_lock lock(g_mutex);
    g_result = MakeResult();
    g_result.state = CABBIRD_DUMP_V1_RESULT_PENDING;
}

// The walk's own progress callback, handed to the dumper so the numbers come from the
// thread that is actually doing the work.
//
// It takes NO lock and touches only atomics, because it runs inside the hot loop: taking
// `g_mutex` here would put the game thread's polling behind a metadata walk, which is the
// exact stall this whole service is built to avoid.  The consequence is that a reader can
// observe classes and fields from slightly different instants -- accepted deliberately,
// because progress is a liveness signal, not a result.  The authoritative numbers are the
// final ones the walk returns.
void CABBIRD_CALL OnDumpProgress(
    void*, std::size_t images, std::size_t classes, std::size_t fields) noexcept {
    g_progress_images.store(images, std::memory_order_relaxed);
    g_progress_classes.store(classes, std::memory_order_relaxed);
    g_progress_fields.store(fields, std::memory_order_relaxed);
    // The heartbeat, and the reason this callback increments something of its own rather than
    // letting a reader infer liveness from `classes`: a walk over an image with no classes would
    // legitimately report zero while being perfectly alive, so `classes` alone cannot distinguish
    // "no classes yet" from "dead".  See `kStallLimitMs`.
    g_progress_heartbeat.fetch_add(1, std::memory_order_relaxed);
}

// The loaded image's address range, or {0, 0} when it is not loaded.
//
// WHY THIS IS COMPUTED FROM THE PE HEADERS AND NOT ASKED OF THE RUNTIME: the range has one
// consumer -- deciding whether `MethodInfo::methodPointer` is real code -- and that decision
// has to be available before the first `il2cpp_*` call, because it is the VALIDATION of the
// offset those calls are read through.  `SizeOfImage` is section-alignment padded on purpose:
// a pointer into the padding is not a function, but it IS inside the image, and the dumper
// reports both counts so the distinction stays visible rather than being silently rounded
// away here.
//
// The size this yields matches the module table `cabbird-cli modules` prints, so the header
// arithmetic and the loader agree.
bool ImageRangeOf(HMODULE module, std::uintptr_t* base, std::uintptr_t* size) noexcept {
    if (base != nullptr) *base = 0;
    if (size != nullptr) *size = 0;
    if (module == nullptr || base == nullptr || size == nullptr) return false;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

    std::uintptr_t end = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        const std::uintptr_t section_end = static_cast<std::uintptr_t>(section->VirtualAddress) +
                                          section->Misc.VirtualSize;
        if (section_end > end) end = section_end;
    }
    *base = reinterpret_cast<std::uintptr_t>(bytes);
    *size = end;
    return end != 0;
}

// Whether the caller's request struct is big enough to contain the metadata fields.
//
// WHY A SIZE TEST AND NOT A VERSION TEST: this service is append-only.  An out-of-tree plugin
// compiled against the previous header passes its own smaller `sizeof`, and the bytes past its
// copy of the struct are not ours to read -- reading them would turn a supported older caller
// into a crash or, worse, into an unrelated string interpreted as a path.  The same rule the
// dumper's own option structs use, applied at the ABI edge where it actually matters.
bool HasMetadataPath(const CabbirdDumpRequestV1& request) noexcept {
    return request.struct_size >= sizeof(CabbirdDumpRequestV1);
}

// Turn the SDK's flag bits into the dumper's own options.  Kept as a pure function so
// that the mapping -- which is the whole substance of the request contract -- is
// testable without a runtime.
il2cpp::DumpOptions MakeOptions(const CabbirdDumpRequestV1& request,
                                std::uintptr_t module_base,
                                std::uintptr_t module_size) {
    il2cpp::DumpOptions options{};
    options.include_fields =
        (request.flags & CABBIRD_DUMP_V1_FIELDS) != 0u;
    options.include_methods =
        (request.flags & CABBIRD_DUMP_V1_METHODS) != 0u;
    options.include_properties =
        (request.flags & CABBIRD_DUMP_V1_PROPERTIES) != 0u;
    options.include_interfaces =
        (request.flags & CABBIRD_DUMP_V1_INTERFACES) != 0u;
    options.max_classes_per_image = request.max_classes_per_image;
    options.max_methods_per_class = request.max_methods_per_class;

    // The module range, so that a method pointer becomes an RVA and not a raw VA.
    //
    // Leaving these at zero is what makes the output say `// RVA: <ptr> Offset: <ptr>
    // VA: <ptr>` -- three columns holding one raw VA, different on every launch (ASLR), and
    // impossible to file against the on-disk GameAssembly.dll.  The dumper already validated
    // the pointer against this range; what was missing was filling the range.
    options.module_base = module_base;
    options.module_size = module_size;

    // Progress reporting.  Wired here rather than at the call site so every caller of this
    // service gets a live heartbeat, not just the one path the plugin happens to use.
    options.progress = &OnDumpProgress;
    options.progress_user = nullptr;
    // Time-primary with a small class bound, not the 1024-class schedule this shipped with.
    // See the field's comment in il2cpp_dump.hpp: the class-count-only schedule produced a
    // false stall warning because one image can hold few classes but enormous
    // method counts.
    options.progress_class_interval = 64;
    options.progress_interval_ms = 500;

    // Recovery is part of the same request, so it is mapped here rather than in a second entry
    // point: one call, one artifact set, one place where "what did this dump actually produce"
    // is answered.
    options.recover_metadata = (request.flags & CABBIRD_DUMP_V1_METADATA) != 0u;
    if (options.recover_metadata && HasMetadataPath(request) &&
        request.metadata_path != nullptr && request.metadata_path_length != 0) {
        options.metadata_path.assign(request.metadata_path,
                                     request.metadata_path + request.metadata_path_length);
    }

    // SPLIT, not copied wholesale.  See SplitMethodImageFilter above: a list pushed as one
    // needle can never match, and the result is a dump that reports the method walk as
    // requested while containing no methods at all.
    options.method_image_filter =
        SplitMethodImageFilter(request.image_filter, request.image_filter_size);

    // Off deliberately.  See the header: attaching a thread WE created is the case the
    // fix exists for, and the default is what the working configuration used.
    options.attach_thread = true;
    return options;
}

bool WriteDumpFile(const std::wstring& path, const std::string& text, std::string* error) {
    std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error != nullptr) *error = "output stream could not be opened";
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
        if (error != nullptr) *error = "write failed";
        return false;
    }
    return true;
}

// The worker.  By the time this runs, `g_running` is already true, so no second
// request can slip in behind it.
void RunDump(std::string filter, std::wstring path, il2cpp::DumpOptions options) {
    CabbirdDumpResultV1 result{};
    result.struct_size = sizeof(result);

    // Reset and start the clock BEFORE the walk, so the first poll after the button press
    // already has a live elapsed time rather than a zero that means nothing.
    g_progress_classes.store(0, std::memory_order_relaxed);
    g_progress_fields.store(0, std::memory_order_relaxed);
    g_progress_images.store(0, std::memory_order_relaxed);
    // Cleared with the counters: a stale final duration from the previous walk must not be
    // reported once a new one is claimed, or the window would show the old walk's time while
    // the new one is visibly climbing.
    g_progress_final_ms.store(0, std::memory_order_relaxed);
    const std::uint64_t started = NowTicks();
    g_progress_ticks.store(started, std::memory_order_relaxed);

    // Resolve the runtime HERE rather than gating on `Ready()` alone.
    //
    // This one line is the difference between a service that works and one that can
    // never work: nothing else in this tree calls `il2cpp::Initialize()`, so a
    // `Ready()`-only check answers "false" FOREVER, no matter how correct everything
    // else is.
    //
    // `Initialize()` is the retryable primitive, not a one-shot: it is mutex-guarded,
    // clears half-parsed state before each attempt, and only latches success.  So calling it
    // per attempt is the
    // cheap, correct move: the first attempt after GameAssembly.dll is loaded resolves,
    // and every attempt before that fails fast and harmlessly.
    //
    // The failed attempt's reason is kept for the result's diagnostics rather than
    // swallowed, because "unavailable" with no reason is the shape of bug this service
    // was written to end.
    const bool resolved = il2cpp::Ready() || il2cpp::Initialize();

    if (!resolved) {
        result.state = CABBIRD_DUMP_V1_RESULT_UNAVAILABLE;
    } else {
        std::string text;
        // DumpTypeSystem appends, so the buffer starts empty; reserving keeps the
        // 9 MB of growth from repeatedly reallocating while the runtime is attached.
        text.reserve(1u << 20u);
        const il2cpp::DumpStats stats = il2cpp::DumpTypeSystem(text, options);
        result.class_count = stats.classes;
        result.field_count = stats.fields;
        result.method_count = stats.methods;
        result.method_rva_inside = stats.method_pointers_inside;
        result.method_rva_outside = stats.method_pointers_outside;
        result.contained_faults = static_cast<std::uint32_t>(stats.contained_faults);
        result.metadata_size = stats.metadata_size;
        result.metadata_version = static_cast<std::uint32_t>(stats.metadata_version);
        result.metadata_address = stats.metadata_address;
        result.metadata_scan_ms = stats.metadata_scan_ms;
        result.metadata_written = stats.metadata_written ? 1u : 0u;

        // Serve the recovered metadata from the service, not just from the library cache.
        //
        // WHY A COPY AND WHY HERE: `metadata_data` is called from a plugin at an arbitrary
        // moment, while this worker may already be running the NEXT request, so the bytes a
        // caller reads must not be the ones a producer is writing.  The copy happens once,
        // inside the walk that recovered them, and is published under the same lock the dump
        // text uses -- so a reader sees either the previous blob or this one, never a
        // half-filled buffer.
        //
        // The library's own cache is left alone deliberately: it is the process's single copy
        // of a 50 MB scan, and the next request must hit it rather than rescan.
        if (stats.metadata_size != 0) {
            std::string metadata_error;
            const il2cpp::MetadataBlob* blob = il2cpp::CachedMetadataBlob(metadata_error, nullptr);
            if (blob != nullptr && !blob->bytes.empty()) {
                std::scoped_lock lock(g_mutex);
                g_metadata.assign(reinterpret_cast<const char*>(blob->bytes.data()),
                                  blob->bytes.size());
            }
        }

        if (!path.empty()) {
            std::string error;
            if (WriteDumpFile(path, text, &error)) {
                result.bytes_written = text.size();
            } else {
                // The walk succeeded but the artifact did not land.  Reporting COMPLETE
                // here would tell a caller its dump is on disk when it is not, which is
                // exactly the "green check on a path nobody hits" failure this project
                // keeps finding.
                result.state = CABBIRD_DUMP_V1_RESULT_FAILED;
                // The clock is frozen here too, on the failure path.  This branch returns
                // early, so it never reaches the freeze below -- and without this line a
                // failed dump reports "0.0 s" no matter how long it actually ran, which
                // reads as "it failed instantly" rather than "it failed after two minutes".
                result.progress_elapsed_ms = NowTicks() - started;
                g_progress_final_ms.store(result.progress_elapsed_ms, std::memory_order_relaxed);
                std::scoped_lock lock(g_mutex);
                g_dump = std::move(text);
                g_result = result;
                g_state.store(result.state, std::memory_order_release);
                g_running.store(false, std::memory_order_release);
                return;
            }
        }

        // `state` IS SET ON EVERY PATH, INCLUDING THE ONE THAT WRITES NOTHING.
        //
        // This is the defect behind "the dump finished and the window still says running".
        // The walk's state used to be assigned as CABBIRD_DUMP_V1_RESULT_COMPLETE here
        // unconditionally, but the assignment sat BELOW the early-returning failure branch and
        // the whole block was gated on `!path.empty()`.  A request with no output path -- which
        // the plugin reaches whenever the "output file" field is emptied, or whenever its
        // UTF-16 conversion fails, see the note in the plugin -- therefore ran the entire walk
        // and then stored `result.state` at its zero-initialised value, which IS
        // CABBIRD_DUMP_V1_RESULT_PENDING.  The service then reported "pending" about a dump
        // that had already finished, `claimed` went false, and the plugin -- whose window shows
        // "running" for as long as the state says PENDING -- showed a live-looking dump
        // forever.  It presented as an elapsed-time bug ("the timer keeps climbing after it is
        // done") because the elapsed clock was the only thing still moving, but the elapsed
        // arithmetic was never wrong; the state was.
        //
        // IN_MEMORY rather than COMPLETE, because "the walk finished but there is no file" is
        // a different outcome from "the dump is on disk", and collapsing the two is the
        // green-check-on-a-path-nobody-hits failure the failure branch above already refuses.
        result.state = path.empty() ? CABBIRD_DUMP_V1_RESULT_IN_MEMORY
                                    : CABBIRD_DUMP_V1_RESULT_COMPLETE;
        std::scoped_lock lock(g_mutex);
        g_dump = std::move(text);
    }

    // The walk's total duration, published with the final result so a caller can report
    // "2.3 s" rather than just "complete".
    //
    // Frozen HERE, in the same block that freezes the counters, and published through a
    // dedicated atomic rather than recomputed by `DumpState`.  `DumpState` used to derive
    // elapsed time as `NowTicks() - started` on every read, which is correct only while the
    // walk is in flight: once it finished, `started` stayed set, so the elapsed time kept
    // growing forever on a finished dump.  The user sees a timer that never stops and has no
    // way to tell "still walking" from "done, and the clock is lying".
    result.progress_elapsed_ms = NowTicks() - started;
    g_progress_final_ms.store(result.progress_elapsed_ms, std::memory_order_relaxed);
    result.progress_classes = result.class_count;

    {
        std::scoped_lock lock(g_mutex);
        g_result = result;
    }
    g_state.store(result.state, std::memory_order_release);
    g_running.store(false, std::memory_order_release);
}

// --- service entry points -------------------------------------------------

// The one piece of text this service hands back.
//
// `CabbirdStatusV1::message` is documented as "a borrowed view owned by the host, valid
// until the next call on the same thread", which is exactly a per-thread string -- and
// the first version of this file used `thread_local` for it.  That does not work here:
// this image is MANUALLY MAPPED and a mapped image has no loader to run TLS
// initialisation for it.  A `thread_local` std::string put a TLS directory into the
// image and the mapper refused it, correctly.
//
// So the message lives in a fixed static buffer instead.  That is safe here rather than a
// compromise: this text is only ever produced on the refusal path, and `DumpRun` holds
// the service's own lock for that whole path, so no two threads can be writing it at
// once.  A caller on another thread loses it -- which is precisely why the reason is ALSO
// carried in the result where it matters, and why this comment exists rather than a
// silent fixed buffer.

CabbirdStatusV1 MakeStatusWithMessage(CabbirdStatusCodeV1 code, const std::string& message) {
    CabbirdStatusV1 status{};
    status.code = static_cast<std::uint32_t>(code);
    std::scoped_lock lock(g_message_mutex);
    const std::size_t count = message.size() < sizeof(g_status_message) - 1
        ? message.size()
        : sizeof(g_status_message) - 1;
    std::memcpy(g_status_message, message.data(), count);
    g_status_message[count] = '\0';
    status.message.data = BorrowedServiceText(23, std::string_view(g_status_message, count));
    status.message.size = count;
    return status;
}

CabbirdStatusV1 CABBIRD_CALL DumpRun(
    void*, const CabbirdDumpRequestV1* request, CabbirdDumpResultV1* result) {
    if (result == nullptr) return MakeStatus(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    if (request == nullptr || request->struct_size < sizeof(CabbirdDumpRequestV1)) {
        *result = MakeResult();
        result->state = CABBIRD_DUMP_V1_RESULT_FAILED;
        return MakeStatusWithMessage(
            CABBIRD_STATUS_V1_INVALID_ARGUMENT, "request struct is smaller than the ABI");
    }

    // Refuse BEFORE claiming the slot when the runtime has never resolved, because
    // otherwise the caller has to poll a request that was never going to do anything.
    // The check is cheap (a latched flag plus, once, the resolve attempt) and it turns
    // "pending forever" into an immediate, explained UNAVAILABLE.
    if (!il2cpp::Ready() && !il2cpp::Initialize()) {
        *result = MakeResult();
        result->state = CABBIRD_DUMP_V1_RESULT_UNAVAILABLE;
        {
            std::scoped_lock lock(g_mutex);
            g_result = *result;
        }
        g_state.store(result->state, std::memory_order_release);
        return MakeStatusWithMessage(CABBIRD_STATUS_V1_UNAVAILABLE, il2cpp::LastError());
    }

    // Claim the slot before doing anything else, so the check and the claim cannot be
    // split by another caller.
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        *result = MakeResult();
        result->state = CABBIRD_DUMP_V1_RESULT_FAILED;
        // CONFLICT rather than a bespoke code: the SDK has no BUSY, and inventing one
        // would mean the plugin layer and every plugin had to agree on a number the ABI
        // does not describe.  What the caller needs to know is "one is already running",
        // and the CONFLICT code says exactly that with the status enum that exists.
        return MakeStatus(CABBIRD_STATUS_V1_CONFLICT);
    }

    // The loaded module's range, taken HERE -- on the caller's thread, before the worker
    // exists -- so the value the worker validates method pointers against cannot change under
    // it.  A module that is not loaded yet yields {0, 0}, which the dumper treats as "no range":
    // it still prints addresses, and the dump's own header says they are VAs rather than
    // pretending they are RVAs.
    std::uintptr_t module_base = 0;
    std::uintptr_t module_size = 0;
    ImageRangeOf(il2cpp::Functions().module, &module_base, &module_size);

    PublishRunning();
    *result = MakeResult();
    result->state = CABBIRD_DUMP_V1_RESULT_PENDING;
    // Publish the state into the SERVICE, not just into this call's output struct.
    //
    // This write is the half of the defect that was missing.  `*result` is the return value
    // for the caller that pressed the button; `g_state` is what every LATER poll of
    // `DumpState` reads.  Without this line the walk ran for ~70 s while `g_state` still
    // held whatever it was before -- the initialiser, or the previous walk's terminal state
    // -- so pollers were told "unavailable"/"complete" about a dump that was in flight.
    g_state.store(CABBIRD_DUMP_V1_RESULT_PENDING, std::memory_order_release);
    // The stall detector's window starts here, and its heartbeat is re-armed against whatever the
    // PREVIOUS walk left behind -- otherwise a new walk would inherit the old heartbeat value and
    // be declared stalled before its first report.
    g_claim_ticks.store(NowTicks(), std::memory_order_relaxed);
    g_last_heartbeat_seen = g_progress_heartbeat.load(std::memory_order_relaxed);
    g_last_heartbeat_ticks = 0;

    try {
        std::string filter;
        if (request->image_filter != nullptr && request->image_filter_size != 0) {
            filter.assign(request->image_filter, request->image_filter + request->image_filter_size);
        }
        std::wstring path;
        if (request->output_path != nullptr && request->output_path_length != 0) {
            path.assign(request->output_path, request->output_path + request->output_path_length);
        }
        const il2cpp::DumpOptions options = MakeOptions(*request, module_base, module_size);
        ServiceLease work(UnityAdapterServices::Current()->Endpoint());
        if (!work) throw std::runtime_error("adapter is stopping");
        std::thread worker([lease = std::move(work), filter = std::move(filter),
                            path = std::move(path), options]() mutable {
            ServiceOwnerScope scope(lease.Owner());
            try { RunDump(std::move(filter), std::move(path), options); }
            catch (...) {
                g_state.store(CABBIRD_DUMP_V1_RESULT_FAILED, std::memory_order_release);
                g_running.store(false, std::memory_order_release);
            }
        });
        worker.detach();
    } catch (...) {
        // A thread that could not be created is not a dump in progress.
        CabbirdDumpResultV1 failed = MakeResult();
        failed.state = CABBIRD_DUMP_V1_RESULT_FAILED;
        {
            std::scoped_lock lock(g_mutex);
            g_result = failed;
        }
        g_state.store(failed.state, std::memory_order_release);
        g_running.store(false, std::memory_order_release);
        *result = failed;
        return MakeStatus(CABBIRD_STATUS_V1_FAILED);
    }
    return MakeStatus(CABBIRD_STATUS_V1_OK);
}

} }  // namespace (adapter internals)
// The elapsed time to report, given whether a walk is currently claimed.
//
// ONE helper because this was written twice and fixed once.  `DumpState` (the plugin's
// path) got the state-aware version while `SnapshotHostUnityDump` (the host's own status
// JSON) kept deriving from the clock unconditionally -- so the window was fixed while the
// diagnostic still showed `elapsedMs` climbing on a COMPLETE dump (41814 -> 44856 -> 47885
// on a finished walk).  Two publishers of one number must share one implementation, or the
// next fix lands in only one of them, which is exactly what happened here.
//
// While a walk is claimed, the clock is the truth: "how long has this been running".
// Once it is not, `g_progress_ticks` is stale (it is the start of the LAST walk), so the
// frozen final duration is the only honest answer.
std::uint64_t ReportedElapsedMs(bool claimed) noexcept {
    using namespace unity_adapter_dump_detail;
    if (claimed) {
        const std::uint64_t started = g_progress_ticks.load(std::memory_order_relaxed);
        if (started != 0) {
            const std::uint64_t now = NowTicks();
            return now > started ? now - started : 0;
        }
        return 0;
    }
    return g_progress_final_ms.load(std::memory_order_relaxed);
}

// NOTE: SnapshotHostUnityDump must NOT live in the anonymous namespace above, which is
// where it was first written.  An anonymous namespace gives INTERNAL linkage, so the
// symbol is invisible outside this translation unit and `cabbird_core_image` failed to
// link with `LNK2019 unresolved external symbol cabbird::SnapshotHostUnityDump` -- while
// the header declaration let every CALLER compile cleanly.  A declaration plus a
// same-named internal definition is a link error no diagnostic points the namespace at.
HostDumpStats SnapshotHostUnityDump() {
    ServiceCall adapter_call;
    if (!adapter_call) return {};
    using namespace unity_adapter_dump_detail;
    HostDumpStats stats{};
    stats.state = g_state.load(std::memory_order_acquire);
    stats.claimed = g_running.load(std::memory_order_acquire);
    stats.classes = g_progress_classes.load(std::memory_order_relaxed);
    stats.fields = g_progress_fields.load(std::memory_order_relaxed);
    stats.images = g_progress_images.load(std::memory_order_relaxed);
    stats.elapsed_ms = ReportedElapsedMs(stats.claimed);
    return stats;
}

namespace { namespace unity_adapter_dump_detail {

CabbirdStatusV1 CABBIRD_CALL DumpState(void*, CabbirdDumpResultV1* result) {
    if (result == nullptr) return MakeStatus(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    // STATE AND RESULT ARE READ UNDER THE SAME LOCK, and that is the point.
    //
    // They were two separate reads -- `g_state` (atomic) and `g_result` (mutex) -- so a
    // poller could observe a state from BEFORE a write and a result from AFTER it, or the
    // reverse.  That produced a contradiction in the window: a live walk (counters climbing
    // every sample) published together with `state == UNAVAILABLE`, so the plugin correctly
    // refused to draw the progress block, leaving three zeroes next to "running".
    //
    // `g_running` is the authoritative "a walk is in flight" flag -- it is what rejects a
    // second request -- so the state handed out is derived from it rather than trusted.
    const bool claimed = g_running.load(std::memory_order_acquire);
    {
        std::scoped_lock lock(g_mutex);
        *result = g_result;
    }
    result->struct_size = sizeof(*result);
    result->state = claimed ? CABBIRD_DUMP_V1_RESULT_PENDING
                            : g_state.load(std::memory_order_acquire);

    // A CLAIMED WALK WITH NO HEARTBEAT IS A DEAD WALK, AND SAYING SO IS THIS SERVICE'S JOB.
    //
    // `claimed` is only cleared by the worker itself, so a worker that died inside the walk -- or
    // that never got past attaching to the runtime -- leaves the slot taken and the state PENDING
    // forever.  Nothing else in this file could produce a terminal state for that case, which is
    // exactly how a window came to show "running" indefinitely with `classes` and `elapsedMs`
    // both at zero.
    //
    // Detecting it HERE rather than in the worker is deliberate: a check the dead worker has to
    // run is not a check.  The reader is the only party guaranteed to still be alive.
    if (claimed) {
        const auto now = NowTicks();
        const auto heartbeat = g_progress_heartbeat.load(std::memory_order_relaxed);
        if (heartbeat != g_last_heartbeat_seen) {
            // The walk is alive; remember where it got to and re-arm the stall window.
            g_last_heartbeat_seen = heartbeat;
            g_last_heartbeat_ticks = now;
        } else {
            // No new report since the last poll.  Time it from the CLAIM rather than from the
            // first poll, so a walk that never reports at all is still bounded -- timing from the
            // first poll would leave this at zero forever and detect nothing.
            if (g_last_heartbeat_ticks == 0) g_last_heartbeat_ticks = g_claim_ticks.load(
                std::memory_order_relaxed);
            if (g_last_heartbeat_ticks != 0 && now > g_last_heartbeat_ticks &&
                now - g_last_heartbeat_ticks > kStallLimitMs) {
                // It has said nothing for longer than any real walk would.  Publish the terminal
                // state the worker failed to publish, and release the slot so the user can retry.
                result->state = CABBIRD_DUMP_V1_RESULT_FAILED;
                {
                    std::scoped_lock lock(g_mutex);
                    g_result = *result;
                }
                g_state.store(CABBIRD_DUMP_V1_RESULT_FAILED, std::memory_order_release);
                g_running.store(false, std::memory_order_release);
                // Re-arm for the next walk; `DumpRun` also does this on claim.
                g_last_heartbeat_ticks = 0;
            }
        }
    }
    // THE COUNTERS ARE PUBLISHED IN EVERY STATE, not only while PENDING.
    //
    // Gating this block on `state == PENDING` is wrong: `state` is not a reliable
    // "is a walk running"
    // predicate for a reader: the walk finishes in ~70 s and the state then becomes
    // COMPLETE, so a caller polling at the wrong moment -- or reading after the fact --
    // got `field_count` / `progress_*` as ZEROES even though tens of thousands of classes
    // had just been walked.
    //
    // The counters are monotonic by construction: zero before any request, climbing during
    // the walk, and retaining the final total afterwards.  That is exactly what a reader
    // wants to display, so it is published unconditionally and the "is it still going"
    // question is left to `state`, where it belongs.
    //
    // Still documented rather than silent, because it changes what `class_count` means: it
    // is "visited so far" in PENDING and "total" in any terminal state.
    result->progress_classes = g_progress_classes.load(std::memory_order_relaxed);
    result->field_count = g_progress_fields.load(std::memory_order_relaxed);
    // A FINISHED WALK REPORTS ITS FINAL DURATION, NOT A GROWING ONE.
    //
    // `state` decides which of the two numbers is the truth.  While a walk is claimed, the
    // elapsed time is genuinely "how long has it been running", so it is derived from the
    // clock.  Once the walk is over, `started` is stale -- it is the start of the LAST walk
    // -- so deriving from the clock would report "still running" forever, which is what the
    // window showed.
    //
    // Shared with `SnapshotHostUnityDump` through `ReportedElapsedMs` on purpose: this was
    // fixed here first and the diagnostic kept its own copy of the bug for a round.
    result->progress_elapsed_ms = ReportedElapsedMs(claimed);
    return MakeStatus(CABBIRD_STATUS_V1_OK);
}

CabbirdStatusV1 CABBIRD_CALL DumpData(
    void*, std::uint64_t offset, char* buffer, std::uint64_t capacity, std::uint64_t* copied) {
    if (copied == nullptr || (buffer == nullptr && capacity != 0)) {
        return MakeStatus(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    }
    *copied = 0;
    std::scoped_lock lock(g_mutex);
    if (g_state.load(std::memory_order_acquire) != CABBIRD_DUMP_V1_RESULT_COMPLETE) {
        return MakeStatus(CABBIRD_STATUS_V1_UNAVAILABLE);
    }
    const std::uint64_t size = g_dump.size();
    if (offset >= size) return MakeStatus(CABBIRD_STATUS_V1_OK);
    const std::uint64_t available = size - offset;
    const std::uint64_t count = available < capacity ? available : capacity;
    if (count != 0) {
        std::memcpy(buffer, g_dump.data() + offset, static_cast<std::size_t>(count));
    }
    *copied = count;
    return MakeStatus(CABBIRD_STATUS_V1_OK);
}

std::uint64_t CABBIRD_CALL DumpSize(void*) {
    std::scoped_lock lock(g_mutex);
    return g_state.load(std::memory_order_acquire) == CABBIRD_DUMP_V1_RESULT_COMPLETE
        ? static_cast<std::uint64_t>(g_dump.size())
        : 0u;
}

// --- the recovered metadata, served in chunks ------------------------------------------
//
// WHY THESE DO NOT CONSULT `g_state`.  The dump text is only served when the last walk
// reported COMPLETE, because an empty buffer and an unwritten one must not look alike.  The
// metadata has no such ambiguity: it is either in `g_metadata` (recovered, complete, already
// validated against its own header by the scan that found it) or it is empty (never recovered).
// Serving it is a memcpy, not a runtime call, so it cannot fault the game -- which is also why
// these two are the ONLY dump entry points a plugin may call from the render domain without a
// queue and without a stall.
CabbirdStatusV1 CABBIRD_CALL MetadataData(
    void*, std::uint64_t offset, char* buffer, std::uint64_t capacity, std::uint64_t* copied) {
    if (copied == nullptr || (buffer == nullptr && capacity != 0)) {
        return MakeStatus(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    }
    *copied = 0;
    std::scoped_lock lock(g_mutex);
    if (g_metadata.empty()) return MakeStatus(CABBIRD_STATUS_V1_UNAVAILABLE);
    const std::uint64_t size = g_metadata.size();
    if (offset >= size) return MakeStatus(CABBIRD_STATUS_V1_OK);  // documented: 0 bytes copied
    const std::uint64_t count = (size - offset) < capacity ? (size - offset) : capacity;
    if (count != 0) {
        std::memcpy(buffer, g_metadata.data() + offset, static_cast<std::size_t>(count));
    }
    *copied = count;
    return MakeStatus(CABBIRD_STATUS_V1_OK);
}

std::uint64_t CABBIRD_CALL MetadataSize(void*) {
    std::scoped_lock lock(g_mutex);
    return static_cast<std::uint64_t>(g_metadata.size());
}


} }  // namespace (adapter internals)


}  // namespace cabbird

#undef g_claim_ticks
#undef g_dump
#undef g_metadata
#undef g_last_heartbeat_seen
#undef g_last_heartbeat_ticks
#undef g_message_mutex
#undef g_mutex
#undef g_progress_classes
#undef g_progress_fields
#undef g_progress_final_ms
#undef g_progress_heartbeat
#undef g_progress_images
#undef g_progress_ticks
#undef g_result
#undef g_running
#undef g_state
#undef g_status_message


/* ---- UnityAdapter component: entities ---- */

// Host backend for `cabbird.unity.entity-esp` -- the entity source an ESP plugin pulls
// from.  This is the piece that was missing while the plugin, the overlay surface and the
// offline regression were all already done: `grep` for CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID
// found exactly one reference in the whole tree (a capability mapping in
// plugin_capability_policy.cpp) and NO producer, so the plugin correctly degraded to
// "entity source unavailable" and drew nothing.
//
// WHAT MAKES THIS DIFFERENT FROM ANOMALY, AND WHY THERE IS NO SIGNATURE PROFILE
// ----------------------------------------------------------------------------
// The sibling port of this facade targets UE5, where reflection metadata is stripped and
// every engine address has to be recovered from a byte-pattern signature that is
// re-verified on every game update.  Unity + IL2CPP keeps the type system: the runtime
// exports `il2cpp_class_from_name`, `il2cpp_field_get_offset` and
// `il2cpp_class_get_static_field_data`.  So the binding below is not a set of guessed
// addresses -- it is a set of offsets that `il2cpp_field_get_offset` REPORTED on a live
// process.
//
// The offsets are therefore written here as constants with their provenance attached,
// rather than loaded from a profile file.  A profile would imply they are a heuristic
// that may be wrong in a way a re-scan could fix; they are not.  If a future build moves
// them, the fix is to re-run the dump and update the numbers -- which is a normal code
// change with a diff, not a configuration drift nobody notices.
//
// PROVENANCE (every offset below is read out of an IL2CPP dump of this title's
// `GameAssembly.dll`, and each line was re-checked by class name):
//
//   Lens.Gameplay.World.Tools.BattleInfoMono      : BattleMonoBehaviour   (a MonoBehaviour)
//       battleField                          @ 0x20
//   Lens.Gameplay.World.Tools.BattleFieldInfo     : BattleSerializable
//       guid @ 0x10, id @ 0x14, describe @ 0x18   (from BattleSerializable)
//       slotInfos  @ 0x20   List<BattleSlotInfo>
//       itemInfos  @ 0x28   List<BattleItemInfo>
//       enemyInfos @ 0x30   List<BattleEnemyInfo>     <-- the one this file uses
//   Lens.Gameplay.World.Tools.BattleEnemyInfo     : BattleSerializable
//       guid @ 0x10, id @ 0x14, describe @ 0x18
//       camp         @ 0x20   Lens.Gameplay.Modules.BigWorld.ECampType
//       groupId      @ 0x24   System.Int32
//       isBlueActive @ 0x28   System.Boolean
//       unitGroupId  @ 0x2C   System.Int32
//       position     @ 0x30   UnityEngine.Vector3          <-- what an ESP boxes
//       angle        @ 0x3C   UnityEngine.Vector3
//
// THE IL2CPP `List<T>` LAYOUT, which is Unity's own and not a private convention of this
// game: `_items` (an Il2CppArray*) at +0x10, `_size` (int32) at +0x18.  An Il2CppArray's
// element data starts at header(0x10) + bounds(0x08) = +0x18, matching the layout
// constants this runtime reports (objhdr=16 arrhdr=32 arrlen=24 arrbounds=16).
//
// THE PART THAT IS STILL MISSING, STATED PLAINLY
// ----------------------------------------------
// `Refresh` below resolves the class and then REFUSES to walk, because the entry point
// from a static to a live `BattleInfoMono`/`BattleFieldInfo` is NOT yet verified:
//
//   * `BattleInfoMono` is a MonoBehaviour, so it has no static instance field -- the
//     scene has to be searched, or a manager has to hand it over.
//   * A static entry EXISTS and is confirmed in the dump:
//         private static Lens.Framework.Utility.IBattleConfigManager
//             <BattleConfigManager>k__BackingField
//         public  static Lens.Framework.Utility.IBattleConfigManager get_BattleConfigManager()
//     but that is an INTERFACE-typed static.  Turning it into `enemyInfos` requires
//     either the concrete implementing type (not established) or a virtual call through
//     `il2cpp_runtime_invoke` (which boxes its return value -- a per-call allocation).
//
// So this file deliberately publishes a service whose `entity_count` answers 0, and says
// WHY in the diagnostics rather than pretending to have entities.  Zero-with-a-reason is
// recoverable; a guessed pointer chain that happens to read plausible floats is not --
// it would put boxes on screen in the wrong places and look like a working feature.


// The engine-side transform capability this service CONSUMES: the offset chain from a
// `BaseData` to its `UnityEngine.Transform`, the liveness rule, and `Transform::get_position`.
//
// This is an include of a SIBLING in the same adapter module, which is the point of the
// refactor: the chain used to be owned by this file and borrowed by the player service through
// this file's own header, so an engine fact travelled through a read-only service's public
// surface.  Now both consumers reach the same implementation, and this file neither owns it nor
// exports it.


namespace cabbird {
namespace { namespace unity_adapter_entities_detail {

constexpr std::size_t kOffBattleField = 0x20;

constexpr std::size_t kOffEnemyInfos = 0x30;

constexpr std::size_t kOffEnemyPosition = 0x30;

constexpr std::size_t kOffEnemyCamp = 0x20;

constexpr std::size_t kOffEnemyId = 0x14;

constexpr std::size_t kListItems = 0x10;

constexpr std::size_t kListSize = 0x18;

constexpr std::size_t kArrayData = 0x18;

constexpr std::size_t kArrayLength = 0x18;

constexpr const char* kEntityImage = "Assembly-CSharp.dll";

constexpr const char* kEntityNamespace = "Lens.Gameplay.Modules.BigWorld";

constexpr const char* kHandleClass = "EntityHandle";

constexpr const char* kEntityClass = "Entity";

constexpr const char* kDataClass = "BaseData";

constexpr const char* kRelativeClass = "RelativeTransform";

constexpr const char* kManagerNamespace = "Lens.Gameplay.Modules.BigWorld";

constexpr const char* kManagerClass = "EntityManager";

constexpr const char* kSingletonField = "p_instance";

constexpr std::size_t kOffManagerData = 0x30;

constexpr std::size_t kOffInfoMonoBattleField = 0x20;

constexpr std::size_t kOffMonoCachedPtr = 0x10;

constexpr const char* kFormulaNamespace = "Lens.Framework.Utility";

constexpr const char* kFormulaClass = "CombatNumericalCalculationUtility";

constexpr const char* kCampMonsterField = "<CampType_Monster>k__BackingField";

constexpr const char* kCampPlayerField = "<CampType_Player>k__BackingField";

constexpr const char* kCameraManagerNamespace = "Lens.Framework.Managers";

constexpr const char* kCameraManagerClass = "CameraManager";

constexpr const char* kMainCameraField = "<mainCamera>k__BackingField";

constexpr std::size_t kOffCachedPtr = 0x10;

struct CameraBinding {
    bool resolved{};
    // The `UnityEngine.Camera` static backing field holding the game's main camera.
    std::uintptr_t main_camera_field{};
    // `WorldToScreenPoint(Vector3)` and the `Vector3` class used to pass its argument.  Both
    // are runtime handles, so they stay valid for the life of the class -- which in IL2CPP is
    // forever, and is what makes caching them safe.
    std::uintptr_t method{};
    std::uintptr_t vector3_class{};
    // The render-target size, read from the camera (`get_pixelWidth` / `get_pixelHeight`) rather
    // than assumed.  Every number in `view_projection` becomes pixels through these, so a wrong
    // pair scales every box by a constant -- a defect that reads as "the ESP is slightly off"
    // and is invisible in a screenshot of a single entity.
    double pixel_width{};
    double pixel_height{};
    // `Camera.projectionMatrix * Camera.worldToCameraMatrix`, READ FROM THE GAME and stored row
    // major (`row * 4 + column`), which is how `ProjectWorldPoint` consumes it.
    //
    // THIS IS THE FIELD FOUR HAND-DERIVED SOLVERS FAILED TO PRODUCE, and the reason is now
    // clear: it should never have been derived.  Both factors are properties the engine already
    // maintains, so the product is the engine's own answer instead of a reconstruction of it.
    //
    // The reconstruction was also UNVERIFIABLE rather than merely hard.  Its self-check
    // projected a point through the matrix being tested, so it agreed with itself by
    // construction -- it once reported `check_ok: true` beside a 18462-pixel error, and shipped
    // boxes in the wrong place four times on that basis.
    float view_projection[16]{};
    bool view_projection_valid{};
    // The `MethodInfo*` for the two matrix properties, resolved once alongside the camera and
    // cached for the same reason every other handle here is: they cannot change while the class
    // is loaded, and re-resolving per tick costs a table walk on the game thread.
    std::uintptr_t world_to_camera_method{};
    std::uintptr_t projection_method{};
    // Written by the refresh for the render domain to read.  A plain struct of doubles, copied
    // under the state lock, so the render domain never touches IL2CPP to get a matrix.
    bool valid{};
    // The two factors as read, and the viewport, published for diagnosis.  A transposed
    // multiplication and a wrong camera both produce "boxes are in the wrong place", and only
    // the factors can tell them apart -- the same lesson `camera_raw` taught when the derived
    // tangents were impossible and the raw samples were not.
    float world_to_camera[16]{};
    float projection[16]{};
    double camera_pos[3]{};
    std::string reason{"camera not resolved yet"};
};

constexpr std::uint32_t kKindUnclassified = 0;

constexpr std::uint32_t kKindPlayer = 1;

constexpr std::uint32_t kKindMonster = 2;

constexpr std::uint32_t kKindNpc = 3;

constexpr std::uint32_t kKindWorld = 4;

constexpr std::size_t kMaximumEntities = 512;

constexpr double kApproxHalfExtent[3] = {0.6, 1.0, 0.6};

constexpr double kEntityBaseOffset = kApproxHalfExtent[1];

struct EntityRecord {
    std::uint64_t id{};
    std::uint32_t kind{};
    // 1 when the category above came from the entity's own class name rather than from a
    // camp comparison.  The camp path refines this answer when the game supplies it, so the
    // flag is what keeps "no camp available" from erasing a category that is already right.
    std::uint32_t kind_from_class{};
    // THE GAME'S OWN `EWorldObjectType`, read from `WorldEntityData::worldType` at 0x3C0.
    //
    // Recorded for the same reason `data_class` is: when the four-way split looks wrong, the
    // question is immediately "what did the game actually say this thing was", and a count of
    // misclassified entities cannot answer it.  `world_type_read` is separate from the value
    // because `EWorldObjectType::None` is 0, and "the game says None" must not be reported the
    // same way as "the read failed".
    std::int32_t world_type{};
    std::uint32_t world_type_read{};
    double center[3]{};
    // THE EIGHT AABB CORNERS, PROJECTED BY THE GAME, IN THE GAME'S PIXELS.
    //
    // These are filled on the GAME thread by `ReadEntity`, for the reason recorded there: the
    // game's projection function is only callable from the thread the runtime owns, and the
    // render callback runs on the presenter's.  The render side therefore reads an answer
    // instead of computing one, which is also how the shipped UE5 host does it.
    //
    // This replaced a `view_projection` matrix that four derivations failed to reconstruct.
    // The matrix was not merely hard, it was UNVERIFIABLE: its self-check projected a point
    // through the matrix under test, so it agreed with itself by construction and once reported
    // `ok` beside an 18462-pixel error.  These coordinates come from OUTSIDE that loop.
    double screen_points[8][2]{};
    std::uint32_t screen_mask{};
    // The display name, copied off managed memory on the game thread.
    //
    // A char array rather than a `std::string` ON PURPOSE: the walk allocates nothing per
    // entity beyond the vector itself, and a name is at most a few dozen bytes.  The label is
    // decoration, so truncation is the correct failure -- an entity with a 4 KB name is not
    // worth a heap allocation on the game thread.
    char label[64]{};
    std::uint32_t label_size{};
    // WHICH LIVE CLASS THIS ENTITY IS, recorded so `status` can report the actual population.
    //
    // `kinds: unclassified=71` and `labelReads: 0` both say a lookup failed without saying what
    // WAS there, and "no monsters in this scene" and "my class table is wrong" are opposite
    // problems that a failure count cannot tell apart.  A name is copied rather than the class
    // pointer kept, because the pointer is meaningless outside the process and the name is the
    // thing a human reads.
    char data_class[40]{};
    // THE MANAGED DATA OBJECT'S ADDRESS, published through `entity_at` so a plugin can walk
    // the config chain on its own.  See the field's comment in `unity.h`: the point is that
    // name exploration then happens on the side that HOT-RELOADS instead of the side that
    // costs a full game restart.
    std::uintptr_t data{};
    // THE LIVE `UnityEngine.Transform`, kept so the per-tick cheap path can re-read this entity's
    // position without walking the game's container again -- the walk already resolved it, and
    // re-deriving the chain per tick would be a second implementation of the offset chain that
    // could disagree with the first.
    //
    // A raw address in the target process, exactly like `data` above.  Nothing outside the game
    // thread may call through it: it is the game's object, and `get_position` reads Unity's
    // native transform behind it.
    std::uintptr_t transform{};
};

struct LabelSource {
    std::uintptr_t data_class{};
    std::size_t off_config{};
    // The class name is COPIED here at resolve time.  `il2cpp_class_get_name` returns a pointer
    // into runtime metadata, and calling it per entity per walk just to build a diagnostic
    // histogram would add a runtime call to the hot path for information that cannot change
    // while the class is loaded.
    char class_name[48]{};
};

constexpr std::size_t kLabelSourceCapacity = 6;

struct LabelNameSource {
    std::uintptr_t klass{};   // the class whose instance carries the LangString
    std::size_t off_name{};   // offset of that LangString INSIDE that class
    char class_name[40]{};    // its SIMPLE name, which is the identity used at read time
};

constexpr std::size_t kLabelNameSourcesCapacity = 8;

struct WorldTypeField {
    const char* class_name;
    std::size_t offset;
};

constexpr WorldTypeField kWorldTypeFields[]{
    {"NpcData", 0x3C0},
    {"WorldItemData", 0x3C0},
};

constexpr std::size_t kWorldTypeFieldCount =
    sizeof(kWorldTypeFields) / sizeof(kWorldTypeFields[0]);

struct LabelBinding {
    bool resolved{};
    std::string reason;
    // `il2cpp_object_get_class`: how the walk learns which live class an entity is.
    std::uintptr_t object_get_class{};
    std::uintptr_t lang_string_class{};
    LabelSource sources[kLabelSourceCapacity]{};
    std::size_t source_count{};
    // SUPERSEDED by `ComponentState().label_name_sources` and deliberately left at 0.
    //
    // One offset cannot serve config types whose layouts differ.  The field is kept so the
    // diagnostic that prints it does not have to change, and a non-zero value here now means a
    // caller went looking for a single offset that no longer exists.
    std::size_t off_config_name{};
    std::size_t off_lang_group{};   // LangString::group   (dump: 0x10)
    std::size_t off_lang_key{};     // LangString::langKey (dump: 0x18)
    // Why the camp read succeeded or failed, recorded AT the attempt rather than inferred from
    // `camps_known` later.
    std::string camp_stage;
};

struct LabelCacheEntry {
    std::uint64_t id{};
    char text[64]{};
    std::uint32_t size{};
};

constexpr std::size_t kLabelCacheCapacity = 512;

struct WalkProfile {
    std::int64_t position_reads{};
    std::int64_t position_cache_hits{};
    std::int64_t position_failures{};
    std::int64_t label_reads{};
    std::int64_t label_cache_hits{};
    std::int64_t label_micros{};
    std::int64_t position_micros{};
    // WHERE THE NAME CHAIN BREAKS, by step.  "No name" was indistinguishable from "no such class"
    // and from "the config pointer was null", and only one of those is a bug in this file.
    std::int64_t label_unmatched{};    // the live class is not in the binding table at all
    std::int64_t label_no_config{};    // matched, but the config field read back zero
    std::int64_t label_no_name{};      // config read, but TEnemy::<name> read back zero
    std::int64_t label_no_group{};     // name read, but LangString::group was null or empty
    std::int64_t label_key_only{};     // fell back to the localisation key (a name WAS produced)
    // The `config` object's class was not one this chain knows how to read, so the read was
    // SKIPPED rather than attempted at a guessed offset.  Deliberately distinct from
    // `label_no_name`: this one means "I refused to read unverified memory", not "no name exists".
    std::int64_t label_class_mismatch{};
};

struct LiveBinding {
    bool resolved{};
    // `FieldInfo*` for `Singleton<T>::p_instance`, as an integer.  Stored rather than an
    // address computed from it: a FieldInfo comes from the runtime and stays valid for the
    // life of the class, while an address computed as `static_data + offset` would be built
    // on an offset the runtime reports as -1 for statics.  See `ReadStaticField`.
    std::uintptr_t instance_field{};
    // Offsets, each VERIFIED against the dump or resolved at runtime -- never assumed.
    std::size_t off_entity_list{};    // EntityManager::m_entityList      (dump: 0x88)
    std::size_t off_handle_entity{};  // EntityHandle::<entity>           (dump: 0x10)
    std::size_t off_entity_data{};    // Entity::data                     (dump: 0x18)
    std::size_t off_data_transform{}; // BaseData::<transform>            (dump: 0xA0)
    std::size_t off_transform{};      // RelativeTransform::m_transform   (dump: 0x10)
    std::size_t off_data_camp{};      // BaseData::<campType>             (dump: 0x134)
    std::size_t off_data_type{};      // BaseData::<entityType>           (dump: 0x140)
    std::size_t off_list_items{};     // List<T>::_items                  (dump: 0x10)
    std::size_t off_array_data{};     // Il2CppArray element data         (dump: 0x18)
    std::size_t off_list_size{};      // List<T>::_size                   (dump: 0x18)
    // Runtime handles, cached because they cannot change while the class is loaded.
    std::uintptr_t vector3_class{};
    std::uintptr_t get_position{};    // UnityEngine.Transform::get_position
    // `UnityEngine.Transform::set_position`.  Resolved alongside the getter because they are
    // the same question asked in two directions, and because the ONLY correct way to move a
    // live Unity object is to call the property -- the position itself lives in Unity's
    // native Transform behind `m_CachedPtr`, whose layout this project refuses to guess.
    std::uintptr_t set_position{};
    std::int32_t camp_monster{};
    std::int32_t camp_player{};
    bool camps_known{};
    // Why the camp read succeeded or failed, recorded AT the attempt rather than inferred
    // from `camps_known` later.  `campsKnown: false` alone cannot distinguish "class not
    // found" from "field not found" from "field read gave nothing", and those need
    // different fixes, and the dump alone cannot tell them apart.
    std::string camp_stage;
};

struct ServiceState {
    std::mutex mutex;
    std::vector<EntityRecord> entities;
    // ================= STABLE STORAGE FOR THE PUBLISHED LABEL =================
    //
    // WHY THIS EXISTS: `entity_at` used to hand the consumer `record.label`, a pointer INTO
    // `entities`.  That vector is REPLACED by every refresh (`state.entities = std::move(...)`,
    // under this same mutex), which frees the old elements.  A consumer that copies the label
    // after `entity_at` returns -- which is exactly what the SDK tells it to do -- therefore read
    // freed memory, and the crash landed in the game process, not in the plugin.
    //
    // THE BUG WAS INVISIBLE UNTIL NAMES WORKED: while the label chain was dead code
    // `label_size` was always 0, so `entity->label` was always `nullptr` and the dangling read
    // never happened.  The moment names started resolving, every `entity_at` handed out a
    // pointer into a vector that the next walk would free.  That is why the crashes began
    // exactly when the names did.
    //
    // These slots are addressed by entity INDEX and are never reallocated, so the pointer the
    // consumer receives stays valid until the next `entity_at` -- the promise the SDK already
    // makes, now actually kept.
    static constexpr std::size_t kPublishedLabelCapacity = 512;
    static constexpr std::size_t kPublishedLabelBytes = sizeof(EntityRecord::label);
    char published_label[kPublishedLabelCapacity][kPublishedLabelBytes]{};
    std::uint64_t generation{};
    std::string unavailable_reason{"entity source has not resolved yet"};
    std::string name_probe;
    bool class_resolved{};
    // Written only from the game domain (`RefreshUnityEntityEsp`), read under `mutex` as
    // well.  Kept out of `mutex` for the whole walk: the walk must not hold a lock that the
    // render domain's `entity_count` needs, because the walk touches memory that can fault
    // and a held lock would turn "one bad read" into "the overlay stops drawing".
    LiveBinding binding;
    // The camera probe's findings.  See `HostEntityStats` for why the probe lives here.
    bool camera_anchor_resolved{};
    bool main_camera_present{};
    std::uint64_t main_camera_object{};
    std::uint64_t main_camera_native{};
    std::string camera_reason{"camera not probed yet"};
    bool camera_matrix_valid{};
    bool camera_check_ok{};
    double camera_check_error_pixels{};
    CameraBinding camera;
    // The raw camera samples, as a pre-formatted string.
    //
    // This exists because the derived numbers alone were not enough to diagnose a wrong
    // projection: "half-angle tangent came out non-positive" says the samples were bad but not
    // WHICH one or HOW.  Formatting here rather than in the status builder keeps the render
    // path free of allocation and keeps the JSON builder's job to a copy.
    std::string camera_raw;
    // `game=(x,y) matrix=(x,y) delta=n` for a few entities, in ONE string, for the same
    // reason `camera_raw` exists: the derived verdict ("the boxes are in the wrong place") is
    // not actionable, the two conflicting coordinates are.
    std::string camera_cross_check;
    // How many entities the game actually answered for, and how many got a COMPLETE set of
    // eight corners.  A plugin needs all eight to build an AABB, so a large gap between these
    // two numbers is the difference between "the projection is broken" and "the plugin's own
    // completeness requirement is dropping everything" -- and the second is invisible in a
    // box count of zero.
    std::size_t projected_entities{};
    std::size_t full_mask_entities{};
    std::size_t total_entities{};
    // Game-thread cost of one refresh, in microseconds, smoothed over 64 ticks.  Measured
    // because "the ESP made the game slow" is not actionable on its own: `cameraMicros` and
    // `walkMicros` are five metadata calls against two `runtime_invoke` calls per entity, and
    // they need opposite fixes.
    std::string data_class_histogram;
    std::string label_stage_summary;
    std::string label_samples;
    std::string kind_histogram;
    bool camps_known{};
    std::int32_t camp_monster{};
    std::int32_t camp_player{};
    bool labels_resolved{};
    std::string label_reason;
    std::string camp_stage;
    std::int64_t tick_micros{};
    std::int64_t camera_micros{};
    std::int64_t walk_micros{};
    std::uint64_t ticks_measured{};
    // Broken down walk cost, so "the ESP is the frame rate" can be attributed.
    std::int64_t position_cache_hits{};
    std::int64_t position_cache_rejects{};
    std::int64_t position_reads{};
    std::int64_t position_failures{};
    std::int64_t label_reads{};
    std::int64_t label_cache_hits{};
    std::int64_t label_micros{};
    std::int64_t position_micros{};
    std::int64_t label_cache_rejects{};
};

struct EntityNameSource {
    const char* data_class;      // the live entity's class, as `dataClasses` reports it
    const char* name_space;      // the namespace the CONFIG type lives in
    const char* config_class;    // the `TD*` value type holding the row cursor
    std::size_t config_offset;   // where that struct sits inside the entity data object
};

constexpr EntityNameSource kEntityNameSources[] = {
    {"HeroData", "Azur.Gameplay.Table", "TDHero", 0x3C0},
    {"MonsterData", "Azur.Gameplay.Table", "TDEnemy", 0x484},
    {"NpcData", "Azur.Gameplay.Table", "TDWorldSpawner", 0x488},
    {"PetData", "Azur.Gameplay.Table", "TDPet", 0x458},
};

constexpr std::size_t kEntityNameSourceCount =
    sizeof(kEntityNameSources) / sizeof(kEntityNameSources[0]);

struct ResolvedNameMethods {
    bool attempted{};
    bool ok{};
    const il2cpp::MethodInfo* get_name{};
    const il2cpp::MethodInfo* get_value{};
};

constexpr std::size_t kBaseDataConfigNameOffset = 0x150;

constexpr std::size_t kBaseDataConfigIdOffset = 0x148;

constexpr std::size_t kWorldEntitySpawnerIdOffset = 0x3D0;

constexpr std::size_t kWorldItemRegisteredIdOffset = 0x498;

constexpr std::size_t kWorldItemModelPathOffset = 0x480;

enum WorldNameKey {
    kWorldKeyConfigId,
    kWorldKeySpawnerId,
    // `WorldItemData.registeredID @0x498` -- the last id on the object that has not been tried.
    //
    // Neither `configId` nor `BlueprintId` matched anything, and the lookups that do run fail
    // with `spawnerId=8000000`, which is a round sentinel rather than a spawner -- so those
    // items are never going to answer through the spawner.  `registeredID` is the remaining
    // candidate: its name is what a config registration looks like, and it is read from a field
    // the dump documents.
    kWorldKeyRegisteredId,
};

struct WorldNameTable {
    const char* table_class;
    const char* row_class;
    WorldNameKey key;
    // WHERE `rowOffset` SITS INSIDE THE ROW, WHICH IS NOT ALWAYS 0.
    //
    // This is the field that cost the most rounds of anything in this file.  `TDWorldCollecting`
    // and `TDBattlefieldItem` are `{ int rowOffset; }`, so a bare 4-byte local worked for them --
    // and the first table that actually MATCHED was the one where it does not:
    //
    //     public struct TDWorldSpawner {
    //         private byte m_DynamicLimitCount; // 0x0
    //         private bool m_Limitted;          // 0x1
    //         public int rowOffset;             // 0x4   <- not 0x0
    //     }
    //
    // `GetRowOffset` returned the right number, `get_name` was called with the right method, and
    // the `this` it received had the offset four bytes away from where the callee looks for it.
    // Out came the string "0" -- which reads like a placeholder localisation, and sent three
    // rounds of work after the wrong table.  `TDCommonItem` (0x8) and `TDWorldItem` (0x10) have
    // the same shape, so this is not a one-off.
    //
    // The NPC route never hit it because `NpcData.spwanerData @0x488` is an INLINE row: the
    // address passed is the struct's own, so whatever its layout is, it is correct.
    std::size_t row_offset_field;
    // A two-or-three character alias, because the failure chain for THIRTEEN tables has to fit
    // in one status field.  The previous run's chain was cut off after the fifth table by the
    // width limit -- so the tables that had not been reached were invisible, which is how a
    // working `TDWorldSpawner` result stayed hidden for a round.
    const char* alias;
    const char* note;
};

constexpr WorldNameTable kWorldNameTables[] = {
    {"TDWorldSpawnerTable", "TDWorldSpawner", kWorldKeySpawnerId, 0x4, "Sp", "spawner -- HasKey already hit"},
    {"TDBattlefieldItemTable", "TDBattlefieldItem", kWorldKeyConfigId, 0x0, "BF", "field item -- displayName"},
    {"TDWorldSpawnerTable", "TDWorldSpawner", kWorldKeyRegisteredId, 0x4, "SpR", "spawner by registeredID"},
    {"TDBattlefieldItemTable", "TDBattlefieldItem", kWorldKeyRegisteredId, 0x0, "BFR", "field item by registeredID"},
    {"TDWorldCollectingTable", "TDWorldCollecting", kWorldKeyRegisteredId, 0x0, "ColR", "collecting by registeredID"},
    {"TDWorldCollectingTable", "TDWorldCollecting", kWorldKeyConfigId, 0x0, "Col", "collecting node"},
    {"TDWorldCollectingTypeTable", "TDWorldCollectingType", kWorldKeyConfigId, 0x0, "ColT", "collecting type"},
    // `TDWorldInteract` and `TDWorldHud` are the two tables the last failure chain PROVED are
    // reachable: both resolved and both then reported `row has no get_name/get_displayName`.
    // That was the getter search being too narrow, not the table being wrong.
    //
    //   `TDWorldInteract` -- `desc: LangString`, plus `relatedHuds: List<int>`
    //   `TDWorldHud`      -- `text: LangString`, attached to a world object by type
    //
    // An interactable world object (a mechanism, a teleporter) is exactly the class of item that
    // has no gatherable name and no named spawner, which is the set still showing resource paths.
    {"TDWorldInteractTable", "TDWorldInteract", kWorldKeyConfigId, 0x0, "Int", "interact desc"},
    {"TDWorldHudTable", "TDWorldHud", kWorldKeyConfigId, 0x0, "Hud", "hud text"},
    {"TDWorldInteractTable", "TDWorldInteract", kWorldKeyRegisteredId, 0x0, "IntR", "interact by rid"},
    {"TDWorldHudTable", "TDWorldHud", kWorldKeyRegisteredId, 0x0, "HudR", "hud by rid"},
    {"TDWorldEntityTipsTable", "TDWorldEntityTips", kWorldKeyRegisteredId, 0x0, "Tip", "entity tips"},
    {"TDWorldSpawnerTable", "TDWorldSpawner", kWorldKeyConfigId, 0x4, "SpC", "spawner by configId"},
    {"TDWorldBorthposTable", "TDWorldBorthpos", kWorldKeyConfigId, 0x0, "Bor", "birth position"},
    {"TDWorldAreaLevelCollectTable", "TDWorldAreaLevelCollect", kWorldKeyConfigId, 0x0, "ALC", "area collect"},
    {"TDWorldRangeFilterTable", "TDWorldRangeFilter", kWorldKeyConfigId, 0x0, "Rng", "range filter"},
    {"TDWorldFilterMarkTable", "TDWorldFilterMark", kWorldKeyConfigId, 0x0, "FM", "filter mark"},
    {"TDWorldDifficultyTable", "TDWorldDifficulty", kWorldKeyConfigId, 0x0, "Dif", "difficulty"},
    {"TDWorldMapEffectTable", "TDWorldMapEffect", kWorldKeyConfigId, 0x0, "ME", "map effect"},
    {"TDWorldAreaTable", "TDWorldArea", kWorldKeyConfigId, 0x0, "Ar", "area"},
    {"TDCommonItemTable", "TDCommonItem", kWorldKeyConfigId, 0x8, "CI", "common item"},
};

constexpr std::size_t kWorldNameTableCount =
    sizeof(kWorldNameTables) / sizeof(kWorldNameTables[0]);

struct WorldTableState {
    bool attempted{};
    il2cpp::Il2CppClass* table_class{};
    const il2cpp::MethodInfo* get_instance{};
    const il2cpp::MethodInfo* has_key{};
    const il2cpp::MethodInfo* get_row_offset{};
    const il2cpp::MethodInfo* get_name{};
    const il2cpp::MethodInfo* get_count{};
    il2cpp::Il2CppObject* instance{};
    // WHICH STEP FAILED, because "unresolved" alone is not a diagnosis.
    //
    // The previous run printed `unresolved: TDBattlefieldItemTable` for a class that resolves
    // perfectly -- what was missing was `get_name`, because that row spells its column
    // `displayName` (so the getter is `get_displayName`).  A missing getter read exactly like a
    // missing class, and cost a round.  The failing step is now named at the point it fails.
    const char* failure{};
    // Row count, as reported by the table itself.  A zero here means "this table has no data
    // loaded" and NOT "this table does not contain the key" -- a distinction every previous
    // round could not make, because `HasKey` answers `false` for both.
    std::int32_t count{};
    bool forced{};
};

constexpr int kWorldFailureLogLimit = 4;

struct CollectingName {
    std::int32_t spawner_id{};
    char name[48]{};
};

constexpr std::size_t kCollectingNameCapacity = 512;

struct RememberedWorldName {
    std::uint64_t entity_id{};
    char name[64]{};
};

constexpr std::size_t kRememberedWorldNameCount = 256;

enum WorldLabelSource {
    kWorldLabelTable = 0,
    kWorldLabelConfigName,
    kWorldLabelUnnamed,
    kWorldLabelClassTag,
    kWorldLabelSourceCount,
};

struct Vector3Out {
    double x{};
    double y{};
    double z{};
};

constexpr std::size_t kPositionCacheCapacity = 1024;

constexpr std::uint64_t kPositionRefreshTicks = 4;

struct PositionCacheEntry {
    std::uint64_t id{};
    std::uintptr_t transform{};
    Vector3Out position{};
    std::uint64_t tick{};
};

// THE DIRECT POSITION CALL: one resolved body address, verified before it is ever used.
//
// This is the high-performance path for `UnityEngine.Transform::get_position`, and it exists
// because the reflection route is what made the ESP the frame rate: `il2cpp_runtime_invoke`
// marshals an argument array, installs an exception frame, and boxes the `Vector3` return into
// a fresh managed object, per entity per tick.  The compiled body does none of that -- it is
// the function the runtime's own invoker calls, and it can be called straight from here.
//
// It is NOT a guess about an ABI.  See the profile record `unity.transform.get_position`: the
// recorded prologue bytes of this build's body show the function zeroing twelve bytes through
// RCX before anything else, which is a caller-supplied `Vector3` return buffer -- so the call
// is `void (*)(float* out, void* self, const MethodInfo*)` and the buffer is OURS.  Those same
// bytes are re-read from the live process before the first call, so a build whose body differs
// is refused rather than called.
//
// The three states are terminal once decided: `rejected` is never retried, because whatever
// went wrong (a mismatch, a fault, a disagreeing value) will go wrong again, and retrying a
// call that already faulted is how a diagnostic becomes a crash loop.
enum class DirectPositionState : int {
    untried = 0,
    adopted = 1,
    rejected = 2,
};

struct DirectPositionCall {
    int state{static_cast<int>(DirectPositionState::untried)};
    std::uintptr_t body{};              // the verified entry point
    std::uintptr_t method{};            // the `MethodInfo*`, passed in the third slot
    char reason[256]{};                 // why it was refused, verbatim, for the status document
    std::int64_t reads{};               // calls made through it
    std::int64_t faults{};              // calls that faulted and were contained by SEH
    std::int64_t micros{};              // what those calls cost, to compare against the invoke
};

constexpr std::uint64_t kEntityConsumerIdleMillis = 250;

struct EntityComponentState : ServiceState {
    std::atomic<std::uint64_t> requested_until{};
    std::atomic_bool player_refresh_requested{false};
    // NO `refresh_divisor` FIELD.  There used to be one, whose comment claimed "DEFAULT 8, AND
    // THIS NUMBER IS THE WHOLE FRAME-RATE FIX" -- and NOTHING EVER ASSIGNED IT, so the walk ran
    // every tick while the code said it ran every eighth.  The divisor is now
    // `g_entity_refresh_divisor`, set from the adapter's `UnitySnapshotSamplingOptions`; see its
    // definition beside `RefreshUnityEntityEsp`.
    std::uint64_t refresh_tick{};
    // Whether a consumer was reading entities on the PREVIOUS tick.  Written and read only by the
    // game thread, which is the only thread that runs the walk.  It is what makes "the first walk
    // after a consumer appears is never skipped" true: the demand lease expires by wall time, so
    // the transition from absent to present is the event, not any tick number.
    bool consumer_active{};
    LabelNameSource label_name_sources[kLabelNameSourcesCapacity]{};
    std::size_t label_name_source_count{};
    LabelBinding label_binding{};
    std::string name_probe{};
    std::int64_t label_cache_rejects{};
    LabelCacheEntry label_cache[kLabelCacheCapacity]{};
    std::uint64_t label_resolve_attempts{};
    int label_reads_enabled{1};
    WalkProfile walk_profile{};
    std::int64_t position_cache_rejects{};
    ResolvedNameMethods name_methods[kEntityNameSourceCount];
    std::string name_probe_log{};
    bool name_source_ok[kEntityNameSourceCount]{};
    std::size_t name_probe_index{};
    bool world_item_diagnostic_done{};
    WorldTableState world_tables[kWorldNameTableCount];
    long long world_table_wins[kWorldNameTableCount]{};
    int world_failure_logs{};
    CollectingName collecting_names[kCollectingNameCapacity];
    std::size_t collecting_name_count{};
    int collecting_index_state{};
    std::string collecting_index_log{};
    RememberedWorldName remembered_world_names[kRememberedWorldNameCount];
    std::size_t remembered_world_name_next{};
    long long world_label_counts[kWorldLabelSourceCount]{};
    PositionCacheEntry position_cache[kPositionCacheCapacity]{};
    std::uint64_t position_tick{};
    DirectPositionCall position_call{};
    char probed[8][40]{};
    std::size_t probed_count{};
};

EntityComponentState& ComponentState() {
    return UnityAdapterServices::Current()->Component<EntityComponentState>(
        UnityAdapterComponent::Entities);
}

ServiceState& State() {
    return ComponentState();
}

// The divisor the walk actually uses.
//
// The configured value is the BOOKKEEPING cadence: how often the entity set, its classes, its
// labels and its camp comparisons are rebuilt.  It is NOT the box refresh rate any more -- the
// boxes and the positions they are drawn at are refreshed on every tick by the cheap path.
//
// One override, and it is a safety one: when the direct position call is not in use the walk
// really does cost ~1.29 ms per entity, so an interval of 1 would take the frame rate with it.
// The floor is applied, not the configured value, and the reason is published in the status
// document (`directPosition`) rather than being silent.
constexpr std::uint32_t kDivisorWhenPositionsNeedReflection = 8;

// How many faults the direct call may produce before the walk stops using it.
//
// A fault is contained by the SEH guard, so it is never fatal; but a call that faults repeatedly
// is a call whose preconditions this file has wrong, and continuing to make it would be choosing
// the fast path over the game's stability.  The counter is not a tolerance for a wrong calling
// convention -- the prologue check settles that before the first call -- it is for the object
// that was destroyed between the read and the call.
constexpr std::int64_t kDirectPositionFaultLimit = 8;

bool DirectPositionAvailable() noexcept {
    return EntityDirectPositionEnabled() &&
           ComponentState().position_call.state == static_cast<int>(DirectPositionState::adopted);
}

// The build-profile path, for the one thing in this file that needs per-build data.
//
// An empty path is returned when the adapter is not published (offline tooling that only links
// this file), which the adoption test reports as a refusal like any other.
const std::filesystem::path& EntityBuildProfilePath() {
    static const std::filesystem::path kNoProfile;
    UnityAdapterServices* const services = UnityAdapterServices::Current();
    if (services == nullptr) return kNoProfile;
    return services->ProfilePath();
}

// --- verified offsets -----------------------------------------------------
//
// Named, not inlined, so that a re-dump turns into a small diff of this one block.

// MonoBehaviour -> ... -> BattleInfoMono::battleField
// BattleFieldInfo::enemyInfos
// BattleEnemyInfo::position
// BattleEnemyInfo::camp  (ECampType).  Present but UNUSED: the dump gives the type name
// and no value semantics, so "which value is the enemy" is not established and this file
// must not invent a filter.  See `flags` below.
// BattleSerializable::id -- stable within a session, which is what `entity_id` needs.

// IL2CPP List<T> / Il2CppArray.
// NOTE the two DIFFERENT 0x18s, which is why only one of them is named: a `List<T>`'s count
// is at +0x18, and an `Il2CppArray`'s element data ALSO starts at +0x18 (header 0x10 + bounds
// 0x08).  An array's own length is at +0x18 as well -- the FIELD is at the same offset on the
// array object as the list's count is on the list object, and they are never confused here
// because one is read from a `List` and the other from an array.

// The live world-entity registry.  NOT `BattleConfigManager`: its `m_data` is a
// `BattleConfigData` (CONFIG), so indexing a live `BattleFieldInfo` through it reads an
// unrelated object -- which is exactly what the first shipped attempt did.

// --- the live-instance path, all of it established from the 30 MB dump ------------------
//
// `BattleInfoMono` is a MonoBehaviour, so it has no static instance field and there is no
// static that points at a `BattleFieldInfo` either -- a search of the dump finds exactly
// three mentions of `battleField` and all three are DECLARATIONS, never a static holder.
// The chain therefore has to reach a MonoBehaviour that something else owns, and the
// something else is the game's own singleton:
//
//   Lens.Gameplay.Modules.BigWorld.BattleConfigManager
//       : Lens.Framework.Core.Singleton`1            <-- `protected static T p_instance`
//
// `Singleton<T>.p_instance` is the one static entry point that exists, and it is a
// PROTECTED field rather than a property, so it is read directly instead of through
// `get_Instance()`.  That is not just tidiness: `get_Instance()` returns `T`, and calling a
// generic method that returns a reference type through `il2cpp_runtime_invoke` boxes the
// result -- an allocation per call, from a render-adjacent path.
//
// The field lives on the GENERIC INSTANTIATION, not on `BattleConfigManager` itself, so the
// lookup walks `il2cpp_class_get_parent` once.  This is the Unity/IL2CPP equivalent of the
// UE case, where the same information is reached by walking a `UObject` outward and finding
// a static `UPROPERTY` -- except that IL2CPP resolves it by NAME through the runtime's own
// tables instead of a signature scan, which is the whole reason this port needs no
// signature profile where the UE5 one does.
// BattleConfigManager::m_data (BattleConfigData, a ScriptableObject)
// BattleInfoMono::battleField.  NOTE: the dump reports this at 0x20 on `BattleInfoMono`,
// while the FIRST field of every other Battle* data class is also its 0x20 -- so the
// 0x20 here is not shared with the manager chain above by coincidence, it is the object
// header (0x10) plus one padded slot.

// `UnityEngine.Object`'s m_CachedPtr is the last field of the native part; a managed object
// whose cached pointer is null is a destroyed Unity object still reachable from managed
// code, and reading through it is a guaranteed fault.  Unity 2022.3 puts it here.

// Camp values, read LIVE rather than assumed.  see `ResolveCampValues`.

// --- the camera anchor -------------------------------------------------------------------
//
// `Lens.Framework.Managers.CameraManager : Lens.Framework.Core.SingletonMono\`1` holds the
// game's own idea of the active camera in a STATIC backing field:
//
//     private static UnityEngine.Camera <mainCamera>k__BackingField;  // offset 0x8
//
// That is the same shape as Unity's own `Camera.main` (a static cache the engine fills when a
// camera is enabled), except this one is the GAME's, so it is the one the player is actually
// looking through rather than whatever `Camera.main` would pick.
//
// It is a static field on the DERIVED class here, not on a generic base -- so the hierarchy
// walk below is not needed for this one.  It is used anyway, because a field that moves to a
// base class in a patch should degrade to "not found" rather than to "wrong camera".
// `UnityEngine.Object::m_CachedPtr`.  Unity's managed wrapper is a header plus this one
// pointer; everything about the camera that the engine actually uses lives behind it.

// --- the projection: the engine's own matrices, read, never rebuilt ---------------------
//
// Unity does NOT need a projection matrix reconstructed, and the four attempts that tried are
// why this comment exists.
//
// The dump answers the question directly.  `UnityEngine.Camera` (dump.cs line 2318838) declares
//
//     public Matrix4x4 worldToCameraMatrix       { get; set; }
//     public Matrix4x4 projectionMatrix          { get; set; }
//     public int pixelWidth / pixelHeight        { get; }
//
// and `UnityEngine.Matrix4x4` (dump.cs line 2335192) declares `op_Multiply`.  Both factors are
// therefore READY-MADE PROPERTIES the engine maintains for its own use.  Composing them is a
// multiply, not a derivation.
//
// WHY THE NATIVE LAYOUT IS NOT THE CONCERN IT LOOKS LIKE.  Reading a matrix the obvious way
// means reading Unity's native `Camera` struct, whose layout is undocumented and not in the
// dump (`UnityEngine.Camera`, `Component` and `Transform` all have EMPTY managed bodies --
// every byte of interest is behind `m_CachedPtr`), and a wrong offset there yields a
// plausible-looking matrix that puts boxes near, but not on, the entities.  The PROPERTY is the
// supported way in: it is managed metadata, so `il2cpp_class_get_method_from_name` finds it and
// `il2cpp_runtime_invoke` calls it, with no offset guessed anywhere.
//
// WHAT WAS TRIED INSTEAD, AND WHY IT FAILED.  The previous approach called
// `Camera.WorldToScreenPoint` at four known world points and solved for the matrix rows.  It
// was wrong four times, and every time its own self-check reported success -- because a check
// that projects a point through the matrix UNDER TEST can only ever agree with itself.  The
// measurement itself was also unsound: `WorldToScreenPoint` does not reject a point the camera
// cannot see, it EXTRAPOLATES, so calling it at the camera's own position returned
// (-4702, 2912) in a 2560x1362 viewport and those numbers were fed to the solver as data.
//
// The published raw numbers are what finally named that, which is the argument for publishing
// them rather than only the verdict they support.


// Host-assigned entity categories.  `CabbirdUnityEntityV1::kind` is documented as
// "host-assigned category; 0 = unclassified", so these are the host's own numbering and not
// the game's: the plugin must not have to know what `ECampType` means.
//
// THE VALUES MATCH `CabbirdEntityEspKindV1` IN THE PUBLIC SDK HEADER, and 1/2 keep their original
// meaning so an older plugin still colours enemies and players correctly.
// NPCs, split out of the old enemy bucket: sending `NpcData` and `MonsterData` both to enemy
// put NPCs and monsters in the same category, which is not a fact about the game -- it was a
// shortcut.
// World objects: chests, breakable items, transfer points, and everything else the game's own
// `EWorldObjectType` names but that is not an NPC.  Distinct from UNCLASSIFIED on purpose:
// "this is a world item" and "I do not know what this is" are different statements, and only one
// of them is a bug.

// A walk that reports more than this is not a battle, it is a misread count.  Bounding it
// keeps one bad pointer from turning `Refresh` into a multi-second stall on the game thread.

// A box needs a height.  `BattleEnemyInfo` has NO size field -- so this is an
// approximation and is marked as one here and in the entity's flags.  Distance-independent
// on purpose: a fixed world extent projects to a box that shrinks with distance, which is
// what reads as "correct" to a player, whereas a fixed PIXEL size does not.

// THE WORLD POSITION IS THE OBJECT'S BASE, NOT ITS CENTRE.
//
// A Unity transform's position is the model's pivot, and for this game's characters and world
// objects that pivot sits ON THE GROUND.  An AABB built symmetrically around it spans one unit
// BELOW the ground and one unit above it, which puts the TOP EDGE at the character's mid-body --
// at a level view that reads as "the box's top edge lines up with the character's centre, not
// the top of the head".
//
// A level view is the case that exposes it: with the camera pitch at zero there is no perspective
// foreshortening to hide the error, and the top edge lands wherever `position.y + halfExtent`
// actually is -- the waist.
//
// The evidence for "the pivot is the base" is the report itself.  The top edge sat at the
// character's centre, and the top edge is `position.y + 1.0`, so `position.y` is one unit below
// the visual centre of a roughly two-unit-tall character: the ground.
//
// Raising the box centre by one half-height makes the box STAND ON the ground instead of being
// buried in it: bottom at the position, top at twice the half-extent above it.  This applies to
// every kind uniformly -- a world object's pivot is on the ground too, so its box also stops
// hanging half-underground.


// --- entity display names ---------------------------------------------------------------
//
// WHERE A NAME ACTUALLY LIVES, and why it is not simply a field on the entity.
//
// The chain, all of it read off the dump rather than guessed:
//
//   Entity                                   dump:308601
//     -> data                      @0x18     `BaseData`
//   MonsterData : WorldEntityData : ...      dump:275587
//     -> config                    @0x484    a VALUE type, so it needs its getter: a value-type
//                                            field is stored INLINE and `field_get_value` on a
//                                            struct produces a boxed copy, which is an
//                                            allocation plus a copy of the whole table row
//   TEnemy                                   dump:400000
//     -> name                      @0x18     `LangString`, ALSO a value type (dump:400939)
//   LangString
//     -> value : string            getter that does the localisation lookup
//
// So the name is three calls in, two of them on value types.  That is expensive enough that
// reading it every tick for every entity is not acceptable, and it is why the result is cached
// against the entity id below: a name does not change, so it is read once per entity and then
// never again.
//
// The class is matched by NAME at runtime (`il2cpp_object_get_class` + `il2cpp_class_get_name`)
// rather than by a hardcoded class pointer, because the entity list holds several different
// `BaseData` subclasses and only some of them have a `config`.  A class that is not recognised
// simply produces no label -- which is the honest outcome, and visible, rather than a guess at
// an offset belonging to a different type.

// EVERY entity type that carries a display name -- not just the monster one.
//
// The first version bound `MonsterData` alone and tested `il2cpp_object_get_class(data) ==
// MonsterData`, which produced `labelReads: 0` against 71 live entities: not one of them was a
// monster.  The dump shows the live data classes are SIBLINGS, not one class:
//
//   MonsterData : WorldEntityData   -- TDEnemy  <config>       (0x484)
//   NpcData     : WorldEntityData   -- (no config field of its own)
//   PlayerData  : AliveData         -- TDUnit   unitConfig
//   PetData     : AliveData         -- TDPet    <config>       (0x458)
//   HeroData    : AliveData         -- TDHero   <heroConfig>   (0x3C0)
//
// A single hard-coded class was never going to match a scene that contains the player.  Each
// entry is `(live data class, its config field offset)`, and the first matching class wins.


// Where a name can be read from, one entry per `config` type the game actually uses.
//
// This exists because the game has MORE THAN ONE config type and they do NOT share a layout.  The
// dump alone shows four classes carrying a `LangString` name, at offsets 0x18, 0x18, 0x18 and
// 0x20, in at least two different namespaces:
//
//   TEnemy        Lens.Gameplay.Modules.BigWorld.Config  <name> @ 0x18
//   TEnemy_pack   Lens.Gameplay.Csv                      <name> @ 0x18
//
// and `MonsterData::config` is declared as `TDEnemy` -- a STRUCT whose `name` is a computed
// property rather than a field.  A single class name and a single offset was never going to cover
// that, which is exactly what `labelReason: ... name@0x0` and `labelStages: noName=5` were saying.

// `config -> LangString`, one entry per config type the live data classes point at.

// WHERE `EWorldObjectType worldType` LIVES, PER LIVE DATA CLASS.
//
// A TABLE KEYED BY CLASS NAME RATHER THAN ONE CONSTANT, because the offset is only valid on the
// classes that actually inherit `WorldEntityData`: the dump shows `HeroData : AliveData` and
// `PetData : AliveData` directly (dump.cs:277550 / 282053), so 0x3C0 is NOT their `worldType` --
// on `HeroData` it is `TDHero heroConfig`.  Reusing a base-class offset on a class that does not
// have that base is a real failure mode, not a hypothetical one: in game it shows up as world
// resources drawn in the unclassified colour.
//
// Only the two classes that inherit `WorldEntityData` are listed.  `MonsterData` is deliberately
// absent: it IS a `WorldEntityData`, but its category comes from the stronger class-name branch,
// and leaving it out keeps the table a statement about what has been CHECKED rather than about
// what might work.

// Returns `false` for any class not in the table, which is the point: "unknown class" must not
// silently become "read offset 0x3C0 anyway".
bool WorldTypeFieldOffset(const char* class_name, std::size_t* offset) {
    if (class_name == nullptr) return false;
    for (std::size_t index = 0; index < kWorldTypeFieldCount; ++index) {
        if (std::strcmp(kWorldTypeFields[index].class_name, class_name) == 0) {
            *offset = kWorldTypeFields[index].offset;
            return true;
        }
    }
    return false;
}


// The resolved label binding.  Resolved ONCE, because every step is a runtime-table walk.


// The one-entity name probe's result, published verbatim.  Empty until the probe runs, and
// written on the game thread only -- the same domain that writes every other binding global.

// Entity id -> name.  Fixed capacity, linear probe, no allocation: written and read on the game
// thread only, and it exists so a name is read once per entity for the life of the process
// rather than once per tick.
// Slow-cadence retry counter for the name chain.  See the retry site.


// Whether names are read at all.  The plugin declares its intent through `set_want_labels`; this

// defaults ON because the feature was requested, and the declaration exists so a plugin can turn
// it OFF, not so one has to remember to turn it on.

// --- what the entity walk actually costs, broken down -----------------------------------------
//
// The first counter set said the walk WAS the frame rate.  That is the right kind of number and
// still not an actionable one: the walk does several different things per entity and they have
// different fixes.  These counters split it, because the alternative is another rewrite of the
// wrong part.

// Times the position cache had no free slot.  Non-zero means entities are being re-sampled at the

// refresh rate, which is the whole cost this cache exists to remove.
// Times the label cache had no free slot.  Same failure mode as above: a cache that silently
// refuses to store looks exactly like a cache that does not help.

// Resolve `FieldInfo` -> byte offset via the runtime.  Declared here because the label binding
// resolves a config offset from the runtime when the dump gives no constant for it; defined
// further down with the rest of the field helpers.
std::size_t FieldOffsetOr(const il2cpp::Api& api, il2cpp::Il2CppClass* klass, const char* name,
                          std::size_t fallback, bool* exact);

// Where a field actually IS, asked of the runtime instead of copied out of a dump.
//
// A dump is a SNAPSHOT of one build of this game, and it is stale for exactly the fields this
// chain needs: `noName=5` means a matched class whose `config` read fine but whose `+0x18` was
// zero, i.e. the name field is not at the offset the dump reports.  Metadata offsets move
// between builds; the ONLY authority on where a field lives inside the running process is the
// runtime's own `Il2CppClass`.
//
// These walk the fields of a class and its bases with `il2cpp_class_get_fields`, which is a
// metadata read: nothing here allocates and nothing here calls into managed code.
bool IsStringTypeName(const char* type_name) noexcept {
    return type_name != nullptr && std::strstr(type_name, "String") != nullptr;
}
bool IsInt64TypeName(const char* type_name) noexcept {
    return type_name != nullptr && (std::strcmp(type_name, "long") == 0 ||
                                    std::strcmp(type_name, "System.Int64") == 0);
}
bool IsLangStringTypeName(const char* type_name) noexcept {
    return type_name != nullptr && std::strstr(type_name, "LangString") != nullptr;
}

// A string that is safe to hand to `il2cpp_class_from_name` as a namespace or a simple name.
//
// `il2cpp_type_get_name` reports whatever the metadata says, including generic and nested forms
// such as `TDEnemyTable<TDEnemy, int>`.  Splitting one of those on its last dot and passing the
// halves to a runtime lookup is how a metadata read becomes a crash: the lookup is entitled to
// trust its arguments.  This is a cheap textual check that keeps every malformed name out.
bool IsIdentifierName(const char* text) noexcept {
    if (text == nullptr || *text == 0) return false;
    for (const char* cursor = text; *cursor != 0; ++cursor) {
        const unsigned char ch = static_cast<unsigned char>(*cursor);
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '.';
        if (!ok) return false;
    }
    return true;
}

template <typename Accept>
bool FieldOffsetByType(const il2cpp::Api& api, il2cpp::Il2CppClass* klass, std::size_t* out,
                       Accept accept) {
    if (klass == nullptr || out == nullptr) return false;
    if (api.il2cpp_class_get_fields == nullptr || api.il2cpp_field_get_name == nullptr ||
        api.il2cpp_field_get_offset == nullptr || api.il2cpp_field_get_type == nullptr ||
        api.il2cpp_type_get_name == nullptr) {
        return false;
    }
    for (il2cpp::Il2CppClass* current = klass; current != nullptr;) {
        void* iterator = nullptr;
        for (il2cpp::FieldInfo* field = api.il2cpp_class_get_fields(current, &iterator);
             field != nullptr; field = api.il2cpp_class_get_fields(current, &iterator)) {
            const std::size_t offset = api.il2cpp_field_get_offset(field);
            if (offset == static_cast<std::size_t>(-1)) continue;  // static: not an instance field
            const il2cpp::Il2CppType* const type = api.il2cpp_field_get_type(field);
            const char* const type_name =
                type != nullptr ? api.il2cpp_type_get_name(type) : nullptr;
            if (accept(api.il2cpp_field_get_name(field), type_name)) {
                *out = offset;
                return true;
            }
        }
        if (api.il2cpp_class_get_parent == nullptr) break;
        current = api.il2cpp_class_get_parent(current);
    }
    return false;
}

// The same walk as `FieldOffsetByType`, but it also hands back the DECLARED TYPE NAME of the
// field it found.  That name is how the config class is located without assuming what it is
// called or which namespace it lives in: the field already knows what it points at.
template <typename Accept>
bool FieldOffsetByTypeEx(const il2cpp::Api& api, il2cpp::Il2CppClass* klass, std::size_t* out,
                         std::string* type_name_out, Accept accept) {
    if (klass == nullptr || out == nullptr) return false;
    if (api.il2cpp_class_get_fields == nullptr || api.il2cpp_field_get_name == nullptr ||
        api.il2cpp_field_get_offset == nullptr || api.il2cpp_field_get_type == nullptr ||
        api.il2cpp_type_get_name == nullptr) {
        return false;
    }
    for (il2cpp::Il2CppClass* current = klass; current != nullptr;) {
        void* iterator = nullptr;
        for (il2cpp::FieldInfo* field = api.il2cpp_class_get_fields(current, &iterator);
             field != nullptr; field = api.il2cpp_class_get_fields(current, &iterator)) {
            const std::size_t offset = api.il2cpp_field_get_offset(field);
            if (offset == static_cast<std::size_t>(-1)) continue;
            const il2cpp::Il2CppType* const type = api.il2cpp_field_get_type(field);
            const char* const type_name =
                type != nullptr ? api.il2cpp_type_get_name(type) : nullptr;
            if (accept(api.il2cpp_field_get_name(field), type_name)) {
                *out = offset;
                if (type_name_out != nullptr && type_name != nullptr) *type_name_out = type_name;
                return true;
            }
        }
        if (api.il2cpp_class_get_parent == nullptr) break;
        current = api.il2cpp_class_get_parent(current);
    }
    return false;
}

// The same walk, matching the field NAME against a list of candidates.  Used first for the
// `config`-shaped field, because a name states intent more strongly than a type when both exist.
bool FieldOffsetByName(const il2cpp::Api& api, il2cpp::Il2CppClass* klass,
                       const char* const* names, std::size_t name_count, std::size_t* out) {
    if (names == nullptr || out == nullptr) return false;
    if (api.il2cpp_class_get_fields == nullptr || api.il2cpp_field_get_name == nullptr ||
        api.il2cpp_field_get_offset == nullptr) {
        return false;
    }
    for (il2cpp::Il2CppClass* current = klass; current != nullptr;) {
        void* iterator = nullptr;
        for (il2cpp::FieldInfo* field = api.il2cpp_class_get_fields(current, &iterator);
             field != nullptr; field = api.il2cpp_class_get_fields(current, &iterator)) {
            const std::size_t offset = api.il2cpp_field_get_offset(field);
            if (offset == static_cast<std::size_t>(-1)) continue;
            const char* const field_name = api.il2cpp_field_get_name(field);
            if (field_name == nullptr) continue;
            for (std::size_t index = 0; index < name_count; ++index) {
                if (std::strcmp(field_name, names[index]) == 0) {
                    *out = offset;
                    return true;
                }
            }
        }
        if (api.il2cpp_class_get_parent == nullptr) break;
        current = api.il2cpp_class_get_parent(current);
    }
    return false;
}

bool ResolveLabelBinding(const il2cpp::Api& api, LabelBinding* binding, std::string* reason) {
    // ONLY the class-identification entries, because this chain reads FIELDS and calls nothing.
    // Requiring the call entries would keep a dependency the implementation does not have, and a
    // missing call entry would disable a feature that never calls anything at all.
    if (api.il2cpp_class_from_name == nullptr || api.il2cpp_object_get_class == nullptr) {
        *reason = "the IL2CPP API table is missing the class-identification entries";
        return false;
    }
    il2cpp::Il2CppImage* const image = il2cpp::FindImage(kEntityImage);
    if (image == nullptr) {
        *reason = std::string("image ") + kEntityImage + " is not loaded";
        return false;
    }
    // `LangString` is where the name text finally lives.  Read as a field; never called.
    il2cpp::Il2CppClass* const lang =
        api.il2cpp_class_from_name(image, "Lens.Gameplay.Modules.BigWorld.Config", "LangString");
    if (lang == nullptr) {
        *reason = "LangString was not found; entity names are unavailable";
        return false;
    }

    // EVERY live data class that carries a `config`-shaped field, with the offset the DUMP
    // reports for that field.  The class names come from the dump and the offsets sit next to
    // them, so a mismatch shows up in a diff instead of hiding in a literal.
    //
    // The offsets are GONE from this table on purpose.  A dump offset is a fact about one build,
    // and `noName=5` in the field proved the dump's `TEnemy::<name>` offset is stale for this
    // one.  What remains here is only what a dump can be trusted for: the CLASS NAMES.
    struct Candidate {
        const char* name;
        const char* const* field_names;
        std::size_t field_name_count;
    };
    // `config` is the base name, and the compiler-generated BACKING FIELD is what actually holds
    // it: the dump reads `private TDEnemy <config>k__BackingField; // 0x484`, NOT `config`.  A
    // plain `strcmp("config")` therefore matched nothing and the offset fell through to the type
    // search, which is how `mismatch`/`unmatched` happened with a class that was plainly present.
    static const char* const kConfigNames[]{
        "config",
        "<config>k__BackingField",
        "heroConfig",
        "<heroConfig>k__BackingField",
        "petConfig",
        "<petConfig>k__BackingField",
        "unitConfig",
        "<unitConfig>k__BackingField",
    };
    static constexpr Candidate kCandidates[]{
        {"MonsterData", kConfigNames, 8},
        {"PetData", kConfigNames, 8},
        {"HeroData", kConfigNames, 8},
        {"NpcData", kConfigNames, 8},
        {"PlayerData", kConfigNames, 8},
    };
    binding->source_count = 0;
    // The config type names found so far, so the same type is not walked twice.
    ComponentState().label_name_source_count = 0;
    std::string seen_types;
    std::string found;
    for (const Candidate& candidate : kCandidates) {
        il2cpp::Il2CppClass* const klass = api.il2cpp_class_from_name(
            image, "Lens.Gameplay.Modules.BigWorld", candidate.name);
        if (klass == nullptr) continue;
        // A NAME match first: `config` states the intent outright wherever it survives.
        std::size_t off_config = 0;
        bool matched_name = FieldOffsetByName(api, klass, candidate.field_names,
                                              candidate.field_name_count, &off_config);
        if (!matched_name) {
            // No field of that name.  Fall back to the TYPE: any instance field whose declared
            // type names an enemy config is the config, whatever the field is called.  `Enemy`
            // rather than `TEnemy` because the real type is `TDEnemy`, which the narrower
            // substring did not match -- the same miss that left the name table empty.
            matched_name = FieldOffsetByType(api, klass, &off_config,
                                             [](const char*, const char* type_name) {
                                                 return type_name != nullptr &&
                                                        std::strstr(type_name, "Enemy") != nullptr;
                                             });
        }
        if (!matched_name || off_config == 0) continue;
        if (binding->source_count >= kLabelSourceCapacity) break;
        LabelSource& source = binding->sources[binding->source_count++];
        source.data_class = reinterpret_cast<std::uintptr_t>(klass);
        source.off_config = off_config;
        std::snprintf(source.class_name, sizeof(source.class_name), "%s@0x%zX", candidate.name,
                      off_config);
        if (!found.empty()) found += " ";
        found += source.class_name;

        // WHAT TYPE DOES THIS config FIELD POINT AT?  Taken per source instead of once from
        // `sources[0]`: the whole reason this chain failed twice is that the config types DIFFER
        // between data classes, so the name offset for one is not the name offset for another.
        std::string config_type_name;
        std::size_t probe = 0;
        // THE TYPE TEST IS "DOES IT CARRY A LangString", NOT "DOES IT SAY Enemy".
        //
        // The first version tested `strstr(reported, "Enemy")`, which is why `labelReason` listed
        // only `names[TEnemy@0x18 TEnemy_pack@0x18]`: `HeroData.heroConfig` is a `TDHero` and
        // `PetData`'s config is a `TDPet`, and neither name contains "Enemy", so both data classes
        // were discarded at this line.  The user has SIX heroes in a typical scene and no monsters
        // at all, so the one source that survived was the one that could never fire.
        //
        // What actually matters here is not the type's name but whether the thing we are about to
        // bind can produce a name, and the check immediately below already establishes that by
        // finding a `LangString` field.  A name-based filter can only ever be as wide as the
        // vocabulary someone thought of; "has a LangString" is the property the code depends on.
        if (!FieldOffsetByTypeEx(api, klass, &probe, &config_type_name,
                                 [](const char*, const char* reported) {
                                     return IsLangStringTypeName(reported) ||
                                            (reported != nullptr &&
                                             std::strstr(reported, "Enemy") != nullptr);
                                 })) {
            // A CONFIG FIELD THAT REPORTS NO TYPE STILL GETS A CHANCE, because a `TD*` struct is
            // inlined into its data class and may not report a usable type name at all.  The
            // name-based branch above already found `off_config`; the type is only needed to
            // locate the LangString carrier, and the fallback table below does that by class name.
            if (off_config == 0) continue;
            config_type_name = candidate.field_names[0];
        }
        if (seen_types.find(config_type_name) != std::string::npos) continue;
        seen_types += config_type_name;

        // A GENERIC or nested type name is not a class name, and passing its halves to a runtime
        // lookup is the shape of metadata read that crashes rather than fails.  Every part has to
        // survive a strict identifier check before it is used.
        if (!IsIdentifierName(config_type_name.c_str())) continue;
        std::string name_space;
        std::string simple = config_type_name;
        const std::size_t dot = config_type_name.rfind('.');
        if (dot != std::string::npos) {
            name_space = config_type_name.substr(0, dot);
            simple = config_type_name.substr(dot + 1);
        }
        if (!IsIdentifierName(simple.c_str())) continue;
        if (!name_space.empty() && !IsIdentifierName(name_space.c_str())) continue;
        il2cpp::Il2CppClass* const config_class =
            api.il2cpp_class_from_name(image, name_space.c_str(), simple.c_str());
        if (config_class == nullptr) continue;
        // The LangString field inside THAT config type, at whatever offset it happens to live.
        std::size_t off_name = 0;
        if (!FieldOffsetByType(api, config_class, &off_name,
                               [](const char*, const char* reported) {
                                   return IsLangStringTypeName(reported);
                               })) {
            continue;
        }
        if (ComponentState().label_name_source_count >= kLabelNameSourcesCapacity) break;
        LabelNameSource& entry = ComponentState().label_name_sources[ComponentState().label_name_source_count++];
        entry.klass = reinterpret_cast<std::uintptr_t>(config_class);
        entry.off_name = off_name;
        // The class is keyed by SIMPLE NAME rather than by pointer, because the object's class
        // and the class returned by a metadata lookup are not guaranteed to be the same pointer
        // and the name is the stable identity across that difference.
        std::snprintf(entry.class_name, sizeof(entry.class_name), "%s", simple.c_str());
    }

    // FALLBACK: the config classes the dump confirms exist, used when the `config` field's own
    // reported type led nowhere.
    //
    // This path is not a guess about layout.  Each class is resolved from metadata and then
    // CHECKED for a `LangString` field by type, so a class without one is simply rejected.  It is
    // needed because `MonsterData::config` reports `TDEnemy` -- a struct whose `name` is a computed
    // property, not a field -- so the field's own type name can lead to a type that carries no
    // name at all, while the real carrier (`TEnemy`) has to come from the class registry.
    if (ComponentState().label_name_source_count == 0) {
        static constexpr struct {
            const char* name_space;
            const char* name;
        } kKnownConfigClasses[]{
            {"Lens.Gameplay.Modules.BigWorld.Config", "TEnemy"},
            {"Lens.Gameplay.Csv", "TEnemy_pack"},
        };
        for (const auto& known : kKnownConfigClasses) {
            if (ComponentState().label_name_source_count >= kLabelNameSourcesCapacity) break;
            if (!IsIdentifierName(known.name_space) || !IsIdentifierName(known.name)) continue;
            il2cpp::Il2CppClass* const candidate =
                api.il2cpp_class_from_name(image, known.name_space, known.name);
            if (candidate == nullptr) continue;
            std::size_t off_name = 0;
            if (!FieldOffsetByType(api, candidate, &off_name,
                                   [](const char*, const char* reported) {
                                       return IsLangStringTypeName(reported);
                                   })) {
                continue;  // exists, but carries no name: rejected rather than trusted
            }
            LabelNameSource& entry = ComponentState().label_name_sources[ComponentState().label_name_source_count++];
            entry.klass = reinterpret_cast<std::uintptr_t>(candidate);
            entry.off_name = off_name;
            std::snprintf(entry.class_name, sizeof(entry.class_name), "%s", known.name);
            if (!found.empty()) found += " ";
            found += known.name;
        }
    }
    // `LangString`: a string field and a 64-bit integer field.  `Key` comes before `Value` in
    // the declaration order the dump shows, so the FIRST string is the group and the FIRST
    // integer is the key -- and neither is assumed to be at a fixed offset any more.  The live
    // run confirmed this resolution agrees with the dump (`group@0x10 key@0x18`), so this part
    // of the chain has never been the problem.
    std::size_t off_group = 0;
    std::size_t off_key = 0;
    FieldOffsetByType(api, lang, &off_group, [](const char*, const char* type_name) {
        return IsStringTypeName(type_name);
    });
    FieldOffsetByType(api, lang, &off_key, [](const char*, const char* type_name) {
        return IsInt64TypeName(type_name);
    });

    binding->object_get_class = reinterpret_cast<std::uintptr_t>(api.il2cpp_object_get_class);
    binding->lang_string_class = reinterpret_cast<std::uintptr_t>(lang);
    // SUPERSEDED by the per-class name table, and left at 0 so nothing reads it by accident.
    binding->off_config_name = 0;
    binding->off_lang_group = off_group;
    binding->off_lang_key = off_key;
    // NOT RESOLVED WITHOUT A SINGLE NAME SOURCE.
    //
    // `resolved = true` is what STOPS the retry, and the previous version set it even when the
    // name table came out empty.  The first resolution happens on the first tick of the process
    // -- in the loading scene, where only `BattleFieldData` exists -- so MonsterData and the rest
    // were not loaded yet, the table came out empty, and the binding then refused to ever look
    // again.  That is why `dataClasses` listed `MonsterData=2` while the label chain reported
    // `names[none]`: the classes appeared after the single attempt had been spent.
    binding->resolved = ComponentState().label_name_source_count != 0;
    // The resolved table goes into the reason string so it is readable from `status` without a
    // debugger.  "Which classes did it bind?" is the question that cost one round here, and
    // "which config types and name offsets did it find?" cost the next one.
    std::string name_table;
    for (std::size_t index = 0; index < ComponentState().label_name_source_count; ++index) {
        if (!name_table.empty()) name_table += " ";
        name_table += ComponentState().label_name_sources[index].class_name;
        char offset[24]{};
        std::snprintf(offset, sizeof(offset), "@0x%zX", ComponentState().label_name_sources[index].off_name);
        name_table += offset;
    }
    char tail[160]{};
    std::snprintf(tail, sizeof(tail), " group@0x%zX key@0x%zX names[%s]", off_group, off_key,
                  name_table.empty() ? "none - will retry" : name_table.c_str());
    *reason = (found.empty() ? std::string("no name-carrying data class was found")
                             : ("bound: " + found)) +
              tail;
    // Always true so the caller keeps the reason; whether the chain is USABLE is `resolved`.
    return true;
}

// A boxed return from `il2cpp_runtime_invoke`, or null with `reason` set.
il2cpp::Il2CppObject* InvokeBoxed(const il2cpp::Api& api, std::uintptr_t method,
                                  std::uintptr_t self) {
    if (method == 0) return nullptr;
    il2cpp::Il2CppObject* exception = nullptr;
    il2cpp::Il2CppObject* const returned = api.il2cpp_runtime_invoke(
        reinterpret_cast<const il2cpp::MethodInfo*>(method),
        reinterpret_cast<il2cpp::Il2CppObject*>(self), nullptr, &exception);
    if (exception != nullptr) return nullptr;
    return returned;
}

// --- the name cache ---------------------------------------------------------------------------
//
// A name costs several runtime-table reads, and the answer for a given entity never changes, so
// reading it once per entity for the life of the process is the entire reason a labelled ESP is
// affordable.  Lookup and store are both a bounded linear probe over a fixed array: no
// allocation and no locking, because both run on the game thread inside the walk.
//
// An EMPTY name is cached too.  An entity whose class is not in the binding table would otherwise
// be re-probed on every single tick forever, which is exactly the kind of cost that looks like
// "the labels are expensive" when it is really "the failure is expensive".
const LabelCacheEntry* FindCachedLabel(std::uint64_t id) noexcept {
    const std::size_t slot = static_cast<std::size_t>(id) % kLabelCacheCapacity;
    for (std::size_t probe = 0; probe < kLabelCacheCapacity; ++probe) {
        const LabelCacheEntry& entry = ComponentState().label_cache[(slot + probe) % kLabelCacheCapacity];
        if (entry.id == id) return &entry;
        if (entry.id == 0) return nullptr;  // empty slot ends the probe
    }
    return nullptr;
}

void StoreCachedLabel(std::uint64_t id, const char* text, std::uint32_t size) noexcept {
    if (id == 0 || size >= sizeof(ComponentState().label_cache[0].text)) {
        ++ComponentState().label_cache_rejects;
        return;
    }
    const std::size_t slot = static_cast<std::size_t>(id) % kLabelCacheCapacity;
    for (std::size_t probe = 0; probe < kLabelCacheCapacity; ++probe) {
        LabelCacheEntry& entry = ComponentState().label_cache[(slot + probe) % kLabelCacheCapacity];
        if (entry.id == id || entry.id == 0) {
            entry.id = id;
            std::memcpy(entry.text, text, size);
            entry.text[size] = 0;
            entry.size = size;
            return;
        }
    }
    ++ComponentState().label_cache_rejects;
}

// Drops every cached label, so the next walk recomputes them.
//
// NEEDED WHEN A NAME CHAIN STARTS WORKING MID-SESSION.  A label that was computed before its
// class's probe succeeded is cached as the class tag (`PetData#12345`) for the life of the
// process, and the cache is what makes a labelled ESP affordable -- so the fix is not to disable
// it, it is to invalidate it exactly once, on the tick the probe proves the chain works.
// Without this, `PetData` keeps its fallback label forever even though the chain resolves.
//
// No lock: both this and the cache are touched on the game thread inside the walk.
void InvalidateLabelCache() noexcept {
    std::memset(ComponentState().label_cache, 0, sizeof(ComponentState().label_cache));
}

// Copy a managed `string` into `out`, UTF-8 encoded.  Returns the byte count.
std::uint32_t ReadManagedString(const il2cpp::Api& api, il2cpp::Il2CppObject* text, char* out,
                                std::uint32_t capacity) {
    if (text == nullptr || out == nullptr || capacity == 0 || api.il2cpp_string_chars == nullptr) {
        return 0;
    }
    // BOUNDED ON PURPOSE.  `il2cpp_string_length` reads a header field of an object the game
    // may be mutating; a garbage length must truncate, not run off the end of a buffer, so
    // the cap below is what keeps that read from running off the end.
    const std::int32_t length = api.il2cpp_string_length(text);
    if (length <= 0) return 0;
    const std::uint32_t limit =
        static_cast<std::uint32_t>(length) < capacity - 1 ? static_cast<std::uint32_t>(length)
                                                          : capacity - 1;
    const il2cpp::Il2CppChar* const chars = api.il2cpp_string_chars(text);
    if (chars == nullptr) return 0;
    // UTF-16 -> UTF-8, done by hand because <codecvt> is deprecated and the strings here are
    // short.  Surrogate pairs are handled: a name with an emoji or a CJK extension character
    // would otherwise be cut in half and render as a replacement glyph.
    std::uint32_t written = 0;
    for (std::uint32_t index = 0; index < limit;) {
        std::uint32_t code = chars[index++];
        if (code >= 0xD800 && code <= 0xDBFF && index < limit) {
            const std::uint32_t low = chars[index];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++index;
            }
        }
        if (code < 0x80) {
            if (written + 1 >= capacity) break;
            out[written++] = static_cast<char>(code);
        } else if (code < 0x800) {
            if (written + 2 >= capacity) break;
            out[written++] = static_cast<char>(0xC0 | (code >> 6));
            out[written++] = static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            if (written + 3 >= capacity) break;
            out[written++] = static_cast<char>(0xE0 | (code >> 12));
            out[written++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out[written++] = static_cast<char>(0x80 | (code & 0x3F));
        } else {
            if (written + 4 >= capacity) break;
            out[written++] = static_cast<char>(0xF0 | (code >> 18));
            out[written++] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out[written++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out[written++] = static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    out[written] = '\0';
    return written;
}

// Read one entity's display name, if the chain applies to it.
//
// The class test first: `Entity::data` holds whatever `BaseData` subclass this entity is, and
// only `MonsterData` is known to carry a localised config name.  Anything else returns no
// label rather than reading `MonsterData`'s offset off a different type -- a wrong offset there
// does not crash, it produces a plausible string from the middle of another object.
// THE NAME, READ AS FIELDS -- ZERO MANAGED CALLS, which is the whole point.
//
// The obvious chain is three `runtime_invoke` calls (`MonsterData::get_config`,
// `TEnemy::get_name`, `LangString::get_value`) with two value-type boxings, and that is exactly
// the shape of call this file already measured at 2.5 us each and already disabled a game crash
// over.  Nothing about it needs a call.  The dump gives every field it touches:
//
//   MonsterData::<config>  -> TDEnemy     (0x484)  the enemy's static config object
//   TEnemy::<name>         -> LangString  (0x18)   the name, as a localisation key
//   LangString::group      -> string      (0x10)
//   LangString::langKey    -> long        (0x18)
//
// Four pointer reads, all through the validated memory path, and the offsets are constants with
// provenance rather than guesses.  `config` is typed `TDEnemy`, a class NESTED inside
// `TDEnemyTable`, so it is not addressable by the `(namespace, name)` lookup used elsewhere in
// this file -- reading it by offset sidesteps that instead of adding a second lookup shape.
//
// `LangString` carries only `group` + `langKey`; the LOCALISED text comes from the game's own
// `value` getter.  That getter is deliberately not called: the key is stable and unique, so it
// identifies an enemy exactly as well as its display name, and costs no managed call at all.
// Resolving a display name later would be one call per DISTINCT key, cached -- the shape of
// solution the sibling UE5 project ships.

// The resolved binding.  Resolved ONCE and cached, because every step below is a
// runtime-table walk.
//
// The per-frame work must not re-resolve anything: `il2cpp_class_from_name` searches an
// image's class table and `il2cpp_class_get_field_from_name` walks a class's field list, so
// doing either per frame on the GAME thread -- the thread a player is looking at -- is a
// cost paid sixty times a second for an answer that cannot change while a class is loaded.
//
// What CAN change is the INSTANCE, so `p_instance_address` is cached (the address of the
// static slot, which is fixed for the life of the class) while the VALUE at that address is
// re-read every frame.  A manager that is torn down and recreated -- a scene transition,
// which is exactly when a battle starts -- is therefore picked up automatically, whereas
// caching the instance pointer would have frozen the first one ever seen.


// Read `size` bytes out of the target process.
//
// Goes through `mem::ReadMemory` rather than a bare memcpy on purpose: an IL2CPP walk
// follows pointers taken out of managed memory, and one of them can legitimately be stale
// (an object freed on another thread between two reads).  `mem::ReadMemory` is
// SEH-guarded, so such a pointer yields "unreadable" instead of taking the game down --
// the same discipline the dumper's `contained_faults` counter exists to enforce one layer
// down.  `size` must stay well under its 4096-byte cap; every read here is at most 12.
// Read `size` bytes out of the target process.
//
// Goes through `mem::ReadMemory` rather than a bare memcpy on purpose: an IL2CPP walk
// follows pointers taken out of managed memory, and one of them can legitimately be stale
// (an object freed on another thread between two reads).  `mem::ReadMemory` is
// SEH-guarded, so such a pointer yields "unreadable" instead of taking the game down --
// the same discipline the dumper's `contained_faults` counter exists to enforce one layer
// down.
//
// WAS CAPPED AT 12 BYTES, WHICH IS WHY THE ENEMY WALK COULD NOT EXIST.
//
// The cap was a leftover from when the only reads were a Vector3 and two ints, and every
// caller happened to fit under it.  The walk this file now performs needs three doubles
// at a time from a `BattleEnemyInfo`, and the cap turned that into "unreadable" with no
// explanation -- a self-inflicted limit masquerading as a runtime failure.  The bound now
// lives in `mem::ReadMemoryInto`, which is the layer that actually knows the limit, and
// callers pass explicitly-sized reads.
bool ReadMemory(std::uintptr_t address, void* destination, std::size_t size) {
    if (size == 0) return false;
    return mem::ReadMemoryInto(address, destination, size);
}

template <typename T>
bool ReadValue(std::uintptr_t address, T* value) {
    return ReadMemory(address, value, sizeof(T));
}

// Find a field by walking the class and then its bases.
//
// `Singleton<T>.p_instance` is declared on the generic BASE, not on the class the dump names
// as deriving from it, so a lookup that only consults `klass` fails with "field not found"
// on a field that is plainly visible in the dump.  Walking parents is what makes the
// difference, and it is the same walk `il2cpp::FieldOffset` already does for INSTANCE
// fields -- which is exactly why it was easy to assume the static side did it too.  It does
// not: `il2cpp_field_get_offset` reports an instance offset and is meaningless for a static.
//
// THIS FUNCTION USED TO FILTER OUT CANDIDATES WITH A NON-(-1) OFFSET, WHICH FOUND NOTHING.
//
// The filter was an attempt to distinguish a static field from an instance field of the same
// name, using `il2cpp_field_get_offset(field) != (size_t)-1` as the static test.  It is
// wrong on this runtime, and it failed for `p_instance` AND for
// `CameraManager::<mainCamera>k__BackingField` -- two unrelated classes and two unrelated
// static fields -- which is what identified it as systematic rather than a bad name:
//
//     "no static `p_instance` on BattleConfigManager or any of its bases"
//     "no static `<mainCamera>k__BackingField` on CameraManager"
//
// Both were reported by the diagnostic this file publishes, on the first run that had them.
// The filter is gone.  Name matching alone is sufficient here because every caller asks for
// a name that exists ONLY as a static on these types, and a wrong-but-instance field would
// be caught by the read below rather than silently used: `il2cpp_field_static_get_value` on
// an instance field does not produce a plausible pointer.
il2cpp::FieldInfo* FindStaticFieldInHierarchy(
    const il2cpp::Api& api, il2cpp::Il2CppClass* klass, const char* name) {
    for (il2cpp::Il2CppClass* current = klass; current != nullptr;
         current = api.il2cpp_class_get_parent != nullptr
             ? api.il2cpp_class_get_parent(current)
             : nullptr) {
        if (il2cpp::FieldInfo* const field =
                api.il2cpp_class_get_field_from_name(current, name)) {
            return field;
        }
    }
    return nullptr;
}

// Read a STATIC field through the runtime's own accessor.
//
// NOT `il2cpp_class_get_static_field_data() + il2cpp_field_get_offset()`.
//
// That pair is the obvious way to do this and it is wrong: `il2cpp_field_get_offset`
// reports an INSTANCE offset and returns -1 for a static field, so the sum is
// `static_base - 1` -- a read one byte below the static block, which either faults or
// returns the last byte of the previous field.  It is wrong in the quiet direction, which
// is the dangerous one.  `il2cpp_field_static_get_value` is the runtime's own accessor: it
// THE NAME, READ AS FIELDS -- ZERO MANAGED CALLS.
//
// The obvious chain is three `runtime_invoke` calls (`get_config`, `get_name`, `get_value`) with
// two value-type boxings, which is exactly the shape of call this file already measured at 2.5 us
// each and already disabled a game crash over.  Nothing about it needs a call: the dump gives
// every field it touches, so four pointer reads through the validated memory path are enough.
//
// `LangString` carries only `group` + `langKey`; the LOCALISED text comes from the game's own
// `value` getter.  That getter is deliberately NOT called -- the key is stable and unique, so it
// identifies an enemy exactly as well as a display name does, at no managed-call cost.  Reading a
// real display name later would be one call per DISTINCT key, cached: the shape of solution the
// sibling UE5 project ships.
// NO REFLECTION FALLBACK.  There was one here, and it crashed the game on the first launch --
// the same outcome as every previous time an unverified managed-call path was enabled in this
// process.  `il2cpp_runtime_invoke` is the runtime's supported entry point in principle, but
// "supported" is not "exercised": this chain had never once completed a call in-game, and a
// safety net that has never been tested is not a safety net, it is an untested call on the hot
// path of somebody else's process.
//
// The rule this file keeps re-learning: a managed call is enabled only after it has been shown
// to work, and the way to show that is ONE call on ONE entity with the result reported, never a
// default that runs for every entity on every refresh.
std::uint32_t WriteClassLabel(char* out, std::uint32_t capacity, const char* data_class,
                              std::uint64_t id);

// ============================================================================
// THE LABEL, AND WHY THIS FUNCTION NO LONGER TOUCHES THE GAME'S CONFIG OBJECT
// ============================================================================
//
// This function used to walk: entity -> `<config>k__BackingField` -> `il2cpp_object_get_class`
// on that pointer -> match the class name against a table -> read a `LangString` -> read a
// localisation key.  That chain crashes the game, and it is the chain itself that does it: while
// the label block was dead code (its gate read an `entities` vector that had not been filled yet)
// this function never ran and the game was stable; once the chain ran, the game crashed within a
// minute.
//
// The prime suspect is `il2cpp_object_get_class` on a pointer read from a field declared
// `TDEnemy`, which the dump shows is a VALUE TYPE (`struct TDEnemy { int rowOffset; }`) rather
// than the `Il2CppObject` that call requires.  It has been safe on every DATA object this file
// touches -- which is exactly why it looked safe here too.
//
// A name is decoration; a crash is not.  The label is therefore built from facts this file
// already holds and has published without incident: the entity's own data class name (from
// `il2cpp_object_get_class` + `il2cpp_class_get_name` on a real managed object -- the
// `dataClasses` histogram) and the entity id.
//
// That yields `MonsterData#1234`: per-entity, readable, and produced with ZERO additional IL2CPP
// calls and ZERO dereferences of game memory beyond what already worked.
//
// TO GET THE LOCALISED NAME BACK, the config-class lookup must be REMOVED rather than
// re-enabled.  The binding already resolved `off_name`, so the class of the config object is not
// needed to choose the offset -- resolution did that.  Read the LangString at the binding's own
// offset, verify on ONE entity, and only then ship it.
// ============================================================================
// THE ONE-ENTITY NAME PROBE.  READS ONE ENTITY'S CONFIG CHAIN, ONCE PER PROCESS.
// ============================================================================
//
// The real name lives on the far side of the config pointer, and that is the side that crashed
// three builds.  Rather than switch the whole tick back onto it, this probe walks ONE entity's
// chain ONCE and publishes every step separately:
//
//   step 1  the config pointer at the data class's resolved offset (MonsterData: 0x484)
//   step 2  whether that pointer looks like a managed object (its first word is a class pointer)
//   step 3  the class name the runtime reports for that object  <-- the call under suspicion
//   step 4  the `LangString` at the offset the binding table resolved for that class
//   step 5  the managed `group` string at LangString+0x10, via `ReadManagedString`
//
// Every branch names itself, so "no name" cannot come back without saying which read failed.
// The pointer plausibility test in step 2 runs BEFORE step 3's runtime call: if the config field
// does not hold an object at all, the probe reports that and stops, instead of handing an invalid
// pointer to `il2cpp_object_get_class`.
bool IsManagedObjectPointer(std::uintptr_t candidate) {
    if (candidate == 0 || (candidate % alignof(void*)) != 0) return false;
    // A managed object begins with a pointer to its Il2CppClass, and a class begins with the
    // image pointer.  Both land in committed, readable memory, so both words can be read and
    // compared against each other -- a random integer would not survive this.
    std::uintptr_t klass = 0;
    if (!ReadValue(candidate, &klass) || klass == 0) return false;
    if ((klass % alignof(void*)) != 0) return false;
    std::uintptr_t image = 0;
    if (!ReadValue(klass, &image) || image == 0) return false;
    return (image % alignof(void*)) == 0;
}

// Whether a class still needs the name probe, and the per-class deduplication behind it.
//
// ONLY THESE THREE ARE PROBED.  They are the classes the dump shows carrying a config field,
// and probing any other class produces a null that looks like a failure while being the correct
// answer -- `PlayerData config@0x338 -> config=NULL` was exactly that, and it cost a round.
//
// The list is a preference ORDER as well as a filter: if the walk meets a MonsterData it is
// probed first, so the report leads with the class whose config type the binding resolved by
// name (`TEnemy`).
bool NameProbeShouldRun(const char* class_name) {
    if (class_name == nullptr || class_name[0] == 0) return false;
    const bool interesting = std::strcmp(class_name, "MonsterData") == 0 ||
                             std::strcmp(class_name, "HeroData") == 0 ||
                             std::strcmp(class_name, "PetData") == 0;
    if (!interesting) return false;
    // Deduplicated by name, so each class is probed exactly once per process.
    auto& probed = ComponentState().probed;
    auto& probed_count = ComponentState().probed_count;
    for (std::size_t index = 0; index < probed_count; ++index) {
        if (std::strcmp(probed[index], class_name) == 0) return false;
    }
    if (probed_count >= 8) return false;
    std::snprintf(probed[probed_count], sizeof(probed[0]), "%s", class_name);
    ++probed_count;
    return true;
}

// ============================================================================
// THE NAME, VIA `il2cpp_runtime_invoke` RATHER THAN A RAW POINTER DEREFERENCE
// ============================================================================
//
// WHAT WENT WRONG BEFORE, STATED PRECISELY.
//
// Three builds (`9bbaced`, `314324d`, `b16d728`) died here, and the cause was not "reading a
// name is unsafe".  It was a TYPE error with two halves, both now measured rather than assumed:
//
//   1. `<heroConfig>k__BackingField` (HeroData@0x3C0, dump.cs:277550) is declared `TDHero`,
//      which is a STRUCT -- `public struct TDHero { int rowOffset; }`.  A value-type field is
//      stored INLINE, so reading eight bytes there yields an INTEGER, never a pointer.  Live:
//      0x2BBCDB.  The old code then handed that integer to `il2cpp_object_get_class`, which
//      dereferences its argument twice (klass, then image) -- a wild read.
//
//   2. `TDHero.get_name()` RETURNS `LangString` BY VALUE.  On Win64 a 16-byte struct return
//      uses a hidden return buffer, so a hand-written detour that declares the return `void*`
//      never allocates one and the callee writes 16 bytes to whatever `rdx` happened to hold.
//
// `il2cpp_runtime_invoke` fixes both at once and is the runtime's own supported entry point:
// it boxes the instance, allocates the return buffer, and unboxes the result.  It is already
// used in this file for `Transform::get_position` and `Camera::get_worldToCameraMatrix`, both
// of which return structs, and both of which work.
//
// THE SHAPE OF THE CALL, and why the instance pointer is safe:
//
//   `get_heroConfig` is invoked on `(&heroConfig - 0x3C0)`, which is the address of the managed
//   HeroData object ITSELF.  That is a real GC-tracked object on the game thread -- the same
//   thread and the same object the entity walk already dereferences -- so passing it does not
//   introduce a new lifetime hazard.  What is NOT safe, and is not done, is treating
//   `data + 0x3C0` (the inline struct) as an object pointer.// ============================================================================
// THE NAME, WITHOUT EVER LOCATING THE TABLE
// ============================================================================
//
// THE INSIGHT THAT REMOVES THE LAST UNKNOWN.
//
// `TDHero.get_name()` returns `LangString` BY VALUE, and `TDHero.name` is declared on the
// struct (dump.cs:886076 -- `public struct TDHero { public int rowOffset; }` plus a list of
// properties, `name` among them).  So `TDHero.get_name` does not need `Hero : TableBase<THero>`
// to be found first -- the ONLY thing it needs is the address of the inline `TDHero` struct,
// which is `HeroData + 0x3C0` and has been known since the config offset was resolved.
//
// The earlier search for the table was solving a problem that only exists if the name has to be
// fetched by indexing `THero` out of `TableBase<THero>`.  It does not.  The `rowOffset` cursor is
// opaque -- `get_name` accepts it, and what it does with it is the game's business.
//
// `this` FOR A VALUE TYPE.  `data_object + 0x3C0` points at the struct INSIDE a managed object
// -- a live, GC-tracked allocation on the game thread, the same one the entity walk already
// dereferences.  `il2cpp_runtime_invoke` boxes a value-type `this` itself (it inspects
// `method->klass->valuetype`), so the raw struct pointer is the correct thing to pass, and a
// getter that only reads cannot be affected by the boxing copy.
//
// WHAT STILL HAS A GATE, AND WHY.  The rule -- 「须先做单实体单次调用验证」 -- is unchanged.
// Candidates are tried ONE PER TICK, so a bad method address
// costs one call and one published failure, never a crash inside a loop, and only a candidate
// that returns real text turns the path on for every entity.
// ============================================================================
// ONE ROW PER ENTITY CLASS: WHERE ITS NAME LIVES AND HOW TO ASK FOR IT
// ============================================================================
//
// THE CHAIN, NOW VERIFIED END TO END ON THE LIVE GAME:
//
//     entity data object  +  <config>  ->  TD* struct (value type, INLINE)
//         ->  TD*.get_name()           ->  LangString (boxed, returned by value)
//         ->  LangString.get_value()   ->  the localised DISPLAY NAME
//
// The chain was confirmed against a live process: for a `TDHero`, `get_name` returns a
// `LangString` whose `value` is the localised display name, and `get_dec` /
// `get_lifeskillDesc` do the same for their own text.
//
// WHY `get_value` IS THE LAST STEP AND NOT `group`.  An earlier run returned `"hero"` from all
// three properties, which reads like a broken chain.  It is not: `LangString` is
//
//     public class LangString { public string group; public long langKey; public string value; }
//     // dump.cs:400939, get_value at RVA 0x1FB6E80
//
// and `group` is the LOCALISATION DOMAIN ("hero"), identical for every hero.  Reading a field
// that exists is not the same as reading the field that answers the question.
//
// THE NAMESPACE DIFFERS FROM THE ENTITY'S, AND ASSUMING OTHERWISE COST A ROUND.
//
// Every entity data class lives in `Lens.Gameplay.Modules.BigWorld` (`HeroData` confirmed there
// by `dataClasses`), so that namespace was reused for `TDHero` -- and the runtime answers
// `class not found: TDHero` three times.  The table types live in `Azur.Gameplay.Table`:
//
//     // Namespace: Azur.Gameplay.Table       <- dump.cs:886075, immediately above TDHero
//     public struct TDHero
//
// `dump.cs` prints `// Namespace:` directly above each declaration.  It is checkable in one
// grep, and guessing it cost a restart.
//
// EVERY `TD*` BELOW HAS A `get_name` RETURNING `LangString` (verified in dump.cs by RVA):
//
//     TDHero        RVA 0x373F2C0     MonsterData -> TDEnemy        @0x484
//     TDEnemy       RVA 0x3721270     NpcData     -> TDWorldSpawner @0x488
//     TDPet         RVA 0x3798260     PetData     -> TDPet          @0x458
//     TDWorldSpawner RVA 0x386C330    HeroData    -> TDHero         @0x3C0
//
// `PlayerData` is deliberately absent: it declares no config field at all (dump.cs:285168), so
// there is nothing to read a name from and it keeps the class-name label.

// THE RESOLVED GETTERS ARE CACHED, BECAUSE RESOLVING THEM IS A METADATA WALK.
//
// `il2cpp_class_from_name` + `il2cpp_class_get_method_from_name` walk the runtime's class and
// method tables.  Doing that per label read put every name behind two metadata walks plus two
// managed calls; the pointers never change for the life of the process, so they are resolved
// once per class and kept.

std::size_t FindEntityNameSource(const char* data_class) {
    if (data_class == nullptr || data_class[0] == 0) return kEntityNameSourceCount;
    for (std::size_t index = 0; index < kEntityNameSourceCount; ++index) {
        if (std::strcmp(data_class, kEntityNameSources[index].data_class) == 0) return index;
    }
    return kEntityNameSourceCount;
}

// Accumulates EVERY candidate verdict instead of keeping only the last.
//
// `ComponentState().name_probe` was overwritten on each tick, so a run that produced four verdicts published
// one -- and the one it published was whichever candidate happened to be last.  The single most
// useful fact (what `TDHero.get_name` itself returned) was computed and then thrown away, which
// cost a restart to learn.  Nothing here is on a hot path: it runs at most `kMaxNameCandidates`
// times in the life of the process.

// PER-CLASS, not global: `TDHero` may resolve while `TDEnemy` does not, and one shared flag

// would either disable a working class or enable a broken one.
// Which source class the one-call-per-tick probe is currently testing.

// Resolves `TD*.get_name` and `LangString.get_value` for one source, ONCE, and caches them.
bool ResolveNameMethods(const il2cpp::Api& api, std::size_t source_index) {
    ResolvedNameMethods& cache = ComponentState().name_methods[source_index];
    if (cache.attempted) return cache.ok;
    cache.attempted = true;
    if (api.il2cpp_class_from_name == nullptr ||
        api.il2cpp_class_get_method_from_name == nullptr ||
        api.il2cpp_runtime_invoke == nullptr) {
        return false;
    }
    il2cpp::Il2CppImage* const image = il2cpp::FindImage(kEntityImage);
    if (image == nullptr) return false;
    const EntityNameSource& source = kEntityNameSources[source_index];
    il2cpp::Il2CppClass* const config_class =
        api.il2cpp_class_from_name(image, source.name_space, source.config_class);
    if (config_class == nullptr) return false;
    cache.get_name = api.il2cpp_class_get_method_from_name(config_class, "get_name", 0);
    if (cache.get_name == nullptr) return false;
    // `LangString` is resolved from `Assembly-CSharp.dll` too, and its namespace is
    // `Lens.Gameplay.Modules.BigWorld.Config` (dump.cs:400939) -- NOT the table one.
    il2cpp::Il2CppClass* const lang_class = api.il2cpp_class_from_name(
        image, "Lens.Gameplay.Modules.BigWorld.Config", "LangString");
    if (lang_class == nullptr) return false;
    cache.get_value = api.il2cpp_class_get_method_from_name(lang_class, "get_value", 0);
    if (cache.get_value == nullptr) return false;
    cache.ok = true;
    return true;
}

// The whole chain for one entity, in one call.  Text is written to `out`; the return value is
// the length, and 0 means "no name" without distinguishing why -- `diagnostic` carries that.
std::uint32_t ReadEntityName(const il2cpp::Api& api, std::uintptr_t data_object,
                             std::size_t source_index, char* out, std::uint32_t capacity,
                             std::string* diagnostic) {
    if (out == nullptr || capacity == 0 || source_index >= kEntityNameSourceCount) return 0;
    out[0] = 0;
    if (!ResolveNameMethods(api, source_index)) {
        if (diagnostic != nullptr) *diagnostic = "getters unresolved";
        return 0;
    }
    const EntityNameSource& source = kEntityNameSources[source_index];
    const ResolvedNameMethods& methods = ComponentState().name_methods[source_index];
    // `data_object + config_offset` is the ADDRESS of the inline `TD*` value type.  The runtime
    // boxes a value-type `this` itself, so a raw struct pointer is the correct argument.
    void* const instance = reinterpret_cast<void*>(data_object + source.config_offset);
    il2cpp::Il2CppObject* exception = nullptr;
    il2cpp::Il2CppObject* const lang = api.il2cpp_runtime_invoke(
        methods.get_name, instance, nullptr, &exception);
    if (exception != nullptr) {
        if (diagnostic != nullptr) *diagnostic = "get_name threw";
        return 0;
    }
    if (lang == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "get_name null";
        return 0;
    }
    exception = nullptr;
    il2cpp::Il2CppObject* const value = api.il2cpp_runtime_invoke(
        methods.get_value, lang, nullptr, &exception);
    if (exception != nullptr) {
        if (diagnostic != nullptr) *diagnostic = "get_value threw";
        return 0;
    }
    if (value == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "get_value null";
        return 0;
    }
    const std::uint32_t written = ReadManagedString(api, value, out, capacity);
    if (written == 0 && diagnostic != nullptr) *diagnostic = "value was empty";
    return written;
}

std::string ProbeEntityName(const il2cpp::Api& api, std::uintptr_t data_object,
                            const char* class_name) {
    if (class_name == nullptr || class_name[0] == 0) return {};
    // Only a class whose config offset the binding table actually resolved.
    std::size_t off_config = 0;
    for (std::size_t index = 0; index < ComponentState().label_binding.source_count; ++index) {
        const LabelSource& source = ComponentState().label_binding.sources[index];
        const char* const at = std::strchr(source.class_name, '@');
        const std::size_t length = at != nullptr
                                       ? static_cast<std::size_t>(at - source.class_name)
                                       : std::strlen(source.class_name);
        if (std::strlen(class_name) == length &&
            std::strncmp(class_name, source.class_name, length) == 0) {
            off_config = source.off_config;
            break;
        }
    }
    if (off_config == 0) return {};
    char report[384]{};
    std::snprintf(report, sizeof(report), "%s config@0x%zX ", class_name, off_config);
    const auto append = [&report](const char* text) {
        std::strncat(report, text, sizeof(report) - std::strlen(report) - 1);
    };
    std::uintptr_t config = 0;
    if (!ReadValue(data_object + off_config, &config) || config == 0) {
        append("-> config=NULL");
        return report;
    }
    char step[96]{};
    std::snprintf(step, sizeof(step), "-> config=%llX ", static_cast<unsigned long long>(config));
    append(step);
    // STEP 2 AND 3.  The plausibility test first, because step 3 is the call suspected of
    // crashing three builds: if the field does not hold an object, it is not called at all.
    if (!IsManagedObjectPointer(config)) {
        append("NOT-AN-OBJECT (skipping il2cpp_object_get_class)");
        return report;
    }
    if (api.il2cpp_object_get_class == nullptr || api.il2cpp_class_get_name == nullptr) {
        append("no class API");
        return report;
    }
    il2cpp::Il2CppClass* const config_class = api.il2cpp_object_get_class(
        reinterpret_cast<il2cpp::Il2CppObject*>(config));
    if (config_class == nullptr) {
        append("-> class=NULL");
        return report;
    }
    const char* const config_name = api.il2cpp_class_get_name(config_class);
    std::snprintf(step, sizeof(step), "-> class=%s ", config_name != nullptr ? config_name : "?");
    append(step);
    // STEP 4.  The offset is taken from the BINDING TABLE, keyed by class name, so selection
    // needs no second runtime call.
    std::size_t off_name = 0;
    if (config_name != nullptr) {
        for (std::size_t index = 0; index < ComponentState().label_name_source_count; ++index) {
            if (std::strcmp(config_name, ComponentState().label_name_sources[index].class_name) == 0) {
                off_name = ComponentState().label_name_sources[index].off_name;
                break;
            }
        }
    }
    if (off_name == 0) {
        append("-> no name field for this class");
        return report;
    }
    std::snprintf(step, sizeof(step), "name@0x%zX ", off_name);
    append(step);
    std::uintptr_t name_object = 0;
    if (!ReadValue(config + off_name, &name_object) || name_object == 0) {
        append("-> LangString=NULL");
        return report;
    }
    std::snprintf(step, sizeof(step), "-> LangString=%llX ",
                  static_cast<unsigned long long>(name_object));
    append(step);
    // STEP 5.  `langString.group` is a managed string, so its UTF-16 payload is what
    // `ReadManagedString` already reads safely everywhere else in this file.
    std::uintptr_t group = 0;
    if (!ReadValue(name_object + ComponentState().label_binding.off_lang_group, &group) || group == 0) {
        append("-> group=NULL");
        return report;
    }
    char text[96]{};
    const std::uint32_t written = ReadManagedString(
        api, reinterpret_cast<il2cpp::Il2CppObject*>(group), text, sizeof(text));
    std::snprintf(step, sizeof(step), "-> group=%llX text=%u [%s]",
                  static_cast<unsigned long long>(group), written, written != 0 ? text : "(none)");
    append(step);
    return report;
}
// THE PROBE, AND WHY IT DOES NOT LIVE IN `ReadEntityLabel`
// =======================================================
//
// The first version put this inside `ReadEntityLabel`, which the walk calls ONLY ON A LABEL CACHE
// MISS.  That makes the probe depend on an event that stops happening: the report stops at the
// last cache miss -- `probe[1/4]`, `probe[2/4]`, `probe[3/4]` and never `probe[4/4]` -- while
// every one of those classes is present in the scene.
//
// The scene's entity order is `... HeroData, PetData, MonsterData, NpcData`: `PetData` is read
// and cached while the probe index still points at HeroData/MonsterData, and by the time the
// index reaches `PetData` its label is cached, so `ReadEntityLabel` is never entered for it
// again.  The probe was waiting for a call that the cache exists to prevent.
//
// Moving it into the WALK fixes that structurally: the walk visits every entity on every tick,
// cached or not, so each class is seen each tick and the index always finds its turn.  It also
// removes the probe's dependence on iteration order, which is what made the failure look like an
// ordering coincidence rather than a bug.
//
// Still one call per tick: one entity, one call.
void ProbeEntityNameSource(const il2cpp::Api& api, std::uintptr_t data_object,
                           const char* data_class) {
    if (ComponentState().name_probe_index >= kEntityNameSourceCount) return;
    if (data_object == 0 || data_class == nullptr || data_class[0] == 0) return;
    const std::size_t source = FindEntityNameSource(data_class);
    if (source != ComponentState().name_probe_index) return;
    char text[96]{};
    std::string diagnostic;
    const std::uint32_t length =
        ReadEntityName(api, data_object, source, text, sizeof(text), &diagnostic);
    char report[320]{};
    if (length != 0) {
        ComponentState().name_source_ok[source] = true;
        std::snprintf(report, sizeof(report), "probe[%zu/%zu][%s] \"%.60s\" || ",
                      source + 1, kEntityNameSourceCount, data_class, text);
        // This class's labels were cached as class tags while its chain was still unproven, and
        // the cache would otherwise keep them that way for the life of the process.
        InvalidateLabelCache();
    } else {
        std::snprintf(report, sizeof(report), "probe[%zu/%zu][%s] %s || ",
                      source + 1, kEntityNameSourceCount, data_class, diagnostic.c_str());
    }
    ComponentState().name_probe_log += report;
    ComponentState().name_probe = ComponentState().name_probe_log;
    ++ComponentState().name_probe_index;
}

// THE ONE NAME EVERY ENTITY CLASS HAS: `BaseData.configName`.
//
// `WorldItemData` has no config field of any kind -- the dump gives it `modelPath` (a resource
// path), `onGroundOffSet`, `registeredID` and the `WorldEntityData` integers, and nothing that
// resolves to a `TD*` row.  So the `TD*.get_name` route cannot serve the 70 world resources in a
// typical scene, and neither can `ItemData.blueprint` (dump.cs:279437): that class DOES hold a
// `TWorld_blueprint` with a localised `displayName`, but the live objects are `WorldItemData`,
// which is a SIBLING of `ItemData` under `WorldEntityData`, not a subclass.
//
// What all of them share is the root base class:
//
//     public class BaseData : IClass {            // dump.cs:203828
//         private int    <configId>k__BackingField;    // 0x148
//         private string <configName>k__BackingField;  // 0x150
//     }
//     WorldItemData : WorldEntityData : AliveData : BaseData
//
// `configName` is a plain managed `string`, so this costs ONE memory read plus a string copy --
// no metadata walk, no `il2cpp_runtime_invoke`, and therefore none of the risk that made the
// `TD*` route need a one-call-per-tick probe.  It is used as a FALLBACK, after the localised
// name, because a localised name is what the user actually asked for.
//
// Guarded by `IsManagedObjectPointer` before the string is touched: `ReadManagedString` calls
// `il2cpp_string_length`, which dereferences the object header, so a stale or wrong offset would
// be an access violation rather than a wrong label.
// `BaseData.configId` (dump.cs:203828), the row id of the entity's config.  For a world resource
// this is the ONLY non-zero id on the object -- `configName` is null and `BlueprintId` is 0, both
// confirmed live -- so it is the key into whichever `TD*` table names the resource.
// `WorldEntityData.spawnerId` (dump.cs:291699).  The id whose lookup table (`TDWorldSpawner`,
// `get_name` RVA 0x386C330) is already proven to name NPCs, and therefore the id a world object
// is most likely to be named by.
// `WorldItemData.registeredID` (dump.cs:289307).  The last id on the object that has not been
// used as a table key; every failing item carried the sentinel `spawnerId=8000000`, so the
// spawner route cannot answer for them and this is the remaining candidate.
// `WorldItemData.modelPath` (dump.cs:289307).  A `string`, verified in the dump rather than
// guessed: the same class declares `onGroundOffSet` at 0x488, and the `WorldEntityData` fields
// end at 0x47D, so 0x480 is the only place `modelPath` can sit.

std::uint32_t ReadEntityConfigName(const il2cpp::Api& api, std::uintptr_t data_object,
                                   char* out, std::uint32_t capacity) {
    if (out == nullptr || capacity == 0 || data_object == 0) return 0;
    out[0] = 0;
    std::uintptr_t text = 0;
    if (!ReadValue(data_object + kBaseDataConfigNameOffset, &text)) return 0;
    if (!IsManagedObjectPointer(text)) return 0;
    return ReadManagedString(api, reinterpret_cast<il2cpp::Il2CppObject*>(text), out, capacity);
}

// The id fields a world item carries, and the string fields it carries, dumped once.
//
// WHY A DUMP RATHER THAN ANOTHER GUESS.  The world-resource chain has now been narrowed by
// elimination and every remaining candidate needs a number that only the live object can supply:
//
//   * `TDWorldItem` (dump.cs:902964) is a RESOURCE table -- `resPath`, `meshPath`, `effectPath`,
//     `aabb` and asset hashes, and NOT a single name property.  So `TDWorldItemTable` cannot
//     answer "what is this thing called".
//   * `TDWorldBlueprint` (dump.cs:900436) likewise has no name: `id`, `path[]`, `param`,
//     `stringParam`, `intParam`, `boolParam`, `collisionType`, `paramTemplate`.
//   * `TWorld_blueprint.displayName` IS a `LangString`, but the only type that references
//     `TWorld_blueprint` anywhere in the dump is `ItemData` (dump.cs:279437), and the live
//     objects are `WorldItemData` (66 of them) -- a SIBLING under `WorldEntityData`, not a
//     subclass.  No class in the dump holds the `World_blueprint` table.
//   * `TDWorldCollecting` (dump.cs:952465), `TDWorldArea`, `TDWorldSpawner`, `TDWorldMapEffect`
//     DO carry `name` as a `LangString`, and `TDBaseTable<TDataTable, TData, TKey>` carries a
//     `private static TDataTable s_Instance` with a static `GetInstance(bool force)` -- so those
//     tables are addressable without locating a holder.  What is missing is only the KEY.
//
// `WorldEntityData` offers three candidate keys -- `configId` (0x148, on `BaseData`),
// `spawnerId` (0x3D0) and `BlueprintId` (0x3D8) -- plus `configName` (0x150) and
// `modelPath` (0x480) as strings.  Printing all five costs one tick and one line, and it turns
// the next step from a guess into a lookup.  This is the same discipline that the `nameProbe`
// field enforces for the `TD*` route.

void DiagnoseWorldItem(const il2cpp::Api& api, std::uintptr_t data_object) {
    if (ComponentState().world_item_diagnostic_done || data_object == 0) return;
    ComponentState().world_item_diagnostic_done = true;
    std::uintptr_t config_name = 0;
    std::uintptr_t model_path = 0;
    std::int32_t config_id = 0;
    std::int32_t spawner_id = 0;
    std::int32_t blueprint_id = 0;
    static_cast<void>(ReadValue(data_object + kBaseDataConfigNameOffset, &config_name));
    static_cast<void>(ReadValue(data_object + 0x148, &config_id));
    static_cast<void>(ReadValue(data_object + 0x3D0, &spawner_id));
    static_cast<void>(ReadValue(data_object + 0x3D8, &blueprint_id));
    static_cast<void>(ReadValue(data_object + kWorldItemModelPathOffset, &model_path));
    char name_text[72]{};
    char path_text[96]{};
    if (IsManagedObjectPointer(config_name)) {
        ReadManagedString(api, reinterpret_cast<il2cpp::Il2CppObject*>(config_name), name_text,
                          sizeof(name_text));
    }
    if (IsManagedObjectPointer(model_path)) {
        ReadManagedString(api, reinterpret_cast<il2cpp::Il2CppObject*>(model_path), path_text,
                          sizeof(path_text));
    }
    char report[420]{};
    std::snprintf(report, sizeof(report),
                  "world[1] configName=0x%llX \"%.36s\" modelPath=\"%.48s\" configId=%d "
                  "spawnerId=%d blueprintId=%d || ",
                  static_cast<unsigned long long>(config_name), name_text, path_text, config_id,
                  spawner_id, blueprint_id);
    ComponentState().name_probe_log += report;
    ComponentState().name_probe = ComponentState().name_probe_log;
}

// The readable tail of a resource path: `.../SM_Chest_01.prefab` -> `SM_Chest_01`.
//
// NOT a substitute for the localised name, and not presented as one.  It is what a world item
// can be labelled with today: the chain that would give `TDWorldCollecting.name` needs the id
// that indexes the table, and `DiagnoseWorldItem` exists to find which of the three that is.  A
// resource name identifies the object; a class tag and an entity id do not.
std::uint32_t ReadEntityModelName(const il2cpp::Api& api, std::uintptr_t data_object, char* out,
                                  std::uint32_t capacity) {
    if (out == nullptr || capacity == 0 || data_object == 0) return 0;
    out[0] = 0;
    std::uintptr_t path = 0;
    if (!ReadValue(data_object + kWorldItemModelPathOffset, &path)) return 0;
    if (!IsManagedObjectPointer(path)) return 0;
    char text[160]{};
    if (ReadManagedString(api, reinterpret_cast<il2cpp::Il2CppObject*>(path), text,
                          sizeof(text)) == 0) {
        return 0;
    }
    const char* tail = text;
    for (const char* cursor = text; *cursor != 0; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') tail = cursor + 1;
    }
    // Drop the extension: `.prefab`/`.asset` are noise in a label.
    std::size_t length = std::strlen(tail);
    for (std::size_t index = 0; index < length; ++index) {
        if (tail[index] == '.') {
            length = index;
            break;
        }
    }
    if (length == 0) return 0;
    const std::uint32_t limit =
        static_cast<std::uint32_t>(length) < capacity - 1 ? static_cast<std::uint32_t>(length)
                                                          : capacity - 1;
    std::memcpy(out, tail, limit);
    out[limit] = 0;
    return limit;
}

// ============================================================================
// WORLD RESOURCES: `configId` -> `GetRowOffset` -> `TD*.get_name`
// ============================================================================
//
// THE MISSING KEY WAS `BaseData.configId`, AND THE MISSING ACCESSOR WAS `GetRowOffset`.
//
// The live dump from `DiagnoseWorldItem` settled three things the dump file alone could not:
//
//     world[1] configName=0x0  modelPath="FieldItem/World/pre_world_collection027_1_step.p"
//              configId=903189  spawnerId=10562  blueprintId=0
//
//   1. `configName` is genuinely NULL for a world item (0x0), not rejected by the guard -- so
//      a `configName` fallback can never label a world resource.
//   2. `BlueprintId` is 0, which rules out the whole `TWorld_blueprint.displayName` route.
//      `ItemData.blueprint` would have been empty anyway.
//   3. `configId` is a large non-zero row id, and `modelPath` says `collection` -- which points
//      at `TDWorldCollecting`, the one world table with a `name` that fits the resource.
//
// `TDBaseTable<TDataTable, TData, TKey>` (dump.cs:791097) provides, in this order:
//
//     public static TDataTable GetInstance(bool force = False)   // L791135
//     public bool HasKey(TKey key)                               // L799555
//     public int  GetRowOffset(TKey key)                         // L801843
//
// `GetRowOffset` returns exactly the `int rowOffset` that the `TD*` row structs are made of, so
// the row can be BUILT LOCALLY -- `TDWorldCollecting` is `{ int rowOffset; }` with the field at
// offset 0x0, so a 4-byte local is a valid `this` once it holds that value.  No struct-return
// buffer, no `il2cpp_object_unbox` of a row, and no table-held row object: the columnar layout
// that leaves no row object to index is worked around by asking the table for the offset
// instead of the row.
//
// Every call below is a scalar argument or a class-typed return, so this path needs none of the
// 16-byte-return machinery a hand-written detour would have to get right.
// WHICH id on the entity is the key -- the first probe asked the wrong question of the right
// table, and that is what this field exists to prevent happening twice.
//
// Every `configId` lookup answers `no row for id 903189`, and the ids seen
// were 903189 and 903190 on two different items: they advance per entity, which is what a
// runtime instance id does and a table row id does not.  `spawnerId` was 10562 -- small, and
// pointing at the table whose `get_name` (RVA 0x386C330) is the SAME method that already names
// NPCs through `NpcData.spwanerData @0x488`.  A world object is spawned by a spawner, so the
// spawner is where its name lives.
// ORDERED BY LIKELIHOOD; the first table that yields a usable name freezes.
//
// `TDWorldSpawner` is FIRST because it is the one table that has already answered: `HasKey(10562)`
// returned TRUE on the first run, so the table and the key were right all along and only the row
// was misread.  `TDBattlefieldItemTable` is next -- the live `modelPath` begins `FieldItem/World/`
// and that row carries a `displayName`.

// A name made only of digits is a key, not a name.
//
// This is the check that the "0" result forces.  It is deliberately narrow -- it rejects only
// the all-digit case rather than guessing at quality -- because the alternative is a heuristic
// that discards a short real name.  `TD*.get_name` returning `"0"` for an unlocalised row is the
// observed failure, and this catches exactly that shape while leaving every alphabetic or CJK
// name alone.
bool IsUsableWorldName(const char* text) {
    if (text == nullptr || text[0] == 0) return false;
    for (const char* cursor = text; *cursor != 0; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return true;
    }
    return false;
}

// WHICH TABLE ACTUALLY CARRIES THE NAMES.

//
// `table=25` says twenty-five items were named by SOME table and says nothing about which one.
// The chain is ordered by a guess about likelihood, and a guess that is not scored cannot be
// improved: if `Col` names everything and `BF` names nothing, the order should say so.  Counted
// per alias and published next to the totals, so the next ordering change is evidence-driven.
// How many distinct items have fallen all the way through the table list.  Capped, because the
// useful information is the ids of a few failures, not a line per world object.
// Whether the labels cached before any world table answered have been re-read yet.  Once is
// enough, and once is all the frame budget can afford.

// `count` for a resolved table.  Reading it is what separates the two ways `HasKey` can say no,
// and it is cheap: one invoke, cached with the instance.
std::int32_t ReadWorldTableCount(const il2cpp::Api& api, std::size_t index) {
    WorldTableState& state = ComponentState().world_tables[index];
    if (state.instance == nullptr || state.get_count == nullptr) return -1;
    il2cpp::Il2CppObject* exception = nullptr;
    il2cpp::Il2CppObject* const boxed =
        api.il2cpp_runtime_invoke(state.get_count, state.instance, nullptr, &exception);
    if (exception != nullptr || boxed == nullptr) return -1;
    const void* const payload = api.il2cpp_object_unbox(boxed);
    if (payload == nullptr) return -1;
    std::int32_t value = 0;
    std::memcpy(&value, payload, sizeof(value));
    return value;
}

bool ResolveWorldTable(const il2cpp::Api& api, std::size_t index) {
    WorldTableState& state = ComponentState().world_tables[index];
    if (state.attempted) return state.get_name != nullptr;
    state.attempted = true;
    if (api.il2cpp_class_from_name == nullptr ||
        api.il2cpp_class_get_method_from_name == nullptr ||
        api.il2cpp_runtime_invoke == nullptr || api.il2cpp_object_unbox == nullptr) {
        state.failure = "api missing";
        return false;
    }
    il2cpp::Il2CppImage* const image = il2cpp::FindImage(kEntityImage);
    if (image == nullptr) {
        state.failure = "image missing";
        return false;
    }
    const WorldNameTable& table = kWorldNameTables[index];
    state.table_class = api.il2cpp_class_from_name(image, "Azur.Gameplay.Table",
                                                   table.table_class);
    if (state.table_class == nullptr) {
        state.failure = "class not found";
        return false;
    }
    // `GetInstance` is inherited from the generic base, so the search has to walk up to it; the
    // concrete override is what gets returned.
    state.get_instance = api.il2cpp_class_get_method_from_name(state.table_class, "GetInstance", 1);
    state.has_key = api.il2cpp_class_get_method_from_name(state.table_class, "HasKey", 1);
    state.get_row_offset =
        api.il2cpp_class_get_method_from_name(state.table_class, "GetRowOffset", 1);
    state.get_count = api.il2cpp_class_get_method_from_name(state.table_class, "get_count", 0);
    if (state.get_instance == nullptr) {
        state.failure = "GetInstance missing";
        return false;
    }
    if (state.has_key == nullptr) {
        state.failure = "HasKey missing";
        return false;
    }
    if (state.get_row_offset == nullptr) {
        state.failure = "GetRowOffset missing";
        return false;
    }
    il2cpp::Il2CppClass* const row_class =
        api.il2cpp_class_from_name(image, "Azur.Gameplay.Table", table.row_class);
    if (row_class == nullptr) {
        state.failure = "row class not found";
        return false;
    }
    // A ROW COLUMN CAN BE SPELLED MANY WAYS, AND THE GETTER NAME FOLLOWS THE COLUMN.
    //
    // `TDWorldCollecting` and `TDWorldSpawner` call it `name` (getter `get_name`);
    // `TDBattlefieldItem` calls it `displayName` (getter `get_displayName`).  The list below is
    // not exhaustive: `Tip` and `Hud` rows resolve and then fail on the getter.
    //
    // Both tables RESOLVE and then fail on the getter -- and `TDWorldHud` holds
    // `text: LangString`, which is exactly the HUD text a world object displays.  Its getter is
    // `get_text`, a name this only ever looked for under two spellings.  The dump has 219 tables
    // whose row carries a `LangString name|displayName`, and many more that carry `text`,
    // `desc`, `tip` or `title` instead; hard-coding the spelling per table is a guessing game
    // that costs a game launch per guess.
    //
    // So all the spellings are tried, in the order of how often the dump uses them.  A wrong
    // guess here costs one class lookup, not a run.
    constexpr const char* kRowGetterNames[] = {
        "get_name", "get_displayName", "get_text", "get_desc", "get_tip", "get_title", "get_label",
    };
    state.get_name = nullptr;
    for (const char* const getter : kRowGetterNames) {
        state.get_name = api.il2cpp_class_get_method_from_name(row_class, getter, 0);
        if (state.get_name != nullptr) break;
    }
    if (state.get_name == nullptr) {
        state.failure = "row has no name-ish getter";
        return false;
    }
    // THE INSTANCE IS NOT CACHED, AND THAT IS THE FIX FOR THE LABELS REVERTING.
    //
    // The previous version fetched the singleton once and kept it, on the reasoning that
    // `GetInstance` may parse the table on first call.  That reasoning was wrong about the
    // lifetime of the object: these tables are UNLOADED AND REBUILT when the world streams a new
    // area, and a rebuilt table is a different object.  The cached pointer then refers to a dead
    // one, `HasKey` answers `false` for every id it owns, and every world label silently falls
    // back to the resource path.
    //
    // That is the reported symptom exactly: `金麦` was on screen and turned into an English
    // resource name while the user watched.  It is not a re-localisation, and it is not the
    // scene changing -- it is a stale pointer.
    //
    // The class and method lookups ARE kept: those are `MethodInfo` in the image metadata, which
    // does not move when a table's data is reloaded.  Only the instance is re-fetched, and the
    // cost is one invoke per table query, paid on a label cache miss only.
    return true;
}

// The table singleton, fetched fresh every time.  See the note above `ResolveWorldTable`'s
// return: caching this across an area load is what made good names revert.
//
// A TABLE THAT HAS NOT BEEN PARSED YET ANSWERS `false` TO EVERY `HasKey`, AND THAT IS
// INDISTINGUISHABLE FROM "THIS ID IS NOT IN THE TABLE".
//
// That is the mechanism behind the report that gatherables WITH Chinese names were showing
// English asset paths: `GetInstance(false)` returns an instance holding no rows until something
// asks for a forced load, every lookup against it misses, and the miss was then cached as a
// final answer.  The `force = true` retry existed for exactly this and was dropped in the
// refactor that stopped caching the instance -- a regression introduced while fixing a
// different bug, which is the third time that has happened in this thread.
//
// The row count is cached PER TABLE once it is non-zero, so the force path runs at most once per
// table per process and the steady state costs nothing extra.
il2cpp::Il2CppObject* WorldTableInstance(const il2cpp::Api& api, std::size_t index) {
    WorldTableState& state = ComponentState().world_tables[index];
    if (!ResolveWorldTable(api, index)) return nullptr;
    bool force = false;
    void* arguments[1] = {&force};
    il2cpp::Il2CppObject* exception = nullptr;
    il2cpp::Il2CppObject* instance =
        api.il2cpp_runtime_invoke(state.get_instance, nullptr, arguments, &exception);
    if (exception != nullptr || instance == nullptr) return nullptr;
    state.instance = instance;
    if (state.count > 0) return instance;
    state.count = ReadWorldTableCount(api, index);
    if (state.count > 0) return instance;
    // Empty: ask for the forced load, ONCE PER TABLE.  A table that is genuinely empty would
    // otherwise pay two extra invokes on every single lookup for the rest of the session.
    if (state.forced) return instance;
    bool force_load = true;
    void* force_arguments[1] = {&force_load};
    exception = nullptr;
    il2cpp::Il2CppObject* const loaded =
        api.il2cpp_runtime_invoke(state.get_instance, nullptr, force_arguments, &exception);
    if (exception == nullptr && loaded != nullptr) {
        state.instance = loaded;
        state.forced = true;
        state.count = ReadWorldTableCount(api, index);
        return loaded;
    }
    return instance;
}

// Whether ANY candidate table is known to hold rows.
//
// Until this is true, a failed lookup says nothing about the item -- it says the tables were not
// loaded yet.  The walk uses this to decide whether a fallback label may be CACHED: before the
// tables are up, a fallback must not be, because it freezes a transient failure into a permanent
// English label.  That is the "gatherable with a Chinese name showing an asset path" bug, and
// caching the negative result is what made it permanent instead of momentary.
bool WorldTablesLoaded() {
    for (std::size_t index = 0; index < kWorldNameTableCount; ++index) {
        if (ComponentState().world_tables[index].count > 0) return true;
    }
    return false;
}

// THE COLLECTING TABLE IS THE OTHER WAY ROUND.
//
// `余烬菇` and `闪锡矿` are gathered nodes, and they were showing an asset path while items
// whose spawner carried a name showed `尘石`.  The reason is the direction of the key:
//
//     TDWorldCollecting { id, type, itemType, itemId, name: LangString, spawnerId: List<int> }
//
// The NAME is on the collecting row, and that row points AT the spawners.  Asking it by
// `configId` can only ever return `noRow` -- the row is not identified by the item's config id,
// it is identified by its own id and located by SEARCHING its `spawnerId` list.  Eighteen rounds
// of `Col:noRow[cid]` were the table answering a question it does not answer that way.
//
// The table holds 48 rows, so the reverse map is built ONCE by walking a bounded id range and
// asking `HasKey`, then every item lookup is a linear scan of a few hundred entries.  The scan is
// bounded so a table with sparse ids costs a fixed number of invokes and not an unbounded one.
// 0 = not attempted, 1 = built, -1 = failed.  One attempt, whatever happens.

std::uint32_t LookupCollectingName(std::int32_t spawner_id, char* out,
                                   std::uint32_t capacity) {
    if (out == nullptr || capacity == 0 || spawner_id == 0) return 0;
    out[0] = 0;
    for (std::size_t index = 0; index < ComponentState().collecting_name_count; ++index) {
        if (ComponentState().collecting_names[index].spawner_id == spawner_id) {
            std::snprintf(out, capacity, "%s", ComponentState().collecting_names[index].name);
            return static_cast<std::uint32_t>(std::strlen(out));
        }
    }
    return 0;
}

// Read a row's `name` as localised text.  Same two-step as `ReadWorldResourceName`:
// `TD*.get_name` returns a `LangString` BY VALUE, so the invoke result is the boxed struct and
// `get_value` is what performs the localisation.  Reading the box as a string reads `group`.
std::uint32_t ReadRowNameText(const il2cpp::Api& api, const il2cpp::MethodInfo* get_name,
                              void* row, char* out, std::uint32_t capacity) {
    if (get_name == nullptr || row == nullptr || out == nullptr || capacity == 0) return 0;
    out[0] = 0;
    il2cpp::Il2CppObject* exception = nullptr;
    il2cpp::Il2CppObject* const lang = api.il2cpp_runtime_invoke(get_name, row, nullptr, &exception);
    if (exception != nullptr || lang == nullptr) return 0;
    il2cpp::Il2CppClass* const lang_class = api.il2cpp_object_get_class(lang);
    if (lang_class == nullptr) return 0;
    const il2cpp::MethodInfo* const get_value =
        api.il2cpp_class_get_method_from_name(lang_class, "get_value", 0);
    if (get_value == nullptr) return 0;
    exception = nullptr;
    il2cpp::Il2CppObject* const value = api.il2cpp_runtime_invoke(get_value, lang, nullptr, &exception);
    if (exception != nullptr || value == nullptr) return 0;
    return ReadManagedString(api, value, out, capacity);
}

// WALK THE COLLECTING TABLE ONCE AND INVERT IT: spawner id -> node name.
//
// Bounded id range, so a table whose ids are sparse costs a fixed number of `HasKey` invokes and
// not an unbounded search.  Runs at most once per process, whichever way it ends, and publishes
// its outcome so "no gatherable names" can be told apart from "the scan did not run".
void BuildCollectingNameIndex(const il2cpp::Api& api) {
    if (ComponentState().collecting_index_state != 0) return;
    ComponentState().collecting_index_state = -1;
    char report[160]{};
    std::size_t table = kWorldNameTableCount;
    for (std::size_t index = 0; index < kWorldNameTableCount; ++index) {
        if (std::strcmp(kWorldNameTables[index].row_class, "TDWorldCollecting") == 0) {
            table = index;
            break;
        }
    }
    if (table == kWorldNameTableCount || !ResolveWorldTable(api, table)) {
        std::snprintf(report, sizeof(report), "collectingIndex: unable to resolve the table; ");
        ComponentState().collecting_index_log = report;
        return;
    }
    WorldTableState& state = ComponentState().world_tables[table];
    // Same route `ResolveWorldTable` uses for every other row class: image + namespace + name.
    il2cpp::Il2CppImage* const class_image = il2cpp::FindImage(kEntityImage);
    il2cpp::Il2CppClass* const row_class =
        class_image != nullptr
            ? api.il2cpp_class_from_name(class_image, "Azur.Gameplay.Table", "TDWorldCollecting")
            : nullptr;
    const il2cpp::MethodInfo* const get_spawner_ids =
        row_class != nullptr ? api.il2cpp_class_get_method_from_name(row_class, "get_spawnerId", 0)
                             : nullptr;
    if (get_spawner_ids == nullptr) {
        std::snprintf(report, sizeof(report), "collectingIndex: no get_spawnerId; ");
        ComponentState().collecting_index_log = report;
        return;
    }
    constexpr std::int32_t kMaximumCollectingId = 8192;
    std::int32_t rows_seen = 0;
    // Fetched ONCE for the whole scan.  `WorldTableInstance` re-resolves the singleton on every
    // call, so calling it inside an 8192-iteration loop would be two managed invokes per
    // iteration -- a visible hitch on the frame that builds the index, for no benefit.
    il2cpp::Il2CppObject* const instance = WorldTableInstance(api, table);
    if (instance == nullptr) {
        std::snprintf(report, sizeof(report), "collectingIndex: no instance; ");
        ComponentState().collecting_index_log = report;
        return;
    }
    for (std::int32_t id = 1; id <= kMaximumCollectingId; ++id) {
        if (ComponentState().collecting_name_count >= kCollectingNameCapacity) break;
        il2cpp::Il2CppObject* exception = nullptr;
        void* key_arguments[1] = {&id};
        il2cpp::Il2CppObject* const has =
            api.il2cpp_runtime_invoke(state.has_key, instance, key_arguments, &exception);
        if (exception != nullptr || has == nullptr) break;
        const void* const payload = api.il2cpp_object_unbox(has);
        bool present = false;
        if (payload == nullptr) break;
        std::memcpy(&present, payload, sizeof(present));
        if (!present) continue;
        void* offset_arguments[1] = {&id};
        exception = nullptr;
        il2cpp::Il2CppObject* const row_offset_box =
            api.il2cpp_runtime_invoke(state.get_row_offset, instance, offset_arguments, &exception);
        if (exception != nullptr || row_offset_box == nullptr) continue;
        const void* const offset_payload = api.il2cpp_object_unbox(row_offset_box);
        std::int32_t row_offset = 0;
        if (offset_payload == nullptr) continue;
        std::memcpy(&row_offset, offset_payload, sizeof(row_offset));
        std::uint8_t row[40]{};
        std::memcpy(row + kWorldNameTables[table].row_offset_field, &row_offset,
                    sizeof(row_offset));
        ++rows_seen;
        char node_name[48]{};
        if (ReadRowNameText(api, state.get_name, row, node_name, sizeof(node_name)) == 0) continue;
        exception = nullptr;
        il2cpp::Il2CppObject* const list =
            api.il2cpp_runtime_invoke(get_spawner_ids, row, nullptr, &exception);
        if (exception != nullptr || list == nullptr) continue;
        // `List<int>` on 64-bit IL2CPP: `_items` at 0x10, `_size` at 0x18; the array's elements
        // begin after the 0x20 object+bounds+length header.
        std::int32_t list_size = 0;
        std::uintptr_t items = 0;
        const std::uintptr_t list_address = reinterpret_cast<std::uintptr_t>(list);
        if (!ReadValue(list_address + 0x18, &list_size) ||
            !ReadValue(list_address + 0x10, &items)) {
            continue;
        }
        if (items == 0 || list_size < 0 || list_size > 64) continue;
        for (std::int32_t slot = 0; slot < list_size; ++slot) {
            if (ComponentState().collecting_name_count >= kCollectingNameCapacity) break;
            std::int32_t spawner_id = 0;
            if (!ReadValue(items + 0x20 + static_cast<std::uintptr_t>(slot) * 4, &spawner_id)) break;
            if (spawner_id == 0) continue;
            CollectingName& entry = ComponentState().collecting_names[ComponentState().collecting_name_count];
            bool duplicate = false;
            for (std::size_t seen = 0; seen < ComponentState().collecting_name_count; ++seen) {
                if (ComponentState().collecting_names[seen].spawner_id == spawner_id) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            entry.spawner_id = spawner_id;
            std::snprintf(entry.name, sizeof(entry.name), "%s", node_name);
            ++ComponentState().collecting_name_count;
        }
    }
    ComponentState().collecting_index_state = 1;
    std::snprintf(report, sizeof(report), "collectingIndex: rows=%d spawners=%d; ", rows_seen,
                  static_cast<int>(ComponentState().collecting_name_count));
    ComponentState().collecting_index_log = report;
}

// `configId`/`spawnerId` -> the localised name, or empty with `diagnostic` set.  `table_index`
// picks the table; `kWorldNameTables[table_index].key` picks which entity id to ask it with.
std::uint32_t ReadWorldResourceName(const il2cpp::Api& api, std::int32_t config_id,
                                    std::int32_t spawner_id, std::int32_t registered_id,
                                    std::size_t table_index, char* out,
                                    std::uint32_t capacity, std::string* diagnostic) {
    if (out == nullptr || capacity == 0 || table_index >= kWorldNameTableCount) return 0;
    out[0] = 0;
    const WorldNameTable& descriptor = kWorldNameTables[table_index];
    std::int32_t config_id_used = config_id;
    const char* key_name = "cid";
    if (descriptor.key == kWorldKeySpawnerId) {
        config_id_used = spawner_id;
        key_name = "sid";
    } else if (descriptor.key == kWorldKeyRegisteredId) {
        config_id_used = registered_id;
        key_name = "rid";
    }
    if (config_id_used == 0) {
        if (diagnostic != nullptr) {
            // SHORT CODES, because the whole chain has to fit in one field.  `noRow` is the
            // common case and does not repeat the id or the table name -- the alias in the
            // caller says which table, and the `worldfail` header says which ids.
            *diagnostic = std::string(key_name) + "=0";
        }
        return 0;
    }
    if (!ResolveWorldTable(api, table_index)) {
        if (diagnostic != nullptr) {
            const WorldTableState& failed = ComponentState().world_tables[table_index];
            *diagnostic = std::string("unresolved[") +
                          (failed.failure != nullptr ? failed.failure : "unknown") + "]";
        }
        return 0;
    }
    WorldTableState& state = ComponentState().world_tables[table_index];
    // FRESH EVERY TIME -- the cached instance goes stale across an area load.  See
    // `WorldTableInstance`.
    il2cpp::Il2CppObject* const instance = WorldTableInstance(api, table_index);
    if (instance == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "no instance";
        return 0;
    }
    state.count = ReadWorldTableCount(api, table_index);
    std::int32_t id = config_id_used;
    void* arguments[1] = {&id};
    il2cpp::Il2CppObject* exception = nullptr;
    // HasKey first: `GetRowOffset` on an absent key is the call that would throw or return a
    // garbage offset, and a garbage offset is a garbage `this` inside `get_name`.
    il2cpp::Il2CppObject* const has =
        api.il2cpp_runtime_invoke(state.has_key, instance, arguments, &exception);
    if (exception != nullptr || has == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "HasKey threw";
        return 0;
    }
    const void* const has_payload = api.il2cpp_object_unbox(has);
    bool present = false;
    if (has_payload == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "HasKey unbox failed";
        return 0;
    }
    std::memcpy(&present, has_payload, sizeof(present));
    if (!present) {
        if (diagnostic != nullptr) {
            *diagnostic = std::string("noRow[") + key_name + "]";
        }
        return 0;
    }
    exception = nullptr;
    il2cpp::Il2CppObject* const offset_box =
        api.il2cpp_runtime_invoke(state.get_row_offset, instance, arguments, &exception);
    if (exception != nullptr || offset_box == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "GetRowOffset threw";
        return 0;
    }
    const void* const offset_payload = api.il2cpp_object_unbox(offset_box);
    if (offset_payload == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "GetRowOffset unbox failed";
        return 0;
    }
    std::int32_t row_offset = 0;
    std::memcpy(&row_offset, offset_payload, sizeof(row_offset));
    // THE ROW STRUCT IS BUILT HERE, LOCALLY -- WITH THE PADDING THE ROW ACTUALLY HAS.
    //
    // `TD*` rows are not all `{ int rowOffset; }`: `TDWorldSpawner` carries two fields ahead of
    // it (0x4), `TDCommonItem` has three (0x8), `TDWorldItem` more still (0x10).  A bare local
    // therefore hands `get_name` a `this` whose `rowOffset` field is not where the callee looks,
    // and the callee answers with whatever the padding happened to contain.  `descriptor
    // .row_offset_field` is that layout, read from the dump rather than assumed.
    //
    // The buffer is over-sized and zeroed so the bytes in front of the offset are deterministic
    // rather than stack leftovers -- some of these rows have real fields there, and leaving them
    // as garbage would reintroduce the same class of bug through a different door.
    std::uint8_t row[40]{};
    std::memcpy(row + descriptor.row_offset_field, &row_offset, sizeof(row_offset));
    exception = nullptr;
    il2cpp::Il2CppObject* const lang =
        api.il2cpp_runtime_invoke(state.get_name, row, nullptr, &exception);
    if (exception != nullptr || lang == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "row get_name threw";
        return 0;
    }
    // `TD*.get_name` returns a `LangString` BY VALUE, so `lang` is the boxed 16-byte struct, not
    // a managed string.  Its `value` property is what does the localisation -- reading the boxed
    // object as a string would read the `group` field's header instead.  Same two-step the
    // entity path uses, for the same reason.
    il2cpp::Il2CppClass* const lang_class = api.il2cpp_object_get_class(lang);
    if (lang_class == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "row get_name returned no class";
        return 0;
    }
    const il2cpp::MethodInfo* const get_value =
        api.il2cpp_class_get_method_from_name(lang_class, "get_value", 0);
    if (get_value == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "LangString.get_value missing";
        return 0;
    }
    exception = nullptr;
    il2cpp::Il2CppObject* const value =
        api.il2cpp_runtime_invoke(get_value, lang, nullptr, &exception);
    if (exception != nullptr || value == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "LangString.get_value threw";
        return 0;
    }
    const std::uint32_t written = ReadManagedString(api, value, out, capacity);
    if (written == 0 && diagnostic != nullptr) *diagnostic = "name was empty";
    return written;
}


// THE WHOLE LIST, PER ITEM -- WHICH IS WHAT A FROZEN TABLE CANNOT DO.
//
// The whole-list read works, and the freeze does not, in the same breath:
//
//     world[1/13] TDWorldSpawnerTable(spawnerId,rows=2156) -> "尘石"
//     WorldItemData -> 尘石
//
// `尘石` is the real localised name, so the table, the key and the 0x4 row layout are all
// correct.  But freezing there meant every item whose `spawnerId` is absent, zero, or not in the
// spawner table fell straight through to the `modelPath` resource name -- which is why the user
// saw Chinese on some items and `pre_world_*` on the rest.
//
// A table is not a global answer; it is an answer about ONE item.  So the list is walked per
// entity, newest-best first, and the first usable name wins.  `HasKey` gates every candidate, so
// a miss costs ONE invoke -- the whole list is cheaper than a single `GetRowOffset`+`get_name`
// pair, and the result is cached with the label like every other read.
//
// `g_world_name_table` is kept only as the fast path: the last table that worked is tried first,
// which makes the common case one lookup.
// A NAME, ONCE EARNED, IS NOT GIVEN BACK.
//
// The user watched `金麦` turn into an English resource name.  Every previous fix attacked the
// cause -- the wrong table, the wrong key, the wrong row layout, a stale instance -- and each
// was a real bug and each was still not this symptom, which arrives LATE, after a name has
// already been on screen.  That means the entity was named once, and a LATER re-read failed.
//
// Whatever makes a later read fail (an unloaded table, a rebuilt instance, a spawner that stops
// resolving after a transition), the label must not be allowed to regress: a downgrade to an
// internal resource path is never more correct than the localised name that was already found.
// So the name is remembered, and a failed re-read falls back to it instead of to `modelPath`.
//
// KEYED ON `BaseData.entityId @0x130`, NOT ON THE THREE CONFIG IDS.
//
// The id triple looked equivalent and is not: the live failures all carry `spawnerId=8000000`
// and `registeredID=-1`, so items that share a `configId` -- or that both carry the round
// sentinel -- collide, and the second item is handed the first item's name.  `entityId` is what
// the object is actually identified by, so a name can only ever be recalled for the entity it
// was found on.
//
// Fixed size and linearly searched: this is at most a few hundred live world entities, it is
// touched only on a label cache miss, and an eviction costs one re-resolution rather than
// correctness.

std::uint32_t LookupRememberedWorldName(std::uint64_t entity_id, char* out,
                                        std::uint32_t capacity) {
    if (out == nullptr || capacity == 0 || entity_id == 0) return 0;
    out[0] = 0;
    for (std::size_t index = 0; index < kRememberedWorldNameCount; ++index) {
        if (ComponentState().remembered_world_names[index].name[0] != 0 &&
            ComponentState().remembered_world_names[index].entity_id == entity_id) {
            std::snprintf(out, capacity, "%s", ComponentState().remembered_world_names[index].name);
            return static_cast<std::uint32_t>(std::strlen(out));
        }
    }
    return 0;
}

void RememberWorldName(std::uint64_t entity_id, const char* name) {
    if (name == nullptr || name[0] == 0 || entity_id == 0) return;
    RememberedWorldName& entry =
        ComponentState().remembered_world_names[ComponentState().remembered_world_name_next++ % kRememberedWorldNameCount];
    entry.entity_id = entity_id;
    std::snprintf(entry.name, sizeof(entry.name), "%.62s", name);
}

std::uint32_t ResolveWorldResourceLabel(const il2cpp::Api& api, std::int32_t config_id,
                                        std::int32_t spawner_id, std::int32_t registered_id,
                                        std::uint64_t entity_id, char* out,
                                        std::uint32_t capacity, std::string* diagnostic) {
    if (out == nullptr || capacity == 0) return 0;
    out[0] = 0;
    // THE STICKY NAME, CONSULTED FIRST.
    //
    // Every earlier attempt to fix the reversion worked on WHY a table answered, and each was
    // wrong.  This works on the consequence instead: once an entity has been named, it keeps
    // that name for the session.  A table that has been unloaded and rebuilt, a spawner that
    // stops resolving, an id that changes meaning after a map transition -- none of them can
    // take a name off the screen any more, which is the thing the user actually saw happen.
    char remembered[64]{};
    if (LookupRememberedWorldName(entity_id, remembered, sizeof(remembered)) != 0) {
        std::snprintf(out, capacity, "%.62s", remembered);
        return static_cast<std::uint32_t>(std::strlen(out));
    }
    // THE COLLECTING NODE FIRST, BECAUSE THAT IS WHERE GATHERABLE NAMES LIVE.
    //
    // `余烬菇`, `闪锡矿` and `月麻` are gathered nodes and they were the ones showing asset paths.
    // `TDWorldCollecting` carries both the `name` and the `spawnerId` list that identifies the
    // nodes it covers, so the mapping is a reverse lookup that the table itself supplies -- it is
    // simply not a `HasKey` lookup, which is why eighteen rounds of `Col:noRow[cid]` never found
    // it.  See `BuildCollectingNameIndex`.
    BuildCollectingNameIndex(api);
    char collecting[64]{};
    if (LookupCollectingName(spawner_id, collecting, sizeof(collecting)) != 0 &&
        IsUsableWorldName(collecting)) {
        ++ComponentState().world_table_wins[0];
        RememberWorldName(entity_id, collecting);
        std::snprintf(out, capacity, "%.62s", collecting);
        return static_cast<std::uint32_t>(std::strlen(out));
    }
    // THE ORDER IS FIXED.  THERE IS NO "LAST TABLE THAT WORKED" FAST PATH.
    //
    // There was one, and it was a correctness bug, not an optimisation.  It tried the table that
    // had won for a PREVIOUS item first, so the answer for one item depended on which item had
    // been read before it.  Item A wins with `Sp`, so `Sp` is tried first for item B -- and if
    // `Sp` happens to hold a row under B's id, B is named by a table that is not the one that
    // describes it.  That is exactly the reported behaviour: some names getting WORSE after a
    // change that only touched the order, and the same item reading differently depending on
    // what else is in the scene.
    //
    // A fixed order is the only version of this that can be reasoned about.  The cost is that a
    // table which will miss is still asked, which is one `HasKey` per table per cache miss --
    // and the list is short because scoring said so: `worldwin: Sp=15 CI=3` means the other
    // eighteen candidates have never once answered.
    for (std::size_t table = 0; table < kWorldNameTableCount; ++table) {
        char text[96]{};
        std::string why;
        if (ReadWorldResourceName(api, config_id, spawner_id, registered_id, table, text,
                                  sizeof(text), &why) != 0 &&
            IsUsableWorldName(text)) {
            ++ComponentState().world_table_wins[table];
            // NO CACHE INVALIDATION HERE, AND THAT IS DELIBERATE.
            //
            // The previous version cleared the whole label cache the first time a table answered,
            // to re-read labels that had been cached before tables worked.  That made the answer
            // depend on WHEN the wipe happened: an entity read before it kept its resource path,
            // the same entity read after it got Chinese, and which one happened depended on the
            // walk order and on what had already been cached.  It is the direct cause of "some
            // good ones got worse and some bad ones got better" after a change that only touched
            // the ordering.
            //
            // It is unnecessary now: the chain is fixed and complete, so the FIRST read of a
            // world item already tries every candidate.  A label is therefore right the first
            // time it is computed, and nothing needs re-reading.
            RememberWorldName(entity_id, text);
            if (std::snprintf(out, capacity, "%.62s", text) > 0) {
                return static_cast<std::uint32_t>(std::strlen(out));
            }
        }
        if (why.empty()) why = "no usable name";
        if (diagnostic != nullptr) {
            // ALIAS ONLY: thirteen tables have to fit in one status field, and the previous cap
            // cut the chain off at the fifth -- which is why the tables that had not been
            // reached stayed invisible for a round.
            *diagnostic += std::string(kWorldNameTables[table].alias) + ":" + why + " ";
        }
    }
    return 0;
}

// WHERE EVERY WORLD-ITEM LABEL CAME FROM, COUNTED.
//
// The run that produced this said "Chinese on some items, English on the rest", and the
// diagnostic as written could not answer the question a user actually asks: how many, and from
// which branch.  `labelSamples` shows ONE entity per class -- and the one it showed was
// `Blueprint[300031]`, which is a `configName`, not a table name.  A count per branch turns
// "some are Chinese" into a number.
//
// This also settled what the earlier `blueprint[NNN]` label was.  It is NOT written by this
// file: `BaseData.configName @0x150` HOLDS that text.  For an item the game has no localised
// name for, the game stores `Blueprint[` + blueprint id + `]` there itself, and the
// `ReadEntityConfigName` fallback below returns it verbatim.  An earlier round grepped for the
// literal `blueprint` and concluded the string was not ours -- wrong, because the format is
// assembled at runtime from the field's contents.
const char* const kWorldLabelSourceNames[kWorldLabelSourceCount] = {
    "table", "configName", "unnamed", "classTag",
};

std::uint32_t ReadEntityLabel(const il2cpp::Api& api, std::uintptr_t data_object,
                              char* out, std::uint32_t capacity, const char* data_class,
                              std::uint64_t id) {
    if (out == nullptr || capacity == 0) return 0;
    out[0] = 0;
    // ==========================================================================
    // THE TWO CALLS THIS USED TO MAKE, AND THE ONE IT MUST NEVER MAKE AGAIN
    // ==========================================================================
    //
    // SAFE, and what the label is built from: the entity's own data class name.  The walk has
    // already computed it into `data_class` via `il2cpp_object_get_class` +
    // `il2cpp_class_get_name` on a real managed object -- the same call pair behind the
    // `dataClasses` status histogram, which has been correct for many builds, long before any
    // name work started.
    //
    // UNSAFE, and now absent: `il2cpp_object_get_class` on the pointer read from
    // `<config>k__BackingField`.  That field is declared `TDEnemy`, a VALUE TYPE
    // (`struct TDEnemy { int rowOffset; }`), so the pointer is not the `Il2CppObject` that call
    // requires.  Calling it there is the shape that crashed three builds -- `9bbaced`,
    // `314324d` and `b16d728` -- while `a7c33b8` stayed stable purely because this function had
    // never actually run.
    //
    // THE NAME, ON TOP OF THAT SAFE BASE.
    //
    // The chain above is still not run -- it is still the one that requires an object where the
    // game stores an inline struct.  What replaces it goes through `il2cpp_runtime_invoke`,
    // which is the runtime's own entry point and handles both halves of what the old chain got
    // wrong: it boxes the value-type instance, and it allocates the hidden return buffer that a
    // by-value `LangString` needs.  `TryNameCandidate` reports which LINK failed, and the
    // `probe[i/n]` prefix in `nameProbe` says which candidate produced the verdict -- so a
    // failure is readable from `status` instead of being a silent class-name fallback.
    //
    // ==========================================================================
    // ONE ENTITY, ONE CANDIDATE PER TICK -- AND THE PATH ONLY OPENS ON A REAL NAME
    // ==========================================================================
    //
    // A previous `il2cpp_runtime_invoke` fallback crashed the game on first launch, and the
    // rule it closed with is explicit:
    //
    //     「在此之前不再启用任何托管调用。若需本地化显示名，须先做"单实体单次调用"验证。」
    //
    // A per-entity call on every tick is the exact shape that rule forbids.  The gate below is
    // that rule expressed as code rather than as a comment: `g_name_candidate_index` advances by
    // ONE per tick, so each tick makes at most one managed call, and a failure costs one call and
    // one published verdict rather than a crash inside a loop.  `g_name_path_proven` is the only
    // thing that enables the per-entity path, and it is set by nothing except real display text.
    //
    // THE GATE MUST BE ARMED ONLY FOR A CLASS THE PROBE CAN ACTUALLY PROBE.
    //
    // Arming on any entity with a data object is wrong: the first `WorldItemData` -- which has
    // no config getter at all -- would consume the one allowed call answering "this class is not
    // supported", which says nothing about `HeroData`.  The class is therefore chosen FIRST and
    // the gate second, so the call the gate admits is the call that answers the question.
    //
    // ==========================================================================
    // PROBE ONE CLASS PER TICK, REMEMBER PER CLASS WHETHER IT WORKED
    // ==========================================================================
    //
    // A previous `il2cpp_runtime_invoke` fallback crashed the game on first launch, and the
    // rule it closed with is explicit:
    //
    //     「在此之前不再启用任何托管调用。若需本地化显示名，须先做"单实体单次调用"验证。」
    //
    // That rule is `ProbeEntityNameSource`, and it is called from the WALK rather than from here.
    // See its comment: this function runs only on a label cache miss, which is exactly why the
    // probe stalls at 3 of 4 classes.
    //
    // `ComponentState().name_source_ok` is PER CLASS, not global, because the four chains are genuinely
    // independent: `TDHero` may resolve while `TDEnemy` does not, and a single flag would either
    // disable working classes or enable broken ones.  Only the classes that actually returned
    // text take the name path; the rest keep the class-name label.
    //
    // The name path, for the classes whose probe returned text.
    const std::size_t name_source = FindEntityNameSource(data_class);
    if (name_source < kEntityNameSourceCount && ComponentState().name_source_ok[name_source] &&
        data_object != 0) {
        char text[96]{};
        std::string diagnostic;
        if (ReadEntityName(api, data_object, name_source, text, sizeof(text), &diagnostic) != 0) {
            const int count = std::snprintf(out, capacity, "%.62s", text);
            if (count > 0) {
                const std::uint32_t size = static_cast<std::uint32_t>(count);
                return size < capacity ? size : capacity - 1;
            }
        }
    }
    // FALLBACK: `BaseData.configName`, which every entity data object carries.
    //
    // IT MUST NOT RUN FOR A WORLD ITEM, AND THAT ORDERING BUG COST NAMES.
    //
    // `configName @0x150` holds the game's own placeholder -- `Blueprint[300031]` -- for entities
    // it has no localised name for, and this branch runs BEFORE the world-resource chain below.
    // So any world item that carried a `configName` returned it and NEVER REACHED THE TABLE
    // LOOKUP.  The `configName=12` in the live counts is exactly that: twelve items that were
    // answered with a placeholder while a real name was one table query away.
    //
    // A placeholder is never better than a lookup that has not been tried, so the world path
    // gets first refusal and `configName` is its fallback instead of its gatekeeper.
    const bool is_world_item = data_object != 0 && data_class != nullptr &&
                               std::strcmp(data_class, "WorldItemData") == 0;
    if (data_object != 0 && !is_world_item) {
        char text[96]{};
        if (ReadEntityConfigName(api, data_object, text, sizeof(text)) != 0) {
            const int count = std::snprintf(out, capacity, "%.62s", text);
            if (count > 0) {
                // This branch is where `Blueprint[300031]` came from: `configName` is the game's
                // own placeholder for an entity it has no localised name for, and it is returned
                // verbatim.  Counted so the next run can say how many items land here.
                ++ComponentState().world_label_counts[kWorldLabelConfigName];
                const std::uint32_t size = static_cast<std::uint32_t>(count);
                return size < capacity ? size : capacity - 1;
            }
        }
    }
    // LAST RESORT: the readable tail of `modelPath`, for world resources specifically.
    if (is_world_item) {
        // One line, once, naming the three id fields and the two strings a world item carries --
        // so the step from "resource name" to the game's localised name is a lookup rather than
        // another guess.  See `DiagnoseWorldItem`.
        DiagnoseWorldItem(api, data_object);
        // THE LOCALISED NAME, ONCE A TABLE HAS BEEN PROVEN.  Tried before the resource-path
        // label because it is what the user actually asked for; the path label remains the
        // fallback for as long as no table has answered.
        std::int32_t config_id = 0;
        std::int32_t spawner_id = 0;
        std::int32_t registered_id = 0;
        static_cast<void>(ReadValue(data_object + kBaseDataConfigIdOffset, &config_id));
        static_cast<void>(ReadValue(data_object + kWorldEntitySpawnerIdOffset, &spawner_id));
        // `WorldItemData.registeredID @0x498` -- the last untried id on the object.  Every
        // logged failure so far carried the round sentinel `spawnerId=8000000`, so the spawner
        // route cannot answer for those items and this is the remaining candidate key.
        static_cast<void>(ReadValue(data_object + kWorldItemRegisteredIdOffset, &registered_id));
        if (config_id != 0 || spawner_id != 0 || registered_id != 0) {
            char text[96]{};
            std::string diagnostic;
            // THE WHOLE LIST, FOR THIS ITEM.  A frozen table answered only for the items whose id
            // it happened to hold; the rest fell back to the resource path.  See
            // `ResolveWorldResourceLabel`.
            if (ResolveWorldResourceLabel(api, config_id, spawner_id, registered_id, id, text,
                                          sizeof(text), &diagnostic) != 0) {
                const int count = std::snprintf(out, capacity, "%.62s", text);
                if (count > 0) {
                    ++ComponentState().world_label_counts[kWorldLabelTable];
                    const std::uint32_t size = static_cast<std::uint32_t>(count);
                    return size < capacity ? size : capacity - 1;
                }
            } else if (ComponentState().world_failure_logs < kWorldFailureLogLimit) {
                // EVERY ITEM THAT FELL THROUGH, WITH ITS IDS AND THE FULL CHAIN.
                //
                // The Chinese seen on some items and `pre_world_*` on the rest is a per-item
                // difference, so the diagnostic that matters is the one that names the ids of
                // the items that failed -- not the ids of the first item, which succeeded.
                //
                // The chain is printed whole, against all thirteen tables that are actually
                // tried: an earlier cap cut it off after the third table, which is exactly where
                // the interesting tables start.
                ++ComponentState().world_failure_logs;
                char report[460]{};
                char path[80]{};
                static_cast<void>(ReadEntityModelName(api, data_object, path, sizeof(path)));
                std::snprintf(report, sizeof(report),
                              "worldfail[%d] cid=%d sid=%d rid=%d model=\"%.30s\" :: %.330s || ",
                              ComponentState().world_failure_logs, config_id, spawner_id, registered_id, path,
                              diagnostic.c_str());
                ComponentState().name_probe_log += report;
                ComponentState().name_probe = ComponentState().name_probe_log;
            }
        }
        // THE GAME'S OWN PLACEHOLDER, NOW AS A FALLBACK RATHER THAN A GATE.
        //
        // Reaching here means no table named this item.  `configName` is still better than an
        // internal resource path when it exists, because it is what the game itself would show.
        {
            char text[96]{};
            if (ReadEntityConfigName(api, data_object, text, sizeof(text)) != 0) {
                const int count = std::snprintf(out, capacity, "%.62s", text);
                if (count > 0) {
                    ++ComponentState().world_label_counts[kWorldLabelConfigName];
                    const std::uint32_t size = static_cast<std::uint32_t>(count);
                    return size < capacity ? size : capacity - 1;
                }
            }
        }
        char text[96]{};
        if (ReadEntityModelName(api, data_object, text, sizeof(text)) != 0) {
            // NO LABEL FOR AN OBJECT THE GAME ITSELF DOES NOT NAME.
            //
            // This used to return the resource path (`pre_repair_empty`, `pre_forestfield_fog_04`),
            // which is how the ESP ended up with the majority of its world labels in English.  The
            // evidence that there is nothing better to show is now complete for these objects:
            //
            //   * twenty candidate tables, every one of them `noRow` for their `configId`
            //   * `TDWorldSpawner` HAS a row for their `spawnerId` and its `name` is EMPTY
            //   * `WorldItemData.registeredID` is -1, so that key has no value to look up
            //
            // `pre_repair_empty` is an internal asset name.  It is not a name in any language, and
            // printing it made it impossible to see which labels were real -- which is also why
            // "some got better and some got worse" was so hard to read from the screen: real names
            // and asset paths looked equally like answers.
            //
            // Returning zero length lets the plugin skip the label; it already tests
            // `label_size != 0` before drawing, so nothing in the plugin changes.  The ENTITY BOX
            // is unaffected -- it comes from the transform -- so an unnamed object stays visible.
            ++ComponentState().world_label_counts[kWorldLabelUnnamed];
            static_cast<void>(text);
            return 0;
        }
    }
    // A world item reaches here with no name from any source, and a class-name-plus-id label is
    // the same kind of non-answer as the asset path.  Silent, for the same reason.
    if (is_world_item) {
        ++ComponentState().world_label_counts[kWorldLabelUnnamed];
        return 0;
    }
    ++ComponentState().world_label_counts[kWorldLabelClassTag];
    return WriteClassLabel(out, capacity, data_class != nullptr ? data_class : "", id);
}

// The label that is always safe: the entity's data class name plus its id.
//
// Both inputs are already on hand -- `data_class` is filled by the walk from
// `il2cpp_class_get_name` on a real managed object, and `id` keys the entity cache.  Nothing here
// dereferences game memory, so this function cannot be why a frame crashes.
//
// `%.40s` bounds the class name to the width of the record's own buffer, so a malformed name
// cannot overflow even if the caller passed something longer.
std::uint32_t WriteClassLabel(char* out, std::uint32_t capacity, const char* data_class,
                              std::uint64_t id) {
    if (out == nullptr || capacity == 0) return 0;
    const int written = std::snprintf(out, capacity, "%.40s#%llu", data_class,
                                      static_cast<unsigned long long>(id));
    if (written <= 0) return 0;
    const std::uint32_t size = static_cast<std::uint32_t>(written);
    return size < capacity ? size : capacity - 1;
}

// WHICH CLASSES ARE ACTUALLY IN THE SCENE.
//
// `kinds: unclassified=71 enemy=0` said only that classification failed; it could not say
// whether the scene held no monsters or whether the camp values were unreadable, and those need
// opposite fixes.  This histogram is the same distinction for names: counting every distinct
// live data class turns "no names" into either "these classes are not in my table" or "my table
// matched but the field chain is wrong", and the two are not guessable from a count alone.
//
// The names are pre-copied into the binding, so no runtime call happens here.
void CountEntityClasses(std::uintptr_t data_object, std::size_t* histogram,
                        std::size_t histogram_size) {
    if (data_object == 0 || il2cpp::Functions().il2cpp_object_get_class == nullptr) return;
    il2cpp::Il2CppClass* const klass = il2cpp::Functions().il2cpp_object_get_class(
        reinterpret_cast<il2cpp::Il2CppObject*>(data_object));
    if (klass == nullptr) return;
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(klass);
    for (std::size_t index = 0; index < ComponentState().label_binding.source_count; ++index) {
        const LabelSource& source = ComponentState().label_binding.sources[index];
        if (source.data_class == address && index < histogram_size) {
            ++histogram[index];
            return;
        }
    }
}
// knows where a static lives for the concrete instantiation it was handed, and it performs
// the GC write barrier, which reading the memory directly would skip.
template <typename T>
bool ReadStaticField(const il2cpp::Api& api, il2cpp::FieldInfo* field, T* value) {
    if (api.il2cpp_field_static_get_value == nullptr || field == nullptr) return false;
    T buffer{};
    api.il2cpp_field_static_get_value(field, &buffer);
    *value = buffer;
    return true;
}

// Resolve `il2cpp::FieldInfo` -> byte offset via the runtime.  Returns `fallback` when the
// runtime cannot answer, and records that fact in `exact` so the caller can tell a
// runtime-confirmed offset from a dump-derived one.
std::size_t FieldOffsetOr(
    const il2cpp::Api& api, il2cpp::Il2CppClass* klass, const char* name,
    std::size_t fallback, bool* exact) {
    if (klass != nullptr && api.il2cpp_class_get_field_from_name != nullptr &&
        api.il2cpp_field_get_offset != nullptr) {
        il2cpp::FieldInfo* const field = api.il2cpp_class_get_field_from_name(klass, name);
        if (field != nullptr) {
            const std::size_t offset = api.il2cpp_field_get_offset(field);
            if (offset != static_cast<std::size_t>(-1)) {
                *exact = true;
                return offset;
            }
        }
    }
    *exact = false;
    return fallback;
}

// A raw `Vector3`, used only to carry a return value out of an invocation.  Deliberately not
// `EntityRecord::center`, so the reader cannot accidentally publish a position it has not
// validated.
//
// `il2cpp_runtime_invoke(Transform::get_position)` was measured at ~2.5 ms per call on the
// live game.  At 82 entities per refresh it made the ESP the frame rate.  An entity's position
// does not need to be sampled at the display rate, so it is sampled once per
// `kPositionRefreshTicks` and reused in between.
//
// The entry holds the TRANSFORM POINTER as well as the id, and a change in either is a miss.
// The id is the data object's address, which IL2CPP can recycle after a scene change; keying
// on a recycled address alone would put an old entity's position on a new entity's box --
// a plausible-looking wrong answer, which is the failure mode this whole file keeps guarding
// against.
// ============================================================================================
// DIRECT METHOD CALLS -- the same thing the sibling UE5 host does, and the reason it is fast.
// ============================================================================================
//
// `il2cpp_runtime_invoke` is the runtime's REFLECTION entry point.  It marshals an argument
// array, installs an exception frame, and -- the part that makes it unusable here -- BOXES
// every value-type return into a freshly allocated managed object.  Measured on the live game,
// every entity position cost one such call per entity per refresh, and the walk containing them
// was 213 ms of a single game tick.
//
// The runtime does not require that path.  `MethodInfo::methodPointer` is the compiled body, and
// calling it directly is what a native host is supposed to do.  `src/mem/il2cpp_dump.cpp` reads
// that field for the dumper and range-checks it rather than trusting it, because the offset is 0
// only by a Unity declaration from a version other than this target's.
//
// --------------------------------------------------------------------------------------------
// THE FAST PATH THAT WAS REMOVED HERE, AND WHAT IT ACTUALLY GOT WRONG
// --------------------------------------------------------------------------------------------
//
// A direct-call fast path existed before, emptied the entity set, added a periodic hitch, and was
// deleted with the note that "the pointer being read was not the function body".  THAT DIAGNOSIS
// WAS WRONG, and the wrong diagnosis is why the fix looked impossible.  The pointer was right.
//
// The body it pointed at begins:
//
//     48 89 5C 24 08   mov  [rsp+8], rbx
//     57               push rdi
//     48 83 EC 20      sub  rsp, 0x20
//     33 C0            xor  eax, eax
//     48 8B FA         mov  rdi, rdx
//     48 89 01         mov  [rcx], rax        <-- twelve bytes written THROUGH RCX
//     48 8B D9         mov  rbx, rcx
//     89 41 08         mov  [rcx+8], eax      <-- the Vector3 return buffer, zeroed
//
// which is a `Vector3` returned in a CALLER-SUPPLIED BUFFER: `float* out` in RCX, `this` in RDX.
// The removed path passed `this` in RCX, so the callee zeroed the first twelve bytes of a live
// managed object -- its class pointer and its sync block.  That is precisely "the entity set
// emptied and there was a periodic hitch": the walk's own data objects were being destroyed by
// its own position read.
//
// So the ABI is not a guess any more, and it is not verified by comparing return values after the
// fact either -- a check that runs after the dangerous act is not a check.  Three things are
// verified BEFORE the first call, all of them facts about the bytes:
//
//   1. The profile record `unity.transform.get_position` carries the first 24 bytes of THIS
//      build's body (dumped offline: dump.cs line 2343381, RVA 0xC687860).  The address the
//      runtime resolves must still hold those bytes.  A build whose body differs is REFUSED, and
//      the refusal is reported -- which is the difference between "this cannot be verified" and
//      "this was not verified".
//   2. The prologue is what the calling convention above was read from, so pinning the prologue
//      pins the convention.
//   3. The first call is made with the return buffer being a LOCAL ARRAY, and the managed object
//      it is passed is compared byte-for-byte across the call (`Canary`).  If anything about the
//      convention is still wrong, the damage lands in our own buffer, not in the game's objects,
//      and the canary says so.
//
// A fourth check is the cheap one: the first direct result must equal what `il2cpp_runtime_invoke`
// returns for the same transform in the same tick.  It is not the guard (it runs after the call);
// it is the assertion that the two paths agree, and disagreement rejects the fast path.
//
// Refusal is terminal.  A mismatch, a fault or a disagreement disables the direct call for the
// life of the process and `il2cpp_runtime_invoke` keeps being used, with the reason published in
// the status document's `directPosition` field.


std::size_t PositionSlot(std::uint64_t id) noexcept {
    const std::uint64_t mixed = id * 0x9E3779B97F4A7C15ull;
    return static_cast<std::size_t>(mixed >> 40) % kPositionCacheCapacity;
}

bool ReadCachedPosition(std::uint64_t id, std::uintptr_t transform, std::uint64_t now_tick,
                        Vector3Out* out) {
    const std::size_t slot = PositionSlot(id);
    for (std::size_t probe = 0; probe < 8; ++probe) {
        const PositionCacheEntry& entry =
            ComponentState().position_cache[(slot + probe) % kPositionCacheCapacity];
        if (entry.id == id && entry.transform == transform) {
            if (now_tick - entry.tick > kPositionRefreshTicks) return false;
            *out = entry.position;
            return true;
        }
    }
    return false;
}

void StoreCachedPosition(std::uint64_t id, std::uintptr_t transform, const Vector3Out& position,
                         std::uint64_t now_tick) {
    const std::size_t slot = PositionSlot(id);
    for (std::size_t probe = 0; probe < 8; ++probe) {
        PositionCacheEntry& entry = ComponentState().position_cache[(slot + probe) % kPositionCacheCapacity];
        if (entry.id == id || entry.id == 0) {
            entry.id = id;
            entry.transform = transform;
            entry.position = position;
            entry.tick = now_tick;
            return;
        }
    }
    ++ComponentState().position_cache_rejects;
}
// A destroyed `UnityEngine.Object` still has a reachable managed pointer but a null native
// one, and calling a method on it would fault inside the runtime.  Defined here rather than
// beside the camera that also uses it, because the entity walk needs it earlier in the file.
std::uintptr_t ReadCameraNative(std::uintptr_t object) {
    std::uintptr_t native = 0;
    if (!ReadValue(object + kOffCachedPtr, &native)) return 0;
    return native;
}

void MultiplyMatrix4x4(const float lhs[16], const float rhs[16], float out[16]) {
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            double sum = 0.0;
            for (int inner = 0; inner < 4; ++inner) {
                sum += static_cast<double>(lhs[row * 4 + inner]) *
                       static_cast<double>(rhs[inner * 4 + column]);
            }
            out[row * 4 + column] = static_cast<float>(sum);
        }
    }
}

// Project one world point through `Camera.projectionMatrix * Camera.worldToCameraMatrix`, in
// the game's own pixels, WITH THE DIVIDE DONE HERE.
//
// THE DIVIDE IS THE ENTIRE POINT.  A point behind the near plane comes back from a raw matrix
// multiply with a NEGATIVE `w`, and dividing by it mirrors the result through the origin --
// which is how an entity behind the player lands in front, and how one such corner stretches a
// box across the whole screen.  Unity's own convention is that a clip-space `w <= 0` is not
// visible, so it is reported as a failure rather than divided through.
bool ProjectWorldPoint(const float vp[16], const double world[3], double viewport_width,
                       double viewport_height, double screen[2], double* depth) {
    const double x = static_cast<double>(vp[0]) * world[0] + static_cast<double>(vp[1]) * world[1] +
                     static_cast<double>(vp[2]) * world[2] + static_cast<double>(vp[3]);
    const double y = static_cast<double>(vp[4]) * world[0] + static_cast<double>(vp[5]) * world[1] +
                     static_cast<double>(vp[6]) * world[2] + static_cast<double>(vp[7]);
    const double w = static_cast<double>(vp[12]) * world[0] +
                     static_cast<double>(vp[13]) * world[1] +
                     static_cast<double>(vp[14]) * world[2] + static_cast<double>(vp[15]);
    if (!(w > 1e-9)) return false;
    // Depth in metres is carried in the third clip coordinate once divided; Unity's is positive
    // in front of the camera for the standard perspective projection.
    const double ndc_z = (static_cast<double>(vp[8]) * world[0] +
                          static_cast<double>(vp[9]) * world[1] +
                          static_cast<double>(vp[10]) * world[2] +
                          static_cast<double>(vp[11])) /
                         w;
    screen[0] = (x / w * 0.5 + 0.5) * viewport_width;
    // The viewport's y grows DOWNWARD while clip-space y grows upward: the same flip the
    // overlay applies when it converts clip to pixels (`FrameProject`).
    screen[1] = (0.5 - y / w * 0.5) * viewport_height;
    if (depth != nullptr) *depth = ndc_z;
    return std::isfinite(screen[0]) && std::isfinite(screen[1]);
}

// Call `Transform::get_position` on a live transform.
//
// The same mechanism as `SampleCameraPoint`, for the same reason: it passes a real managed
// object as the instance and unboxes the managed return, rather than assuming how the runtime
// marshals anything.  The difference is that this method takes no parameters, so the argument
// block is null, which `il2cpp_runtime_invoke` accepts.

// The position call, through the runtime's own reflection entry point.
//
// This is now the FALLBACK, not the design.  `ReadDirectPosition` below is tried first and this
// is what runs when it was refused or has not been adopted yet.  It is kept for exactly that
// reason, and it is still the only route that needs no per-build data at all.
bool InvokeGetPosition(const il2cpp::Api& api, const LiveBinding& binding,
                       std::uintptr_t transform, Vector3Out* out) {
    if (api.il2cpp_runtime_invoke == nullptr || api.il2cpp_object_unbox == nullptr) {
        return false;
    }
    il2cpp::Il2CppObject* exception = nullptr;
    il2cpp::Il2CppObject* const returned = api.il2cpp_runtime_invoke(
        reinterpret_cast<const il2cpp::MethodInfo*>(binding.get_position),
        reinterpret_cast<il2cpp::Il2CppObject*>(transform), nullptr, &exception);
    if (exception != nullptr || returned == nullptr) return false;
    const void* const payload = api.il2cpp_object_unbox(returned);
    if (payload == nullptr) return false;
    // `UnityEngine.Vector3` is three consecutive floats at the head of its payload -- the same
    // fact `SampleCameraPoint` relies on, confirmed by the dump's `x@0x10, y@0x14, z@0x18`.
    const float* const values = static_cast<const float*>(payload);
    out->x = values[0];
    out->y = values[1];
    out->z = values[2];
    return true;
}

// ============================================================================================
// THE DIRECT POSITION CALL -- see the long note beside `PositionSlot` for the machine-code
// evidence behind the calling convention and for what the removed attempt got wrong.
// ============================================================================================

// `void (*)(float* out, void* self, const MethodInfo*)`.
//
// The third parameter is never read by this body (it takes no arguments and its `MethodInfo*` is
// a formality), but it is passed because that is the convention the generated code was compiled
// against, and calling a function with fewer arguments than it declares is only accidentally
// harmless.
using PositionGetterFn = void (*)(float* out, void* self, const void* method);

// The call itself, SEH-guarded.
//
// POD ONLY, and that is a compiler requirement rather than a style: MSVC refuses `__try` in a
// function that has objects needing unwinding (C2712), and the alternative -- catching the fault
// in the caller -- would leave the caller's `std::scoped_lock` and `std::vector` mid-frame.  A
// fault here is contained and reported as `false`; it is not a crash and it is not a wrong
// number, because the output buffer belongs to this function's caller.
bool CallPositionGetterRaw(std::uintptr_t body, float* out, void* self,
                           const void* method) noexcept {
    __try {
        reinterpret_cast<PositionGetterFn>(body)(out, self, method);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// Reads the first `count` bytes at `address`, or false when they cannot be read.
bool ReadBytesAt(std::uintptr_t address, unsigned char* out, std::size_t count) {
    const auto bytes = mem::ReadMemory(address, count);
    if (!bytes.has_value() || bytes->size() != count) return false;
    std::memcpy(out, bytes->data(), count);
    return true;
}

// THE ADOPTION TEST, run once, on a live `Transform` that is already in hand.
//
// `expected` is what `il2cpp_runtime_invoke` returned for the SAME transform in the SAME tick, so
// the two paths are compared on one observation rather than on two samples of a moving object --
// and the comparison still allows a few ULPs, because the object may move between the two calls
// (see step 4).
//
// Order matters and is the whole point: the profile bytes are checked before the call, the
// managed object is canaried across it, and only then is the value compared.  The first failure
// wins and is recorded verbatim.
void TryAdoptDirectPositionCall(const il2cpp::Api& api, std::uintptr_t get_position_method,
                                std::uintptr_t transform, const Vector3Out& expected) {
    DirectPositionCall& call = ComponentState().position_call;
    if (call.state != static_cast<int>(DirectPositionState::untried)) return;
    const auto reject = [&call](const std::string& why) {
        call.state = static_cast<int>(DirectPositionState::rejected);
        std::snprintf(call.reason, sizeof(call.reason), "%s", why.c_str());
    };

    // The switch is checked FIRST, and its refusal is recorded like any other: with the direct
    // call off, the walk is on the 1.29 ms reflection route and the divisor floor has to apply,
    // which is what a `rejected` state triggers.  Reporting `untried` here would leave the walk
    // unthrottled on the slow path.
    if (!EntityDirectPositionEnabled()) {
        reject("disabled by configuration ([Performance] DirectPositionCall=0)");
        return;
    }
    if (get_position_method == 0) {
        reject("no MethodInfo for Transform::get_position");
        return;
    }
    // ---- 1. THE BODY ADDRESS, FROM THE PROFILE, BYTES INCLUDED ------------------------------
    //
    // `UnityBuildProfile::Resolve` tries the metadata route first (build-independent) and the
    // prologue-checked RVA second.  Both return an address; neither is called yet.
    std::string profile_error;
    const UnityBuildProfile profile =
        UnityBuildProfile::Select(EntityBuildProfilePath(), {}, &profile_error);
    if (!profile.Valid()) {
        reject("profile not loaded: " + profile_error);
        return;
    }
    const UnityResolvedMethod resolved = profile.Resolve("unity.transform.get_position");
    if (!resolved.resolved) {
        reject("not resolved: " + resolved.how);
        return;
    }
    // ---- 2. THE BYTES AT IT MUST BE THIS BUILD'S BODY ---------------------------------------
    //
    // This is the check the removed attempt did not have, and it is what makes the calling
    // convention below a verified fact rather than an assumption: the convention was read from
    // these very bytes, so if they are not there, the convention is not established either.
    const std::vector<unsigned char> recorded =
        profile.PrologueFor("unity.transform.get_position");
    if (recorded.empty()) {
        reject("profile has no prologue for unity.transform.get_position");
        return;
    }
    std::vector<unsigned char> found(recorded.size(), 0);
    if (!ReadBytesAt(resolved.address, found.data(), found.size())) {
        reject("cannot read the body at the resolved address");
        return;
    }
    if (std::memcmp(found.data(), recorded.data(), recorded.size()) != 0) {
        char detail[128]{};
        std::snprintf(detail, sizeof(detail), "prologue mismatch (resolved via %s)",
                      resolved.how.c_str());
        reject(detail);
        return;
    }
    // The address the METADATA route resolves must be the same body the profile describes.  If
    // they differ, one of the two is describing something else, and calling either is not
    // something this file does on a disagreement.
    if (resolved.how == "metadata") {
        std::uintptr_t metadata_pointer = 0;
        const auto pointer_bytes = mem::ReadMemory(get_position_method, sizeof(metadata_pointer));
        if (!pointer_bytes.has_value() || pointer_bytes->size() != sizeof(metadata_pointer)) {
            reject("cannot read MethodInfo::methodPointer");
            return;
        }
        std::memcpy(&metadata_pointer, pointer_bytes->data(), sizeof(metadata_pointer));
        if (metadata_pointer != resolved.address) {
            reject("metadata pointer and profile address disagree");
            return;
        }
    }
    // ---- 3. THE CALL, WITH THE MANAGED OBJECT CANARIED ACROSS IT ----------------------------
    unsigned char before[16]{};
    unsigned char after[16]{};
    if (!ReadBytesAt(transform, before, sizeof(before))) {
        reject("cannot read the managed object before the call");
        return;
    }
    float values[3]{};
    if (!CallPositionGetterRaw(resolved.address, values, reinterpret_cast<void*>(transform),
                               reinterpret_cast<const void*>(get_position_method))) {
        reject("the direct call faulted");
        return;
    }
    if (!ReadBytesAt(transform, after, sizeof(after))) {
        reject("cannot read the managed object after the call");
        return;
    }
    if (std::memcmp(before, after, sizeof(before)) != 0) {
        reject("the direct call wrote to the managed object (wrong calling convention)");
        return;
    }
    // ---- 4. THE TWO PATHS MUST AGREE --------------------------------------------------------
    //
    // A FEW ULPs, not an exact match, and the reason is physical rather than sloppy: the object
    // can move between the reflection call and this one, so an exact comparison would refuse a
    // correct fast path for a fast-moving entity -- a false refusal that costs the frame rate it
    // was meant to save.  The tolerance is relative 1e-5, about eighty float ULPs at 1.0: far
    // below what any wrong field, wrong offset or wrong convention produces (whole units, or
    // orders of magnitude) and far above the distance covered in the microseconds between the two
    // calls.
    const auto close_enough = [](double a, double b) {
        const double scale = std::max({1.0, std::fabs(a), std::fabs(b)});
        return std::fabs(a - b) <= 1e-5 * scale;
    };
    if (!close_enough(static_cast<double>(values[0]), expected.x) ||
        !close_enough(static_cast<double>(values[1]), expected.y) ||
        !close_enough(static_cast<double>(values[2]), expected.z)) {
        char detail[160]{};
        std::snprintf(detail, sizeof(detail),
                      "direct call disagrees with the reflection call (%.6g,%.6g,%.6g vs "
                      "%.6g,%.6g,%.6g)",
                      static_cast<double>(values[0]), static_cast<double>(values[1]),
                      static_cast<double>(values[2]), expected.x, expected.y, expected.z);
        reject(detail);
        return;
    }
    call.body = resolved.address;
    call.method = get_position_method;
    call.state = static_cast<int>(DirectPositionState::adopted);
    std::snprintf(call.reason, sizeof(call.reason), "adopted via %s", resolved.how.c_str());
}

// One position, through the direct call when it was adopted.
//
// `false` means "not available", never "the position is (0,0,0)": the caller falls back to the
// reflection route, which is also what happens for every call made before adoption.
bool ReadDirectPosition(std::uintptr_t transform, Vector3Out* out) {
    DirectPositionCall& call = ComponentState().position_call;
    if (call.state != static_cast<int>(DirectPositionState::adopted) || transform == 0) return false;
    float values[3]{};
    const auto started = std::chrono::steady_clock::now();
    const bool ok = CallPositionGetterRaw(call.body, values, reinterpret_cast<void*>(transform),
                                          reinterpret_cast<const void*>(call.method));
    call.micros += std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - started)
                       .count();
    ++call.reads;
    if (!ok) {
        // A fault is contained, so it is never fatal -- but a call that keeps faulting is a call
        // whose preconditions this file has wrong, and continuing to make it would be choosing
        // the fast path over the game's stability.  After the limit the path is refused for the
        // rest of the session and the reason is published; the entity is served by the fallback
        // either way.
        ++call.faults;
        if (call.faults >= kDirectPositionFaultLimit) {
            call.state = static_cast<int>(DirectPositionState::rejected);
            std::snprintf(call.reason, sizeof(call.reason),
                          "the direct call faulted %lld times; fell back to reflection",
                          static_cast<long long>(call.faults));
        }
        return false;
    }
    out->x = values[0];
    out->y = values[1];
    out->z = values[2];
    return true;
}

// Read one live entity into `out`.
//
// POSITION COMES FROM A METHOD CALL, NOT FROM A FIELD, AND THAT IS THE WHOLE POINT.
//
// The live entity's transform is `BaseData::<transform>` (0xA0) -> `RelativeTransform`, whose
// only interesting member is `m_transform` (0x10) -- a `UnityEngine.Transform`.  There is NO
// managed position field anywhere on that path: `UnityEngine.Transform`'s managed body is
// EMPTY (dump:67674) and its position lives in Unity's native object behind `m_CachedPtr`,
// which is the undocumented layout this project refuses to guess.
//
// `Transform::get_position` is a live managed method, so it can simply be CALLED.  One
// invocation per entity returns a boxed `Vector3` whose three floats are read directly.
//
// This costs one small managed allocation per entity per tick.  That is acceptable HERE and
// nowhere else in this file, for the reason the camera samples already established: the call
// is made on the GAME thread, the thread the runtime owns and that the plugin host's own
// Update callbacks allocate on.  The same call from the diagnostic pipe's thread is the crash
// this file records for that thread.
// THE EIGHT PROJECTED CORNERS OF ONE RECORD, from the ENGINE'S OWN view-projection matrix.
//
// Factored out of `ReadEntity` because there are now TWO callers with different cadences: the walk
// (which re-reads the entity and projects what it just read) and the per-tick re-projection (which
// projects a centre that is already published).  Both must produce the same pixels for the same
// centre, and a second copy of this arithmetic is exactly how the two would stop agreeing.
//
// `camera == nullptr` and an invalid matrix both leave the mask at 0 rather than clamping a
// coordinate: a box with a corner the matrix rejects has no correct screen-space extent, and
// drawing one anyway is the "one giant box across the screen" bug this project already fixed once.
void ProjectRecordCorners(const CameraBinding* camera, EntityRecord* record) {
    record->screen_mask = 0;
    if (camera == nullptr || !camera->view_projection_valid) {
        return;
    }
    const double viewport_width = camera->pixel_width;
    const double viewport_height = camera->pixel_height;
    for (std::uint32_t corner = 0; corner < 8; ++corner) {
        double world[3]{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double sign = (corner & (1U << axis)) != 0 ? 1.0 : -1.0;
            world[axis] = record->center[axis] + kApproxHalfExtent[axis] * sign;
        }
        double screen[2]{};
        if (!ProjectWorldPoint(camera->view_projection, world, viewport_width, viewport_height,
                               screen, nullptr)) {
            // Behind the near plane.  The bit is left out rather than the coordinate being
            // clamped: see the note above.
            continue;
        }
        record->screen_points[corner][0] = screen[0];
        record->screen_points[corner][1] = screen[1];
        record->screen_mask |= (1U << corner);
    }
}

bool ReadEntity(const il2cpp::Api& api, const LiveBinding& binding, std::uintptr_t handle,
                const CameraBinding* camera_binding, EntityRecord* out,
                std::uintptr_t* data_out = nullptr) {
    std::uintptr_t entity = 0;
    if (!ReadValue(handle + binding.off_handle_entity, &entity) || entity == 0) return false;
    std::uintptr_t data = 0;
    if (!ReadValue(entity + binding.off_entity_data, &data) || data == 0) return false;
    // Handed back so the walk can run the ONE-ENTITY NAME PROBE against a real data pointer,
    // without adding a member to `EntityRecord` that exists only for a diagnostic.
    if (data_out != nullptr) *data_out = data;
    if (data_out != nullptr) *data_out = data;
    out->data = data;

    // The transform chain AND the liveness check now live in `cabbird.unity.transform`, one
    // implementation shared with the player service instead of two.  It reads
    // `BaseData::<transform>` -> `RelativeTransform::m_transform` and then requires a non-null
    // native `m_CachedPtr`, because a destroyed managed wrapper still has a reachable address
    // and calling a method on one faults inside the runtime.
    std::uintptr_t transform = 0;
    if (!ResolveUnityTransformFromData(data, &transform)) return false;

    // IDENTITY FIRST, because the position cache below is keyed on it.  It is the data
    // object's address, which is stable for the object's life and costs nothing.
    out->id = static_cast<std::uint64_t>(data);
    // The transform too, and for the same reason: the per-tick cheap path re-reads this entity's
    // position through it long after this walk has finished.
    out->transform = transform;

    // THE LIVE CLASS NAME, recorded before anything can fail on this entity.
    //
    // This is the number that turns "the name lookup returned nothing" into a fact.  It is read
    // through the runtime, and then compared against the label table so the summary can say
    // whether a class is present-but-unbound (my table is incomplete) or simply absent (there is
    // nothing to name here).  Those need opposite fixes.
    if (api.il2cpp_object_get_class != nullptr && api.il2cpp_class_get_name != nullptr) {
        il2cpp::Il2CppClass* const klass = api.il2cpp_object_get_class(
            reinterpret_cast<il2cpp::Il2CppObject*>(data));
        if (klass != nullptr) {
            const char* const name = api.il2cpp_class_get_name(klass);
            if (name != nullptr) {
                std::snprintf(out->data_class, sizeof(out->data_class), "%s", name);
            }
        }
    }
    // ==========================================================================
    // CATEGORY FROM THE ENTITY'S OWN CLASS NAME **AND** THE GAME'S OWN WORLD TYPE
    // ==========================================================================
    //
    // The class name alone is not enough, and using it alone is what puts NPCs and monsters in
    // the same category: that version read
    //
    //     MonsterData -> enemy, NpcData -> enemy
    //
    // which collapses two facts the game keeps apart.  A monster is a thing you fight; an NPC is a
    // thing you talk to.  `EMonsterType` and `EWorldObjectType` are the game's own answer, and the
    // second one is already sitting on every `WorldEntityData` at a known offset.
    //
    // `WorldEntityData::worldType` is `EWorldObjectType` at 0x3C0 (dump.cs:291710).  It must NOT
    // be read for the four classes that re-purpose that same offset for their config: `HeroData`
    // redeclares 0x3C0 as `TDHero heroConfig` (dump.cs:277550).  Reusing a sub-class's offset on a
    // base-class read is the exact mistake that cost this project a session before, so the
    // name-based branches come first and the offset is only consulted for classes that do not
    // shadow it.
    //
    // Confirmed in the dump: `EWorldObjectType { None=0, Npc=1, NpcKiBo=2, Item=3, Environment=4,
    // BreakableItem=12, PuzzleItem=13, ... Chest=30, ... }` (dump.cs:195779).  Anything that is
    // neither Npc nor NpcKiBo is a world object rather than a person.
    {
        // NOTE: `EWorldObjectType::Npc` (1) and `NpcKiBo` (2) are deliberately NOT tested here.
        // They are the values a spawner would record for the object it places, and testing them
        // was this block's previous rule -- which could never fire for the NPCs the user actually
        // has, because NPC-ness is decided by the `NpcData` class branch above.  Kept as a
        // comment rather than dead constants so the next reader does not re-add the test.
        const char* const klass_name = out->data_class;
        std::size_t world_type_offset = 0;
        if (std::strcmp(klass_name, "MonsterData") == 0) {
            out->kind = kKindMonster;
        } else if (std::strcmp(klass_name, "PlayerData") == 0 ||
                   std::strcmp(klass_name, "HeroData") == 0 ||
                   std::strcmp(klass_name, "PetData") == 0) {
            out->kind = kKindPlayer;
        } else if (std::strcmp(klass_name, "NpcData") == 0) {
            // NPC-NESS COMES FROM THE CLASS, NOT FROM `worldType`.
            //
            // `NpcData : WorldEntityData` is a fact in the dump (dump.cs:289663), and it is the
            // fact that decides this bucket.  `EWorldObjectType` also has `Npc = 1` / `NpcKiBo = 2`
            // (dump.cs:195779), but those are the values a SPAWNER records for the object it
            // places -- and reading them was never verified for a live `NpcData`.  The class name
            // is the part that cannot be wrong, so it decides first, and `worldType` is left to
            // describe only WHAT KIND of world object something is.
            out->kind = kKindNpc;
            // Still read and recorded, because knowing whether a live NPC reports `worldType`
            // 0, 1 or 2 is the measurement that would let the offset rule take over later.
            if (data != 0 && WorldTypeFieldOffset(klass_name, &world_type_offset) &&
                ReadValue(data + world_type_offset, &out->world_type)) {
                out->world_type_read = 1;
            }
        } else if (data != 0 && WorldTypeFieldOffset(klass_name, &world_type_offset) &&
                   ReadValue(data + world_type_offset, &out->world_type)) {
            // A READ THAT SUCCEEDED IS STILL RECORDED, because `EWorldObjectType::None` (0) is a
            // real value and "read 0" must not look like "the read failed".
            out->world_type_read = 1;
            out->kind = kKindWorld;
        } else {
            // WORST CASE IS "WORLD OBJECT", NOT "UNCLASSIFIED".
            //
            // This branch is the one the user's live report landed on: "world resources are blue,
            // they got put in unclassified".  The mistake was treating an unreadable `worldType`
            // as "I do not know what this is", when the class-name branches above have ALREADY
            // eliminated every category that is not a world object.  An entity that is not
            // MonsterData, not PlayerData/HeroData/PetData, and not an NPC-shaped `worldType` is
            // a world object by elimination -- the offset read was only ever going to tell us
            // WHICH KIND of world object.
            //
            // `kKindUnclassified` is therefore reserved for a genuine unknown, and the only way
            // to reach it now is `data == 0`, which the walk already treats as a bad record.
            out->kind = kKindWorld;
        }
        out->kind_from_class = 1;
    }


    // ==========================================================================
    // THE POSITION, IN THREE TIERS -- AND THE FIRST ONE IS THE WHOLE POINT
    // ==========================================================================
    //
    // 1. THE DIRECT CALL.  `ReadDirectPosition` calls this build's compiled
    //    `Transform::get_position` body, verified byte-for-byte against the profile before the
    //    first call (see the note beside `PositionSlot`).  It allocates nothing and takes about
    //    as long as a function call, so the position is re-read on EVERY tick with no cache and
    //    no staleness.
    //
    // 2. THE CACHE.  Only reached before adoption or after a refusal.  A cached position is up
    //    to `kPositionRefreshTicks` ticks old, which is a real world-space offset for a moving
    //    entity -- acceptable as a fallback, not as the design.
    //
    // 3. THE REFLECTION CALL.  `il2cpp_runtime_invoke` on the same method.  Measured on the
    //    live game: 158791 us of tick against 123370 reads, i.e. **1.29 ms per call** (the
    //    "~2.5 ms" this comment used to claim was that same pair divided wrongly).  At 82
    //    entities per walk that is ~106 ms of a single game tick, which is why the divisor and
    //    the cache exist at all -- and why the direct call above is worth having: it removes
    //    that entire cost from every walk, so the walk no longer needs throttling to be
    //    affordable.
    const std::uint64_t now_tick = ComponentState().position_tick;
    Vector3Out position;
    if (ReadDirectPosition(transform, &position)) {
        // Nothing to do: the position is fresh, unboxed, and cost a function call.
    } else if (ReadCachedPosition(out->id, transform, now_tick, &position)) {
        ++ComponentState().walk_profile.position_cache_hits;
    } else {
        const auto position_started = std::chrono::steady_clock::now();
        const bool position_ok = InvokeGetPosition(api, binding, transform, &position);
        ComponentState().walk_profile.position_micros +=
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                 position_started)
                .count();
        ++ComponentState().walk_profile.position_reads;
        if (!position_ok) {
            ++ComponentState().walk_profile.position_failures;
            return false;
        }
        // A non-finite position would produce a non-finite screen coordinate and then a box
        // drawn nowhere or across the whole screen.  Rejected at the source, and NOT cached.
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z)) {
            return false;
        }
        // THE ADOPTION TEST'S SECOND CHANCE, on this entity's own transform.
        //
        // The first attempt is made in `ResolveCameraBinding`, on the camera's transform, because
        // that is where a reflection result for a `Transform` is already in hand for free.  This
        // one exists because that attempt needs the camera's managed transform to be reachable,
        // and on a tick where it is not, the camera binding still resolves -- so the camera route
        // would never try again and the walk would keep paying 1.29 ms per entity with the fast
        // path sitting `untried` forever.
        //
        // It is not a second implementation of the test: same function, same evidence, same order.
        // The `position` compared against is the value the reflection route just returned for the
        // transform being tested.
        if (ComponentState().position_call.state ==
            static_cast<int>(DirectPositionState::untried)) {
            TryAdoptDirectPositionCall(api, binding.get_position, transform, position);
        }
        StoreCachedPosition(out->id, transform, position, now_tick);
    }

    // The camp comparison refines only the monster/player question, and only where that question
    // is the right one to ask.  The id was assigned at the top of this function, before the
    // position cache could be consulted; the duplicate assignment that used to sit here (with a
    // second copy of the explanation) read as if identity arrived after the cache, which is
    // exactly the misreading a duplicated line invites.
    // THE CAMP COMPARISON REFINES ONLY THE MONSTER/PLAYER QUESTION, AND ONLY WHERE THAT QUESTION
    // IS THE RIGHT ONE TO ASK.
    //
    // Two separate reasons for the guard:
    //
    // 1. NPCs are NOT enemies.  `camp_monster` is the game's "hostile camp" value, and an NPC can
    //    legitimately share it -- a guard standing in a hostile camp is still an NPC.  The camp
    //    answers "whose side is this on", never "is this a person or a chest", so letting it
    //    overwrite `kKindNpc` or `kKindWorld` would recreate the collapsed buckets the user
    //    reported.  The class/worldType answer is the one that knows what a thing IS.
    // 2. An UNCLASSIFIED entity has no answer to lose, so the camp is strictly an improvement
    //    there, and that is the case this block was originally written for.
    if (!out->kind_from_class) out->kind = kKindUnclassified;
    if (binding.camps_known &&
        (out->kind == kKindUnclassified || out->kind == kKindMonster ||
         out->kind == kKindPlayer)) {
        std::int32_t camp = 0;
        if (ReadValue(data + binding.off_data_camp, &camp)) {
            if (camp == binding.camp_monster) { out->kind = kKindMonster; out->kind_from_class = 0; }
            else if (camp == binding.camp_player) { out->kind = kKindPlayer; out->kind_from_class = 0; }
        }
    }
    out->center[0] = position.x;
    // THE BOX IS LIFTED OFF THE GROUND BY ONE HALF-HEIGHT.  `position` is the pivot, which sits at
    // the object's base; `out->center` is the CENTRE OF THE BOX.  Everything downstream --
    // the eight projected corners, the published `bounds_center`, and the distance filter -- uses
    // this one value, so the box that is drawn and the box that is measured cannot disagree.
    // See `kEntityBaseOffset`.
    out->center[1] = position.y + kEntityBaseOffset;
    out->center[2] = position.z;

    // Project the eight AABB corners HERE, on the game thread, from the CAMERA'S OWN MATRICES.
    //
    // THE ARCHITECTURE IS COPIED FROM THE SHIPPED UE5 HOST, and copying it is the point.  That
    // host answers its `project` entry by invoking the GAME's own projection function rather
    // than reconstructing UE's view-projection matrix, and its ESP is the one this project is
    // modelled on.  Unity's counterpart to that function is
    // not `WorldToScreenPoint` -- which extrapolates rather than failing for a point the
    // camera cannot see, and is what produced a sample sitting at (-4702, 2912) in a 2560-wide
    // viewport -- but `Camera.worldToCameraMatrix` and `Camera.projectionMatrix`, which the
    // engine has ALREADY computed.  Those are read, never rebuilt.
    //
    // Four earlier attempts reconstructed a matrix from projected pixel samples and were wrong
    // four times, each while the matrix's own self-check reported success: a check that
    // projects through the matrix under test can only agree with itself.  The pixels below come
    // from the engine's matrices and are therefore outside that loop entirely.
    // THE PROJECTION ITSELF LIVES IN `ProjectRecordCorners`, and the comment that explains WHY the
    // engine's matrices rather than `WorldToScreenPoint` lives with it.  It is called from here and
    // from the per-tick re-projection path, so a box's pixels cannot depend on which of the two
    // last touched it.
    ProjectRecordCorners(camera_binding, out);

    // THE NAME, read once per entity and then never again.
    //
    // `LangString::get_value` is a localisation lookup, and the whole chain is three
    // `runtime_invoke` calls with two value-type boxings -- which is far too much to pay for
    // every entity on every game tick.  The cache is keyed on the entity id, so the cost is
    // paid once per entity for the life of the process.  That is what makes a labelled ESP
    // affordable; without it the labels are the single most expensive thing in the tick.
    out->label_size = 0;
    // THE BINDING IS NO LONGER A PRECONDITION.
    //
    // This read used to require `ComponentState().label_binding.resolved`, because the label was the localised
    // name and the binding held the offset chain for it.  The label is now derived from the
    // entity's own data class (see `ReadEntityLabel`), which the walk already has, so gating it
    // on the name binding would disable a label that no longer depends on it -- and would leave
    // the label blank forever in any scene where that binding never resolves.
    if (ComponentState().label_reads_enabled != 0) {
        // THE PROBE RUNS HERE, OUTSIDE THE CACHE BRANCH, so it sees every class every tick.
        // Placing it inside the `else` (the cache-miss path) is what stalls the probe at
        // three of four classes -- see `ProbeEntityNameSource`.
        ProbeEntityNameSource(api, data, out->data_class);
        const LabelCacheEntry* const cached = FindCachedLabel(out->id);
        if (cached != nullptr) {
            ++ComponentState().walk_profile.label_cache_hits;
            std::memcpy(out->label, cached->text, cached->size);
            out->label[cached->size] = 0;
            out->label_size = cached->size;
        } else {
            ++ComponentState().walk_profile.label_reads;
            const auto label_started = std::chrono::steady_clock::now();
            const std::uint32_t size =
                ReadEntityLabel(api, data, out->label, sizeof(out->label), out->data_class,
                                out->id);
            ComponentState().walk_profile.label_micros += std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - label_started)
                                              .count();
            out->label_size = size;
            // A NON-ANSWER IS NOT CACHED WHILE THE TABLES ARE STILL COLD.
            //
            // Caching an empty label unconditionally is what made a transient miss permanent: an
            // entity read before the game had parsed its config table got the asset-path fallback,
            // and the fallback was then cached for the rest of the session.  A gatherable with a
            // Chinese name in the table showed an English path and never recovered.
            //
            // Before any candidate table is known to hold rows, an empty result carries no
            // information about the item, so it is not stored and the next tick tries again.  Once
            // a table is up, an empty result IS information -- the game has no name for this
            // object -- and it is cached, so scenery is not re-probed forever.
            if (size != 0 || WorldTablesLoaded()) {
                StoreCachedLabel(out->id, out->label, size);
            }
        }
    }
    return true;
}

// Resolve the whole live-instance binding from the runtime's own tables.
//
// Returns true and fills `binding` on success; on failure returns false and writes the FIRST
// step that failed to `reason`.  Naming the failing step is the point: this chain has several
// links and "it did not resolve" would be indistinguishable from "the battle has not started
// yet" -- two problems with completely different answers.
//
// THE PREVIOUS CHAIN WAS WRONG, AND THE DIAGNOSTIC IS WHAT SAID SO.
//
// It walked `BattleConfigManager::m_data` and treated the result as a `BattleInfoMono`.  The
// dump says `m_data` is a `BattleConfigData` -- a CONFIG type, not the live state -- so the
// final hop read `BattleFieldInfo::enemyInfos` out of an object that is not a
// `BattleFieldInfo`, and the run reported, honestly and uselessly:
//
//     "BattleFieldInfo::enemyInfos is null"   -- while the player was in a battle
//
// A wrong-but-readable chain is exactly the failure mode this file keeps warning about, and
// it was live in the code.  The correct live-entity source is `EntityManager`
// (dump:330992), the game's own world-entity registry.  `BattleEnemyInfo` / `BattleFieldInfo`
// are level-layout DATA -- their `slotInfos` / `itemInfos` are arrangement records -- rather
// than the runtime units.
bool ResolveLiveBinding(LiveBinding* binding, std::string* reason) {
    // Resolve the runtime HERE rather than gating on `Ready()` alone.
    //
    // This is the SAME defect that shipped once already in the dump service in this file,
    // and that the first real-machine run of THIS service reproduced:
    //
    //     {"entities":0,"generation":185,"classResolved":false,
    //      "reason":"IL2CPP runtime is not ready (GameAssembly.dll not resolved)"}
    //
    // The retry loop can be alive while `classResolved` stays false -- because nothing
    // else in this tree calls `il2cpp::Initialize()`, so `Ready()` answers false
    // FOREVER and a `Ready()`-only gate can never open.
    //
    // `Initialize()` is the retryable primitive, not a one-shot: mutex-guarded, it clears
    // half-parsed state before each attempt and only latches success.  Calling it from
    // the host's GAME-domain tick is safe and cheap: a failed attempt costs a module lookup,
    // and the success path needs no `ThreadScope` because the caller is already the game's
    // own attached thread.
    if (!il2cpp::Ready() && !il2cpp::Initialize()) {
        // Report the runtime's OWN words rather than a sentence written here.  If it says
        // "GameAssembly.dll is not loaded yet" the operator knows to wait; if it says
        // something else, that is a different problem and paraphrasing would hide it.
        *reason = "IL2CPP runtime is not ready: " + il2cpp::LastError();
        return false;
    }
    const il2cpp::Api& api = il2cpp::Functions();
    if (api.il2cpp_class_from_name == nullptr || api.il2cpp_class_get_parent == nullptr ||
        api.il2cpp_class_get_field_from_name == nullptr ||
        api.il2cpp_field_get_offset == nullptr ||
        api.il2cpp_class_get_method_from_name == nullptr) {
        *reason = "IL2CPP API table is missing the class/field/method resolution entries";
        return false;
    }

    auto find_class = [&api](const char* namespaze, const char* name) {
        return api.il2cpp_class_from_name(il2cpp::FindImage(kEntityImage), namespaze, name);
    };

    il2cpp::Il2CppClass* const manager_class = find_class(kManagerNamespace, kManagerClass);
    il2cpp::Il2CppClass* const handle_class = find_class(kEntityNamespace, kHandleClass);
    il2cpp::Il2CppClass* const entity_class = find_class(kEntityNamespace, kEntityClass);
    il2cpp::Il2CppClass* const data_class = find_class(kEntityNamespace, kDataClass);
    il2cpp::Il2CppClass* const relative_class = find_class(kEntityNamespace, kRelativeClass);
    if (manager_class == nullptr || handle_class == nullptr || entity_class == nullptr ||
        data_class == nullptr || relative_class == nullptr) {
        *reason = std::string("one of ") + kManagerClass + " / " + kHandleClass + " / " +
                  kEntityClass + " / " + kDataClass + " / " + kRelativeClass +
                  " is not present in TableData";
        return false;
    }

    // Step 1: the static slot that holds the manager instance.
    il2cpp::FieldInfo* const instance_field =
        FindStaticFieldInHierarchy(api, manager_class, kSingletonField);
    if (instance_field == nullptr) {
        *reason = std::string("no static `") + kSingletonField + "` on " + kManagerClass +
                  " or any of its bases";
        return false;
    }
    // The slot is cached, the VALUE is not: `instance_field` is a FieldInfo the RUNTIME handed
    // us, so it stays valid for as long as the class is loaded -- and a class is never
    // unloaded in IL2CPP, which is what makes caching it safe where caching a computed
    // address would not be.  Re-reading the value every frame means a manager torn down and
    // recreated (a scene transition, which is exactly when a battle starts) is picked up
    // automatically, whereas caching the instance pointer would freeze the first one seen.
    std::uintptr_t instance_now = 0;
    if (!ReadStaticField(api, instance_field, &instance_now)) {
        *reason = "the manager's static instance field is not readable";
        return false;
    }
    binding->instance_field = reinterpret_cast<std::uintptr_t>(instance_field);

    // Step 2: offsets.  Runtime first, dump value as the documented fallback, and a failure
    // to confirm is REPORTED rather than fatal -- a game patch then shows up as a named diff
    // instead of as boxes in the wrong place.
    bool unconfirmed = false;
    const auto offset = [&api, &unconfirmed](il2cpp::Il2CppClass* klass, const char* name,
                                             std::size_t fallback) {
        bool exact = false;
        const std::size_t value = FieldOffsetOr(api, klass, name, fallback, &exact);
        if (!exact) unconfirmed = true;
        return value;
    };
    binding->off_entity_list = offset(manager_class, "m_entityList", 0x88);
    binding->off_handle_entity = offset(handle_class, "entity", 0x10);
    binding->off_entity_data = offset(entity_class, "data", 0x18);
    binding->off_data_transform = offset(data_class, "transform", 0xA0);
    binding->off_transform = offset(relative_class, "m_transform", 0x10);
    binding->off_data_camp = offset(data_class, "campType", 0x134);
    binding->off_data_type = offset(data_class, "entityType", 0x140);

    // Step 3: the method used to read a position.  `Transform::get_position` has no
    // parameters, so arity 0 identifies it.
    il2cpp::Il2CppClass* const transform_class =
        api.il2cpp_class_from_name(il2cpp::FindImage("UnityEngine.CoreModule.dll"),
                                   "UnityEngine", "Transform");
    if (transform_class == nullptr) {
        *reason = "UnityEngine.Transform was not found in CoreModule";
        return false;
    }
    const il2cpp::MethodInfo* const get_position =
        api.il2cpp_class_get_method_from_name(transform_class, "get_position", 0);
    if (get_position == nullptr) {
        *reason = "UnityEngine.Transform::get_position was not found";
        return false;
    }
    // The setter is resolved the same way and its absence is NOT fatal: reading positions is
    // what the entity walk is for, and a build in which only the getter resolves should still
    // draw boxes.  It only means the player service has no write path, which it reports.
    const il2cpp::MethodInfo* const set_position =
        api.il2cpp_class_get_method_from_name(transform_class, "set_position", 1);
    il2cpp::Il2CppClass* const vector3 =
        api.il2cpp_class_from_name(il2cpp::FindImage("UnityEngine.CoreModule.dll"),
                                   "UnityEngine", "Vector3");
    if (vector3 == nullptr) {
        *reason = "UnityEngine.Vector3 was not found in CoreModule";
        return false;
    }
    binding->get_position = reinterpret_cast<std::uintptr_t>(get_position);
    binding->set_position = reinterpret_cast<std::uintptr_t>(set_position);
    binding->vector3_class = reinterpret_cast<std::uintptr_t>(vector3);

    // Step 4: the camp values, so the caller never has to guess which camp is the enemy.
    binding->camps_known = false;
    // WHY THE CAMP READ FAILED, recorded as it happens.
    //
    // `campsKnown: false` with `kinds: unclassified=77 enemy=0 player=0` says only that
    // classification is not happening.  It cannot distinguish the three ways this fails -- the
    // class was not found, the field was not found, or the field read returned nothing -- and
    // those need different fixes.  A round was already spent guessing at this from the dump; the
    // answer is one string away.
    std::string camp_stage = "camp: ";
    if (il2cpp::Il2CppClass* const formula = find_class(kFormulaNamespace, kFormulaClass)) {
        camp_stage += "class=ok";
        const auto read_static_int = [&api, formula](const char* name,
                                                     std::int32_t* value) -> bool {
            return ReadStaticField(
                api, FindStaticFieldInHierarchy(api, formula, name), value);
        };
        std::int32_t monster = 0;
        std::int32_t player = 0;
        // Each half is reported separately, so "monster failed" cannot hide behind "player
        // worked" -- and so a read that SUCCEEDS but yields 0 is visible as such.
        const bool have_monster = read_static_int(kCampMonsterField, &monster);
        const bool have_player = read_static_int(kCampPlayerField, &player);
        char detail[96]{};
        std::snprintf(detail, sizeof(detail), " monster=%s(%d) player=%s(%d)",
                      have_monster ? "ok" : "miss", static_cast<int>(monster),
                      have_player ? "ok" : "miss", static_cast<int>(player));
        camp_stage += detail;
        // Both or neither: one known camp and one unknown would classify monsters and
        // silently leave players unclassified, which is a wrong answer that looks partial.
        if (have_monster && have_player) {
            binding->camp_monster = monster;
            binding->camp_player = player;
            binding->camps_known = true;
        }
    } else {
        camp_stage += "class MISSING";
    }
    binding->camp_stage = camp_stage;

    // Resolved, and the instance may still be null -- that is the normal state outside a
    // world scene, so it is reported by the WALK rather than treated as an unresolved binding.
    binding->off_list_items = kListItems;
    binding->off_list_size = kListSize;
    binding->off_array_data = kArrayData;
    binding->resolved = true;
    if (unconfirmed) {
        *reason = "the runtime could not confirm every instance offset; dump values are in use";
    }
    return true;
}

// Follow the chain from the singleton slot to the live entity list, then read it.
//
// Returns false with a step-named reason.  A null instance is NOT an error: the manager
// simply does not exist yet (main menu, loading screen), and saying so is more useful than
// reporting a failure the operator would try to fix.
bool WalkEntities(const LiveBinding& binding, const CameraBinding* camera_binding,
                  std::vector<EntityRecord>* entities, std::string* reason) {
    ++ComponentState().position_tick; // One age step per walk, not per entity in that walk.
    const il2cpp::Api& api = il2cpp::Functions();
    std::uintptr_t manager = 0;
    if (!ReadStaticField(api, reinterpret_cast<il2cpp::FieldInfo*>(binding.instance_field),
                         &manager)) {
        *reason = "the manager's static instance field became unreadable";
        return false;
    }
    if (manager == 0) {
        *reason = std::string(kManagerClass) +
                  " has not been instantiated yet (not in a world scene)";
        return false;
    }
    // THE PLAUSIBILITY GATE.  Everything above this point is a chain of reads that all
    // "succeed" when the pointers are wrong-but-readable, which is exactly how an ESP ends up
    // drawing boxes at coordinates that came from an unrelated object.  `m_entityList` must
    // look like a `List<T>`: a backing array and a byte count that agrees with it.
    std::uintptr_t list = 0;
    if (!ReadValue(manager + binding.off_entity_list, &list) || list == 0) {
        *reason = std::string(kManagerClass) + "::m_entityList is null";
        return false;
    }
    std::int32_t count = 0;
    if (!ReadValue(list + binding.off_list_size, &count) || count < 0) {
        *reason = "m_entityList has no readable count";
        return false;
    }
    if (count == 0) {
        *reason = "m_entityList is empty (the manager is up but tracks no entity)";
        return false;
    }
    if (count > static_cast<std::int32_t>(kMaximumEntities)) {
        *reason = "m_entityList reports " + std::to_string(count) +
                  " entries, more than this walk will trust";
        return false;
    }
    std::uintptr_t items = 0;
    if (!ReadValue(list + binding.off_list_items, &items) || items == 0) {
        *reason = "m_entityList has no backing array";
        return false;
    }
    // The array's own length must cover the list's count.  This is the check that turns
    // "readable garbage" into "a refusal": a real List<T> can never have a count larger than
    // its array.
    std::int32_t capacity = 0;
    if (!ReadValue(items + kArrayLength, &capacity) || capacity < count ||
        capacity > static_cast<std::int32_t>(kMaximumEntities)) {
        *reason = "m_entityList count (" + std::to_string(count) +
                  ") does not match its backing array's length (" + std::to_string(capacity) +
                  "), so the binding is wrong and no entity is reported";
        return false;
    }
    const std::size_t bounded = static_cast<std::size_t>(count);
    entities->reserve(bounded);
    std::size_t unreadable = 0;
    for (std::size_t index = 0; index < bounded; ++index) {
        std::uintptr_t handle = 0;
        if (!ReadValue(items + binding.off_array_data + index * sizeof(void*), &handle) ||
            handle == 0) {
            ++unreadable;
            continue;
        }
        EntityRecord record;
        std::uintptr_t entity_data = 0;
        if (ReadEntity(api, binding, handle, camera_binding, &record, &entity_data)) {
            entities->push_back(record);
            // THE ONE-ENTITY-PER-CLASS NAME PROBE.
            //
            // It probes each class that is KNOWN TO CARRY A CONFIG FIELD, ONCE, and records the
            // result per class.  The first version probed whichever nameable entity the walk met
            // first and reported `PlayerData config@0x338 -> config=NULL` -- useless, because
            // PlayerData has no config field at all: the runtime-resolved offset 0x338 lands in
            // its `int playerIndex`/`int <guid>` run, so null is the CORRECT answer there and
            // says nothing about whether the chain works.
            //
            // The classes that matter are the ones the dump shows carrying a config field:
            // MonsterData (TDEnemy @0x484), HeroData (TDHero @0x3C0), PetData (TDPet @0x458).
            // Each is probed once, and each result is published -- so one run distinguishes
            // "the chain is broken" from "this particular entity has no config".
            // RETIRED, NOT DELETED: its answer is already known and its output now conflicts.
            //
            // `ProbeEntityName` walks "read `<config>` -> treat it as an object pointer", which
            // is wrong at the FIRST step: those fields are declared
            // `TDEnemy`/`TDHero`/`TDPet`, all STRUCTS, so they hold an inlined value and never a
            // pointer.  It has been answering `NOT-AN-OBJECT` for that reason, correctly but
            // uninformatively.
            //
            // It is switched off because it wrote into the SAME `ComponentState().name_probe` string the invoke
            // gate publishes, so the two probes overwrote each other and `nameProbe` came out as
            // a run-on sentence with the gate's answer first and three stale verdicts after it.
            // The gate's answer is the one that decides the feature, so it gets the field alone.
            static_cast<void>(NameProbeShouldRun);
            static_cast<void>(ProbeEntityName);
        } else {
            ++unreadable;
        }
    }
    if (entities->empty()) {
        *reason = "every one of the " + std::to_string(bounded) +
                  " entity handles was unreadable (no transform or no position)";
        return false;
    }
    if (unreadable != 0) {
        // Reported, not hidden: a partially-readable list is what a stale backing array
        // looks like, and a box count that quietly disagrees with the game is worse than a
        // note saying why.
        *reason = std::to_string(unreadable) + " of " + std::to_string(bounded) +
                  " entity handles had no readable position";
    }
    return true;
}

// Solve a 4x4 linear system in place by Gaussian elimination with partial pivoting.
//
// Partial pivoting is not decoration here: the fourth sample point is the camera itself, and
// if the camera sits on the plane spanned by the other three (which happens whenever the
// camera is at the world origin, or is axis-aligned at integer coordinates) the naive
// elimination divides by a near-zero pivot and returns infinities that then propagate into
// every box on screen as `nan`.
bool Solve4x4(double matrix[4][5], double solution[4]) {
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row) {
            if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) pivot = row;
        }
        if (std::abs(matrix[pivot][column]) < 1e-9) return false;
        if (pivot != column) {
            for (int k = 0; k < 5; ++k) std::swap(matrix[column][k], matrix[pivot][k]);
        }
        const double inverse = 1.0 / matrix[column][column];
        for (int k = column; k < 5; ++k) matrix[column][k] *= inverse;
        for (int row = 0; row < 4; ++row) {
            if (row == column) continue;
            const double factor = matrix[row][column];
            if (factor == 0.0) continue;
            for (int k = column; k < 5; ++k) matrix[row][k] -= factor * matrix[column][k];
        }
    }
    for (int row = 0; row < 4; ++row) solution[row] = matrix[row][4];
    return true;
}


// Read a `Matrix4x4` property off the camera and copy its 16 floats out, TRANSPOSED into the
// row-major layout the rest of this file uses.
//
// THE TRANSPOSE IS THE WHOLE POINT, AND GETTING IT WRONG COST A SESSION.
//
// Unity's `Matrix4x4` is COLUMN-major in memory: the managed fields are declared `m00, m10,
// m20, m30, m01, m11, ...` (dump.cs line 2335192), so consecutive floats are a COLUMN, not a
// row.  Copying them straight into a `float[16]` read as `row * 4 + column` therefore
// TRANSPOSES the matrix, and a transposed view-projection does not fail loudly: it produces
// numbers that look like coordinates.  It shipped boxes projected to about (-30000, -5012) for
// an entity ten metres in front of the camera.
//
// What identified it was arithmetic rather than inspection.  A world-to-camera matrix maps the
// camera's own position to the origin, so `M . (pos, 1) == 0`; with the matrices read straight,
// that product was (-362.3, 149.7, -327.4) -- and `rotation . pos` was exactly its negation,
// which is what a transpose looks like.  Read transposed, the product is (0, 0, 0, 1) to within
// 2e-4.  That check is repeated at runtime below, so this cannot silently regress.
bool ReadCameraMatrixProperty(const il2cpp::Api& api, const CameraBinding& binding,
                              std::uintptr_t camera, std::uintptr_t method, float out[16]) {
    if (method == 0 || api.il2cpp_runtime_invoke == nullptr ||
        api.il2cpp_object_unbox == nullptr) {
        return false;
    }
    static_cast<void>(binding);
    il2cpp::Il2CppObject* exception = nullptr;
    il2cpp::Il2CppObject* const returned = api.il2cpp_runtime_invoke(
        reinterpret_cast<const il2cpp::MethodInfo*>(method),
        reinterpret_cast<il2cpp::Il2CppObject*>(camera), nullptr, &exception);
    if (exception != nullptr || returned == nullptr) return false;
    const void* const payload = api.il2cpp_object_unbox(returned);
    if (payload == nullptr) return false;
    const float* const values = static_cast<const float*>(payload);
    for (int index = 0; index < 16; ++index) {
        if (!std::isfinite(values[index])) return false;
    }
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            out[row * 4 + column] = values[column * 4 + row];
        }
    }
    return true;
}

// `Matrix4x4 * Matrix4x4`, done here rather than by invoking `op_Multiply` so the projection
// path has one fewer managed call and no dependency on the operator being present.
//
// UNITY'S MULTIPLICATION ORDER IS `lhs * rhs` APPLIED AS `lhs` AFTER `rhs` FOR COLUMN VECTORS,
// and `Camera.projectionMatrix * Camera.worldToCameraMatrix` is the composition that turns a
// world point into clip space.  Getting this the wrong way round transposes the result, which
// produces boxes that are individually plausible and collectively nonsense.

// Resolve the game's active camera and the method used to interrogate it.
//
// GAME THREAD ONLY, for the reason spelled out on `HostEntityStats`: a metadata walk from a
// thread the runtime does not own is what took the game down when this was first attempted
// from the diagnostic pipe's thread.
//
// Resolution happens ONCE and is cached; the MATRIX is re-solved every tick (see
// `SolveCameraMatrix`) because the field of view moves.
bool ResolveCamera(CameraBinding* binding, std::string* reason) {
    if (!il2cpp::Ready() && !il2cpp::Initialize()) {
        *reason = "IL2CPP runtime is not ready: " + il2cpp::LastError();
        return false;
    }
    const il2cpp::Api& api = il2cpp::Functions();
    if (api.il2cpp_class_from_name == nullptr ||
        api.il2cpp_class_get_field_from_name == nullptr ||
        api.il2cpp_field_static_get_value == nullptr ||
        api.il2cpp_class_get_method_from_name == nullptr) {
        *reason = "the IL2CPP API table is missing the static-field or method entries";
        return false;
    }
    il2cpp::Il2CppImage* const image = il2cpp::FindImage(kEntityImage);
    if (image == nullptr) {
        *reason = std::string("image ") + kEntityImage + " is not loaded";
        return false;
    }
    il2cpp::Il2CppClass* const manager =
        api.il2cpp_class_from_name(image, kCameraManagerNamespace, kCameraManagerClass);
    if (manager == nullptr) {
        *reason = std::string("class ") + kCameraManagerNamespace + "." + kCameraManagerClass +
                  " was not found";
        return false;
    }
    il2cpp::FieldInfo* const field = FindStaticFieldInHierarchy(api, manager, kMainCameraField);
    if (field == nullptr) {
        *reason = std::string("no static `") + kMainCameraField + "` on CameraManager";
        return false;
    }
    // `UnityEngine.Camera` lives in CoreModule, not Assembly-CSharp.  `WorldToScreenPoint`
    // takes exactly one parameter, so arity 1 picks it out of its overloads unambiguously.
    il2cpp::Il2CppImage* const core = il2cpp::FindImage("UnityEngine.CoreModule.dll");
    if (core == nullptr) {
        *reason = "UnityEngine.CoreModule.dll is not loaded";
        return false;
    }
    il2cpp::Il2CppClass* const camera_class =
        api.il2cpp_class_from_name(core, "UnityEngine", "Camera");
    if (camera_class == nullptr) {
        *reason = "UnityEngine.Camera was not found in CoreModule";
        return false;
    }
    const il2cpp::MethodInfo* const method =
        api.il2cpp_class_get_method_from_name(camera_class, "WorldToScreenPoint", 1);
    if (method == nullptr) {
        *reason = "UnityEngine.Camera::WorldToScreenPoint(Vector3) was not found";
        return false;
    }
    il2cpp::Il2CppClass* const vector3 =
        api.il2cpp_class_from_name(core, "UnityEngine", "Vector3");
    if (vector3 == nullptr) {
        *reason = "UnityEngine.Vector3 was not found in CoreModule";
        return false;
    }
    binding->main_camera_field = reinterpret_cast<std::uintptr_t>(field);
    binding->method = reinterpret_cast<std::uintptr_t>(method);
    binding->vector3_class = reinterpret_cast<std::uintptr_t>(vector3);
    binding->resolved = true;
    return true;
}

// The camera object, or 0 when the game has not published one.
//
// Zero is NOT an error: outside gameplay the manager exists but no camera has been published
// yet.  Kept as a state rather than a failure for the same reason the entity walk separates
// "not in a battle" from "the binding is wrong".
std::uintptr_t ReadMainCamera(const CameraBinding& binding, std::string* reason) {
    const il2cpp::Api& api = il2cpp::Functions();
    std::uintptr_t camera = 0;
    if (!ReadStaticField(api, reinterpret_cast<il2cpp::FieldInfo*>(binding.main_camera_field),
                         &camera)) {
        *reason = "the main-camera static field is not readable";
        return 0;
    }
    if (camera == 0) {
        *reason = "CameraManager reports no main camera (no gameplay camera is active)";
        return 0;
    }
    return camera;
}

// Read the game's own view-projection matrix, and the viewport it is expressed in.
//
// THIS IS THE WHOLE CAMERA SOLVE, AND IT IS NOT A SOLVE.
//
// The dump (`UnityEngine.Camera`, dump.cs line 2318838) lists `worldToCameraMatrix` and
// `projectionMatrix` as properties, and `Matrix4x4` (dump.cs line 2335192) lists `op_Multiply`.
// Both factors are therefore things the ENGINE HAS ALREADY COMPUTED -- there is nothing to
// reconstruct, and four hand-derived reconstructions were wrong four times before this.
//
// The mistake they share is worth naming, because it looked like diligence: each one treated
// the camera as something to be inferred from the pixels it produced.  Inference is only
// necessary when the answer is not available.  Here it is available, exactly, in two fields.
//
// `WorldToScreenPoint` is NOT used for this.  It exists (dump.cs, `Camera`), and it was the
// measurement source for every failed attempt, but it does not reject a point the camera cannot
// see: it extrapolates.  Sampling it at the camera's own position returned (-4702, 2912) in a
// 2560x1362 viewport, and the "responses" derived from such numbers cannot be told apart from
// real ones until the algebra produces an impossible tangent.
bool ReadViewProjection(const il2cpp::Api& api, CameraBinding* binding, std::uintptr_t camera,
                        std::string* reason) {
    if (api.il2cpp_class_get_method_from_name == nullptr ||
        api.il2cpp_runtime_invoke == nullptr || api.il2cpp_object_unbox == nullptr) {
        *reason = "the IL2CPP API table is missing the method or invoke entry";
        return false;
    }
    il2cpp::Il2CppImage* const core = il2cpp::FindImage("UnityEngine.CoreModule.dll");
    if (core == nullptr) {
        *reason = "UnityEngine.CoreModule.dll is not loaded";
        return false;
    }
    il2cpp::Il2CppClass* const camera_class =
        api.il2cpp_class_from_name(core, "UnityEngine", "Camera");
    if (camera_class == nullptr) {
        *reason = "UnityEngine.Camera was not found in CoreModule";
        return false;
    }
    il2cpp::Il2CppClass* const transform_class =
        api.il2cpp_class_from_name(core, "UnityEngine", "Transform");
    // The viewport, from the camera.  A wrong pair scales every box by a constant, which reads
    // as "slightly off" rather than as a bug -- so it is read, not assumed.
    if (const il2cpp::MethodInfo* const get_pixel_height =
            api.il2cpp_class_get_method_from_name(camera_class, "get_pixelHeight", 0)) {
        il2cpp::Il2CppObject* exception = nullptr;
        il2cpp::Il2CppObject* const returned = api.il2cpp_runtime_invoke(
            get_pixel_height, reinterpret_cast<il2cpp::Il2CppObject*>(camera), nullptr,
            &exception);
        if (exception == nullptr && returned != nullptr) {
            const void* const payload = api.il2cpp_object_unbox(returned);
            if (payload != nullptr) {
                binding->pixel_height = *static_cast<const std::int32_t*>(payload);
            }
        }
    }
    if (const il2cpp::MethodInfo* const get_pixel_width =
            api.il2cpp_class_get_method_from_name(camera_class, "get_pixelWidth", 0)) {
        il2cpp::Il2CppObject* exception = nullptr;
        il2cpp::Il2CppObject* const returned = api.il2cpp_runtime_invoke(
            get_pixel_width, reinterpret_cast<il2cpp::Il2CppObject*>(camera), nullptr,
            &exception);
        if (exception == nullptr && returned != nullptr) {
            const void* const payload = api.il2cpp_object_unbox(returned);
            if (payload != nullptr) {
                binding->pixel_width = *static_cast<const std::int32_t*>(payload);
            }
        }
    }
    if (!(binding->pixel_width > 1.0) || !(binding->pixel_height > 1.0)) {
        *reason = "Camera::get_pixelWidth/get_pixelHeight did not report a usable viewport (" +
                  std::to_string(binding->pixel_width) + "x" +
                  std::to_string(binding->pixel_height) + ")";
        return false;
    }
    // The camera's world position, for the diagnostic string only.  It is what makes a wrong
    // camera identifiable by eye: the point the boxes are drawn around is either near this or
    // it is not.
    if (transform_class != nullptr && api.il2cpp_class_get_method_from_name != nullptr) {
        const il2cpp::MethodInfo* const get_position =
            api.il2cpp_class_get_method_from_name(transform_class, "get_position", 0);
        il2cpp::Il2CppClass* const component_class =
            api.il2cpp_class_from_name(core, "UnityEngine", "Component");
        const il2cpp::MethodInfo* const get_transform =
            component_class == nullptr
                ? nullptr
                : api.il2cpp_class_get_method_from_name(component_class, "get_transform", 0);
        if (get_position != nullptr && get_transform != nullptr) {
            il2cpp::Il2CppObject* exception = nullptr;
            il2cpp::Il2CppObject* const transform = api.il2cpp_runtime_invoke(
                get_transform, reinterpret_cast<il2cpp::Il2CppObject*>(camera), nullptr,
                &exception);
            if (exception == nullptr && transform != nullptr) {
                exception = nullptr;
                il2cpp::Il2CppObject* const position = api.il2cpp_runtime_invoke(
                    get_position, transform, nullptr, &exception);
                if (exception == nullptr && position != nullptr) {
                    const void* const payload = api.il2cpp_object_unbox(position);
                    if (payload != nullptr) {
                        const float* const values = static_cast<const float*>(payload);
                        for (int axis = 0; axis < 3; ++axis) {
                            binding->camera_pos[axis] = values[axis];
                        }
                        // ==========================================================================
                        // AND THE DIRECT POSITION CALL IS ADOPTED HERE, ON THIS OBSERVATION
                        // ==========================================================================
                        //
                        // This is the one place in the file that has, at the same instant, a live
                        // `UnityEngine.Transform`, the `MethodInfo*` for `get_position`, and the
                        // value the runtime's own reflection path returned for it.  That is
                        // exactly the evidence the adoption test needs, so it runs here rather
                        // than in the walk -- which would have to make a second reflection call
                        // per entity to get a value to compare against, i.e. pay the cost it is
                        // trying to remove.
                        //
                        // The transform used is the CAMERA's, not an entity's, and that is not a
                        // weakness: `get_position` is one method with one body, the ABI is a
                        // property of that body, and the entity walk calls the same `MethodInfo`.
                        // If the camera's transform cannot be used, nothing is adopted and the
                        // walk keeps using the reflection route -- the state stays `untried`.
                        Vector3Out expected;
                        expected.x = values[0];
                        expected.y = values[1];
                        expected.z = values[2];
                        TryAdoptDirectPositionCall(
                            api, reinterpret_cast<std::uintptr_t>(get_position),
                            reinterpret_cast<std::uintptr_t>(transform), expected);
                    }
                }
            }
        }
    }

    // The two matrix properties.  Resolved once and cached: a `MethodInfo*` stays valid for the
    // life of the class, and re-resolving per tick is a table walk on the game thread.
    if (binding->world_to_camera_method == 0) {
        const il2cpp::MethodInfo* const method =
            api.il2cpp_class_get_method_from_name(camera_class, "get_worldToCameraMatrix", 0);
        if (method == nullptr) {
            *reason = "UnityEngine.Camera::get_worldToCameraMatrix was not found";
            return false;
        }
        binding->world_to_camera_method = reinterpret_cast<std::uintptr_t>(method);
    }
    if (binding->projection_method == 0) {
        const il2cpp::MethodInfo* const method =
            api.il2cpp_class_get_method_from_name(camera_class, "get_projectionMatrix", 0);
        if (method == nullptr) {
            *reason = "UnityEngine.Camera::get_projectionMatrix was not found";
            return false;
        }
        binding->projection_method = reinterpret_cast<std::uintptr_t>(method);
    }
    if (!ReadCameraMatrixProperty(api, *binding, camera, binding->world_to_camera_method,
                                  binding->world_to_camera)) {
        *reason = "Camera::get_worldToCameraMatrix did not return a finite matrix";
        binding->view_projection_valid = false;
        return false;
    }
    if (!ReadCameraMatrixProperty(api, *binding, camera, binding->projection_method,
                                  binding->projection)) {
        *reason = "Camera::get_projectionMatrix did not return a finite matrix";
        binding->view_projection_valid = false;
        return false;
    }
    // THE TRANSPOSE CHECK, AND WHY IT IS NOT A CIRCULAR SELF-CHECK.
    //
    // The four failed derivations were each blessed by a check that projected a point THROUGH
    // the matrix under test, which can only ever agree with itself.  This one is different in
    // the one way that matters: it takes the camera's world position from an INDEPENDENT source
    // (`Transform.get_position`, read above) and requires `worldToCamera . (pos, 1) == 0`,
    // which is the defining property of a world-to-camera transform.
    //
    // It catches exactly one thing, and that thing cost a whole session: Unity stores
    // `Matrix4x4` COLUMN-major, so reading the 16 floats as rows silently TRANSPOSES the
    // matrix.  A transposed view-projection does not produce garbage -- it produces numbers that
    // look like coordinates, and it placed an entity ten metres away at (-30000, -5012).  With
    // the matrices read straight, this product was (-362.3, 149.7, -327.4) and `rotation . pos`
    // was exactly its negation; read transposed it is zero to 2e-4.
    //
    // A tolerance rather than an exact zero: the camera position is a float32 read of a value
    // the matrix was built from, so agreement to a fraction of a unit is as exact as the inputs
    // permit.  The failure it detects is off by hundreds.
    if (binding->camera_pos[0] != 0.0 || binding->camera_pos[1] != 0.0 ||
        binding->camera_pos[2] != 0.0) {
        double mapped[4]{};
        for (int row = 0; row < 4; ++row) {
            mapped[row] = binding->world_to_camera[row * 4 + 0] * binding->camera_pos[0] +
                          binding->world_to_camera[row * 4 + 1] * binding->camera_pos[1] +
                          binding->world_to_camera[row * 4 + 2] * binding->camera_pos[2] +
                          binding->world_to_camera[row * 4 + 3];
        }
        // The fourth component must come out at 1 for an affine transform; dividing it out
        // keeps the check correct even if it does not.
        const double w = mapped[3] != 0.0 ? mapped[3] : 1.0;
        const double error = std::fabs(mapped[0] / w) + std::fabs(mapped[1] / w) +
                             std::fabs(mapped[2] / w);
        if (!(error < 1.0)) {
            char buffer[192]{};
            std::snprintf(buffer, sizeof(buffer),
                          "worldToCameraMatrix does not map the camera position "
                          "(%.1f,%.1f,%.1f) to the origin: it gives (%.1f,%.1f,%.1f) -- the "
                          "matrix is probably transposed",
                          binding->camera_pos[0], binding->camera_pos[1], binding->camera_pos[2],
                          mapped[0] / w, mapped[1] / w, mapped[2] / w);
            *reason = buffer;
            binding->view_projection_valid = false;
            return false;
        }
    }
    // `projection * worldToCamera` is the composition that takes a WORLD point to clip space.
    // The order matters: the reverse transposes the result, which yields boxes that are each
    // individually plausible and collectively nonsense -- the failure mode this project has
    // spent the longest failing to see.
    MultiplyMatrix4x4(binding->projection, binding->world_to_camera, binding->view_projection);

    for (int index = 0; index < 16; ++index) {
        if (!std::isfinite(binding->view_projection[index])) {
            *reason = "the composed view-projection matrix is not finite";
            binding->view_projection_valid = false;
            return false;
        }
    }
    binding->view_projection_valid = true;
    binding->valid = true;
    return true;
}

// The camera solve's raw inputs, for the status command.
//
// WHAT THIS STRING IS FOR, AND WHAT IT COST TO LEARN.
//
// Every published verdict in this file is a summary, and a summary cannot distinguish its own
// causes.  "The boxes are in the wrong place" was answered by editing camera algebra four
// times, because the algebra was the only thing in view; when the raw numbers were finally
// published they named the cause on the first read:
//
//     cameraPos=(-148.896,122.223,-477.088) viewport=2560x1362
//     tanH=0.378397 tanV=0.134604        <- ratio 2.81 against an aspect ratio of 1.88
//     s0=(-4702.76,2912.46)              <- outside a 2560x1362 viewport
//
// `WorldToScreenPoint` was extrapolating, not measuring.  No amount of re-reading a derivation
// reveals that; only the samples do.
//
// So the two factors are printed rather than only their product: a wrong camera and a
// transposed multiply both end as "wrong boxes", and the factors are what separate them.
std::string FormatCameraRaw(const CameraBinding& binding) {
    auto number = [](double value) {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
        return std::string(buffer);
    };
    std::string out = "viewport=" + number(binding.pixel_width) + "x" +
                      number(binding.pixel_height);
    out += " cameraPos=(" + number(binding.camera_pos[0]) + "," + number(binding.camera_pos[1]) +
           "," + number(binding.camera_pos[2]) + ")";
    out += " matrixValid=" + std::string(binding.view_projection_valid ? "1" : "0");
    auto matrix = [&number](const char* name, const float values[16]) {
        std::string text = std::string(" ") + name + "=[";
        for (int row = 0; row < 4; ++row) {
            if (row != 0) text += ";";
            for (int column = 0; column < 4; ++column) {
                if (column != 0) text += ",";
                text += number(values[row * 4 + column]);
            }
        }
        return text + "]";
    };
    out += matrix("worldToCamera", binding.world_to_camera);
    out += matrix("projection", binding.projection);
    out += matrix("viewProjection", binding.view_projection);
    return out;
}

// The camera half of the published state, written under `state.mutex` by BOTH paths.
//
// Why it is shared rather than left in the full walk: the cheap path below refreshes the camera on
// every tick, so a status document that only updated these fields on walk ticks would describe a
// camera up to `divisor - 1` ticks old while the boxes next to it were drawn from a fresh one -- a
// diagnostic that disagrees with the thing it diagnoses, which is the failure this file keeps
// recording.  `camera_reason` is taken by value because the caller is done with it here.
void PublishCameraStateLocked(ServiceState& state, const CameraBinding& camera, bool camera_ok,
                              std::uint64_t camera_object, std::uint64_t camera_native,
                              std::string camera_reason) {
    state.camera_anchor_resolved = camera.resolved;
    state.main_camera_present = camera_ok;
    state.main_camera_object = camera_object;
    state.main_camera_native = camera_native;
    // The matrix is "valid" exactly when the ENGINE handed over both factors.  There is no
    // self-check number any more, and that is a deliberate loss: the old one projected a point
    // through the matrix under test, so it agreed with itself by construction and reported
    // `check_ok: true` beside a 18462-pixel error.  What replaces it is not a check but a
    // SOURCE: `view_projection` is the product of two engine properties, so the question "is
    // this the game's projection?" is answered by where the bytes came from rather than by
    // testing the result with itself.
    state.camera_matrix_valid = camera.view_projection_valid;
    state.camera_check_ok = camera.view_projection_valid;
    state.camera_check_error_pixels = 0.0;
    state.camera_reason = camera_reason.empty() ? std::string("ok") : std::move(camera_reason);
    // The raw inputs, formatted here so the status command can show them without the JSON
    // builder having to know what a view-projection matrix is.
    state.camera_raw = FormatCameraRaw(camera);
    // `state.generation` is NOT bumped here.  It means "the published snapshot changed", and only
    // the caller knows when its publication is complete -- the full walk still has to install the
    // entity list after this returns.
}

// The first few records' centres and masks as one string, for the status document.
//
// One implementation, called by the walk (with the records it just built) and by the per-tick
// re-projection (with the published ones), because the masks and corners it prints are precisely
// what the re-projection rewrites: two copies of this formatting would let the diagnostic print
// the last walk's masks next to boxes drawn from fresh ones.
std::string EntitySampleString(const std::vector<EntityRecord>& entities) {
    std::string samples;
    const std::size_t limit = entities.size() < 3 ? entities.size() : 3;
    for (std::size_t index = 0; index < limit; ++index) {
        const EntityRecord& record = entities[index];
        char buffer[288]{};
        std::snprintf(buffer, sizeof(buffer),
                      "%s[%zu] center=(%.1f,%.1f,%.1f) mask=0x%02X c0=(%.1f,%.1f) c7=(%.1f,%.1f)",
                      samples.empty() ? "" : " | ", index, record.center[0], record.center[1],
                      record.center[2], record.screen_mask, record.screen_points[0][0],
                      record.screen_points[0][1], record.screen_points[7][0],
                      record.screen_points[7][1]);
        samples += buffer;
    }
    return samples;
}

// ================= THE CHEAP PATH: EVERY TICK THAT IS NOT A WALK TICK =================
//
// This is what makes the divisor a bookkeeping cadence rather than a latency knob, and it has two
// jobs, in this order:
//
//   1. RE-READ THE POSITIONS of the already-published entities, through the direct call, using the
//      `transform` each record kept.  This is the job that did not exist while a position cost
//      1.29 ms: without it, a box stays glued to the camera but not to the world, and an entity
//      that walked between two walks is drawn where it used to be.
//   2. RE-PROJECT them through the camera that was just re-read.  With (1) done, this is
//      arithmetic on fresh centres; without it, it is arithmetic on the walk's centres.
//
// So the ticks that are not walk ticks are not "stale" ticks: the camera, the positions and the
// boxes are all new.  What waits for the next walk is the entity SET and its bookkeeping -- which
// entities exist, what class they are, what they are called.
//
// It is NOT free of game memory any more, and the earlier version of this comment claimed it was
// ("touches no game memory at all ... cannot fault").  That claim is only true of the
// re-projection half.  The position half calls one verified function per entity, which cannot be
// the reason a tick is slow (it allocates nothing and invokes nothing) and which is contained by
// SEH if an object it is handed has been torn down.
//
// `state.projected_entities`/`full_mask_entities` are recounted rather than left at their previous
// values, because the masks are exactly what changed: a corner can cross the near plane between
// two walks, and a status document claiming "83 of 83 projected" while the plugin drops boxes for
// partial masks would be the wrong number at the moment it is needed.
//
// AND IT RE-READS THE POSITIONS, when the direct call was adopted.
//
// Re-projecting a stale centre keeps the box glued to the CAMERA but not to the WORLD: an entity
// that walked five metres between two walks would have its box drawn five metres behind it, at
// exactly the screen position its old centre projects to.  The re-projection alone is therefore
// only half the fix, and it is the half that was available while a position cost 1.29 ms through
// the reflection route.  With the direct call it costs a function call, so this loop re-reads
// every published entity's position from its cached `transform` and then projects it -- the
// positions and the boxes are as fresh as the camera, and only the entity SET is sampled.
//
// The transform pointers are the ones the walk already resolved, so this walks no container and
// re-implements no offset chain.  A record whose transform is gone (a despawn the walk has not
// noticed yet) is left alone rather than blanked: it keeps its last known position until the next
// walk removes it, which is the same behaviour the divisor already had.
void ReprojectPublishedEntityCorners(const CameraBinding& camera) {
    auto& state = State();
    const bool refresh_positions = DirectPositionAvailable();
    std::scoped_lock lock(state.mutex);
    std::size_t projected = 0;
    std::size_t full_mask = 0;
    for (EntityRecord& record : state.entities) {
        if (refresh_positions && record.transform != 0) {
            Vector3Out position;
            if (ReadDirectPosition(record.transform, &position) &&
                std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
                record.center[0] = position.x;
                record.center[1] = position.y;
                record.center[2] = position.z;
            }
        }
        ProjectRecordCorners(&camera, &record);
        if (record.screen_mask != 0) ++projected;
        if (record.screen_mask == 0xFFu) ++full_mask;
    }
    state.projected_entities = projected;
    state.full_mask_entities = full_mask;
    // The same string the walk publishes, from the records that were just re-projected: see
    // `EntitySampleString` for why this cannot be left to the walk.
    state.camera_cross_check = EntitySampleString(state.entities);
}

} }  // namespace (adapter internals)
// HOW MANY PLUGINS ARE ACTUALLY READING ENTITIES RIGHT NOW.
//
// THIS IS THE FIX FOR THE FRAME RATE, and the reason is worth stating plainly: the walk was
// called UNCONDITIONALLY from `PluginManager::GameUpdate`, before the plugin loop, so it ran at
// full cost whether or not any plugin was loaded, enabled, or subscribed.  Disabling the ESP
// plugin did not stop it: the walk cost 213 ms per game tick with the plugin switched off.
// A service that no one consumes must not cost anything.
//
// Any consumer can renew the shared lease; stopping one never cancels another.
// A plugin that is
// disabled, unloaded, or simply not drawing stops calling `entity_count`, and the walk then
// switches itself off instead of continuing to cost the game 200 ms per tick forever.
//
// A liveliness stamp rather than an explicit reference count, because the entity service has no
// teardown entry a plugin is obliged to call -- and a service whose cost depends on a plugin
// remembering to say "I am done" is a service that leaks frame time the first time someone
// forgets.  Recovery is automatic: the next read starts the walk again.
#define g_entity_requested_until (ComponentState().requested_until)
#define g_player_refresh_requested (ComponentState().player_refresh_requested)
std::uint64_t EntityClockMillis() {
    using namespace unity_adapter_entities_detail;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// Wall time rather than game ticks: a slow game must not take many seconds to go idle.
// Atomic because readers can run in the render domain. Never-read starts idle.
// ================= THE SAMPLING DIVISOR, AND EXACTLY WHAT IT DELAYS =================
//
// How often the walk REBUILDS THE ENTITY SET, in game ticks.  1 = every tick.  The value comes
// from `[Performance] EntitySnapshotTickInterval` through the adapter's
// `UnitySnapshotSamplingOptions` -- it is no longer a constant nothing assigns.
//
// WHAT IT DELAYS, and this is the part that matters because two earlier versions of this comment
// described designs the code did not have:
//
//   * THROTTLED: the entity SET and its bookkeeping -- which entities exist, their live class,
//     their label, their camp comparison, their kind.  This is what is left of the 213 ms once
//     the position read stopped going through `il2cpp_runtime_invoke`.
//   * NOT THROTTLED: the POSITIONS, the CAMERA, and the eight projected screen corners.  All
//     three are refreshed on EVERY tick by the cheap path below: the camera is five metadata
//     lookups and five invokes (~3% of one walk), a position is now a direct call to the
//     compiled body, and re-projecting a centre is arithmetic.  So a box never lags the camera
//     and never lags the thing it is drawn around.
//
// That split is the answer to "the divisor delays the boxes": it does not, and the reason it does
// not is `DirectPositionCall` above.  Raising the interval now costs entity-set freshness only.
//
// The first walk after a consumer appears is never skipped, so a plugin never sees a spurious
// "0 entities"; and an explicit player refresh request bypasses the divisor entirely.
//
// WHY A PROCESS-WIDE `static` AND NOT A FIELD OF THE PER-THREAD STATE: the field that used to hold
// this value was never assigned by anything, which made the divisor a constant wearing the
// appearance of a configured knob -- the recurring defect shape in this repository.  The value is
// written once from the constructing thread (before any tick can be installed) and read once per
// tick from the game thread, so a relaxed atomic is sufficient.
static std::atomic<std::uint32_t> g_entity_refresh_divisor{1};

std::uint32_t EntityRefreshDivisor() noexcept {
    const std::uint32_t configured = g_entity_refresh_divisor.load(std::memory_order_relaxed);
    // Never zero: the gate takes a modulus.
    return configured == 0 ? 1 : configured;
}

std::uint32_t EffectiveEntityRefreshDivisor() noexcept {
    using namespace unity_adapter_entities_detail;
    const std::uint32_t configured = EntityRefreshDivisor();
    const bool reflection_only =
        !EntityDirectPositionEnabled() ||
        ComponentState().position_call.state == static_cast<int>(DirectPositionState::rejected);
    if (reflection_only) {
        return configured > kDivisorWhenPositionsNeedReflection ? configured
                                                                : kDivisorWhenPositionsNeedReflection;
    }
    return configured;
}

void SetUnityEntityRefreshDivisor(std::uint32_t divisor) noexcept {
    g_entity_refresh_divisor.store(divisor == 0 ? 1 : divisor, std::memory_order_relaxed);
}

// THE ONE SWITCH THAT CANNOT BE A COMMENT.
//
// Everything else in this file READS the game.  The direct position call is the only thing that
// CALLS it, so "is this the fast path or the reflection path" has to be answerable without a
// rebuild -- by the person holding the game, by editing one ini line, when they are trying to
// find out whether a crash belongs to this port or to the game.  Written once from the
// constructing thread before any tick can be installed, read once per tick from the game thread.
static std::atomic<bool> g_entity_direct_position{true};

void SetUnityEntityDirectPosition(bool enabled) noexcept {
    g_entity_direct_position.store(enabled, std::memory_order_relaxed);
}

bool EntityDirectPositionEnabled() noexcept {
    return g_entity_direct_position.load(std::memory_order_relaxed);
}

#define g_entity_refresh_tick (ComponentState().refresh_tick)

// The camera half of the published state, and the per-tick re-projection of the published boxes,
// are DEFINED ABOVE in the entity detail namespace (where `ServiceState`, `CameraBinding`,
// `EntityRecord` and `ProjectRecordCorners` live).  Called from the walk below, which is in this
// namespace and pulls them in with its `using namespace unity_adapter_entities_detail;`.

void RequestUnityPlayerEntityRefresh() {
    ServiceCall adapter_call;
    if (!adapter_call) return;
    using namespace unity_adapter_entities_detail;
    ComponentState().player_refresh_requested.store(true, std::memory_order_release);
    // Edge-triggered: one full walk is requested only for first bind or
    // recovery after the cached Transform becomes invalid.
}

void InvalidateUnityEntityState() noexcept {
    using namespace unity_adapter_entities_detail;
    auto& state = ComponentState();
    std::scoped_lock lock(state.mutex);
    state.entities.clear();
    std::memset(state.published_label, 0, sizeof(state.published_label));
    ++state.generation;
    state.unavailable_reason = "scene invalidated; awaiting entity rebind";
    state.class_resolved = false;
    state.camera_anchor_resolved = false;
    state.main_camera_present = false;
    state.main_camera_object = 0;
    state.main_camera_native = 0;
    state.camera_reason = "scene invalidated; awaiting camera rebind";
    state.camera_matrix_valid = false;
    state.camera_check_ok = false;
    state.camera = {};
    ComponentState().player_refresh_requested.store(true, std::memory_order_release);
}

void RefreshUnityEntityEsp() {
    using namespace unity_adapter_entities_detail;
    // --- THE EARLY OUT, which is what stops this from being a frame-rate regression -----------
    //
    // Nothing below this line runs when no plugin is reading entities.  That is not an
    // optimisation; it is the correctness condition.  Before this, the walk ran at full cost
    // from `PluginManager::GameUpdate` regardless of the plugin list, so a user who had
    // switched the ESP plugin OFF still paid 213 ms per tick for it.
    // The tick counter controls sampling only; demand expires by monotonic wall time.
    const std::uint64_t tick = ++g_entity_refresh_tick;
    const std::uint64_t now = EntityClockMillis();
    const bool esp_requested =
        now <= g_entity_requested_until.load(std::memory_order_relaxed);
    const bool player_requested =
        ComponentState().player_refresh_requested.exchange(false, std::memory_order_acq_rel);
    if (!esp_requested && !player_requested) {
        return;
    }
    // ---- WHICH HALF OF THE TICK THIS IS ----
    //
    // Decided HERE, but the cheap path does not return until after the camera has been refreshed
    // below, because the camera is what it re-projects the boxes from.
    //
    // `player_requested` bypasses the divisor entirely: an explicit refresh request IS the
    // recovery path (first bind, or a scene that invalidated the cached Transform), and skipping
    // it would leave the plugin looking at an empty list for up to `divisor` ticks.
    //
    // THE FIRST WALK AFTER A CONSUMER APPEARS IS NEVER SKIPPED, and it is detected by the DEMAND
    // going from absent to present rather than by a tick number.  The previous version tested
    // `tick <= 1`, which is not the same statement: the counter keeps running while nobody is
    // reading entities (idle ticks increment it too), so a plugin enabled after the process had
    // been up for a while could land on a non-multiple tick and be served an EMPTY published list
    // for up to `divisor - 1` ticks -- "0 entities" in its status panel, which is exactly what the
    // old comment promised could not happen.
    const bool consumer_was_active = ComponentState().consumer_active;
    ComponentState().consumer_active = esp_requested || player_requested;
    const bool first_walk_after_idle = !consumer_was_active;
    // THE EFFECTIVE DIVISOR, not the configured one: when the direct position call was refused the
    // walk still pays 1.29 ms per entity, so an interval of 1 is floored to a playable cadence.
    // That override is a safety net with a published reason, not a silent one.
    const std::uint32_t divisor = EffectiveEntityRefreshDivisor();
    const bool full_walk = player_requested || first_walk_after_idle || divisor <= 1 ||
                           (tick % divisor) == 0;
    // Game domain only: this is the one entry point that may call IL2CPP.  The iteration
    // entries below read the cache and are therefore safe from the render domain, which is
    // where an ESP calls them once per pass.
    //
    // The cache is built OUTSIDE `state.mutex` and swapped in at the end.  The walk follows
    // pointers out of managed memory and any of them can be stale; holding the lock across
    // it would make one unreadable pointer block the render domain's `entity_count`, which
    // is the difference between "this frame has no boxes" and "the overlay stopped
    // drawing".  A local vector plus one short lock is the whole point.
    //
    // TIMED, because this runs on the GAME thread and the game's own tick rate is what pays
    // for it.  Unthrottled, the tick rate fell to 7.6 Hz.  A frame-rate complaint is not
    // actionable until it is known WHICH of these two halves is eating the tick -- the camera
    // read is five metadata lookups and five `runtime_invoke` calls, the entity walk is one
    // `runtime_invoke` per entity for its world position (1.29 ms, and the reason the direct
    // call below exists) across ~80 entities, and they call for completely
    // different fixes.  Guessing between them is how the projection bug survived four
    // rewrites.
    const auto tick_started = std::chrono::steady_clock::now();
    auto& state = State();
    std::vector<EntityRecord> entities;
    std::string reason;
    bool class_resolved = true;
    // How many entities the game actually answered a screen position for.  Published because
    // "80 entities found" and "0 of them projected" is a camera failure while "80 of them
    // projected" is a drawing failure, and the two need different fixes.
    std::size_t projected_entities = 0;
    std::int64_t camera_micros = 0;
    std::int64_t walk_micros = 0;

    if (!state.binding.resolved) {
        LiveBinding binding;
        if (ResolveLiveBinding(&binding, &reason)) {
            state.binding = binding;
        } else {
            class_resolved = false;
        }
    }
    // Names are decoration: a failure here must not fail the ESP.  Retried on a slow cadence
    // rather than every tick -- the resolution is six metadata walks, and running them sixty
    // times a second on the game thread to keep arriving at the same answer is exactly the kind
    // of thing that turns into "the ESP made the game slow".  A genuinely absent class is not
    // going to appear one tick later; a class that loads with the scene will be found within a
    // second.
    // A SINGLE ENTITY IS NOT ENOUGH TO RESOLVE AGAINST.
    //
    // Resolution now walks `il2cpp_class_get_fields` on the game's own data and config classes --
    // metadata reads, but reads against classes the game is still constructing during loading.
    // Waiting until the scene holds more than the one loading-scene object both avoids that
    // window and makes the resolution happen when its answer is meaningful.  The retry cadence
    // below means a scene that never reaches two entities simply never binds, which is correct:
    // there is nothing to name.
    // A SINGLE ENTITY IS NOT ENOUGH TO RESOLVE AGAINST.
    //
    // The resolution now runs below, AFTER the walk, because the gate it needs is
    // the walk's own output.

    // THE CAMERA BLOCK THAT USED TO SIT HERE HAS MOVED, and the reason is the divisor.  It now
    // runs on EVERY tick, immediately below, BEFORE the branch that decides whether this tick is a
    // walk tick -- because the cheap path re-projects the published boxes from the camera it reads.
    // See "THE CAMERA, ON EVERY TICK" below.

    // ================= THE CAMERA, ON EVERY TICK =================
    //
    // The camera is resolved BEFORE the entity walk, and the reason is a change of design
    // rather than a change of order.
    //
    // The walk PROJECTS each entity as it reads it (`ReadEntity`), because the game's
    // projection function is only callable on the game thread -- and this tick IS that thread.
    // So the camera has to be in hand before the first entity is read.
    //
    // This does NOT rebuild the view-projection matrix.  The inputs available on the render side
    // are not the ones the game used, so no amount of algebra on them reproduces the game's
    // answer -- and a matrix that is wrong while its own self-check reports success is worse than
    // no matrix at all.  The shipped UE5 host
    // in the sibling `Anomaly` project answers its `project` entry by CALLING the game's own
    // projection function instead of rebuilding UE's matrices,
    // and that is what this now does too.
    //
    // THE CAMERA IS NOT THROTTLED BY THE DIVISOR.  It sits between the gate's DECISION and the
    // gate's BRANCH, which is the whole arrangement: the boxes are projected from this matrix on
    // every tick, so a camera sampled once every `divisor` ticks would make the entire overlay
    // slide against the world whenever the view moves.  Its cost is five metadata lookups and five
    // `runtime_invoke` calls, against two invokes per entity for the walk -- about 3% of one walk
    // per tick, paid to keep the boxes attached to the camera.
    std::string camera_reason;
    std::uint64_t camera_object = 0;
    std::uint64_t camera_native = 0;
    bool camera_ok = false;
    std::uintptr_t camera = 0;
    if (!state.camera.resolved && !ResolveCamera(&state.camera, &camera_reason)) {
        // Left unresolved so the next tick retries: the camera classes exist from the start,
        // so this only fails while the runtime is still coming up.
    } else if (state.camera.resolved) {
        camera = ReadMainCamera(state.camera, &camera_reason);
        if (camera != 0) {
            camera_object = static_cast<std::uint64_t>(camera);
            camera_native = static_cast<std::uint64_t>(ReadCameraNative(camera));
            camera_ok = true;
            // ALL OF THE CAMERA WORK, in one call: viewport, camera position for the
            // diagnostic, and the composed `projection * worldToCamera`.  Re-read every tick
            // rather than cached, because the field of view moves with aiming, sprinting and
            // cutscenes -- and a matrix kept from a previous fov puts every box at the wrong
            // scale, which reads as "slightly off" rather than as a bug.
            if (!ReadViewProjection(il2cpp::Functions(), &state.camera, camera, &camera_reason)) {
                camera_ok = false;
            } else {
                camera_reason.clear();
            }
        }
    }
    camera_micros = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - tick_started)
                        .count();

    // ================= THE CHEAP PATH: A TICK THAT IS NOT A WALK TICK =================
    //
    // The walk is skipped; the POSITIONS and the BOXES are not.  See
    // `ReprojectPublishedEntityCorners` for what it re-reads and why that is now more than
    // arithmetic, and `PublishCameraStateLocked` for why the camera's diagnostics are refreshed
    // here as well rather than waiting for the next walk.
    if (!full_walk) {
        ReprojectPublishedEntityCorners(state.camera);
        {
            std::scoped_lock lock(state.mutex);
            PublishCameraStateLocked(state, state.camera, camera_ok, camera_object, camera_native,
                                     std::move(camera_reason));
            // The corners and the camera are both new, so the snapshot the render domain reads
            // has changed: consumers that key off `generation` must see that.
            ++state.generation;
        }
        // THE TICK COST IS RECORDED ON THIS PATH TOO, and that is not bookkeeping: `tickMicros`
        // is the number a frame-rate complaint is read from, and a cheap tick that recorded
        // nothing would leave the average showing the last full walk's 213 ms -- so the status
        // document would keep claiming the optimisation had not happened.  `walk_micros` decays
        // toward the truth of a tick with no walk.
        state.tick_micros = (state.tick_micros * 63 + camera_micros) / 64;
        state.camera_micros = (state.camera_micros * 63 + camera_micros) / 64;
        state.walk_micros = (state.walk_micros * 63) / 64;
        state.ticks_measured = state.ticks_measured < 1000000 ? state.ticks_measured + 1
                                                             : state.ticks_measured;
        return;
    }

    if (class_resolved && state.binding.resolved) {
        entities.clear();
        std::string walk_reason;
        if (!WalkEntities(state.binding, &state.camera, &entities, &walk_reason)) {
            // An empty set WITH the reason.  This is the honest state: the plugin shows
            // "0 entities" and says why, instead of a number that came from an unverified
            // pointer chase -- and "not in a battle yet" is now distinguishable from "the
            // binding is wrong", which it was not while both reported the same sentence.
            reason = walk_reason;
        } else if (!walk_reason.empty()) {
            reason = walk_reason;
        }
    }
    walk_micros = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - tick_started)
                      .count() -
                  camera_micros;

    // ================= LABEL RESOLUTION (must run AFTER the walk) =================
    //
    // THIS BLOCK WAS GATED ON `entities.size()`, WHICH IS ALWAYS 0 HERE.
    //
    // `entities` is declared as an empty vector near the top of this function and
    // only filled by `WalkEntities` ~70 lines further down.  While this block sat
    // above the walk the gate read 0 on every tick, `scene_is_ready` was permanent-
    // ly false, and the entire label chain was dead code.  The status output said
    // `entities: 83` while the gate reported `entities=0`, in the same document.
    //
    // After the walk the gate finally means what it says: the scene is ready when
    // the walk itself found something nameable.
    const bool scene_is_ready = entities.size() > 1;
    if (scene_is_ready && !ComponentState().label_binding.resolved && ++ComponentState().label_resolve_attempts % 60 == 1) {
        // The reason is kept on SUCCESS too, not only on failure.
        //
        // This previously assigned `reason` only inside the failure branch, so a successful
        // binding left `ComponentState().label_binding.reason` empty -- and `labelReason` in `status` then read
        // as "" exactly when the binding had worked. That is the second time in this file that a
        // computed diagnosis was thrown away at the last step; the diagnosis is the product here,
        // not a side effect.
        std::string label_reason;
        ResolveLabelBinding(il2cpp::Functions(), &ComponentState().label_binding, &label_reason);
        ComponentState().label_binding.reason = label_reason;
    }
    // Counted rather than assumed: a box is drawable only when the game actually answered for
    // it, and "the camera is resolved" is not the same statement as "the entities are on
    // screen".  This is the number that separates a broken camera from a broken entity chain.
    {
        std::size_t projected = 0;
        std::size_t full_mask = 0;
        for (const EntityRecord& record : entities) {
            if (record.screen_mask != 0) ++projected;
            if (record.screen_mask == 0xFFu) ++full_mask;
        }
        projected_entities = projected;
        state.projected_entities = projected;
        state.full_mask_entities = full_mask;
        // A HISTOGRAM OF `kind`, because "every box is blue" has exactly two possible causes
        // and this separates them: all-zero means the camp comparison never matched (the
        // values or the field offset are wrong), while a mixed histogram with a dominant
        // zero means some entities are simply not classified.  Guessing between those two
        // costs another restart.
        std::size_t kinds[8]{};
        for (const EntityRecord& record : entities) {
            if (record.kind < 8) ++kinds[record.kind];
        }
        char histogram[128]{};
        // THE LABELS MUST NAME THE FOUR BUCKETS THE CODE ACTUALLY FILLS.
        //
        // This handler used to print `unclassified / enemy / player / other`, which was correct
        // when there were three buckets.  After the split it kept printing those four words while
        // reading kinds 0,1,2 and lumping 3 and 4 into "other" -- so `enemy=6` was really MONSTERS
        // and `other=62` was really NPCs plus world objects.  That mislabelled output sent three
        // rounds of debugging at the wrong problem, including a full restart.
        std::snprintf(histogram, sizeof(histogram),
                      "unclassified=%zu player=%zu monster=%zu npc=%zu world=%zu",
                      kinds[kKindUnclassified], kinds[kKindPlayer], kinds[kKindMonster],
                      kinds[kKindNpc], kinds[kKindWorld]);
        state.kind_histogram = histogram;

        // A HISTOGRAM OF LIVE CLASS NAMES, because the previous round could only report that the
        // name lookup had failed -- not what it had failed ON.  With this, a `labelReads: 0` is
        // immediately either "these classes are not in my table" or "there is nothing nameable in
        // this scene", and a bound class carrying a non-zero count but still no name points at the
        // field chain instead.  Bounded and copied, so it cannot grow with the scene.
        {
            struct ClassCount {
                char name[40]{};
                std::size_t count{};
            };
            ClassCount classes[8]{};
            std::size_t class_count = 0;
            std::size_t unbound = 0;
            for (const EntityRecord& record : entities) {
                if (record.data_class[0] == 0) {
                    ++unbound;
                    continue;
                }
                std::size_t slot = class_count;
                for (std::size_t index = 0; index < class_count; ++index) {
                    if (std::strcmp(classes[index].name, record.data_class) == 0) {
                        slot = index;
                        break;
                    }
                }
                if (slot == class_count) {
                    if (class_count >= 8) {
                        ++unbound;
                        continue;
                    }
                    std::snprintf(classes[class_count].name, sizeof(classes[0].name), "%s",
                                  record.data_class);
                    ++class_count;
                }
                ++classes[slot].count;
            }
            std::string summary;
            for (std::size_t index = 0; index < class_count; ++index) {
                if (!summary.empty()) summary += " ";
                summary += classes[index].name;
                summary += "=";
                summary += std::to_string(classes[index].count);
            }
            if (unbound != 0) {
                if (!summary.empty()) summary += " ";
                summary += "unbound=" + std::to_string(unbound);
            }
            state.data_class_histogram = std::move(summary);
        }

        // WHERE THE NAME CHAIN BREAKS, and what it actually produced.
        //
        // `labelReads: 60` with `labelMicros: 19` said the reads happened and were cheap, but not
        // whether they yielded text.  "Matched a class but the config pointer was null" and
        // "produced a name and cached it" are identical in every counter above; these two strings
        // are the difference, and the difference is not guessable from a count.
        {
            char stages[192]{};
            // THE FORMAT STRING MUST HAVE EXACTLY AS MANY SPECIFIERS AS ARGUMENTS.
            //
            // This carried a sixth `mismatch=%lld` left over from an older set of counters that
            // no longer existed, so `keyOnly` was reading a VARIADIC ARGUMENT THAT WAS NEVER
            // PASSED -- whatever happened to be on the stack.  In practice that reads as
            // `keyOnly=1304144`, a number too large to be a per-frame count and too specific to
            // look like noise -- easily taken as evidence that names were being produced from
            // localisation keys.  MSVC's C4473 warning had been pointing at it all along.
            std::snprintf(stages, sizeof(stages),
                          "unmatched=%lld noConfig=%lld noName=%lld noGroup=%lld keyOnly=%lld",
                          static_cast<long long>(ComponentState().walk_profile.label_unmatched),
                          static_cast<long long>(ComponentState().walk_profile.label_no_config),
                          static_cast<long long>(ComponentState().walk_profile.label_no_name),
                          static_cast<long long>(ComponentState().walk_profile.label_no_group),
                          static_cast<long long>(ComponentState().walk_profile.label_key_only));
            state.label_stage_summary = stages;

            // ONE SAMPLE PER DATA CLASS, UP TO `kMaximumLabelSamples` CLASSES.
            //
            // The first version took the first four entities that had a label, whatever they
            // were.  With the scene's entity order that meant `BattleFieldData`, `PlayerData` and
            // two `WorldItemData` -- and NEVER `HeroData`, whose name was the entire point of the
            // feature.  Six consecutive polls showed the same four, which read as "hero names are
            // not being produced" when they were being produced correctly and simply could not
            // reach the window.
            //
            // Sampling per CLASS instead means every class that has a name is visible after one
            // look, which is what the field is for: it answers "which chains work", not "what are
            // the first four labels".
            std::string samples;
            std::size_t sampled_class_count = 0;
            constexpr std::size_t kMaximumLabelSamples = 8;
            const char* sampled_classes[kMaximumLabelSamples]{};
            for (const EntityRecord& record : entities) {
                if (record.label_size == 0 || record.label == nullptr) continue;
                const char* const klass = record.data_class[0] != 0 ? record.data_class : "?";
                bool already = false;
                for (std::size_t index = 0; index < sampled_class_count; ++index) {
                    if (std::strcmp(sampled_classes[index], klass) == 0) {
                        already = true;
                        break;
                    }
                }
                if (already) continue;
                if (sampled_class_count >= kMaximumLabelSamples) break;
                sampled_classes[sampled_class_count++] = klass;
                if (!samples.empty()) samples += " | ";
                samples += klass;
                samples += " -> ";
                samples.append(record.label, record.label_size);
            }
            state.label_samples =
                samples.empty() ? std::string("(no entity produced a name)") : samples;
        }
    }
    state.total_entities = entities.size();
    // Whether the name chain resolved, and why not when it did not.  Reported separately from
    // `unavailable_reason` because names are decoration: "44 boxes, no names" is a working ESP
    // with a broken label chain, and reporting it as unavailable would be a lie about the part
    // that works.
    state.camps_known = state.binding.camps_known;
    state.camp_monster = state.binding.camp_monster;
    state.camp_player = state.binding.camp_player;
    state.labels_resolved = ComponentState().label_binding.resolved;
    // The RESOLVED table goes into the status verbatim, not the word "ok".
    //
    // Reporting a bare `"ok"` whenever the binding resolved would throw away the one piece of
    // information that matters: WHICH classes it bound.  The binding already computes the
    // answer; not publishing it would be a self-inflicted blindness, not a limitation.
    state.label_reason = ComponentState().label_binding.reason;
    state.camp_stage = state.binding.camp_stage;

    // The tick cost, averaged over the last 64 ticks so the number is stable enough to read
    // from a status dump.  `tickMicros` is what the game's frame budget pays per tick; the two
    // halves say which one to optimise, which is the whole question behind "it is running at
    // single-digit fps".
    state.tick_micros = (state.tick_micros * 63 + (camera_micros + walk_micros)) / 64;
    state.camera_micros = (state.camera_micros * 63 + camera_micros) / 64;
    state.walk_micros = (state.walk_micros * 63 + walk_micros) / 64;
    state.ticks_measured = state.ticks_measured < 1000000 ? state.ticks_measured + 1
                                                         : state.ticks_measured;

    // The first few entities' centres and masks, so "no boxes" can be told apart from "no
    // projection".  A mask of 0x00 means the matrix rejected every corner; a PARTIAL mask means
    // the plugin's all-eight requirement is what drops the box -- and those two need opposite
    // fixes, which is exactly the distinction a bare box count cannot make.
    //
    // Built by the SAME function the cheap path calls, because the masks and corners in this string
    // are exactly what the cheap path rewrites every tick: a copy of this formatting left here
    // would print the last walk's masks beside boxes drawn from fresh ones.
    state.camera_cross_check = EntitySampleString(entities);

    std::scoped_lock lock(state.mutex);
    state.class_resolved = class_resolved && state.binding.resolved;
    state.entities = std::move(entities);
    if (!reason.empty()) {
        state.unavailable_reason = std::move(reason);
    } else {
        state.unavailable_reason = "live";
    }
    // The camera half of the snapshot, through the SAME function the cheap path calls, so the two
    // paths cannot disagree about what a published camera looks like.
    PublishCameraStateLocked(state, state.camera, camera_ok, camera_object, camera_native,
                             std::move(camera_reason));
    ++state.generation;
}

// ============================================================================
// THE ENTITY SOURCE, AS A SOURCE OF LIVE TRANSFORMS
// ============================================================================
//
// Consumed by `src/game/unity/unity_adapter.cpp` (the `cabbird.unity.player`
// backend), never by a plugin.  The player service has to MOVE the local player, and
// moving it means calling a managed method on its `UnityEngine.Transform` -- which
// means it needs that pointer, and the only code in this tree that has resolved the
// chain that reaches it is this file.
//
// The alternative was to copy `BaseData::<transform>` / `RelativeTransform::m_transform`
// into the player service.  That is the duplicated-layout failure the repository already
// has a rule about: a game patch would then be confirmed by one copy and silently wrong
// in the other.  So the walk lives here, once.
//
// WHY `m_CachedPtr` IS CHECKED BEFORE THE POINTER IS HANDED OUT: a destroyed
// `UnityEngine.Object` keeps a reachable managed wrapper and a NULL native pointer, and
// calling a method on one faults inside the runtime.  The entity walk refuses the same
// objects for the same reason.

// Is this live data class one of the game's character/player classes?
//
// THE SAME THREE NAMES THE ENTITY WALK USES to decide `CABBIRD_UNITY_ENTITY_V1_KIND_PLAYER`
// (`PlayerData` / `HeroData` / `PetData`).  Kept as its own predicate rather than reading
// `kind` back out of the record, because `kind` is later REFINED by the camp comparison and
// the question here is the class question only.
//
// THE PLAYER SERVICE KEEPS ITS OWN COPY OF THIS PREDICATE, deliberately: it decides the same
// question about the same three names but reads the class out of the published LABEL, and
// the two must be able to disagree loudly if the DTO's label ever stops carrying the class.
// A shared helper here would make the player service depend on a symbol in this translation
// unit for a one-line string comparison.
bool PlayerShapedDataClass(const char* data_class) {
    using namespace unity_adapter_entities_detail;
    if (data_class == nullptr) return false;
    return std::strcmp(data_class, "PlayerData") == 0 ||
           std::strcmp(data_class, "HeroData") == 0 ||
           std::strcmp(data_class, "PetData") == 0;
}

UnityEntityLookup LookupUnityEntityById(std::uint64_t entity_id) {
    ServiceCall adapter_call;
    if (!adapter_call) return {};
    using namespace unity_adapter_entities_detail;
    UnityEntityLookup result;
    result.entity_id = entity_id;
    if (entity_id == 0) {
        result.error = CABBIRD_STATUS_V1_INVALID_ARGUMENT;
        return result;
    }
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    result.generation = state.generation;
    for (const EntityRecord& record : state.entities) {
        if (record.id != entity_id) continue;
        result.found = true;
        result.data = record.data;
        result.kind = record.kind;
        result.player_shaped = PlayerShapedDataClass(record.data_class);
        // COPIED INTO THE RESULT'S OWN BUFFER, which is why `UnityEntityLookup::data_class`
        // became a char array instead of a `const char*`.
        //
        // `record.data_class` lives inside `state.entities`, and the next refresh MOVES that
        // vector, freeing the string it points at -- handing out the pointer would hand out
        // freed memory, the exact bug recorded against `entity_at`'s label before stable
        // storage was added for it.
        //
        // THE FIRST VERSION USED `thread_local char class_copy[40]`, AND IT WAS REFUSED.
        // This image is MANUALLY MAPPED and a mapped image has no loader to run static TLS
        // initialisation for it: one `thread_local` in any translation unit of this image put
        // a TLS directory into the DLL and the mapper rejected it (correctly -- the same lesson
        // is written up at length in include/cabbird/thread_local_value.hpp, and the status
        // message above used to be a `thread_local std::string`).
        //
        // An array in the returned struct is better than that buffer anyway: the caller owns
        // the lifetime, so the name stays valid no matter which thread reads it next.
        std::snprintf(result.data_class, sizeof(result.data_class), "%s", record.data_class);
        return result;
    }
    result.error = CABBIRD_STATUS_V1_NOT_FOUND;
    return result;
}

// ============================================================================
// THIS SERVICE IS READ-ONLY, AND THIS IS WHERE IT STOPS.
//
// `ResolveUnityEntityTransform`, `ReadUnityTransformPosition` and
// `ApplyUnityTransformPosition` were declared and defined in this file until the refactor the
// user asked for.  Two of them read an engine value and one of them MOVED AN OBJECT, which is
// why the header's promise ("publish the cached entity set") and the header's surface
// disagreed: a plugin author reading it could only conclude that the entity source also moved
// things.
//
// All three now live in `unity_adapter.cpp` (`cabbird.unity.transform`), in this
// same adapter directory, because the two offset hops and the managed `Transform` calls are
// ENGINE FACTS -- true regardless of any entity list, any camera, or any overlay.  Both this
// service and the player service CONSUME them, which means:
//
//   * one implementation of `BaseData::<transform> -> RelativeTransform::m_transform`, not
//     two that can drift apart on a game patch;
//   * one copy of the boxed-`Vector3` + `il2cpp_runtime_invoke` boilerplate, shared by
//     `set_position` and `set_eulerAngles`;
//   * a read-only service that is actually read-only.
//
// The local helper `CopyLiveBinding` went with them.  It existed to copy the binding out from
// under this service's mutex "so the pointer walk below does not run while holding a lock the
// render domain's `entity_count` needs" -- and the pointer walk below is gone, so the only
// remaining caller of the binding is the entity walk itself, which reads it under the lock it
// already holds.
//
// Writing is `unity_adapter.cpp`'s job, and only its job -- the two capabilities in the
// manifest are separate for that reason.
// ============================================================================

double UnityEntityBoxBaseOffset() {
    ServiceCall adapter_call;
    if (!adapter_call) return {};
    using namespace unity_adapter_entities_detail;
    return kEntityBaseOffset;
}

bool ProvideEspCameraPosition(double position[3]) {
    ServiceCall adapter_call;
    if (!adapter_call) return false;
    using namespace unity_adapter_entities_detail;
    if (position == nullptr) return false;
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    if (!state.camera.view_projection_valid) return false;
    position[0] = state.camera.camera_pos[0];
    position[1] = state.camera.camera_pos[1];
    position[2] = state.camera.camera_pos[2];
    return true;
}


// The overlay's camera source.
//
// Called from the RENDER domain, once per overlay pass.  It reads the matrix the game thread
// published and NOTHING ELSE -- no IL2CPP, no allocation, no lock held across anything that
// could block.  That split is the whole design: the expensive, runtime-touching half runs on
// the thread the runtime owns, and the per-frame half is a struct copy.
//
// Returns false when there is no valid matrix, which makes `FrameProject` return 0 and the
// plugin skip the entity.  A stale matrix is deliberately not kept: see `RefreshUnityEntityEsp`.
bool CABBIRD_CALL ProvideEspCameraMatrix(
    void* user, const double world[3], double view_projection[16], double* depth) noexcept {
    ServiceCall adapter_call;
    if (!adapter_call) return false;
    using namespace unity_adapter_entities_detail;
    static_cast<void>(user);
    static_cast<void>(world);
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    if (!state.camera_matrix_valid) return false;
    // ELEMENT-WISE, and that is a bug fix rather than a style choice.
    //
    // This used to be `std::memcpy(view_projection, state.camera.view_projection,
    // sizeof(state.camera.view_projection))`.  The source is a `float[16]` (the camera
    // properties return `Matrix4x4`, whose components are floats) and the destination is a
    // `double[16]`, so the memcpy copied HALF a matrix worth of bytes into the low half of the
    // doubles and left the high halves as whatever bytes happened to be there.  It is a
    // type-punned read of indeterminate memory -- formally undefined, and capable of producing
    // a plausible-looking matrix.
    //
    // Nothing called it any more: the plugin gets projected corners from `entity_at` and no
    // longer projects for itself.  That is exactly why it stayed wrong -- dead code is not
    // reviewed, and the next person to use `frame->project` would have inherited a silent
    // corruption.  Kept as a correct implementation rather than deleted, because the entry is
    // part of the published SDK and a plugin compiled against an older SDK still calls it.
    for (int index = 0; index < 16; ++index) {
        view_projection[index] = static_cast<double>(state.camera.view_projection[index]);
    }
    // `w` is NOT 1 for every point, contrary to what this once claimed ("the fourth row is
    // (0,0,0,1)").  The composed matrix's fourth row is the projection's, which for a
    // perspective camera is `(0, 0, -1, 0)` -- so `w` is the view-space depth, and the
    // behind-camera test downstream is meaningful.  Reported as 0 rather than as a fake 1,
    // because a fake 1 told every consumer that depth was unconstrained when the point could be
    // behind the eye.
    if (depth != nullptr) *depth = 0.0;
    return true;
}

HostEntityStats SnapshotHostEntities() {
    ServiceCall adapter_call;
    if (!adapter_call) return {};
    using namespace unity_adapter_entities_detail;
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    HostEntityStats stats{};
    stats.entities = state.entities.size();
    stats.generation = state.generation;
    stats.class_resolved = state.class_resolved;
    stats.unavailable_reason = BorrowedServiceText(0, state.unavailable_reason);
    stats.camera_matrix_valid = state.camera_matrix_valid;
    stats.camera_check_ok = state.camera_check_ok;
    stats.camera_check_error_pixels = state.camera_check_error_pixels;
    stats.camera_anchor_resolved = state.camera_anchor_resolved;
    stats.main_camera_present = state.main_camera_present;
    stats.main_camera_object = state.main_camera_object;
    stats.main_camera_native = state.main_camera_native;
    stats.camera_reason = BorrowedServiceText(1, state.camera_reason);
    stats.camera_raw = BorrowedServiceText(2, state.camera_raw);
    stats.camera_cross_check = BorrowedServiceText(3, state.camera_cross_check);
    stats.projected_entities = state.projected_entities;
    stats.data_class_histogram = BorrowedServiceText(4, state.data_class_histogram);
    stats.label_stage_summary = BorrowedServiceText(5, state.label_stage_summary);
    stats.label_samples = BorrowedServiceText(6, state.label_samples);
    stats.kind_histogram = BorrowedServiceText(7, state.kind_histogram);
    stats.camps_known = state.camps_known;
    stats.camp_monster = state.camp_monster;
    stats.camp_player = state.camp_player;
    stats.full_mask_entities = state.full_mask_entities;
    stats.total_entities = state.total_entities;
    stats.labels_resolved = state.labels_resolved;
    stats.label_reason = BorrowedServiceText(8, state.label_reason);
    stats.tick_micros = state.tick_micros;
    stats.camp_stage = BorrowedServiceText(9, state.camp_stage);
    state.name_probe = ComponentState().name_probe;
    {
        // THE ONE NUMBER THAT ANSWERS "HOW MANY", AND IT IS APPENDED TO THE EXISTING FIELD
        // rather than added to the status schema: the schema is shared with the plugin, and a
        // diagnostic does not warrant a version bump.  `labelSamples` shows one entity per class
        // and the one it happened to show was `Blueprint[300031]`, which is a `configName`.
        char counts[220]{};
        std::snprintf(counts, sizeof(counts),
                      " || worldLabels: table=%lld configName=%lld modelPath=%lld classTag=%lld",
                      ComponentState().world_label_counts[kWorldLabelTable],
                      ComponentState().world_label_counts[kWorldLabelConfigName],
                      ComponentState().world_label_counts[kWorldLabelUnnamed],
                      ComponentState().world_label_counts[kWorldLabelClassTag]);
        state.name_probe += counts;
        state.name_probe += " || ";
        state.name_probe += ComponentState().collecting_index_log;
        // WHICH TABLE WON, so the chain's order stops being a guess that is never scored.
        state.name_probe += " || worldwin:";
        for (std::size_t index = 0; index < kWorldNameTableCount; ++index) {
            if (ComponentState().world_table_wins[index] == 0) continue;
            char one[40]{};
            std::snprintf(one, sizeof(one), " %s=%lld", kWorldNameTables[index].alias,
                          ComponentState().world_table_wins[index]);
            state.name_probe += one;
        }
    }
    stats.name_probe = BorrowedServiceText(10, state.name_probe);
    stats.camera_micros = state.camera_micros;
    stats.walk_micros = state.walk_micros;
    stats.ticks_measured = state.ticks_measured;
    state.position_cache_hits = ComponentState().walk_profile.position_cache_hits;
    state.position_cache_rejects = ComponentState().position_cache_rejects;
    state.position_reads = ComponentState().walk_profile.position_reads;
    state.position_failures = ComponentState().walk_profile.position_failures;
    state.position_micros = ComponentState().walk_profile.position_micros;
    state.label_reads = ComponentState().walk_profile.label_reads;
    state.label_cache_hits = ComponentState().walk_profile.label_cache_hits;
    stats.position_cache_hits = state.position_cache_hits;
    stats.position_cache_rejects = state.position_cache_rejects;
    state.label_cache_rejects = ComponentState().label_cache_rejects;
    state.label_micros = ComponentState().walk_profile.label_micros;
    stats.position_reads = state.position_reads;
    stats.position_failures = state.position_failures;
    stats.position_micros = state.position_micros;
    // The direct call's verdict and its own cost.  `untried` is reported as such rather than as an
    // empty string: an empty field reads as "no such feature", and the feature exists -- it just
    // has not had a camera transform to prove itself on yet.
    {
        const DirectPositionCall& call = ComponentState().position_call;
        const char* const verdict =
            call.state == static_cast<int>(DirectPositionState::untried)
                ? "untried (no camera transform resolved yet)"
                : call.reason;
        stats.direct_position = BorrowedServiceText(31, std::string_view(verdict));
        stats.direct_position_reads = call.reads;
        stats.direct_position_faults = call.faults;
        stats.direct_position_micros = call.micros;
    }
    stats.label_reads = state.label_reads;
    stats.label_cache_rejects = state.label_cache_rejects;
    stats.label_cache_hits = state.label_cache_hits;
    stats.label_micros = state.label_micros;
    return stats;
}

namespace { namespace unity_adapter_entities_detail {

// --- the published service table ------------------------------------------

std::uint64_t CABBIRD_CALL EntityGeneration(void*) noexcept {
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    return state.generation;
}

std::uint32_t CABBIRD_CALL EntityCount(void*) noexcept {
    // THE LIVELINESS STAMP.  This is the render domain telling the game domain "someone is
    // still reading entities", and it is what lets the walk switch itself off instead of
    // costing ~200 ms of every game tick forever. Safe from any thread: an atomic store.
    g_entity_requested_until.store(EntityClockMillis() + kEntityConsumerIdleMillis,
                                   std::memory_order_relaxed);
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    return static_cast<std::uint32_t>(state.entities.size());
}

CabbirdStatusV1 CABBIRD_CALL EntityAt(
    void*, std::uint32_t index, CabbirdUnityEntityV1* entity) {
    if (entity == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    if (index >= state.entities.size()) {
        // NOT_FOUND rather than an empty struct: the caller's index came from
        // `entity_count`, so an out-of-range read means the set changed underneath it, and
        // saying so is more useful than handing back a zeroed entity it would box at the
        // origin.
        return {CABBIRD_STATUS_V1_NOT_FOUND, 0, {}};
    }
    const EntityRecord& record = state.entities[index];
    std::memset(entity, 0, sizeof(*entity));
    entity->struct_size = sizeof(*entity);
    entity->entity_id = record.id;
    entity->kind = record.kind;
    // LOCAL_PLAYER is still not set, and now for a NARROWER reason than before: the camp
    // value distinguishes player from monster, which is what `kind` carries, but it does not
    // identify which PLAYER is the local one (a co-op partner is also camp Player).  Setting
    // the flag from the camp alone would mark every friendly as "you".  Identity needs the
    // local player's object, which nothing here resolves yet -- so the flag stays off and a
    // plugin filtering on it boxes more than it must, which is visible and correctable.
    entity->flags = CABBIRD_UNITY_ENTITY_V1_VALID;
    // THE LABEL GOES INTO STABLE STORAGE, NOT INTO `state.entities` MEMORY.
    //
    // The SDK promises that the pointer stays valid until the next `entity_at`.  Handing out
    // `record.label` did NOT keep that promise: `record` lives in `state.entities`, and the next
    // walk does `state.entities = std::move(...)`, which frees it.  A consumer copying the label
    // a moment later -- the documented usage -- read freed memory.
    //
    // The copy below is what makes the promise true.  `index` addresses a slot that is never
    // reallocated, and the consumer reads its slot immediately after this call, so no other
    // writer can be between them.
    // The address the plugin uses to explore on its own.  Guarded by `struct_size` on the
    // consuming side, so a plugin built against the older struct simply never reads it.
    entity->entity_data = static_cast<std::uint64_t>(record.data);
    entity->label = nullptr;
    entity->label_size = 0;
    if (record.label_size != 0 && index < ServiceState::kPublishedLabelCapacity) {
        const std::size_t bytes =
            record.label_size < ServiceState::kPublishedLabelBytes
                ? static_cast<std::size_t>(record.label_size)
                : ServiceState::kPublishedLabelBytes - 1;
        std::memcpy(state.published_label[index], record.label, bytes);
        state.published_label[index][bytes] = '\0';
        entity->label = BorrowedServiceText(24, std::string_view(record.label, bytes));
        entity->label_size = static_cast<std::uint32_t>(bytes);
    }
    for (int axis = 0; axis < 3; ++axis) {
        entity->bounds_center[axis] = record.center[axis];
        entity->bounds_extent[axis] = kApproxHalfExtent[axis];
    }
    // Left at 0 = "unknown".  Filling it needs the camera, which this service does not
    // have; the camera is its own contract entry (`camera`) for exactly that reason.
    entity->distance_meters = 0.0;
    // The corners the GAME projected, handed over instead of a matrix.
    //
    // This is the contract change that ends the projection problem.  A plugin draws a box by
    // projecting eight corners, which it can only do if it has a projection -- and on the
    // render thread it cannot ask the game for one, because the game's projection is only
    // callable from the game thread.  Publishing the corners themselves moves that call to the
    // thread that owns it and leaves the render side with arithmetic it cannot get wrong.
    //
    // `distance_meters` and `bounds_*` are unchanged, so a consumer that still wants to project
    // for itself is no worse off; `screen_points` is an addition, not a replacement.
    entity->screen_point_mask = record.screen_mask;
    for (std::uint32_t corner = 0; corner < 8; ++corner) {
        for (int axis = 0; axis < 2; ++axis) {
            entity->screen_points[corner][axis] = record.screen_points[corner][axis];
        }
    }
    if (record.screen_mask != 0) {
        entity->flags |= CABBIRD_UNITY_ENTITY_V1_SCREEN_POINTS;
    }
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}

CabbirdStatusV1 CABBIRD_CALL EntityCamera(void*, CabbirdEspCameraV1* camera) noexcept {
    if (camera == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    std::memset(camera, 0, sizeof(*camera));
    camera->struct_size = sizeof(*camera);
    // THE VIEWPOINT, for the plugin's distance filter.  The host already reads the camera's
    // world position every tick (for the transposed-matrix check and for the diagnostic line),
    // so publishing it costs nothing and is the difference between a distance filter that
    // works and one that silently compares against the world origin.
    auto& state = State();
    double position[3]{};
    bool have_position = false;
    {
        std::scoped_lock lock(state.mutex);
        if (state.camera.resolved && state.camera.valid) {
            position[0] = state.camera.camera_pos[0];
            position[1] = state.camera.camera_pos[1];
            position[2] = state.camera.camera_pos[2];
            // A camera that reported exactly (0,0,0) is indistinguishable from one whose
            // position was never read, and publishing that as a viewpoint would measure every
            // distance from the world origin -- a wrong answer that looks like a working
            // filter.  Treated as absent.
            have_position = position[0] != 0.0 || position[1] != 0.0 || position[2] != 0.0;
        }
    }
    if (!have_position) {
        // UNAVAILABLE, not a zeroed camera.  A zeroed camera is a camera at the world origin
        // looking down -Z, which is a valid-looking view that would put every box in a
        // plausible wrong place -- the same mistake `frame->project` returning 0 avoids on the
        // overlay side.
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    camera->position[0] = position[0];
    camera->position[1] = position[1];
    camera->position[2] = position[2];
    // view-projection MATRIX rather than an orientation, so there are no Euler angles to
    // publish, and deriving them from the matrix would be a second, unchecked derivation of
    // exactly the kind that cost this project a session.  The flag claims the position only.
    camera->rotation[0] = 0.0;
    camera->rotation[1] = 0.0;
    camera->rotation[2] = 0.0;
    camera->horizontal_fov_degrees = 0.0f;
    camera->flags = 1u;
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}


// The plugin declares whether it wants names.  Host-side state only -- no IL2CPP, no
// allocation -- so it is safe to call from the game domain at any time.
//
// WHY THIS EXISTS AT ALL: the host used to fill labels unconditionally, which put three
// managed calls per entity (two of them boxing value types) into every refresh for a feature
// that might be switched off.  Measured on the live game, the walk holding them cost 106 ms of
// a 108 ms frame.  A feature must not be able to slow the game down in order to be available.
CabbirdStatusV1 CABBIRD_CALL EntitySetWantLabels(void*, int want) noexcept {
    // Turning labels off also FORGETS the cache.  Otherwise switching the feature back on
    // serves stale names for entities whose data object address was reused by a different
    // entity in the meantime -- a wrong name on the right box, which is worse than no name.
    ComponentState().label_reads_enabled = want != 0 ? 1 : 0;
    if (ComponentState().label_reads_enabled == 0) {
        std::memset(ComponentState().label_cache, 0, sizeof(ComponentState().label_cache));
    }
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}

// --- ONE LOOKUP BY ID, WHICH THE ENTITY SERVICE PUBLISHES --------------------
//
// `LookupUnityEntityById` above answers in the internal `UnityEntityLookup` shape; this converts
// it into the SDK's `CabbirdUnityEntitiesLookupV1`.  The conversion is field-by-field rather
// than a `memcpy` because the two deliberately do NOT agree on layout: the internal struct leads
// with `found` and `entity_id`, the SDK one leads with `struct_size` and `kind`, and a
// `memcpy` between them would compile, run, and hand a consumer a `kind` read out of the wrong
// word.
CabbirdStatusV1 CABBIRD_CALL EntityLookup(
    void*, std::uint64_t entity_id, CabbirdUnityEntitiesLookupV1* out) noexcept {
    if (out == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (out->struct_size < sizeof(CabbirdUnityEntitiesLookupV1)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    const UnityEntityLookup found = LookupUnityEntityById(entity_id);
    if (!found.found) {
        std::memset(out, 0, sizeof(*out));
        out->struct_size = sizeof(*out);
        return {CABBIRD_STATUS_V1_NOT_FOUND, 0, {}};
    }
    out->kind = found.kind;
    out->entity_id = found.entity_id;
    out->generation = found.generation;
    out->data = found.data;
    std::snprintf(out->data_class, sizeof(out->data_class), "%s", found.data_class);
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}

// The ACTIVE CAMERA'S WORLD POSITION, for the player service's identity decision.
//
// Read out of the cache the walk already filled.  It is a plain copy under the mutex and touches
// no IL2CPP, so it is safe from any domain -- which matters, because the player service asks for
// it on the game thread every tick.
//
// Returns UNAVAILABLE rather than the origin when no camera has been read: "the origin" is a
// place, and the identity heuristic picking the entity nearest it would silently choose whatever
// stands at (0,0,0).
CabbirdStatusV1 CABBIRD_CALL EntitiesCameraPosition(void*, double position[3]) noexcept {
    if (position == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    if (!state.camera.view_projection_valid) {
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    position[0] = state.camera.camera_pos[0];
    position[1] = state.camera.camera_pos[1];
    position[2] = state.camera.camera_pos[2];
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}

// The OVERLAY-FACING table.  It no longer enumerates anything: the enumeration is
// `g_entities_service` below, and this is the camera plus the label switch.

// The ENTITY table, published as its own id.  It shares this translation unit with the overlay
// service on purpose: both read the same `state.entities`, so a separate translation unit would
// need a second accessor for the same data and would let the two disagree about what is alive.
// The split is about which QUESTIONS a consumer may ask, not about moving bytes between files.

} }  // namespace (adapter internals)



}  // namespace cabbird

#undef g_entity_refresh_tick
#undef g_entity_requested_until
#undef g_player_refresh_requested


/* ---- UnityAdapter component: il2cpp ---- */


namespace cabbird {
namespace { namespace unity_adapter_il2cpp_detail {
struct DiagnosticJob {
    std::mutex mutex;
    std::condition_variable cv;
    std::string ns, name, field, result;
    std::size_t limit{};
    bool done{};
    std::atomic<bool> cancelled{};
};

struct Il2CppComponentState {
    std::atomic<DWORD> game_thread{};
    std::atomic<bool> published{};
    std::mutex diag_mutex;
    std::shared_ptr<DiagnosticJob> diag_job;
};

Il2CppComponentState& ComponentState() {
    return UnityAdapterServices::Current()->Component<Il2CppComponentState>(
        UnityAdapterComponent::Il2Cpp);
}

#define g_game_thread (ComponentState().game_thread)
#define g_published (ComponentState().published)
#define g_diag_mutex (ComponentState().diag_mutex)
#define g_diag_job (ComponentState().diag_job)
CabbirdStatusV1 Status(uint32_t code, const char* text = "") {
    return {code, 0, {text, std::strlen(text)}};
}
bool ExportApi(CabbirdIl2CppApiV1& api) {
    const auto& native = il2cpp::Functions();
    api.struct_size = sizeof(api);
    api.api_version = 1;
    api.il2cpp_domain_get = native.il2cpp_domain_get;
    if (!api.il2cpp_domain_get) return false;
    api.il2cpp_domain_get_assemblies = native.il2cpp_domain_get_assemblies;
    if (!api.il2cpp_domain_get_assemblies) return false;
    api.il2cpp_assembly_get_image = native.il2cpp_assembly_get_image;
    if (!api.il2cpp_assembly_get_image) return false;
    api.il2cpp_image_get_name = native.il2cpp_image_get_name;
    if (!api.il2cpp_image_get_name) return false;
    api.il2cpp_class_from_name = native.il2cpp_class_from_name;
    if (!api.il2cpp_class_from_name) return false;
    api.il2cpp_class_get_parent = native.il2cpp_class_get_parent;
    if (!api.il2cpp_class_get_parent) return false;
    api.il2cpp_class_get_type = native.il2cpp_class_get_type;
    if (!api.il2cpp_class_get_type) return false;
    api.il2cpp_class_get_field_from_name = native.il2cpp_class_get_field_from_name;
    if (!api.il2cpp_class_get_field_from_name) return false;
    api.il2cpp_class_get_method_from_name = native.il2cpp_class_get_method_from_name;
    if (!api.il2cpp_class_get_method_from_name) return false;
    api.il2cpp_class_is_assignable_from = native.il2cpp_class_is_assignable_from;
    if (!api.il2cpp_class_is_assignable_from) return false;
    api.il2cpp_field_get_type = native.il2cpp_field_get_type;
    if (!api.il2cpp_field_get_type) return false;
    api.il2cpp_field_get_value = native.il2cpp_field_get_value;
    if (!api.il2cpp_field_get_value) return false;
    api.il2cpp_method_get_param = native.il2cpp_method_get_param;
    if (!api.il2cpp_method_get_param) return false;
    api.il2cpp_type_get_name = native.il2cpp_type_get_name;
    if (!api.il2cpp_type_get_name) return false;
    api.il2cpp_type_get_object = native.il2cpp_type_get_object;
    if (!api.il2cpp_type_get_object) return false;
    api.il2cpp_object_get_class = native.il2cpp_object_get_class;
    if (!api.il2cpp_object_get_class) return false;
    api.il2cpp_runtime_invoke = native.il2cpp_runtime_invoke;
    if (!api.il2cpp_runtime_invoke) return false;
    api.il2cpp_object_unbox = native.il2cpp_object_unbox;
    if (!api.il2cpp_object_unbox) return false;
    api.il2cpp_string_new = native.il2cpp_string_new;
    if (!api.il2cpp_string_new) return false;
    api.il2cpp_string_length = native.il2cpp_string_length;
    if (!api.il2cpp_string_length) return false;
    api.il2cpp_string_chars = native.il2cpp_string_chars;
    if (!api.il2cpp_string_chars) return false;
    api.il2cpp_array_length = native.il2cpp_array_length;
    if (!api.il2cpp_array_length) return false;
    api.il2cpp_array_object_header_size = native.il2cpp_array_object_header_size;
    if (!api.il2cpp_array_object_header_size) return false;
    api.il2cpp_gchandle_new = native.il2cpp_gchandle_new;
    if (!api.il2cpp_gchandle_new) return false;
    api.il2cpp_gchandle_get_target = native.il2cpp_gchandle_get_target;
    if (!api.il2cpp_gchandle_get_target) return false;
    api.il2cpp_gchandle_free = native.il2cpp_gchandle_free;
    if (!api.il2cpp_gchandle_free) return false;
    api.il2cpp_free = native.il2cpp_free;
    if (!api.il2cpp_free) return false;
    api.il2cpp_class_get_methods = native.il2cpp_class_get_methods;
    api.il2cpp_method_get_name = native.il2cpp_method_get_name;
    api.il2cpp_method_get_param_count = native.il2cpp_method_get_param_count;
    if (!api.il2cpp_class_get_methods || !api.il2cpp_method_get_name ||
        !api.il2cpp_method_get_param_count) return false;
    return true;
}
CabbirdStatusV1 CABBIRD_CALL WithRuntime(void*, CabbirdIl2CppCallbackV1 callback, void* user) {
    if (!callback) return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "null runtime callback");
    if (!g_published.load()) return Status(CABBIRD_STATUS_V1_UNAVAILABLE, "IL2CPP service revoked");
    const auto game_thread = g_game_thread.load();
    if (!game_thread || GetCurrentThreadId() != game_thread)
        return Status(CABBIRD_STATUS_V1_PERMISSION_DENIED, "IL2CPP invocation requires the game thread (on_update)");
    try {
        if (!il2cpp::Ready() && !il2cpp::Initialize())
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE, "IL2CPP runtime not loaded");
        il2cpp::ThreadScope thread;
        if (!thread.attached()) return Status(CABBIRD_STATUS_V1_UNAVAILABLE, "IL2CPP attach failed");
        CabbirdIl2CppApiV1 api{};
        if (!ExportApi(api)) return Status(CABBIRD_STATUS_V1_UNAVAILABLE, "required IL2CPP export missing");
        return callback(user, &api);
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED, "exception in IL2CPP callback");
    }
}
CabbirdStatusV1 CABBIRD_CALL ReleaseHandles(void*, const CabbirdIl2CppGCHandleV1* handles, size_t count) {
    if ((!handles && count) || count > 4096) return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    if (!count) return Status(CABBIRD_STATUS_V1_OK);
    if (!il2cpp::Ready()) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
    // GC handles are not Unity objects. Releasing them is allowed on an attached
    // lifecycle worker, unlike calling Unity UI setters during on_stop.
    try {
        il2cpp::ThreadScope thread;
        if (!thread.attached()) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        const auto release = il2cpp::Functions().il2cpp_gchandle_free;
        if (!release) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        for (size_t i = 0; i < count; ++i) if (handles[i]) release(handles[i]);
        return Status(CABBIRD_STATUS_V1_OK);
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
} }
static void InitializeUnityIl2CppState() {
    using namespace unity_adapter_il2cpp_detail;
    g_game_thread.store(0);
    g_published.store(true);
}
static void CancelUnityIl2CppDiagnostics() noexcept {
    using namespace unity_adapter_il2cpp_detail;
    g_published.store(false);
    std::scoped_lock lock(g_diag_mutex);
    if (g_diag_job) {
        auto job = std::move(g_diag_job);
        std::scoped_lock job_lock(job->mutex);
        job->cancelled.store(true);
        job->result = R"({"ok":false,"error":"IL2CPP service stopped"})";
        job->done = true;
        job->cv.notify_all();
    }
    g_game_thread.store(0);
}
void ObserveUnityIl2CppGameThread() {
    using namespace unity_adapter_il2cpp_detail;
    // Registered by the composition root, executed before the plugin update loop.
    // No runtime initialization or managed work while there are no consumers.
    g_game_thread.store(GetCurrentThreadId());
}


namespace { namespace unity_adapter_il2cpp_detail {
std::string DiagnosticError(const char* stage, const char* error) {
    return "{\"ok\":false,\"stage\":" + json::Quote(stage) +
        ",\"error\":" + json::Quote(error) + "}";
}
CabbirdStatusV1 CABBIRD_CALL ScanObjects(void* user, const CabbirdIl2CppApiV1* api) {
    using namespace cabbird::sdk::il2cpp;
    auto& job = *static_cast<DiagnosticJob*>(user);
    Context runtime(*api);
    auto fail = [&](const char* stage, const char* message) {
        job.result = DiagnosticError(stage, message);
        return Status(CABBIRD_STATUS_V1_OK);
    };
    const auto& native = il2cpp::Functions();
    if (!native.il2cpp_class_from_type || !native.il2cpp_field_get_flags)
        return fail("exports", "field metadata APIs unavailable");
    Il2CppClass* klass = nullptr;
    std::string image_name;
    std::size_t assembly_count{};
    auto* domain = api->il2cpp_domain_get();
    if (!domain) return fail("domain", "domain unavailable");
    auto** assemblies = api->il2cpp_domain_get_assemblies(domain, &assembly_count);
    if (!assemblies || assembly_count > 4096) return fail("images", "invalid assembly list");
    for (std::size_t i = 0; i < assembly_count; ++i) {
        auto* image = api->il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        auto* match = api->il2cpp_class_from_name(image, job.ns.c_str(), job.name.c_str());
        if (!match) continue;
        if (klass && klass != match) return fail("class", "ambiguous class across images");
        klass = match;
        const char* name = api->il2cpp_image_get_name(image);
        image_name = name ? name : "";
    }
    if (!klass) return fail("class", "class not found in loaded images");
    auto* object = runtime.FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Object");
    if (!object || !api->il2cpp_class_is_assignable_from(object, klass))
        return fail("class", "only UnityEngine.Object subclasses can be enumerated; not a GC heap walk");
    auto* resources = runtime.FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Resources");
    auto* find = resources ? runtime.FindMethodBySignature(resources, "FindObjectsOfTypeAll", {"System.Type"}) : nullptr;
    if (!find) return fail("enumerator", "Resources.FindObjectsOfTypeAll(System.Type) unavailable");
    auto* tmp = runtime.FindClass("Unity.TextMeshPro.dll", "TMPro", "TMP_Text");
    auto* string_class = runtime.FindClass("mscorlib.dll", "System", "String");
    FieldInfo* field = nullptr;
    bool string_field = false;
    if (!job.field.empty()) {
        field = runtime.FindField(klass, job.field.c_str());
        if (!field) return fail("field", "field not found");
        if (native.il2cpp_field_get_flags(field) & 0x10)
            return fail("field", "static fields are not supported");
        auto* field_class = native.il2cpp_class_from_type(api->il2cpp_field_get_type(field));
        string_field = field_class && field_class == string_class;
        if (!string_field && (!field_class || !tmp || !api->il2cpp_class_is_assignable_from(tmp, field_class)))
            return fail("field", "only System.String or TMP_Text instance fields are supported");
    }
    auto* type = api->il2cpp_type_get_object(api->il2cpp_class_get_type(klass));
    if (!type) return fail("type", "System.Type unavailable");
    void* args[]{type};
    Il2CppObject* array = nullptr;
    if (!runtime.Invoke(find, nullptr, args, &array) || !array)
        return fail("enumerator", "managed invocation failed");
    auto handle = api->il2cpp_gchandle_new(array, false);
    if (!handle) return fail("gc", "cannot root discovery array");
    struct Root { const CabbirdIl2CppApiV1* api; Il2CppGCHandle handle;
        ~Root() { api->il2cpp_gchandle_free(handle); } } root{api, handle};
    const auto count = api->il2cpp_array_length(array);
    const auto bound = (std::min)(static_cast<std::size_t>(count), job.limit);
    std::string out = "{\"ok\":true,\"scope\":\"loaded UnityEngine.Object instances including inactive/assets\",\"image\":" +
        json::Quote(image_name) + ",\"class\":" + json::Quote(job.ns + "." + job.name) +
        ",\"objects\":" + std::to_string(count) + ",\"truncated\":" + (count > bound ? "true" : "false") +
        ",\"field\":" + json::Quote(job.field) + ",\"values\":[";
    for (std::size_t i = 0; i < bound; ++i) {
        if (job.cancelled.load()) return fail("cancelled", "request expired");
        if (i) out += ',';
        Il2CppObject* instance = nullptr;
        array = api->il2cpp_gchandle_get_target(handle);
        if (!runtime.ArrayReferenceAt(array, static_cast<std::uint32_t>(i), &instance) || !instance) {
            out += "{\"error\":\"unreadable array element\"}"; continue;
        }
        out += "{\"address\":" + json::Hex(reinterpret_cast<std::uintptr_t>(instance));
        Il2CppObject* value = instance;
        if (field) { value = nullptr; api->il2cpp_field_get_value(instance, field, &value); }
        out += ",\"value_address\":" + json::Hex(reinterpret_cast<std::uintptr_t>(value));
        if (!value) out += ",\"text\":null";
        else {
            Il2CppObject* text = nullptr;
            const bool is_tmp = tmp && api->il2cpp_class_is_assignable_from(tmp, api->il2cpp_object_get_class(value));
            if (string_field) text = value;
            else if (is_tmp) {
                auto* get = runtime.FindMethod(api->il2cpp_object_get_class(value), "get_text", 0);
                if (!get || !runtime.Invoke(get, value, nullptr, &text))
                    out += ",\"error\":\"TMP get_text failed\"";
            }
            if (text) {
                auto decoded = runtime.ReadString(text, 2048);
                out += decoded ? ",\"text\":" + json::Quote(*decoded) : ",\"error\":\"string unreadable or exceeds 2048 UTF16 units\"";
            }
        }
        out += '}';
    }
    job.result = out + "]}";
    return Status(CABBIRD_STATUS_V1_OK);
}
} }
std::string RequestUnityIl2CppObjectScan(std::string_view ns, std::string_view name,
                                      std::string_view field, std::size_t limit) {
    ServiceCall adapter_call;
    if (!adapter_call) return R"({"ok":false,"stage":"service","error":"Unity adapter unavailable"})";
    using namespace unity_adapter_il2cpp_detail;
    if (name.empty() || ns.size() > 256 || name.size() > 256 || field.size() > 256 || !limit || limit > 256)
        return DiagnosticError("request", "invalid arguments");
    if (GetCurrentThreadId() == g_game_thread.load())
        return DiagnosticError("request", "cannot wait on game thread");
    auto job = std::make_shared<DiagnosticJob>();
    job->ns = ns; job->name = name; job->field = field; job->limit = limit;
    {
        std::scoped_lock lock(g_diag_mutex);
        if (!g_published.load()) return DiagnosticError("service", "IL2CPP service unavailable");
        if (g_diag_job) return DiagnosticError("request", "object scan busy");
        g_diag_job = job;
    }
    std::unique_lock lock(job->mutex);
    if (!job->cv.wait_for(lock, std::chrono::seconds(10), [&] { return job->done; })) {
        job->cancelled.store(true);
        return DiagnosticError("timeout", "game diagnostic did not finish within 10 seconds");
    }
    return job->result;
}
void ProcessUnityIl2CppDiagnostics() {
    using namespace unity_adapter_il2cpp_detail;
    std::shared_ptr<DiagnosticJob> job;
    {
        std::scoped_lock lock(g_diag_mutex);
        job = g_diag_job;
    }
    if (!job) return;
    // Local output isolates timeout/shutdown from a running managed invocation.
    DiagnosticJob work;
    work.ns = job->ns; work.name = job->name; work.field = job->field; work.limit = job->limit;
    if (!job->cancelled.load()) {
        const auto status = WithRuntime(nullptr, ScanObjects, &work);
        if (status.code != CABBIRD_STATUS_V1_OK)
            work.result = DiagnosticError("runtime", "runtime acquisition or callback failed");
    } else work.result = DiagnosticError("cancelled", "request expired before game tick");
    {
        std::scoped_lock lock(job->mutex);
        if (!job->done) { job->result = std::move(work.result); job->done = true; }
    }
    job->cv.notify_all();
    {
        std::scoped_lock lock(g_diag_mutex);
        if (g_diag_job == job) g_diag_job.reset();
    }
}
}

#undef g_diag_job
#undef g_diag_mutex
#undef g_game_thread
#undef g_published


/* ---- UnityAdapter component: overlay ---- */

// The host backend for `cabbird.unity.overlay` -- the screen-space surface a plugin draws
// on, so that ESP-style drawing does not have to go through the game's UGUI.
//
// WHY THE NAME MATTERS
// --------------------
// This surface is not the game's: Cabbird owns it.  It was ported from Anomaly, which targets
// UE5, and arrived named after Unreal's own HUD class; a reader who knows that engine takes
// such a name as a claim about what is being hooked, and the claim was false -- nothing of the
// game's is hooked here.  `overlay` is what it is.
//
// THE SUBSCRIPTION MODEL
// ----------------------
// The plugin SUBSCRIBES once; the host calls it back once per render pass with a frame
// that carries the projection and the draw primitives.  That is deliberately the same
// shape Anomaly uses, because it is the shape that keeps the per-frame cost proportional
// to what the plugin actually draws -- there is no per-entity hop through the ABI and no
// per-entity allocation.
//
// Drawing goes to ImGui's BACKGROUND draw list, which is exactly right for an overlay:
// it renders underneath every host window, so an ESP box never covers the plugin's own
// settings UI, and it needs no host window to be open.
//
// WHO MAY DRAW, AND WHY THE PLUGIN LAYER STILL VALIDATES IT
// ---------------------------------------------------------
// This file accepts any well-formed subscription.  The plugin-facing entry point
// (`SubscribeOverlayV1` in plugin_manager.cpp) is what ties a subscription to a plugin's
// SCOPE and GENERATION, so that a plugin which is stopped, unloaded or quarantined cannot
// keep drawing through a handle it no longer owns.  The split is deliberate: this layer
// answers "can something draw", the plugin layer answers "is this caller still alive".
//
// THE CAMERA IS A SEPARATE, UNFINISHED LAYER -- AND IT REPORTS THAT HONESTLY
// ------------------------------------------------------------------------
// `project` needs a view-projection matrix, which needs the game's active camera, which
// needs either an IL2CPP binding or the camera's native struct.  Neither exists yet, so
// `SetOverlayCameraSource` has no producer in production.
//
// When there is no camera, `frame->project` RETURNS 0 and draws nothing instead of
// guessing a matrix.  That is the whole point: a made-up projection would put boxes in
// plausible wrong places, which is far worse than no boxes -- the ESP plugin already
// treats a failed projection as "skip this entity".  The camera source seam exists so the
// projection can be
// TESTED offline today and filled in from a profile binding tomorrow.


// Overlay state remains adapter-owned. The render backend supplies font selection
// for each frame, so this component does not depend on the host UI theme.

namespace cabbird {
namespace { namespace unity_adapter_overlay_detail {

using ProjectFn = bool (*)(
    void* user, const double world[3], double view_projection[16], double* depth);

struct Subscription {
    std::uint64_t id{};
    CabbirdUnityOverlayDrawCallbackV1 callback{};
    void* callback_user{};
};

struct OverlayState {
    std::mutex mutex;
    std::vector<Subscription> subscriptions;
    std::uint64_t next_id{1};
    std::uint64_t frame_count{};
    std::uint64_t draw_calls{};
    std::uint64_t subscriptions_total{};
    std::uint64_t projection_failures{};
    ProjectFn project{};
    void* project_user{};
};

OverlayState& State() {
    return UnityAdapterServices::Current()->Component<OverlayState>(
        UnityAdapterComponent::Overlay);
}

// The projection seam.  Two things use it: a profile binding in production (not built
// yet) and the offline test, which supplies a matrix so the drawing half can be verified
// without a game.
bool ProjectPoint(OverlayState& state, const double world[3], double out[16], double* depth) {
    ProjectFn project{};
    void* user{};
    {
        std::scoped_lock lock(state.mutex);
        project = state.project;
        user = state.project_user;
    }
    if (project == nullptr) return false;
    return project(user, world, out, depth);
}

// ImGui colours are ABGR; the SDK says RGBA because that is what a plugin author expects
// to write.  Converting in one place means a plugin never has to know which order this
// host happens to store its pixels in -- a plugin that got it wrong would produce
// plausible-looking but wrong colours, which is exactly the class of bug this contract
// exists to prevent.
ImU32 ToImGuiColour(std::uint32_t rgba) {
    const std::uint32_t r = (rgba >> 24u) & 0xFFu;
    const std::uint32_t g = (rgba >> 16u) & 0xFFu;
    const std::uint32_t b = (rgba >> 8u) & 0xFFu;
    const std::uint32_t a = rgba & 0xFFu;
    return IM_COL32(r, g, b, a);
}

struct FrameContext {
    OverlayState* state{};
    ImDrawList* draw_list{};
    OverlayFontSelector select_font{};
};

int CABBIRD_CALL FrameProject(
    void* user, const double world[3], float screen[2], double* depth) noexcept {
    auto* const context = static_cast<FrameContext*>(user);
    if (context == nullptr || context->state == nullptr || world == nullptr ||
        screen == nullptr) {
        return 0;
    }
    // `project` returns a full clip-space row-major matrix rather than a point, because
    // the ESP path needs to project EIGHT AABB corners and keep them consistent; a
    // per-point transform would make that eight separate host calls for one entity.
    double matrix[16]{};
    double point_depth{};
    if (!ProjectPoint(*context->state, world, matrix, &point_depth)) {
        ++context->state->projection_failures;
        return 0;
    }
    // Row-major, world point as a column vector: clip = M * (x,y,z,1).
    const double x = matrix[0] * world[0] + matrix[1] * world[1] + matrix[2] * world[2] + matrix[3];
    const double y = matrix[4] * world[0] + matrix[5] * world[1] + matrix[6] * world[2] + matrix[7];
    const double w = matrix[12] * world[0] + matrix[13] * world[1] + matrix[14] * world[2] + matrix[15];
    if (depth != nullptr) *depth = point_depth;
    // w <= 0 means the point is at or behind the eye plane.  Returns 0 rather than a
    // mirrored coordinate: dividing by a negative w flips the sign and produces the
    // classic "one box spans the whole screen" artifact.  A plugin is REQUIRED to treat
    // 0 as "do not draw".
    if (!(w > 0.0)) return 0;
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    const ImVec2 size = ImGui::GetMainViewport()->Size;
    screen[0] = static_cast<float>(origin.x + (x / w * 0.5 + 0.5) * size.x);
    screen[1] = static_cast<float>(origin.y + (0.5 - y / w * 0.5) * size.y);
    return 1;
}

// THE PIXEL HEIGHT THIS TEXT WILL BE DRAWN AT, AND THE BAKE THAT CAN COVER IT.
//
// Both halves of the overlay's text API go through here so that measuring and drawing can never
// disagree.  They used to: `FrameMeasureText` measured the CURRENT font at its own size and
// multiplied by `scale`, while `FrameDrawText` asked ImDrawList for `GetFontSize() * scale`.
// Those agree numerically, but neither picked the bake that could render it, so every label
// larger than the host's 13px default was a MAGNIFIED BITMAP.  At 13px a Chinese ideograph's
// strokes are one or two pixels wide, so magnification reads as blur rather than as size.
//
// `CabbirdUiFontForPixelSize` returns the smallest bake at least as large as the request, so the
// draw ratio is <= 1.0 (a downscale) for every size the label slider can produce.
struct OverlayTextLayout {
    ImFont* font{};
    float size{};
    bool valid{};
};

OverlayTextLayout LayoutOverlayText(const FrameContext& context, const float scale) noexcept {
    OverlayTextLayout layout;
    const float requested = ImGui::GetFontSize() * (scale > 0.0f ? scale : 1.0f);
    if (!std::isfinite(requested) || requested <= 0.0f) {
        return layout;
    }
    layout.font = context.select_font ? context.select_font(requested) : nullptr;
    if (layout.font == nullptr) {
        // The atlas was not the host's, so fall back to exactly the previous behaviour
        // rather than to no text at all.
        layout.font = ImGui::GetFont();
    }
    layout.size = requested;
    layout.valid = layout.font != nullptr;
    return layout;
}

int CABBIRD_CALL FrameMeasureText(
    void* user, CabbirdStringViewV1 text, float scale, float* width, float* height) noexcept {
    auto* const context = static_cast<FrameContext*>(user);
    if (context == nullptr || width == nullptr || height == nullptr) return 0;
    if (text.data == nullptr || text.size == 0) {
        *width = 0.0f;
        *height = 0.0f;
        return 1;
    }
    // Measured through ImGui's own layout rather than the font's average glyph width,
    // because a proportional font's average is wrong for most strings -- and a label
    // centred with a wrong width looks broken in a way that reads as "the plugin is bad"
    // rather than "the host lied about the size".
    char buffer[512]{};
    const std::size_t count = std::min<std::size_t>(text.size, sizeof(buffer) - 1);
    std::memcpy(buffer, text.data, count);
    const OverlayTextLayout layout = LayoutOverlayText(*context, scale);
    if (!layout.valid) {
        *width = 0.0f;
        *height = 0.0f;
        return 0;
    }
    // Measured with the SAME font and the SAME size the draw call will use.  Deriving the size
    // from a different bake would be a smaller error than the magnification this replaces, but it
    // is still an error, and the whole point of this function is that centring is exact.
    const ImVec2 measured =
        layout.font->CalcTextSizeA(layout.size, FLT_MAX, 0.0f, buffer, nullptr);
    *width = measured.x;
    *height = measured.y;
    return 1;
}

int CABBIRD_CALL FrameDrawText(
    void* user, CabbirdStringViewV1 text, float x, float y, std::uint32_t rgba,
    float scale) noexcept {
    auto* const context = static_cast<FrameContext*>(user);
    if (context == nullptr || context->draw_list == nullptr) return 0;
    if (text.data == nullptr || text.size == 0) return 0;
    char buffer[512]{};
    const std::size_t count = std::min<std::size_t>(text.size, sizeof(buffer) - 1);
    std::memcpy(buffer, text.data, count);
    const OverlayTextLayout layout = LayoutOverlayText(*context, scale);
    if (!layout.valid) return 0;
    context->draw_list->AddText(layout.font, layout.size, ImVec2(x, y), ToImGuiColour(rgba),
                                buffer);
    ++context->state->draw_calls;
    return 1;
}

int CABBIRD_CALL FrameDrawLine(
    void* user, float x0, float y0, float x1, float y1, std::uint32_t rgba,
    float thickness) noexcept {
    auto* const context = static_cast<FrameContext*>(user);
    if (context == nullptr || context->draw_list == nullptr) return 0;
    context->draw_list->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), ToImGuiColour(rgba),
                                thickness > 0.0f ? thickness : 1.0f);
    ++context->state->draw_calls;
    return 1;
}

int CABBIRD_CALL FrameDrawRect(
    void* user, float x, float y, float width, float height, std::uint32_t rgba) noexcept {
    auto* const context = static_cast<FrameContext*>(user);
    if (context == nullptr || context->draw_list == nullptr) return 0;
    context->draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height),
                                      ToImGuiColour(rgba));
    ++context->state->draw_calls;
    return 1;
}

CabbirdStatusV1 CABBIRD_CALL OverlaySubscribe(
    void*, CabbirdUnityOverlayDrawCallbackV1 callback, void* callback_user,
    CabbirdGenerationHandleV1* handle) {
    if (callback == nullptr || handle == nullptr) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *handle = {};
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    const std::uint64_t id = state.next_id++;
    // A zero id would look like "no subscription" to the plugin layer, which checks for
    // exactly that (`registration->service_handle.id == 0`), so it is skipped rather than
    // handed out.
    if (id == 0) {
        return {CABBIRD_STATUS_V1_FAILED, 0, {}};
    }
    state.subscriptions.push_back(Subscription{id, callback, callback_user});
    ++state.subscriptions_total;
    handle->id = id;
    handle->generation = UnityAdapterServices::Current()->Endpoint()->generation;
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}

CabbirdStatusV1 CABBIRD_CALL OverlayUnsubscribe(
    void*, CabbirdGenerationHandleV1 handle) noexcept {
    if (handle.id == 0) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (handle.generation != UnityAdapterServices::Current()->Endpoint()->generation) {
        return {CABBIRD_STATUS_V1_NOT_FOUND, 0, {}};
    }
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    const auto found = std::find_if(
        state.subscriptions.begin(), state.subscriptions.end(),
        [handle](const Subscription& entry) { return entry.id == handle.id; });
    if (found == state.subscriptions.end()) {
        // NOT_FOUND rather than OK: the plugin layer records this code, and "you asked me
        // to remove something I do not have" is a fact worth keeping rather than hiding
        // behind a success.
        return {CABBIRD_STATUS_V1_NOT_FOUND, 0, {}};
    }
    state.subscriptions.erase(found);
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}


} }  // namespace (adapter internals)


void SetOverlayCameraSource(
    bool (*project)(void* user, const double world[3], double view_projection[16],
                    double* depth),
    void* user) {
    ServiceCall adapter_call;
    if (!adapter_call) return;
    using namespace unity_adapter_overlay_detail;
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    state.project = project;
    state.project_user = user;
}

std::uint64_t HostOverlayFrameCount() {
    ServiceCall adapter_call;
    if (!adapter_call) return {};
    using namespace unity_adapter_overlay_detail;
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    return state.frame_count;
}

HostOverlayStats SnapshotHostOverlay() {
    ServiceCall adapter_call;
    if (!adapter_call) return {};
    using namespace unity_adapter_overlay_detail;
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    HostOverlayStats snapshot{};
    snapshot.subscribers = state.subscriptions.size();
    snapshot.frames = state.frame_count;
    snapshot.draw_calls = state.draw_calls;
    snapshot.subscriptions_total = state.subscriptions_total;
    snapshot.projection_failures = state.projection_failures;
    snapshot.camera_source_present = state.project != nullptr;
    return snapshot;
}

void RunHostOverlayFrame(void* imgui_context, OverlayFontSelector select_font) {
    ServiceCall adapter_call;
    if (!adapter_call) return;
    using namespace unity_adapter_overlay_detail;
    auto& state = State();
    std::vector<Subscription> subscribers;
    {
        std::scoped_lock lock(state.mutex);
        subscribers = state.subscriptions;
        ++state.frame_count;
    }
    if (subscribers.empty()) return;
    if (imgui_context == nullptr) return;

    ImDrawList* const draw_list = ImGui::GetBackgroundDrawList();
    if (draw_list == nullptr) return;

    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    FrameContext context{&state, draw_list, select_font};

    CabbirdUnityOverlayFrameV1 frame{};
    frame.struct_size = sizeof(frame);
    frame.flags = CABBIRD_UNITY_OVERLAY_FRAME_V1_NONE;
    frame.user = &context;
    frame.viewport_width = static_cast<std::uint32_t>(viewport->Size.x);
    frame.viewport_height = static_cast<std::uint32_t>(viewport->Size.y);
    frame.project = &FrameProject;
    frame.measure_text = &FrameMeasureText;
    frame.draw_text = &FrameDrawText;
    frame.draw_line = &FrameDrawLine;
    frame.draw_rect = &FrameDrawRect;

    // A COPY of the subscriber list, taken above, so a callback that unsubscribes itself
    // (or is revoked by the plugin layer while it runs) cannot invalidate the iteration.
    // Safe because each entry carries only a function pointer and an opaque user value
    // that the plugin layer keeps alive for the duration of the call.
    for (const Subscription& subscription : subscribers) {
        if (subscription.callback == nullptr) continue;
        try {
            subscription.callback(subscription.callback_user, &frame);
        } catch (...) {
            // A throwing draw callback must not take the render pass with it.  The plugin
            // layer marks the offending plugin faulted on its own side; here the only
            // correct behaviour is to keep drawing for everyone else.
        }
    }
}

}  // namespace cabbird


/* ---- UnityAdapter component: player ---- */

// Host backend for `cabbird.unity.player` -- the local player's position, and moving it.
//
// WHY THIS FILE EXISTS, IN ONE PARAGRAPH
// --------------------------------------
// The sibling UE5 project (Anomaly) teleports with `ProcessEvent(actor,
// K2_SetActorLocation)`: one function pointer, reachable from anywhere in the process, and
// the game's own movement code does the rest.  Unity/IL2CPP has no such funnel, and the
// player's world position does not live in managed memory at all -- it is inside Unity's
// NATIVE `Transform`, behind `m_CachedPtr`, whose layout is not in `dump.cs` and which
// this project refuses to guess.  So "move the player" cannot be done by writing a
// field.  It has to be a CALL to `UnityEngine.Transform::set_position`, and a call needs
// `UnityEngine.Transform::set_position`, and a call needs four things that only the host
// has: the runtime's invocation entry, the `MethodInfo*`, an attached game thread, and the
// resolved offset chain that reaches the live transform.
//
// The plugins keep policy.  This file keeps the engine.
//
// WHAT THE TWO HALVES ARE
// -----------------------
//   read  : `snapshot` answers "where is the local player" from a CACHE.  Safe from the
//           RENDER domain, so a plugin can poll it while drawing.  Filled by
//           `RefreshUnityPlayer` on the game thread, which reuses the entity source's
//           verified chain instead of resolving offsets of its own.
//   write : `write_position` records a request and returns OK ("accepted and queued").  The
//           transform write happens on the NEXT GAME TICK, never inside the caller's frame.
//           Two reasons, and both are fatal if ignored: `il2cpp_runtime_invoke` from the
//           overlay's threading domain crashes the process, and blocking a render callback
//           on a game tick deadlocks the overlay.  OK means "queued", NOT "the player
//           moved" -- that is `write_state`'s answer.
//
// WHY THE WRITE IS VERIFIED TWICE, NOT ONCE
// -----------------------------------------
// `set_position` returning without throwing is NOT evidence that the player moved:
//
//   1. a `CharacterController` / `Rigidbody` / movement state machine owns the transform
//      and can put the character back on the very next frame -- "the teleport worked and
//      nothing happened" is the single most common way a Unity teleport fails;
//   2. so immediately after the call the position is re-read (did the call take at all?),
//      and a few ticks later it is read AGAIN (is it still there?).
//
// A write that is correct at step 1 and gone at step 2 is reported as REVERTED, with both
// positions, rather than as a successful teleport.  That distinction is the reason this
// service has a result struct instead of just a status code.
//
// WHAT IS DELIBERATELY NOT HERE
// -----------------------------
//   * no native Transform offsets.  The whole point of `set_position` is to avoid them;
//   * no `MethodInfo::methodPointer` direct calls.  The entity source measured the
//     reflection path at ~2.5 ms per call and this file pays it ONCE per teleport, so the
//     faster-and-unverifiable ABI route buys nothing here;
//   * no guessing about WHICH entity is the local player by class name alone.
//     `Lens.Gameplay...PlayerData` is the game's own class for the player, but the entity
//     source's `kind` bucket also covers `HeroData` and `PetData`, and a co-op partner is
//     player-shaped too.  The tie-break used is the ACTIVE CAMERA: in a third-person game
//     the camera is at the local player, and there is exactly one active camera.  The
//     guess is published (`identity_from_proximity`, `distance_to_camera`) so that a wrong
//     answer is visible and correctable instead of silent.  It is a HEURISTIC and it is
//     labelled as one.


// TWO INCLUDES, AND THE SPLIT BETWEEN THEM IS THE POINT OF THE REFACTOR.
//
// Before this consolidation, the entity component was split from the player component and
// used for BOTH jobs:
//
//   1. "which live entity is this id, and is it player-shaped" -- genuinely the entity
//      source's job, and it stays there (`LookupUnityEntityById`, `ProvideEspCameraPosition`,
//      `UnityEntityBoxBaseOffset`);
//   2. "where is that entity's `UnityEngine.Transform`, and call `set_position` on it" -- an
//      ENGINE fact that had no business being owned by the entity source at all.  The old
//      comment in the entity source said as much while justifying it: "this file is the only
//      place in the tree that has resolved the offset chain that reaches it".
//
// Job 2 is now `cabbird.unity.transform`, below.  So the two services still talk about the same
// entity, but the player service no longer obtains an engine capability THROUGH a read-only
// entity service, and the entity service no longer exports a world-state write.

namespace cabbird {
namespace { namespace unity_adapter_player_detail {
// Only an external reader keeps this adapter active; its own tick is not demand.
struct PlayerComponentState;
PlayerComponentState& ComponentState();

std::int64_t PlayerClockMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void RequestPlayerRefresh();

// --- limits -----------------------------------------------------------------
//
// A teleport target is user input from a text box, so it is checked like user input.
// These bounds are not game rules: they are the range outside which a coordinate is a
// typo or a unit error rather than a place in the world.
constexpr double kMaximumCoordinateMagnitude = 1.0e6;   // 1000 km, generously
// A written position is "there" when it is within this many metres of the target.  Unity
// stores floats, so a metre-scale error is never a rounding artefact.
constexpr double kAcceptanceToleranceMeters = 0.75;
// How far the position may drift between the immediate check and the later one before the
// write is called REVERTED.
constexpr double kDriftToleranceMeters = 0.75;
// Ticks to wait before the second read.  Not 1: the frame in which the call happens is not
// a frame in which the game's controller has run, so a one-tick check would pass every
// teleport including the ones that get undone.  Four ticks is ~66 ms at 60 Hz -- settled
// motion, still fast enough that a user watching the screen sees no lag in the report.
constexpr std::uint64_t kVerifyDelayTicks = 4;

bool ResolveAuthoritativeMainPlayer(std::uintptr_t* data, std::uintptr_t* klass,                                    std::uint64_t* entity_id, std::string* reason) {    const il2cpp::Api& api = il2cpp::Functions();    if (!data || !klass || !entity_id || !reason || api.il2cpp_class_from_name == nullptr ||        api.il2cpp_class_get_parent == nullptr ||        api.il2cpp_class_get_field_from_name == nullptr ||        api.il2cpp_class_get_method_from_name == nullptr ||        api.il2cpp_runtime_invoke == nullptr ||        api.il2cpp_domain_get == nullptr ||        api.il2cpp_domain_get_assemblies == nullptr ||        api.il2cpp_assembly_get_image == nullptr) {        if (reason) *reason = "IL2CPP APIs for EntityManager.get_ControllingEntity are unavailable";        return false;    }    *data = 0;    *klass = 0;    *entity_id = 0;    
// This game does not contain Assembly-CSharp.dll.  Its BigWorld types are    
// distributed across the loaded Azur/Lens images, so search every image    
// instead of assuming a Unity default assembly.
    const auto find_class_any_image = [&api](const char* namespaze, const char* name) {        std::size_t assembly_count = 0;        const il2cpp::Il2CppAssembly** const assemblies =            api.il2cpp_domain_get_assemblies(api.il2cpp_domain_get(), &assembly_count);        if (assemblies == nullptr) return static_cast<il2cpp::Il2CppClass*>(nullptr);        for (std::size_t index = 0; index < assembly_count; ++index) {            if (assemblies[index] == nullptr) continue;            il2cpp::Il2CppImage* const image =                api.il2cpp_assembly_get_image(assemblies[index]);            if (image == nullptr) continue;            if (il2cpp::Il2CppClass* const found =                    api.il2cpp_class_from_name(image, namespaze, name);                found != nullptr) {                return found;            }        }        return static_cast<il2cpp::Il2CppClass*>(nullptr);    };    il2cpp::Il2CppClass* const manager_class =        find_class_any_image("Lens.Gameplay.Modules.BigWorld", "EntityManager");    il2cpp::Il2CppClass* const handle_class =        find_class_any_image("Lens.Gameplay.Modules.BigWorld", "EntityHandle");    il2cpp::Il2CppClass* const entity_class =        find_class_any_image("Lens.Gameplay.Modules.BigWorld", "Entity");    il2cpp::Il2CppClass* const player_entity_class =        find_class_any_image("Lens.Gameplay.Modules.BigWorld", "PlayerEntity");    il2cpp::Il2CppClass* const hero_entity_class =        find_class_any_image("Lens.Gameplay.Modules.BigWorld", "HeroEntity");    if (!manager_class || !handle_class || !entity_class || (!player_entity_class && !hero_entity_class)) {        *reason = "EntityManager/EntityHandle/Entity/PlayerEntity/HeroEntity class was not found";        return false;    }    
// This is a static getter in the live script.json:    
// EntityManager$$get_ControllingEntity -> EntityHandle.    
// Invoking the game's own binding is demand-driven and returns the exact    
// PlayerEntity currently controlled by the local player.
    const il2cpp::MethodInfo* const get_controlling_entity =        api.il2cpp_class_get_method_from_name(manager_class, "get_ControllingEntity", 0);    if (!get_controlling_entity) {        *reason = "EntityManager.get_ControllingEntity was not found";        return false;    }    std::uintptr_t handle = 0;    il2cpp::Il2CppObject* exception = nullptr;    il2cpp::Il2CppObject* const returned = api.il2cpp_runtime_invoke(        get_controlling_entity, nullptr, nullptr, &exception);    if (exception != nullptr) {        *reason = "EntityManager.get_ControllingEntity threw an exception";        return false;    }    handle = reinterpret_cast<std::uintptr_t>(returned);    if (handle == 0) {        *reason = "EntityManager.get_ControllingEntity returned null";        return false;    }    const auto instance_offset = [&api](il2cpp::Il2CppClass* klass, const char* name,                                        std::size_t fallback) {        if (klass && api.il2cpp_class_get_field_from_name &&            api.il2cpp_field_get_offset) {            if (auto* field = api.il2cpp_class_get_field_from_name(klass, name)) {                const std::size_t value = api.il2cpp_field_get_offset(field);                if (value != static_cast<std::size_t>(-1)) return value;            }        }        return fallback;    };    const std::size_t handle_entity_offset =        instance_offset(handle_class, "entity", 0x10);    const std::size_t entity_data_offset =        instance_offset(entity_class, "data", 0x18);    const auto read_pointer = [](std::uintptr_t address, std::uintptr_t* value) {        return mem::ReadMemoryInto(address, value, sizeof(*value));    };    std::uintptr_t entity = 0;    if (!read_pointer(handle + handle_entity_offset, &entity) || entity == 0) {        *reason = "EntityManager.MainPlayer.entity is null or unreadable";        return false;    }    std::uintptr_t player_data = 0;    if (!read_pointer(entity + entity_data_offset, &player_data) || player_data == 0) {        *reason = "EntityManager.get_ControllingEntity.entity.data is null or unreadable";        return false;    }    const auto player_class_ptr = mem::Read<std::uintptr_t>(player_data);    if (!player_class_ptr || *player_class_ptr == 0) {        *reason = "EntityManager.get_ControllingEntity.data has no readable managed class";        return false;    }    const char* const name = api.il2cpp_class_get_name(        reinterpret_cast<il2cpp::Il2CppClass*>(*player_class_ptr));    const bool is_player_data =        name != nullptr &&        (std::strcmp(name, "PlayerData") == 0 ||         std::strcmp(name, "HeroData") == 0 ||         std::strcmp(name, "PetData") == 0);    if (!is_player_data) {        *reason = std::string("EntityManager.get_ControllingEntity.data is ") +                  (name ? name : "<unknown>") +                  ", not PlayerData/HeroData/PetData";        return false;    }    std::uintptr_t controlled_entity_class = 0;    if (!read_pointer(entity, &controlled_entity_class) || controlled_entity_class == 0) {        *reason = "EntityManager.get_ControllingEntity returned an unreadable entity";        return false;    }    const char* const entity_name = api.il2cpp_class_get_name(        reinterpret_cast<il2cpp::Il2CppClass*>(controlled_entity_class));    const bool is_player_entity = entity_name != nullptr && std::strcmp(entity_name, "PlayerEntity") == 0;    const bool is_hero_entity = entity_name != nullptr && std::strcmp(entity_name, "HeroEntity") == 0;    if (!is_player_entity && !is_hero_entity) {        *reason = std::string("EntityManager.get_ControllingEntity returned ") +                  (entity_name ? entity_name : "<unknown>") + ", not PlayerEntity/HeroEntity";        return false;    }    const std::size_t player_entity_id_offset =        instance_offset(is_hero_entity ? hero_entity_class : player_entity_class, "m_entityId", 0x1F8);    std::int32_t controlled_entity = 0;    if (!mem::ReadMemoryInto(entity + player_entity_id_offset, &controlled_entity,                             sizeof(controlled_entity)) ||        controlled_entity == 0) {        *reason = "controlled PlayerEntity/HeroEntity.m_entityId is null or unreadable";        return false;    }    *data = player_data;    *klass = *player_class_ptr;    *entity_id = static_cast<std::uint64_t>(        static_cast<std::uint32_t>(controlled_entity));    reason->clear();    return true;}
struct ResolvedPlayer {
    bool valid{};
    std::uint64_t generation{};
    std::uint64_t entity_id{};
    std::uintptr_t transform{};
    char data_class[32]{};
    double position[3]{};
    double distance_to_camera{};
    bool identity_from_proximity{};
};

struct ServiceState {
    std::mutex mutex;
    // Published for `snapshot` (render domain) and written by the game-domain refresh.
    //
    // THAT IS ALL THIS STATE HOLDS NOW.  The write pipeline -- the queue, the staged request,
    // the staged tick, the last result and the four counters -- is the teleport service's, in
    // its own translation unit, because it is the teleport service's work.  Keeping it here
    // "because the player is what moves" would be the same mistake one level down.
    ResolvedPlayer player;
    std::string reason{"the local player has not been resolved yet"};
};

struct PlayerComponentState : ServiceState {
    std::atomic<std::int64_t> requested_until{};
};

PlayerComponentState& ComponentState() {
    return UnityAdapterServices::Current()->Component<PlayerComponentState>(
        UnityAdapterComponent::Player);
}

void RequestPlayerRefresh() {
    ComponentState().requested_until.store(PlayerClockMillis() + 250, std::memory_order_relaxed);
}

ServiceState& State() {
    return ComponentState();
}

void RequestPlayerBindingIfNeeded() {
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    if (!state.player.valid) RequestUnityPlayerEntityRefresh();
}

double Distance(const double a[3], const double b[3]) {
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// A sanity bound on a position THIS SERVICE READ OUT OF THE GAME.
//
// Not the request validation -- that is the teleport service's, on the values a user typed.  This
// one exists for the opposite direction: `ResolvePlayer` walks every player-shaped entity and
// picks the one nearest the camera, and a reading of NaN or 1e30 means the transform chain
// produced garbage rather than that the entity is far away.  Treating such an entity as a
// candidate would let a bad read win the proximity contest and publish a player that is not one.
constexpr double kMaximumReadingMagnitude = 1.0e6;

bool ReadingAllowed(const double position[3]) {
    if (position == nullptr) return false;
    for (int index = 0; index < 3; ++index) {
        if (!std::isfinite(position[index])) return false;
        if (std::fabs(position[index]) > kMaximumReadingMagnitude) return false;
    }
    return true;
}

// The host's own service table, read through the registry rather than through a captured
// pointer: the table is published at startup and revoked at teardown, and a cached pointer
// would be a pointer into a table that may no longer be published.
//
// IT IS THE ENTITY SERVICE, NOT THE DRAWING ONE.  This used to query
// `cabbird.unity.entity-esp`, which made "which entity is this id" -- a question about the game's
// object model -- travel through the overlay's table.  The entity set is
// `cabbird.unity.entities` now, and this service has no reason to know the ESP table exists.
const CabbirdUnityEntitiesServiceV1* EntitySource() {
    return static_cast<const CabbirdUnityEntitiesServiceV1*>(
        ProcessAdapterServices().Query(CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID,
                                       CABBIRD_UNITY_ENTITIES_SERVICE_V1_VERSION));
}

// The active camera's world position, from the entity service.  `ProvideEspCameraPosition` did
// this before; it is the same cached value, and asking for it through the entity table is what
// keeps this file from depending on the drawing service.
bool EntityCameraPosition(double position[3]) {
    const CabbirdUnityEntitiesServiceV1* const source = EntitySource();
    if (source == nullptr || source->camera_position == nullptr) return false;
    return source->camera_position(source->user, position).code == CABBIRD_STATUS_V1_OK;
}

// `PlayerData#140234567` -> `PlayerData`, used only as a FALLBACK.  The live class name is
// normally read straight off the object (see `LiveDataClass`); this exists because the label
// is the one string the entity source already publishes, so a build in which the class read
// fails can still be classified.
void ClassFromLabel(const char* label, std::uint32_t label_size, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0) return;
    out[0] = '\0';
    if (label == nullptr || label_size == 0) return;
    std::size_t length = 0;
    while (length < label_size && label[length] != '\0' && label[length] != '#') ++length;
    if (length >= capacity) length = capacity - 1;
    std::memcpy(out, label, length);
    out[length] = '\0';
}

// The live C# class name of a managed object, straight from the runtime.
//
// WHY NOT THE PUBLISHED LABEL: the entity source's label is a localised DISPLAY name for
// entities the game names, and an empty string for everything it does not.  Classifying on it
// would make "the player has no localised name yet" indistinguishable from "this is not the
// player", and the label's class-prefixed form is a display format that could change without
// anyone thinking of this file.  The object's class is a fact the runtime owns.
void LiveDataClass(std::uintptr_t data, const char* label, std::uint32_t label_size,
                   char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0) return;
    out[0] = '\0';
    const il2cpp::Api& api = il2cpp::Functions();
    if (data != 0 && api.il2cpp_class_get_name != nullptr) {
        // `data` is a real managed object (it came from the entity source's `entity_data`),
        // so its first field is the `Il2CppClass*`.  Read as raw memory rather than through
        // `il2cpp_object_get_class` because the runtime entry is one more thing that can fail
        // to resolve, and the header read is the SAME layout the GC uses.
        if (const std::optional<std::uintptr_t> klass_address = mem::Read<std::uintptr_t>(data);
            klass_address.has_value() && *klass_address != 0) {
            const char* const name =
                api.il2cpp_class_get_name(reinterpret_cast<il2cpp::Il2CppClass*>(*klass_address));
            if (name != nullptr && name[0] != '\0') {
                std::snprintf(out, capacity, "%s", name);
                return;
            }
        }
    }
    ClassFromLabel(label, label_size, out, capacity);
}

// Is this live data class one of the game's player-shaped classes?
//
// THE SAME THREE NAMES THE ENTITY WALK USES to bucket `CABBIRD_UNITY_ENTITY_V1_KIND_PLAYER`.
// Read off the LIVE object's class rather than off the record's `kind`, because `kind` is
// refined by the camp comparison afterwards and the question here is the class question only.
bool PlayerShapedDataClass(const char* data_class) {
    if (data_class == nullptr || data_class[0] == '\0') return false;
    return std::strcmp(data_class, "PlayerData") == 0 ||
           std::strcmp(data_class, "HeroData") == 0 ||
           std::strcmp(data_class, "PetData") == 0;
}

// --- resolving the local player ---------------------------------------------

// Walk the entity source's cache and pick the player-shaped entity nearest the camera.
//
// WHY THE ENTITY SOURCE AND NOT `KIND_PLAYER`: `kind` is refined by a camp comparison (see
// unity_adapter.cpp), so a monster camp can legitimately move an entity between
// buckets.  What matters here is the CLASS question -- "is this a character the player
// drives" -- which the entity source exposes through `data_class`.
//
// WHY THE CAMERA AND NOT "THE FIRST ONE": the player, their hero, and their pet are all
// player-shaped, and in co-op so are other people's.  A camera-anchored pick is a
// heuristic (labelled as one everywhere it is reported), but it is a heuristic whose input
// is a fact: the active camera's world position, read from the engine by the entity walk.
bool ResolvePlayer(ResolvedPlayer* out, std::string* reason) {
    const CabbirdUnityEntitiesServiceV1* const source = EntitySource();
    if (source == nullptr) { *reason = "cabbird.unity.entities is not published in this host build"; return false; }
    const std::uint32_t count = source->entity_count(source->user);
    const std::uint64_t generation = source->generation(source->user);
    if (count == 0) { *reason = "the entity service reports no live entities (not in a world scene?)"; return false; }
    std::uintptr_t player_data = 0, player_class = 0;
    std::uint64_t entity_id = 0;
    if (!ResolveAuthoritativeMainPlayer(&player_data, &player_class, &entity_id, reason)) return false;
    CabbirdUnityEntityV1 player_entity{};
    bool found_in_cache = false;
    for (std::uint32_t index = 0; index < count; ++index) {
        CabbirdUnityEntityV1 entity{};
        if (source->entity_at(source->user, index, &entity).code != CABBIRD_STATUS_V1_OK) continue;
        if (entity.entity_data == player_data) { player_entity = entity; found_in_cache = true; break; }
    }
    if (!found_in_cache) { *reason = "EntityManager.get_ControllingEntity.data is not present in the live entity cache"; return false; }
    ResolvedPlayer candidate;
    candidate.valid = true;
    candidate.generation = generation;
    candidate.entity_id = entity_id;
    candidate.identity_from_proximity = false;
    const il2cpp::Api& api = il2cpp::Functions();
    const char* name = api.il2cpp_class_get_name != nullptr
        ? api.il2cpp_class_get_name(reinterpret_cast<il2cpp::Il2CppClass*>(player_class)) : nullptr;
    std::snprintf(candidate.data_class, sizeof(candidate.data_class), "%s", name ? name : "<unknown>");
    if (!ResolveUnityTransformFromData(player_data, &candidate.transform)) { *reason = "EntityManager.get_ControllingEntity.data has no readable Transform"; return false; }
    double exact_position[3]{};
    if (!ReadUnityTransformPosition(candidate.transform, exact_position) || !ReadingAllowed(exact_position)) { *reason = "EntityManager.get_ControllingEntity Transform position is unreadable"; return false; }
    double camera[3]{};
    const bool have_camera = EntityCameraPosition(camera);
    std::memcpy(candidate.position, exact_position, sizeof(candidate.position));
    candidate.distance_to_camera = have_camera ? Distance(exact_position, camera) : 0.0;
    *out = candidate;
    reason->clear();
    return true;
}

// --- the write MOVED OUT ---------------------------------------------------------
//
// `ApplyWrite` and `VerifyStaged` were here until the split, together with the queued/staged
// bookkeeping and every write counter.  They are `unity_adapter.cpp`
// (`cabbird.unity.player-teleport`) now, because they are the WRITE POLICY and this service is
// the READ-ONLY snapshot.
//
// WHY THE SPLIT, IN ONE LINE: a snapshot is a poll a plugin may make every frame, and a queued
// mutation is a different act with a different risk.  One grant could not express "may look at
// the player" without also granting "may move it", and the sibling project keeps them apart --
// its `NtePosition` declares `anomaly.nte.player` alone, while `NteTeleport` declares that PLUS
// `anomaly.nte.player-teleport`.
//
// WHAT DID NOT MOVE: the identity heuristic (`ResolvePlayer` above).  The teleport service asks
// THIS service which entity is the player, through `ResolvedHostPlayer()`, because a second copy
// of "which one is the player" could disagree with the first and the disagreement would show up
// as a teleport landing on the wrong character.

// --- the published service table ---------------------------------------------

CabbirdStatusV1 CABBIRD_CALL PlayerSnapshot(
    void*, CabbirdUnityPlayerSnapshotV1* snapshot) noexcept {
    if (snapshot == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    RequestPlayerRefresh();
    RequestPlayerBindingIfNeeded();
    std::memset(snapshot, 0, sizeof(*snapshot));
    snapshot->struct_size = sizeof(*snapshot);
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    if (!state.player.valid) return {CABBIRD_STATUS_V1_NOT_FOUND, 0, {}};
    snapshot->flags = CABBIRD_UNITY_PLAYER_V1_VALID;
    if (state.player.identity_from_proximity) {
        snapshot->flags |= CABBIRD_UNITY_PLAYER_V1_IDENTITY_PROXIMITY;
    }
    snapshot->generation = state.player.generation;
    snapshot->entity_id = state.player.entity_id;
    std::snprintf(snapshot->data_class, sizeof(snapshot->data_class), "%s",
                  state.player.data_class);
    std::memcpy(snapshot->position, state.player.position, sizeof(snapshot->position));
    // The entity source publishes a BOX CENTRE one half-height above the pivot; a snapshot
    // must report the pivot, so the offset is added here rather than the field being left
    // at the pivot and mislabelled.
    snapshot->bounds_center[0] = state.player.position[0];
    snapshot->bounds_center[1] = state.player.position[1] + UnityEntityBoxBaseOffset();
    snapshot->bounds_center[2] = state.player.position[2];
    snapshot->flags |= CABBIRD_UNITY_PLAYER_V1_HAS_BOUNDS;
    snapshot->distance_to_camera = state.player.distance_to_camera;
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}

// The two write entries this table still carries, FORWARDED to
// `cabbird.unity.player-teleport` (`unity_adapter.cpp`), which owns the queue,
// the apply and the verification.
//
// WHY THEY ARE STILL HERE RATHER THAN DELETED
//
// The SDK surface is append-only: a table that loses an entry is a table every already-built
// plugin reads at the wrong offsets.  `cabbird.unity.player` v1 shipped with these two entries,
// so they stay and delegate -- the plugin-visible ABI does not move, while the IMPLEMENTATION
// lives where it belongs.  A plugin that wants the honest grant asks for
// `cabbird.unity.player-teleport` and gets the table documented in the SDK header; one that was
// built against v1 keeps working and is now, in fact, driving the same code.
//
// The delegation is one line each, which is the tell that the split was in the right place: the
// old bodies were ~200 lines of queue/apply/verify policy with no read-side content at all.
CabbirdStatusV1 CABBIRD_CALL PlayerWritePosition(
    void*, const CabbirdUnityPlayerWriteRequestV1* request) noexcept {
    return PlayerTeleportSubmit(request);
}

CabbirdStatusV1 CABBIRD_CALL PlayerWriteState(
    void*, CabbirdUnityPlayerWriteResultV1* result) noexcept {
    return PlayerTeleportQuery(result);
}


} }  // namespace (adapter internals)


void RefreshUnityPlayer() {
    using namespace unity_adapter_player_detail;
    if (PlayerClockMillis() >
        ComponentState().requested_until.load(std::memory_order_relaxed)) {
        return;
    }
    auto& state = State();
    // Probe the authoritative controller even while a transform is cached. Character switching
    // does not necessarily rebuild the entity generation, so generation-only invalidation can
    // leave the service pinned to the previous hero.
    std::uintptr_t authoritative_data = 0;
    std::uintptr_t authoritative_class = 0;
    std::uint64_t authoritative_id = 0;
    std::string authoritative_reason;
    const bool authoritative_ok = ResolveAuthoritativeMainPlayer(
        &authoritative_data, &authoritative_class, &authoritative_id, &authoritative_reason);
    std::uintptr_t transform = 0;
    std::uint64_t entity_id = 0;
    {
        std::scoped_lock lock(state.mutex);
        if (state.player.valid && authoritative_ok &&
            state.player.entity_id == authoritative_id) {
            transform = state.player.transform;
            entity_id = state.player.entity_id;
        } else if (state.player.valid && !authoritative_ok) {
            state.player = ResolvedPlayer{};
            state.reason = authoritative_reason;
        }
    }
    if (transform != 0) {
        double position[3]{};
        if (ReadUnityTransformPosition(transform, position) && ReadingAllowed(position)) {
            double camera[3]{};
            const bool have_camera = EntityCameraPosition(camera);
            std::scoped_lock lock(state.mutex);
            if (state.player.valid && state.player.entity_id == entity_id) {
                std::memcpy(state.player.position, position, sizeof(position));
                state.player.distance_to_camera =
                    have_camera ? Distance(position, camera) : 0.0;
            }
            return;
        }
        {
            std::scoped_lock lock(state.mutex);
            if (state.player.entity_id == entity_id) {
                state.player = ResolvedPlayer{};
                state.reason = "the cached player transform became unreadable";
            }
        }
        RequestUnityPlayerEntityRefresh();
        return;
    }

    const CabbirdUnityEntitiesServiceV1* const source = EntitySource();
    // A host build with no entity service has no generations either; 0 then simply never
    // matches, so the resolve below runs every tick and reports its own reason.
    const std::uint64_t generation = source != nullptr ? source->generation(source->user) : 0;
    // Re-resolve when the entity set has changed.  Done outside the lock: the walk calls
    // IL2CPP and touches managed memory, and holding a mutex the render domain's
    // `snapshot` needs across that is how one bad read becomes "the overlay stopped drawing".
    bool needs_resolve = false;
    {
        std::scoped_lock lock(state.mutex);
        needs_resolve = !state.player.valid || state.player.generation != generation ||
                        !authoritative_ok || state.player.entity_id != authoritative_id;
    }
    if (needs_resolve) {
        ResolvedPlayer resolved;
        std::string reason;
        if (ResolvePlayer(&resolved, &reason)) {
            std::scoped_lock lock(state.mutex);
            state.player = resolved;
            state.reason.clear();
        } else {
            std::scoped_lock lock(state.mutex);
            state.player = ResolvedPlayer{};
            state.reason = std::move(reason);
        }
    }
}

void InvalidateUnityPlayerState() noexcept {
    using namespace unity_adapter_player_detail;
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    state.player = {};
    state.reason = "scene invalidated; awaiting player rebind";
    ComponentState().requested_until.store(PlayerClockMillis() + 250,
                                           std::memory_order_relaxed);
}

HostUnityPlayerStats SnapshotHostUnityPlayer() {
    ServiceCall adapter_call;
    if (!adapter_call) return {};
    using namespace unity_adapter_player_detail;
    HostUnityPlayerStats stats;
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    stats.live = state.player.valid;
    stats.entity_id = state.player.entity_id;
    std::snprintf(stats.data_class, sizeof(stats.data_class), "%s", state.player.data_class);
    stats.identity_from_proximity = state.player.identity_from_proximity;
    std::memcpy(stats.position, state.player.position, sizeof(stats.position));
    stats.distance_to_camera = state.player.distance_to_camera;
    stats.reason = BorrowedServiceText(20, state.reason);
    return stats;
}

HostResolvedPlayer ResolvedHostPlayer() {
    ServiceCall adapter_call;
    if (!adapter_call) return {};
    using namespace unity_adapter_player_detail;
    RequestPlayerRefresh();
    RequestPlayerBindingIfNeeded();
    HostResolvedPlayer out;
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    if (!state.player.valid) return out;
    out.valid = true;
    out.generation = state.player.generation;
    out.entity_id = state.player.entity_id;
    out.transform = state.player.transform;
    std::snprintf(out.data_class, sizeof(out.data_class), "%s", state.player.data_class);
    std::memcpy(out.position, state.player.position, sizeof(out.position));
    out.distance_to_camera = state.player.distance_to_camera;
    out.identity_from_proximity = state.player.identity_from_proximity;
    return out;
}

}  // namespace cabbird


/* ---- UnityAdapter component: teleport ---- */

// Host backend for `cabbird.unity.player-teleport` -- the WRITE policy for the local player.
//
// See `include/cabbird/unity_services.hpp` for WHY this is a separate
// service from `cabbird.unity.player`, and the SDK header for the table's contract.
//
// WHAT THIS FILE OWNS, AND WHAT IT DELIBERATELY DOES NOT
//
// OWNS: the request queue, the apply, the post-check, and the verdict.  That is ~200 lines of
// policy that used to live at the bottom of `unity_adapter.cpp`, where it made a
// read-only snapshot table also a mutation table.
//
// DOES NOT OWN: which entity is the player.  It asks `ResolvedHostPlayer()`, so there is exactly
// one implementation of that decision.  A second copy of "which one is the player" could
// disagree with the first, and the disagreement would surface as a teleport landing on the wrong
// character -- a failure that looks like the feature working.
//
// ALSO DOES NOT OWN: the managed calls.  `set_position`, `set_eulerAngles` and `get_position`
// come from `cabbird.unity.transform` (`unity_adapter.cpp`), so the boxed-`Vector3`
// + `il2cpp_runtime_invoke` boilerplate exists once in the tree rather than three times.


namespace cabbird {
namespace { namespace unity_adapter_teleport_detail {

// --- limits -----------------------------------------------------------------
//
// A teleport target is user input from a text box, so it is checked like user input.  These
// bounds are not game rules: they are the range outside which a coordinate is a typo or a unit
// error rather than a place in the world.
//
// The magnitude limit is ALSO enforced by `cabbird.unity.transform`, and that duplication is
// deliberate rather than sloppy: this one rejects the request before it is queued (so the plugin
// hears about it immediately), and that one rejects it at the engine boundary (so a direct
// caller of the transform service cannot bypass it).  They share the same constant's value.
constexpr double kMaximumCoordinateMagnitude = 1.0e6;   // 1000 km, generously
// A written position is "there" when it is within this many metres of the target.  Unity
// stores floats, so a metre-scale error is never a rounding artefact.
constexpr double kAcceptanceToleranceMeters = 0.75;
// How far the position may drift between the immediate check and the later one before the
// write is called REVERTED.
constexpr double kDriftToleranceMeters = 0.75;
// Ticks to wait before the second read.  Not 1: the frame in which the call happens is not
// a frame in which the game's controller has run, so a one-tick check would pass every
// teleport including the ones that get undone.  Four ticks is ~66 ms at 60 Hz -- settled
// motion, still fast enough that a user watching the screen sees no lag in the report.
constexpr std::uint64_t kVerifyDelayTicks = 4;

struct PendingWrite {
    bool queued{};
    CabbirdUnityPlayerWriteRequestV1 request{};
    std::uint32_t sequence{};
};

struct ServiceState {
    std::mutex mutex;
    // The write pipeline.  `queued` is set by `write_position` from whatever domain the plugin's
    // UI runs in and consumed by the refresh; `staged` is set by the apply and consumed by the
    // verification a few ticks later.
    PendingWrite queued;
    bool staged{};
    std::uint64_t staged_tick{};
    std::uint64_t staged_entity_id{};
    double staged_target[3]{};
    double staged_original[3]{};
    double staged_error{};
    std::uint32_t staged_sequence{};
    CabbirdUnityPlayerWriteResultV1 last_result{};
    bool have_result{};
    std::uint32_t next_sequence{1};
    std::uint64_t writes_requested{};
    std::uint64_t writes_applied{};
    std::uint64_t writes_refused{};
    std::uint64_t writes_reverted{};
    std::uint64_t tick{};
    // Why the last request was refused, when it was.  A fixed buffer, NOT a `thread_local`:
    // this image is MANUALLY MAPPED and a static TLS directory is rejected by the mapper.
    // The same lesson is written up in include/cabbird/thread_local_value.hpp.
    char reason[192]{};
};

// The teleport service is polled by the UI every frame, but its game-domain
// refresh has work only while a request is queued or being verified.  Do not
// take the service mutex once per game tick merely because the window is open.
struct TeleportComponentState : ServiceState {
    std::atomic_bool work_pending{false};
};

TeleportComponentState& ComponentState() {
    return UnityAdapterServices::Current()->Component<TeleportComponentState>(
        UnityAdapterComponent::Teleport);
}

#define g_work_pending (ComponentState().work_pending)

ServiceState& State() {
    return ComponentState();
}

double Distance(const double a[3], const double b[3]) {
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool PositionAllowed(const double position[3]) {
    if (position == nullptr) return false;
    for (int index = 0; index < 3; ++index) {
        if (!std::isfinite(position[index])) return false;
        if (std::fabs(position[index]) > kMaximumCoordinateMagnitude) return false;
    }
    return true;
}

// The entity source, read through the registry rather than through a captured pointer: the table
// is published at startup and revoked at teardown, and a cached pointer would be a pointer into
// a table that may no longer be published.
//
// USED ONLY to re-resolve a player that the cache has moved on from -- see `ApplyWrite`.  The
// normal path takes the resolved player from `cabbird.unity.player`, which owns that decision.
const CabbirdUnityEntitiesServiceV1* EntitySource() {
    return static_cast<const CabbirdUnityEntitiesServiceV1*>(
        ProcessAdapterServices().Query(CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID,
                                       CABBIRD_UNITY_ENTITIES_SERVICE_V1_VERSION));
}

// Apply one queued request.  Returns the result struct for the caller to publish.
CabbirdUnityPlayerWriteResultV1 ApplyWrite(const CabbirdUnityPlayerWriteRequestV1& request,
                                           std::uint32_t sequence) {
    CabbirdUnityPlayerWriteResultV1 result{};
    result.struct_size = sizeof(result);
    result.flags = request.flags;
    result.request_sequence = sequence;
    result.entity_id = request.entity_id;
    std::memcpy(result.applied_position, request.position, sizeof(result.applied_position));
    result.result_code = CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REFUSED;

    auto& state = State();
    // THE PLAYER IS ASKED FOR, NOT RE-DERIVED.  `cabbird.unity.player` owns "which entity is the
    // player" and hands over its resolved transform; this is a point-in-time copy, so a player
    // that disappears between here and the write is caught by the null transform below.
    const HostResolvedPlayer resolved = ResolvedHostPlayer();
    std::uintptr_t transform = 0;
    std::uint64_t entity_id = resolved.entity_id;
    if (resolved.valid) {
        // The request may have been typed against a snapshot from the previous hero. The
        // authoritative controller is the source of truth at write time; never fall back to an
        // old cached id and accidentally move a different character after a role switch.
        transform = resolved.transform;
        entity_id = resolved.entity_id;
    } else if (request.entity_id != 0) {
        // An explicit id may name a player the cache no longer holds -- a scene change between
        // the plugin reading a snapshot and the user pressing the button.  That is a refusal,
        // not a retry: writing to a recycled address is exactly the "wrong object, plausible
        // result" failure this service keeps refusing.
        const UnityEntityLookup lookup = LookupUnityEntityById(request.entity_id);
        if (lookup.found && lookup.player_shaped) {
            if (ResolveUnityTransformFromData(lookup.data, &transform)) {
                entity_id = lookup.entity_id;
            }
        }
    }
    if (transform == 0) {
        result.entity_id = entity_id;
        result.result_code = CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REFUSED;
        std::scoped_lock lock(state.mutex);
        std::snprintf(state.reason, sizeof(state.reason),
                      "no usable transform for entity %llu (the player may have gone away)",
                      static_cast<unsigned long long>(entity_id));
        return result;
    }
    result.entity_id = entity_id;

    // The ORIGINAL position is read first, because "the write did not take" and "the game
    // put it back" cannot be told apart afterwards without it.
    double original[3]{};
    if (!ReadUnityTransformPosition(transform, original)) {
        std::scoped_lock lock(state.mutex);
        std::snprintf(state.reason, sizeof(state.reason),
                      "the player's position could not be read before the write");
        return result;
    }
    const auto started = std::chrono::steady_clock::now();
    const bool applied = WriteUnityTransformPosition(transform, request.position);
    result.apply_micros =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count());
    if (!applied) {
        std::scoped_lock lock(state.mutex);
        std::snprintf(state.reason, sizeof(state.reason),
                      "the managed set_position call did not complete");
        return result;
    }
    const bool rotation_requested =
        (request.flags & CABBIRD_UNITY_PLAYER_WRITE_V1_SET_ROTATION) != 0;
    if (rotation_requested) {
        // A failed rotation is NOT a failed teleport: the position is what was asked for,
        // and the two are independent properties.  The flag staying in the result says the
        // rotation half was requested; the position fields say what actually happened to
        // the position.
        static_cast<void>(WriteUnityTransformEuler(transform, request.euler_degrees));
    }
    double measured[3]{};
    if (!ReadUnityTransformPosition(transform, measured)) {
        std::scoped_lock lock(state.mutex);
        std::snprintf(state.reason, sizeof(state.reason),
                      "the player's position could not be re-read after the write");
        return result;
    }
    std::memcpy(result.measured_position, measured, sizeof(result.measured_position));
    result.position_error = Distance(measured, request.position);
    result.result_code = result.position_error <= kAcceptanceToleranceMeters
        ? CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_PENDING
        : CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REFUSED;

    {
        std::scoped_lock lock(state.mutex);
        if (result.result_code == CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_PENDING) {
            // Staged for the second read.  Published NOW as PENDING so a plugin that polls
            // immediately sees "in progress" rather than the previous teleport's verdict.
            state.staged = true;
            state.staged_tick = state.tick;
            state.staged_entity_id = entity_id;
            state.staged_sequence = sequence;
            std::memcpy(state.staged_target, request.position, sizeof(state.staged_target));
            std::memcpy(state.staged_original, original, sizeof(state.staged_original));
            state.staged_error = result.position_error;
            ++state.writes_applied;
        } else {
            ++state.writes_refused;
            std::snprintf(state.reason, sizeof(state.reason),
                          "the write landed %.3f m from the target (tolerance %.2f m)",
                          result.position_error, kAcceptanceToleranceMeters);
        }
        state.last_result = result;
        state.have_result = true;
    }
    return result;
}

// The second read: is the player still where it was put?
//
// CONTRACT: the caller must NOT hold `state.mutex`.  `State()`'s mutex is not recursive, and the
// version of `RefreshUnityPlayer` that called this from inside its own `scoped_lock` was a
// self-deadlock that threw once per tick.  The call site below releases the lock first, and the
// order is load-bearing rather than stylistic.
void VerifyStaged() {
    auto& state = State();
    std::uint64_t now_tick = 0;
    std::uint64_t entity_id = 0;
    double target[3]{};
    double original[3]{};
    std::uint32_t sequence = 0;
    {
        std::scoped_lock lock(state.mutex);
        if (!state.staged) return;
        now_tick = state.tick;
        if (now_tick < state.staged_tick + kVerifyDelayTicks) return;
        entity_id = state.staged_entity_id;
        sequence = state.staged_sequence;
        std::memcpy(target, state.staged_target, sizeof(target));
        std::memcpy(original, state.staged_original, sizeof(original));
    }
    // Re-resolve through the authoritative local-player path rather than the entity-cache lookup.
    // The cache can be rebuilt between the write and this check; using it here could leave a
    // successful write permanently PENDING. This path is EntityManager.get_ControllingEntity(),
    // so the check remains bound to the character controlled by this client.
    std::uintptr_t transform = 0;
    const HostResolvedPlayer resolved = ResolvedHostPlayer();
    if (resolved.valid && resolved.entity_id == entity_id) {
        transform = resolved.transform;
    }
    double measured[3]{};
    const bool read_ok = transform != 0 && ReadUnityTransformPosition(transform, measured);
    CabbirdUnityPlayerWriteResultV1 result{};
    {
        std::scoped_lock lock(state.mutex);
        if (!state.staged || state.staged_sequence != sequence) return;
        result = state.last_result;
        result.request_sequence = sequence;
        result.entity_id = entity_id;
        std::memcpy(result.applied_position, target, sizeof(result.applied_position));
        if (!read_ok) {
            // The controlled player changed or disappeared between the write and the check.
            // This request is terminally refused; leaving it PENDING would block later requests
            // behind stale verification state.
            result.result_code = CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REFUSED;
            ++state.writes_refused;
            std::snprintf(state.reason, sizeof(state.reason),
                          "the controlled player changed or its Transform became unreadable");
        } else {
            std::memcpy(result.measured_position, measured, sizeof(result.measured_position));
            result.position_error = Distance(measured, target);
            const double drift_from_target = result.position_error;
            const double drift_from_original = Distance(measured, original);
            if (drift_from_target <= kAcceptanceToleranceMeters) {
                result.result_code = CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_OK;
                std::snprintf(state.reason, sizeof(state.reason), "");
            } else if (drift_from_original < drift_from_target - kDriftToleranceMeters) {
                // THE CODE THIS WHOLE POST-CHECK EXISTS FOR.  Unity's `CharacterController` /
                // `Rigidbody` own movement and can put the player back on the next frame; the
                // write "succeeded" and the player is where they were.  Reporting OK here would
                // be the service lying about the one thing it exists to do.
                result.result_code = CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REVERTED;
                ++state.writes_reverted;
                std::snprintf(state.reason, sizeof(state.reason),
                              "the game put the player back (%.3f m from the target)",
                              drift_from_target);
            } else {
                // Moved, but not to the target and not back to the start: the game's own
                // movement is in charge and the request was partly absorbed.  REFUSED is the
                // honest code -- the player is not where it was asked to be.
                result.result_code = CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REFUSED;
                ++state.writes_refused;
                std::snprintf(state.reason, sizeof(state.reason),
                              "the game moved the player to neither the target nor the origin "
                              "(%.3f m from the target)", drift_from_target);
            }
        }
        state.staged = false;
        state.last_result = result;
        state.have_result = true;
    }
}

} }  // namespace (adapter internals)
// --- the two service-table entries -------------------------------------------
//
// These live at `cabbird` scope, NOT in the anonymous namespace above, for one reason: the
// player service's compatibility entries forward to them, and a forward declaration of an
// internal-linkage function is not something a header can offer.  Everything they touch is still
// behind `State()`'s mutex, and the only SDK-visible route to them is the table below.

CabbirdStatusV1 PlayerTeleportSubmit(
    const CabbirdUnityPlayerWriteRequestV1* request) noexcept {
    ServiceCall adapter_call;
    if (!adapter_call) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    using namespace unity_adapter_teleport_detail;
    if (request == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (request->struct_size < sizeof(CabbirdUnityPlayerWriteRequestV1)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    if (!PositionAllowed(request->position)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    // ORDER MATTERS HERE, AND THE FIRST VERSION HAD IT WRONG.
    //
    // "Is there a player to move" is asked BEFORE "can this engine ever be written to".  The
    // reverse order reads as stricter and is worse: with no game attached -- an offline test, or
    // a host that never bound a profile -- the transform binding is unresolvable, so a request
    // with no player would answer UNAVAILABLE ("this build cannot teleport") when the true answer
    // is NOT_FOUND ("there is nobody to teleport").  The first is a statement about the host; the
    // second is a statement about the world, and it is the one a plugin needs to show the user.
    const HostResolvedPlayer player = ResolvedHostPlayer();
    if (!player.valid && request->entity_id == 0) {
        // Refuse EARLY when there is no player to move: queueing it would make the plugin wait a
        // tick to be told what is already known, and would let a request survive into a scene it
        // was never meant for.
        return {CABBIRD_STATUS_V1_NOT_FOUND, 0, {}};
    }
    // The engine half must be resolvable, or no write below can ever succeed.  Only asked once
    // there IS a target, so the answer is about the write rather than about the world.
    if (!ResolveUnityTransformBinding()) {
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    if (state.queued.queued) {
        // One request at a time.  A queue would let a user teleport five times while the game is
        // paused and then watch the player walk through all five, which is neither what was
        // asked for nor debuggable.
        return {CABBIRD_STATUS_V1_CONFLICT, 0, {}};
    }
    state.queued.queued = true;
    state.queued.request = *request;
    state.queued.sequence = state.next_sequence++;
    ++state.writes_requested;
    g_work_pending.store(true, std::memory_order_release);
    // OK, NOT A "PENDING" CODE -- there is no such code in the ABI and inventing one would change
    // the meaning of a table every plugin already reads.  OK here means "the request was
    // accepted and is now queued"; whether the player MOVED is answered by `state`, which is
    // deliberately a different question.
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}

CabbirdStatusV1 PlayerTeleportQuery(CabbirdUnityPlayerWriteResultV1* result) noexcept {
    ServiceCall adapter_call;
    if (!adapter_call) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    using namespace unity_adapter_teleport_detail;
    if (result == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    if (!state.have_result) {
        std::memset(result, 0, sizeof(*result));
        result->struct_size = sizeof(*result);
        return {CABBIRD_STATUS_V1_NOT_FOUND, 0, {}};
    }
    *result = state.last_result;
    result->struct_size = sizeof(*result);
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}

namespace { namespace unity_adapter_teleport_detail {

// The ABI-shaped wrappers.  The table's entries take a leading `void* user` and carry the
// calling convention; the implementation above does not, because it is called from inside the
// process by the player service's compatibility entries as well as from here.  Wrapping rather
// than decorating the implementation keeps ONE body and TWO callers.
CabbirdStatusV1 CABBIRD_CALL TeleportEntryPosition(
    void*, const CabbirdUnityPlayerWriteRequestV1* request) noexcept {
    return PlayerTeleportSubmit(request);
}

CabbirdStatusV1 CABBIRD_CALL TeleportEntryState(
    void*, CabbirdUnityPlayerWriteResultV1* result) noexcept {
    return PlayerTeleportQuery(result);
}


} }  // namespace (adapter internals)


void RefreshUnityPlayerTeleport() {
    using namespace unity_adapter_teleport_detail;
    if (!g_work_pending.load(std::memory_order_acquire)) return;
    auto& state = State();
    // The tick advances even when nothing happens below, because the verification delay is
    // measured in ticks and a paused/absent write must not stall it.
    {
        std::scoped_lock lock(state.mutex);
        ++state.tick;
    }
    // The queued write, drained exactly once.
    //
    // THE LOCK IS RELEASED BEFORE `VerifyStaged()` IS CALLED, AND THAT ORDER IS THE WHOLE POINT
    // OF THIS BLOCK.  The first version called `VerifyStaged()` from inside the `scoped_lock` on
    // the `!queued` branch, so a tick with nothing to write re-locked `state.mutex` on the same
    // thread -- `std::mutex` is not recursive, so MSVC threw "resource deadlock would occur" out
    // of the refresh on EVERY tick.  The host caught it and logged it once per tick, which is how
    // it surfaced: the service had never actually run.
    PendingWrite pending;
    bool have_pending = false;
    {
        std::scoped_lock lock(state.mutex);
        if (state.queued.queued) {
            pending = state.queued;
            state.queued.queued = false;
            have_pending = true;
        }
    }
    if (have_pending) {
        static_cast<void>(ApplyWrite(pending.request, pending.sequence));
    }
    VerifyStaged();
    {
        std::scoped_lock lock(state.mutex);
        if (!state.queued.queued && !state.staged) {
            g_work_pending.store(false, std::memory_order_release);
        }
    }
}

void InvalidateUnityPlayerTeleportState() noexcept {
    using namespace unity_adapter_teleport_detail;
    auto& state = ComponentState();
    std::scoped_lock lock(state.mutex);
    state.queued = {};
    state.staged = false;
    state.have_result = false;
    state.reason[0] = '\0';
    state.work_pending.store(false, std::memory_order_release);
}

HostUnityPlayerTeleportStats SnapshotHostUnityPlayerTeleport() {
    ServiceCall adapter_call;
    if (!adapter_call) return {};
    using namespace unity_adapter_teleport_detail;
    HostUnityPlayerTeleportStats stats;
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    stats.writes_requested = state.writes_requested;
    stats.writes_applied = state.writes_applied;
    stats.writes_refused = state.writes_refused;
    stats.writes_reverted = state.writes_reverted;
    stats.last_request_sequence = state.last_result.request_sequence;
    stats.last_result_code = state.last_result.result_code;
    stats.last_result_applied = state.have_result;
    stats.last_position_error = state.last_result.position_error;
    stats.last_apply_micros = state.last_result.apply_micros;
    stats.reason = BorrowedServiceText(21, state.reason ? state.reason : "");
    return stats;
}

}  // namespace cabbird

#undef g_work_pending


/* ---- UnityAdapter component: transform ---- */

// `cabbird.unity.transform` -- the engine-side half of "move an object".
//
// See `include/cabbird/unity_services.hpp` for WHY this service exists, why
// it lives in the adapter layer, and what it deliberately refuses to know.  This file is the
// implementation: the offset chain, the liveness rule, and the three managed calls.
//
// WHAT WAS MOVED HERE, AND FROM WHERE
//
// This code was `src/game/unity/unity_adapter.cpp`'s `ResolveUnityEntityTransform`,
// `ReadUnityTransformPosition` and `ApplyUnityTransformPosition`, plus a second, independent
// copy of the box+invoke boilerplate in `src/game/unity/unity_adapter.cpp`
// (`ApplyUnityTransformEuler`).  Two things were wrong with that arrangement:
//
//   * the offset chain -- an ENGINE fact -- was owned by a service whose contract is "publish
//     the cached entity set", and that service's header therefore exported a function that
//     changes game world state;
//   * the box+invoke boilerplate existed twice, so a fix to one copy would silently miss the
//     other.
//
// Both are gone now: there is exactly ONE implementation of the chain and one `InvokeSetter`.


namespace cabbird {
namespace { namespace unity_adapter_transform_detail {

// --- constants that come from the DUMP and are therefore only fallbacks ---------------
//
// Every one of these is passed to `FieldOffsetOr`, which asks the RUNTIME first and reports
// whether the answer was confirmed.  The dump is a snapshot of one build; metadata offsets move
// between builds, and this chain has already been caught by that once (see the note on
// `FieldOffsetOr` in the entity ESP service's history).  So a dump constant here is a documented
// FALLBACK that is always reported as unconfirmed, never a silent assumption.
constexpr std::size_t kFallbackDataTransform = 0xA0;  // BaseData::<transform>
constexpr std::size_t kFallbackMTransform = 0x10;     // RelativeTransform::m_transform

// How far from the origin a coordinate may be before it is treated as a mistake rather than a
// place.  Shared with the player service's own request validation on purpose: two different
// limits would mean "the plugin accepted what the engine refuses", and the failure would look
// like a teleport that silently did nothing.
constexpr double kMaximumCoordinateMagnitude = 1.0e6;

constexpr const char* kUnityCoreImage = "UnityEngine.CoreModule.dll";
constexpr const char* kTransformNamespace = "UnityEngine";
constexpr const char* kTransformClass = "Transform";
constexpr const char* kVector3Class = "Vector3";

// --- the raw read helpers ------------------------------------------------------------

bool ReadMemory(std::uintptr_t address, void* destination, std::size_t size) {
    if (size == 0) return false;
    return mem::ReadMemoryInto(address, destination, size);
}

template <typename T>
bool ReadValue(std::uintptr_t address, T* value) {
    return ReadMemory(address, value, sizeof(T));
}

// Where a field actually is, asked of the runtime instead of copied out of a dump.  A field
// that is not found, or that the runtime reports as static (`offset == (size_t)-1`), falls back
// to the dump constant with `exact = false`.
std::size_t FieldOffsetOr(const il2cpp::Api& api, il2cpp::Il2CppClass* klass, const char* name,
                          std::size_t fallback, bool* exact) {
    if (klass != nullptr && api.il2cpp_class_get_field_from_name != nullptr &&
        api.il2cpp_field_get_offset != nullptr) {
        il2cpp::FieldInfo* const field = api.il2cpp_class_get_field_from_name(klass, name);
        if (field != nullptr) {
            const std::size_t offset = api.il2cpp_field_get_offset(field);
            if (offset != static_cast<std::size_t>(-1)) {
                *exact = true;
                return offset;
            }
        }
    }
    *exact = false;
    return fallback;
}

// --- the cached binding ---------------------------------------------------------------

struct Binding {
    bool resolved{};
    std::uintptr_t vector3_class{};
    std::uintptr_t get_position{};
    std::uintptr_t set_position{};
    std::uintptr_t set_euler{};
    std::size_t off_data_transform{};
    std::size_t off_m_transform{};
    // False when any offset above came from the dump rather than from the runtime.  Reported
    // separately from `resolved` because "it works but one hop is a guess" is information a
    // consumer may want, and this project refuses to hide a guess inside a success.
    bool offsets_confirmed{};
};

struct TransformComponentState {
    std::mutex mutex;
    Binding binding;
    char last_error[256] = "the transform binding has not been resolved yet";
};

TransformComponentState& ComponentState() {
    return UnityAdapterServices::Current()->Component<TransformComponentState>(
        UnityAdapterComponent::Transform);
}

#define g_mutex (ComponentState().mutex)
#define g_binding (ComponentState().binding)
// A fixed buffer, NOT a `thread_local`: this image is MANUALLY MAPPED, so a static TLS
// directory is rejected outright by the mapper.  The lesson is written up at length in
// include/cabbird/thread_local_value.hpp and in the status-message notes above, so it is
// stated here before anyone is tempted.
#define g_last_error (ComponentState().last_error)

void SetError(const char* text) {
    std::snprintf(g_last_error, sizeof(g_last_error), "%s", text != nullptr ? text : "");
}

il2cpp::Il2CppClass* ClassInImage(const il2cpp::Api& api, il2cpp::Il2CppImage* image,
                                  const char* class_name) {
    if (image == nullptr || api.il2cpp_class_from_name == nullptr) return nullptr;
    return api.il2cpp_class_from_name(image, kTransformNamespace, class_name);
}

} }  // namespace (adapter internals)
const char* LastUnityTransformError() {
    ServiceCall adapter_call;
    if (!adapter_call) return "Unity adapter unavailable";
    using namespace unity_adapter_transform_detail;
    std::scoped_lock lock(g_mutex);
    return BorrowedServiceText(22, g_last_error);
}

bool ResolveUnityTransformBinding() {
    ServiceCall adapter_call;
    if (!adapter_call) return false;
    using namespace unity_adapter_transform_detail;
    std::scoped_lock lock(g_mutex);
    if (g_binding.resolved) return true;

    if (!il2cpp::Ready()) {
        SetError("the IL2CPP runtime is not ready");
        return false;
    }
    const il2cpp::Api& api = il2cpp::Functions();
    if (api.il2cpp_runtime_invoke == nullptr || api.il2cpp_object_new == nullptr ||
        api.il2cpp_object_unbox == nullptr || api.il2cpp_class_get_method_from_name == nullptr ||
        api.il2cpp_class_from_name == nullptr || api.il2cpp_class_get_field_from_name == nullptr ||
        api.il2cpp_field_get_offset == nullptr) {
        SetError("one or more IL2CPP runtime entries are missing");
        return false;
    }

    il2cpp::Il2CppImage* const core = il2cpp::FindImage(kUnityCoreImage);
    if (core == nullptr) {
        SetError("UnityEngine.CoreModule.dll was not found in the loaded images");
        return false;
    }
    il2cpp::Il2CppClass* const transform_class = ClassInImage(api, core, kTransformClass);
    il2cpp::Il2CppClass* const vector3_class = ClassInImage(api, core, kVector3Class);
    if (transform_class == nullptr || vector3_class == nullptr) {
        SetError("UnityEngine.Transform or UnityEngine.Vector3 was not found");
        return false;
    }
    const il2cpp::MethodInfo* const get_position =
        api.il2cpp_class_get_method_from_name(transform_class, "get_position", 0);
    if (get_position == nullptr) {
        SetError("UnityEngine.Transform::get_position was not found");
        return false;
    }
    // The two setters are resolved SEPARATELY and their absence is NOT fatal to each other.
    // `set_eulerAngles` missing means rotation-only requests are refused and position requests
    // keep working; that is a real build difference and must not be turned into "the service is
    // unavailable".
    const il2cpp::MethodInfo* const set_position =
        api.il2cpp_class_get_method_from_name(transform_class, "set_position", 1);
    if (set_position == nullptr) {
        SetError("UnityEngine.Transform::set_position was not found");
        return false;
    }
    const il2cpp::MethodInfo* const set_euler =
        api.il2cpp_class_get_method_from_name(transform_class, "set_eulerAngles", 1);

    // The two hops from a `BaseData` to its `Transform`.  Resolved from the runtime using the
    // same two class names the entity walk uses, because a `BaseData` address is what callers
    // have.  `FindImage` on a namespaced gameplay assembly is expected to fail in a build that
    // has not entered a world; that is reported rather than worked around.
    bool confirmed = true;
    std::size_t off_data_transform = kFallbackDataTransform;
    std::size_t off_m_transform = kFallbackMTransform;
    {
        il2cpp::Il2CppImage* const big_world = il2cpp::FindImage("Lens.Gameplay.Modules.BigWorld.dll");
        il2cpp::Il2CppClass* const base_data =
            big_world != nullptr
                ? api.il2cpp_class_from_name(big_world, "Lens.Gameplay.Modules.BigWorld", "BaseData")
                : nullptr;
        il2cpp::Il2CppClass* const relative =
            big_world != nullptr
                ? api.il2cpp_class_from_name(big_world, "Lens.Gameplay.Modules.BigWorld",
                                             "RelativeTransform")
                : nullptr;
        bool exact_data = false;
        bool exact_transform = false;
        off_data_transform =
            FieldOffsetOr(api, base_data, "transform", kFallbackDataTransform, &exact_data);
        off_m_transform = FieldOffsetOr(api, relative, "m_transform", kFallbackMTransform,
                                        &exact_transform);
        confirmed = exact_data && exact_transform;
    }

    g_binding.vector3_class = reinterpret_cast<std::uintptr_t>(vector3_class);
    g_binding.get_position = reinterpret_cast<std::uintptr_t>(get_position);
    g_binding.set_position = reinterpret_cast<std::uintptr_t>(set_position);
    g_binding.set_euler = reinterpret_cast<std::uintptr_t>(set_euler);
    g_binding.off_data_transform = off_data_transform;
    g_binding.off_m_transform = off_m_transform;
    g_binding.offsets_confirmed = confirmed;
    g_binding.resolved = true;
    SetError("");
    return true;
}

void InvalidateUnityTransformState() noexcept {
    using namespace unity_adapter_transform_detail;
    auto& state = ComponentState();
    std::scoped_lock lock(state.mutex);
    state.binding = {};
    std::snprintf(state.last_error, sizeof(state.last_error),
                  "%s", "scene invalidated; awaiting transform rebind");
}

bool UnityEntityTransformOffsets(UnityTransformOffsets* offsets) {
    ServiceCall adapter_call;
    if (!adapter_call) return false;
    using namespace unity_adapter_transform_detail;
    if (offsets == nullptr) return false;
    if (!ResolveUnityTransformBinding()) return false;
    std::scoped_lock lock(g_mutex);
    offsets->data_transform = g_binding.off_data_transform;
    offsets->m_transform = g_binding.off_m_transform;
    offsets->confirmed = g_binding.offsets_confirmed;
    return true;
}

std::uintptr_t ReadUnityNativePointer(std::uintptr_t object) {
    ServiceCall adapter_call;
    if (!adapter_call) return {};
    using namespace unity_adapter_transform_detail;
    if (object == 0) return 0;
    std::uintptr_t native = 0;
    if (!ReadValue(object + kUnityCachedPtrOffset, &native)) return 0;
    return native;
}

bool UnityObjectAlive(std::uintptr_t object) {
    ServiceCall adapter_call;
    if (!adapter_call) return false;
    using namespace unity_adapter_transform_detail;
    return ReadUnityNativePointer(object) != 0;
}

bool ResolveUnityTransformFromData(std::uintptr_t data, std::uintptr_t* transform) {
    ServiceCall adapter_call;
    if (!adapter_call) return false;
    using namespace unity_adapter_transform_detail;
    if (transform == nullptr) return false;
    *transform = 0;
    if (data == 0) return false;
    if (!ResolveUnityTransformBinding()) return false;

    std::size_t off_data_transform = 0;
    std::size_t off_m_transform = 0;
    {
        std::scoped_lock lock(g_mutex);
        off_data_transform = g_binding.off_data_transform;
        off_m_transform = g_binding.off_m_transform;
    }
    std::uintptr_t relative = 0;
    if (!ReadValue(data + off_data_transform, &relative) || relative == 0) return false;
    std::uintptr_t found = 0;
    if (!ReadValue(relative + off_m_transform, &found) || found == 0) return false;
    // A destroyed managed wrapper still has a reachable address but a null native pointer, and
    // calling a method on it faults inside the runtime.
    if (!UnityObjectAlive(found)) return false;
    *transform = found;
    return true;
}

namespace { namespace unity_adapter_transform_detail {

// Call `set_position` or `set_eulerAngles` with a managed `Vector3`.
//
// A MANAGED `Vector3`, NOT A STACK ONE: `il2cpp_runtime_invoke` unboxes each entry of the
// argument array, so a stack `float[3]` would have the runtime read a managed object header off
// the stack and then copy from wherever that header pointed.  The box comes from the runtime's
// own allocator and is filled through `il2cpp_object_unbox`, which is the payload offset the
// runtime itself reports -- no layout is assumed.
//
// The `float` narrowing is deliberate and happens ONCE here: `Vector3` is three floats, so a
// `double` target is narrowed at the point of the call, and a caller that verifies the result
// re-reads through `get_position` and compares.
bool InvokeSetter(std::uintptr_t setter, std::uintptr_t transform, const double values[3]) {
    if (setter == 0 || transform == 0 || values == nullptr) return false;
    const il2cpp::Api& api = il2cpp::Functions();
    if (api.il2cpp_runtime_invoke == nullptr || api.il2cpp_object_new == nullptr ||
        api.il2cpp_object_unbox == nullptr) {
        return false;
    }
    std::uintptr_t vector3_class = 0;
    {
        std::scoped_lock lock(g_mutex);
        vector3_class = g_binding.vector3_class;
    }
    if (vector3_class == 0) return false;

    il2cpp::Il2CppObject* const boxed =
        api.il2cpp_object_new(reinterpret_cast<il2cpp::Il2CppClass*>(vector3_class));
    if (boxed == nullptr) return false;
    void* const payload = api.il2cpp_object_unbox(boxed);
    if (payload == nullptr) return false;
    auto* const components = static_cast<float*>(payload);
    components[0] = static_cast<float>(values[0]);
    components[1] = static_cast<float>(values[1]);
    components[2] = static_cast<float>(values[2]);
    void* arguments[1]{payload};
    il2cpp::Il2CppObject* exception = nullptr;
    static_cast<void>(api.il2cpp_runtime_invoke(
        reinterpret_cast<const il2cpp::MethodInfo*>(setter),
        reinterpret_cast<il2cpp::Il2CppObject*>(transform), arguments, &exception));
    // A managed exception means the call did not take effect.  It is reported as failure and
    // NOT rethrown: this runs on the game thread, inside the host's tick, where an escaping
    // exception would take the game process down.
    return exception == nullptr;
}

// A raw `Vector3`, used only to carry a return value out of an invocation.
struct Vector3Out {
    float x{};
    float y{};
    float z{};
};

} }  // namespace (adapter internals)
bool ReadUnityTransformPosition(std::uintptr_t transform, double position[3]) {
    ServiceCall adapter_call;
    if (!adapter_call) return false;
    using namespace unity_adapter_transform_detail;
    if (transform == 0 || position == nullptr) return false;
    if (!ResolveUnityTransformBinding()) return false;
    std::uintptr_t get_position = 0;
    {
        std::scoped_lock lock(g_mutex);
        get_position = g_binding.get_position;
    }
    if (get_position == 0) return false;
    const il2cpp::Api& api = il2cpp::Functions();
    if (api.il2cpp_runtime_invoke == nullptr) return false;

    Vector3Out value{};
    il2cpp::Il2CppObject* exception = nullptr;
    il2cpp::Il2CppObject* const returned = api.il2cpp_runtime_invoke(
        reinterpret_cast<const il2cpp::MethodInfo*>(get_position),
        reinterpret_cast<il2cpp::Il2CppObject*>(transform), nullptr, &exception);
    if (exception != nullptr || returned == nullptr) return false;
    // THE RETURN IS A MANAGED OBJECT, AND IT MUST BE UNBOXED.
    //
    // `il2cpp_runtime_invoke` returns a `Il2CppObject*` even for a value type, so the three
    // floats do NOT start at `returned` -- an object header does.  The first version of this
    // function `memcpy`d `sizeof(Vector3Out)` bytes straight out of `returned`, which reads the
    // header as if it were `x`, `y`, `z`.  `il2cpp_object_unbox` is what the runtime itself
    // reports as the payload offset, so it is asked rather than assumed -- the same rule the
    // argument side already follows.
    const void* const payload = api.il2cpp_object_unbox(returned);
    if (payload == nullptr) return false;
    std::memcpy(&value, payload, sizeof(value));
    position[0] = value.x;
    position[1] = value.y;
    position[2] = value.z;
    return true;
}

bool WriteUnityTransformPosition(std::uintptr_t transform, const double position[3]) {
    ServiceCall adapter_call;
    if (!adapter_call) return false;
    using namespace unity_adapter_transform_detail;
    if (!ResolveUnityTransformBinding()) return false;
    std::uintptr_t setter = 0;
    {
        std::scoped_lock lock(g_mutex);
        setter = g_binding.set_position;
    }
    return InvokeSetter(setter, transform, position);
}

bool WriteUnityTransformEuler(std::uintptr_t transform, const double euler_degrees[3]) {
    ServiceCall adapter_call;
    if (!adapter_call) return false;
    using namespace unity_adapter_transform_detail;
    if (!ResolveUnityTransformBinding()) return false;
    std::uintptr_t setter = 0;
    {
        std::scoped_lock lock(g_mutex);
        setter = g_binding.set_euler;
    }
    // Absence is reported by the caller as UNAVAILABLE for rotation, and says nothing about
    // the position half.
    if (setter == 0) {
        SetError("UnityEngine.Transform::set_eulerAngles was not found in this build");
        return false;
    }
    return InvokeSetter(setter, transform, euler_degrees);
}

// --- the published table --------------------------------------------------------------

namespace { namespace unity_adapter_transform_detail {

// The position check the SDK documents.  Kept here rather than in the player service because it
// is a property of the ENGINE call, not of any teleport policy: a non-finite or absurd
// coordinate would travel through `static_cast<float>` into a managed Vector3 and produce
// garbage the caller could not distinguish from a real reading.
bool PositionAllowed(const double position[3]) {
    if (position == nullptr) return false;
    for (int index = 0; index < 3; ++index) {
        if (!std::isfinite(position[index])) return false;
        if (std::fabs(position[index]) > kMaximumCoordinateMagnitude) return false;
    }
    return true;
}

CabbirdStatusV1 CABBIRD_CALL TransformResolve(void*,
                                              const CabbirdUnityTransformResolveRequestV1* request,
                                              CabbirdUnityTransformHandleV1* handle) {
    if (request == nullptr || handle == nullptr) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    if (request->struct_size < sizeof(CabbirdUnityTransformResolveRequestV1)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    std::memset(handle, 0, sizeof(*handle));
    handle->struct_size = sizeof(*handle);
    handle->data = request->data;
    std::uintptr_t transform = 0;
    if (!ResolveUnityTransformFromData(request->data, &transform)) {
        return {CABBIRD_STATUS_V1_NOT_FOUND, 0, {}};
    }
    handle->transform = transform;
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}

CabbirdStatusV1 CABBIRD_CALL TransformPosition(void*,
                                               const CabbirdUnityTransformHandleV1* handle,
                                               CabbirdUnityTransformPositionV1* position) {
    if (handle == nullptr || position == nullptr) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    if (handle->struct_size < sizeof(CabbirdUnityTransformHandleV1) ||
        position->struct_size < sizeof(CabbirdUnityTransformPositionV1)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    double values[3]{};
    if (!ReadUnityTransformPosition(static_cast<std::uintptr_t>(handle->transform), values)) {
        return {CABBIRD_STATUS_V1_NOT_FOUND, 0, {}};
    }
    position->transform = handle->transform;
    position->position[0] = values[0];
    position->position[1] = values[1];
    position->position[2] = values[2];
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}

CabbirdStatusV1 CABBIRD_CALL TransformWrite(void*,
                                            const CabbirdUnityTransformWriteV1* write) {
    if (write == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (write->struct_size < sizeof(CabbirdUnityTransformWriteV1)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    const std::uintptr_t transform = static_cast<std::uintptr_t>(write->transform);
    if (transform == 0) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    const bool wants_position =
        (write->flags & CABBIRD_UNITY_TRANSFORM_WRITE_V1_SET_POSITION) != 0;
    const bool wants_rotation =
        (write->flags & CABBIRD_UNITY_TRANSFORM_WRITE_V1_SET_ROTATION) != 0;
    if (!wants_position && !wants_rotation) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }

    bool position_ok = true;
    bool rotation_ok = true;
    if (wants_position) {
        if (!PositionAllowed(write->position)) {
            return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
        }
        position_ok = WriteUnityTransformPosition(transform, write->position);
    }
    if (wants_rotation) {
        if (!PositionAllowed(write->euler_degrees)) {
            return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
        }
        rotation_ok = WriteUnityTransformEuler(transform, write->euler_degrees);
    }
    // A rotation-only request that this build cannot satisfy is UNAVAILABLE, NOT FAILED: the
    // engine lacks the method, which is a different statement from "the call was refused".
    if (!position_ok || !rotation_ok) {
        const bool rotation_is_the_only_problem = position_ok && wants_rotation && !wants_position;
        return {static_cast<std::uint32_t>(
                    rotation_is_the_only_problem ? CABBIRD_STATUS_V1_UNAVAILABLE
                                                : CABBIRD_STATUS_V1_FAILED),
                0, {}};
    }
    return {CABBIRD_STATUS_V1_OK, 0, {}};
}


} }  // namespace (adapter internals)
const CabbirdUnityTransformServiceV1* UnityTransformServiceTable() {
    ServiceCall adapter_call;
    if (!adapter_call) return nullptr;
    using namespace unity_adapter_transform_detail;
    return UnityAdapterServices::Current()->Endpoint()
        ? &UnityAdapterServices::Current()->Endpoint()->transform : nullptr;
}



}  // namespace cabbird

#undef g_binding
#undef g_last_error
#undef g_mutex


// All SDK tables are initialized, published and revoked by one adapter generation.
// The component implementations above never publish independent service objects.
namespace cabbird {
namespace {

void ServiceEndpoint::InitializeTables() {
    dump = {sizeof(dump), CABBIRD_UNITY_DUMP_SERVICE_V1_VERSION, this,
        &ServiceEntry<&unity_adapter_dump_detail::DumpRun>,
        &ServiceEntry<&unity_adapter_dump_detail::DumpState>,
        &ServiceEntry<&unity_adapter_dump_detail::DumpData>,
        &ServiceEntry<&unity_adapter_dump_detail::DumpSize>,
        // Appended, not inserted: an out-of-tree plugin indexes this table by POSITION through
        // the SDK header it was built with, so the four original slots keep their offsets and
        // the metadata pair lands after them.
        &ServiceEntry<&unity_adapter_dump_detail::MetadataData>,
        &ServiceEntry<&unity_adapter_dump_detail::MetadataSize>};
    overlay = {sizeof(overlay), CABBIRD_UNITY_OVERLAY_SERVICE_V1_VERSION, this,
        &ServiceEntry<&unity_adapter_overlay_detail::OverlaySubscribe>,
        &ServiceEntry<&unity_adapter_overlay_detail::OverlayUnsubscribe>};
    transform = {sizeof(transform), CABBIRD_UNITY_TRANSFORM_SERVICE_V1_VERSION, this,
        &ServiceEntry<&unity_adapter_transform_detail::TransformResolve>,
        &ServiceEntry<&unity_adapter_transform_detail::TransformPosition>,
        &ServiceEntry<&unity_adapter_transform_detail::TransformWrite>};
    entities = {sizeof(entities), CABBIRD_UNITY_ENTITIES_SERVICE_V1_VERSION, this,
        &ServiceEntry<&unity_adapter_entities_detail::EntityGeneration>,
        &ServiceEntry<&unity_adapter_entities_detail::EntityCount>,
        &ServiceEntry<&unity_adapter_entities_detail::EntityAt>,
        &ServiceEntry<&unity_adapter_entities_detail::EntityLookup>,
        &ServiceEntry<&unity_adapter_entities_detail::EntitiesCameraPosition>,
        &ServiceEntry<&unity_adapter_entities_detail::EntityCamera>,
        &ServiceEntry<&unity_adapter_entities_detail::EntitySetWantLabels>};
    player = {sizeof(player), CABBIRD_UNITY_PLAYER_SERVICE_V1_VERSION, this,
        &ServiceEntry<&unity_adapter_player_detail::PlayerSnapshot>,
        &ServiceEntry<&unity_adapter_player_detail::PlayerWritePosition>,
        &ServiceEntry<&unity_adapter_player_detail::PlayerWriteState>};
    teleport = {sizeof(teleport), CABBIRD_UNITY_PLAYER_TELEPORT_SERVICE_V1_VERSION, this,
        &ServiceEntry<&unity_adapter_teleport_detail::TeleportEntryPosition>,
        &ServiceEntry<&unity_adapter_teleport_detail::TeleportEntryState>};
    il2cpp = {sizeof(il2cpp), CABBIRD_IL2CPP_SERVICE_V1_VERSION, this,
        &ServiceEntry<&unity_adapter_il2cpp_detail::WithRuntime>,
        &ServiceEntry<&unity_adapter_il2cpp_detail::ReleaseHandles>};
}

struct ServiceTableView {
    const char* id;
    std::uint32_t version;
    const void* table;
};
std::array<ServiceTableView, 7> EndpointTables(const ServiceEndpoint& endpoint) noexcept {
    return {{{CABBIRD_UNITY_DUMP_SERVICE_V1_ID, endpoint.dump.service_version, &endpoint.dump},
             {CABBIRD_UNITY_OVERLAY_SERVICE_V1_ID, endpoint.overlay.service_version, &endpoint.overlay},
             {CABBIRD_UNITY_TRANSFORM_SERVICE_V1_ID, endpoint.transform.service_version, &endpoint.transform},
             {CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID, endpoint.entities.service_version, &endpoint.entities},
             {CABBIRD_UNITY_PLAYER_SERVICE_V1_ID, endpoint.player.service_version, &endpoint.player},
             {CABBIRD_UNITY_PLAYER_TELEPORT_SERVICE_V1_ID, endpoint.teleport.service_version, &endpoint.teleport},
             {CABBIRD_IL2CPP_SERVICE_V1_ID, endpoint.il2cpp.service_version, &endpoint.il2cpp}}};
}

bool ServiceEndpoint::Publish() {
    for (const auto& entry : EndpointTables(*this)) {
        if (!ProcessAdapterServices().Publish(entry.id, entry.version, entry.table, {},
                                              shared_from_this())) return false;
    }
    return true;
}

bool ServiceEndpoint::RevokeUntil(std::chrono::steady_clock::time_point deadline) noexcept {
    bool revoked = true;
    for (const auto& entry : EndpointTables(*this)) {
        if (ProcessAdapterServices().RevokeUntil(entry.id, entry.table, deadline) ==
            AdapterServiceRegistry::RevokeResult::TimedOut) revoked = false;
    }
    return revoked;
}

}  // namespace
}  // namespace cabbird
