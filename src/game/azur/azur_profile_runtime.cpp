#include "cabbird/unitymem_compat.hpp"
#include "cabbird/unity_profile_runtime.hpp"
#include "cabbird/unity_build_profile.hpp"
#include "cabbird/artifact_crypto.hpp"
#include "cabbird/dispatch_tick_hook.hpp"


#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cabbird {
namespace {

std::filesystem::path ModulePath(HMODULE module) {
    if (module == nullptr) module = GetModuleHandleW(nullptr);
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return buffer;
}

std::string Quote(std::string_view value) {
    std::string result{"\""};
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(character); break;
        }
    }
    result.push_back('"');
    return result;
}

std::string HexAddress(std::uintptr_t address) {
    if (address == 0) return {};
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << address;
    return stream.str();
}

// The four states the diagnostics JSON reports.  They used to be upstream's
// ProfileResolutionState, which came from the byte-pattern resolver; with that layer gone the
// only thing left to describe is the profile document and the bindings the host performed, so
// the enum lives here and keeps the same four wire strings.
enum class UnityProfileState : std::uint8_t {
    NoProfile,
    ProfileLoaded,
    Degraded,
    Ready,
};

std::string_view ResolutionStateName(UnityProfileState state) noexcept {
    switch (state) {
    case UnityProfileState::NoProfile: return "no-profile";
    case UnityProfileState::ProfileLoaded: return "profile-loaded";
    case UnityProfileState::Degraded: return "degraded";
    case UnityProfileState::Ready: return "ready";
    }
    return "unknown";
}

}  // namespace

class UnityProfileRuntime::Impl final {
public:
    explicit Impl(UnityProfileRuntimeOptions options)
        : options_(std::move(options)) {
        options_.runtime_root = std::filesystem::absolute(options_.runtime_root);
    }

    bool Start(std::stop_token stop_token) {
        std::scoped_lock lock(mutex_);
        if (started_ || stopping_ || adapter_ || stop_token.stop_requested()) return false;
        const auto startup_deadline = std::chrono::steady_clock::now() +
            (std::max)(options_.frame_clock_budget, std::chrono::milliseconds::zero());
        diagnostics_.clear();
        // THE PROFILE DOCUMENT, loaded here and not in the adapter.
        //
        // `profiles/unity-build-profiles.json` is the document the frame clock resolves the
        // game's dispatcher through, and it is the only per-build profile data this tree
        // ships.  Loading it here is what makes the compatibility page's identity fields
        // (build id, source, hash) and the frame clock's own evidence come from one document.
        //
        // An upstream `BuildProfile` layer used to be scanned here as well: a different schema
        // (byte-pattern symbols + features) read out of `<profile_directory>/<game_id>/`, which
        // no file in this repository provides.  Its scan therefore always came back empty, its
        // resolution fed nothing but the diagnostics JSON, and it is deleted -- a second,
        // always-empty answer to "which build am I on?" is worse than no second answer.
        unity_profile_.reset();
        unity_profile_error_.clear();
        unity_profile_hash_.clear();
        unity_profile_path_ = DefaultUnityBuildProfilePath(options_.runtime_root);
        {
            std::string unity_profile_error;
            auto unity_profile =
                UnityBuildProfile::LoadFromFile(unity_profile_path_, &unity_profile_error);
            if (unity_profile.Valid()) {
                unity_profile_hash_ = Sha256FileHex(unity_profile_path_);
                unity_profile_ = std::move(unity_profile);
                diagnostics_.push_back(
                    "unity profile loaded: " + unity_profile_path_.string() +
                    " id=" + unity_profile_->BuildId());
            } else {
                unity_profile_error_ = std::move(unity_profile_error);
                diagnostics_.push_back("unity profile unavailable: " + unity_profile_error_);
            }
        }
        module_path_ = ModulePath(options_.game_module);
        // `options_.snapshot_sampling` is passed rather than ignored, which is the difference
        // between a configuration key and a knob: `[Performance] EntitySnapshotTickInterval` is
        // the entity walk's sampling divisor, and the walk costs ~213 ms of the game thread when
        // it runs on every tick.
        auto adapter = std::make_shared<UnityAdapter>(
            DefaultUnityBuildProfilePath(options_.runtime_root), nullptr,
            options_.snapshot_sampling);
        if (!adapter->StartServices()) {
            diagnostics_.push_back("adapter service publication failed");
            return false;
        }
        adapter_ = adapter;
        if (options_.start_frame_clock) {
            const auto remaining = (std::max)(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    startup_deadline - std::chrono::steady_clock::now()),
                std::chrono::milliseconds::zero());
            if (!adapter->Start(UnityAdapter::TickCallback{}, remaining,
                                options_.frame_clock_factory, stop_token)) {
                // Unknown/missing game bindings must not suppress the host UI.
                diagnostics_.push_back("frame clock unavailable: " + adapter->LastError());
            }
        }
        if (stop_token.stop_requested()) {
            if (adapter->Stop(std::chrono::milliseconds::zero())) adapter_.reset();
            return false;
        }
        started_ = true;
        return true;
    }

    bool Stop(std::chrono::milliseconds timeout) noexcept {
        std::unique_lock lock(mutex_);
        if (stopping_) return false;
        if (!adapter_) {
            started_ = false;
            return true;
        }
        started_ = false;
        stopping_ = true;
        const auto adapter = adapter_;
        // An in-flight tick may query profile diagnostics. Never drain it while
        // holding the profile mutex. Retain timed-out ownership for a later Stop.
        lock.unlock();
        const bool drained = adapter->Stop(timeout);
        lock.lock();
        if (drained) adapter_.reset();
        stopping_ = false;
        return drained;
    }

    bool Started() const noexcept {
        std::scoped_lock lock(mutex_);
        return started_;
    }

    std::shared_ptr<UnityAdapter> Adapter() const {
        std::scoped_lock lock(mutex_);
        return adapter_;
    }

    UnityProfileEvidenceSnapshot Evidence() const {
        std::scoped_lock lock(mutex_);
        UnityProfileEvidenceSnapshot snapshot;
        snapshot.unity_profile_path = unity_profile_path_;
        snapshot.unity_profile_hash = unity_profile_hash_;
        snapshot.unity_profile_error = unity_profile_error_;
        snapshot.unity_profile = unity_profile_;
        snapshot.unity_methods = UnityMethodEvidenceLocked();
        return snapshot;
    }

    std::vector<HookRecordView> Hooks() const {
        std::scoped_lock lock(mutex_);
        // No hook is registered by this layer: the frame clock owns the one hook the runtime
        // installs, and it reports through the adapter.  The accessor stays because it is part
        // of the framework contract src/runtime/core_main.cpp consumes.
        static_cast<void>(lock);
        return {};
    }


    std::string DiagnosticsJson() const {
        std::scoped_lock lock(mutex_);
        const auto methods = UnityMethodEvidenceLocked();
        const UnityProfileState state = UnityResolutionStateLocked(methods);
        std::string json = "{\"ok\":true,\"state\":" +
            Quote(ResolutionStateName(state));
        json += ",\"adapterOwner\":\"unity-profile\"";
        json += ",\"adapterServicesStarted\":" +
            std::string(adapter_ && adapter_->ServicesStarted() ? "true" : "false");
        json += ",\"frameClockStarted\":" +
            std::string(adapter_ && adapter_->Started() ? "true" : "false");
        json += ",\"frameClockError\":" + Quote(adapter_ ? adapter_->LastError() : std::string{});
        // Build identity and profile identity come from the profile document, which is the
        // document the frame clock actually bound through.
        json += ",\"buildId\":" + Quote(
            unity_profile_ ? unity_profile_->BuildId() : std::string{});
        json += ",\"modulePath\":" + Quote(module_path_.string());
        json += ",\"fingerprintAvailable\":false";
        json += ",\"profileHash\":" + Quote(unity_profile_hash_);
        json += ",\"profileChannel\":" + Quote(
            unity_profile_ ? std::string{"bundled"} : std::string{});
        json += ",\"profileSource\":" + Quote(
            unity_profile_ ? unity_profile_path_.string() : std::string{});
        json += ",\"profileError\":" + Quote(unity_profile_error_);
        json += ",\"methods\":[";
        for (std::size_t index = 0; index < methods.size(); ++index) {
            if (index != 0) json.push_back(',');
            json += "{\"id\":" + Quote(methods[index].key) +
                ",\"bound\":" + (methods[index].bound ? std::string("true") : std::string("false")) +
                ",\"address\":" + Quote(methods[index].bound
                    ? HexAddress(methods[index].address) : std::string{}) +
                ",\"how\":" + Quote(methods[index].how) + "}";
        }
        json += "],\"diagnostics\":[";
        bool first = true;
        for (const auto& diagnostic : diagnostics_) {
            if (!first) json.push_back(',');
            first = false;
            json += Quote(diagnostic);
        }
        json += "]}";
        return json;
    }

private:
    // Caller holds mutex_.  One entry per method the profile document declares.  The frame
    // clock binds exactly one of them (`DispatchTickHook::MethodKey()`); reporting that the
    // others are not attempted is the point -- a matrix row must never imply a binding that
    // nobody performed.
    [[nodiscard]] std::vector<UnityProfileMethodEvidence> UnityMethodEvidenceLocked() const {
        std::vector<UnityProfileMethodEvidence> methods;
        if (!unity_profile_) return methods;
        const bool clock_bound = adapter_ && adapter_->Started();
        const std::string clock_error = adapter_ ? adapter_->LastError() : std::string{};
        const std::string clock_how = adapter_ ? adapter_->ResolutionHow() : std::string{};
        const std::uintptr_t clock_address = adapter_ ? adapter_->DispatchAddress() : 0;
        for (const auto& key : unity_profile_->MethodKeys()) {
            UnityProfileMethodEvidence method;
            method.key = key;
            if (key == DispatchTickHook::MethodKey()) {
                method.bound = clock_bound;
                method.address = clock_bound ? clock_address : 0;
                method.how = clock_bound
                    ? (clock_how.empty() ? std::string{"bound"} : clock_how)
                    : (clock_error.empty() ? std::string{"the frame clock is not installed"}
                                           : clock_error);
            } else {
                method.attempted = false;
                method.how = "documented in the profile; this host binds only the frame clock";
            }
            methods.push_back(std::move(method));
        }
        return methods;
    }

    // The state the diagnostics JSON and the compatibility page report: it describes the
    // profile document and the bindings the host really performed.
    //
    // Computed over the methods the host ATTEMPTS, not over every record in the document.  A
    // record the host resolves lazily (the entity walk's `unity.transform.get_position`) is
    // documented data, and counting it as an unbound binding made a healthy profile report
    // `degraded` -- a status that means "some bindings failed" being used for "this address is
    // recorded here".  The two are different claims and only the first is a defect.
    [[nodiscard]] UnityProfileState UnityResolutionStateLocked(
        const std::vector<UnityProfileMethodEvidence>& methods) const {
        if (!unity_profile_) return UnityProfileState::NoProfile;
        if (methods.empty()) return UnityProfileState::ProfileLoaded;
        std::ptrdiff_t attempted = 0;
        std::ptrdiff_t bound = 0;
        for (const UnityProfileMethodEvidence& method : methods) {
            if (!method.attempted) continue;
            ++attempted;
            if (method.bound) ++bound;
        }
        if (attempted == 0) return UnityProfileState::ProfileLoaded;
        if (bound == attempted) return UnityProfileState::Ready;
        return bound == 0 ? UnityProfileState::ProfileLoaded : UnityProfileState::Degraded;
    }

    UnityProfileRuntimeOptions options_;
    mutable std::mutex mutex_;
    bool started_{};
    bool stopping_{};
    std::filesystem::path module_path_;
    // The profile document (see Start): the frame clock's binding source and the
    // compatibility page's identity fields.
    std::filesystem::path unity_profile_path_;
    std::string unity_profile_hash_;
    std::string unity_profile_error_;
    std::optional<UnityBuildProfile> unity_profile_;
    std::shared_ptr<UnityAdapter> adapter_;
    std::vector<std::string> diagnostics_;
};

UnityProfileRuntime::UnityProfileRuntime(UnityProfileRuntimeOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
UnityProfileRuntime::~UnityProfileRuntime() {
    static_cast<void>(Stop(std::chrono::milliseconds::zero()));
}
bool UnityProfileRuntime::Start(std::stop_token stop_token) noexcept {
    try {
        return impl_->Start(stop_token);
    } catch (...) {
        static_cast<void>(impl_->Stop(std::chrono::milliseconds::zero()));
        return false;
    }
}
bool UnityProfileRuntime::Stop(std::chrono::milliseconds timeout) noexcept {
    return impl_->Stop(timeout);
}
bool UnityProfileRuntime::Started() const noexcept { return impl_->Started(); }
std::shared_ptr<UnityAdapter> UnityProfileRuntime::Adapter() const { return impl_->Adapter(); }
UnityProfileEvidenceSnapshot UnityProfileRuntime::Evidence() const { return impl_->Evidence(); }
std::string UnityProfileRuntime::DiagnosticsJson() const { return impl_->DiagnosticsJson(); }
std::vector<HookRecordView> UnityProfileRuntime::Hooks() const { return impl_->Hooks(); }

}  // namespace cabbird
