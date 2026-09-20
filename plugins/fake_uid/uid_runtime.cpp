// Unity adaptation, not a UE5 FakeUID port:
// C:\AzurPromilia-dump.cs:414134-414153 records
// ModuleNetworkInfoView.m_txtUid as TMPro.TMP_Text. Resolve that field by name;
// the dump's 0x40 is evidence, never an executable offset.
#include "uid_runtime.hpp"
#include "cabbird/sdk/il2cpp.hpp"
#include <array>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fake_uid {
namespace {
using namespace cabbird::sdk::il2cpp;
using Clock = std::chrono::steady_clock;

struct Binding {
    bool resolved{};
    Il2CppClass* view{};
    Il2CppClass* tmp{};
    FieldInfo* uid{};
    const MethodInfo* find{};
    const MethodInfo* alive{};
};
struct Target {
    Il2CppGCHandle view{};
    Il2CppGCHandle text{};
    const MethodInfo* get{};
    const MethodInfo* set{};
    std::string original;
    std::string original_uid;
    std::string last_written;
    std::string prefix;
};
struct State {
    std::mutex mutex;
    Settings request{};
    Status published{};
    // Everything below is exclusively owned by the game domain.
    Binding binding;
    std::vector<Target> targets;
    Clock::time_point next_discovery{};
    Clock::time_point next_check{};
    std::string active_uid;
    std::string active_display;
    bool active_hide{};
    Clock::time_point next_runtime_retry{};
    std::uint64_t writes{};
};
State& GetState() { static State state; return state; }

// Invalid strings must not reach the runtime. Reject TMP markup as well: this
// initial version does not alter richText or the widget's layout properties.
bool ValidDisplay(const char* value, std::size_t capacity) {
    const void* end = std::memchr(value, 0, capacity);
    if (!end) return false;
    const auto bytes = static_cast<int>(static_cast<const char*>(end) - value);
    if (bytes == 0) return false;
    wchar_t wide[1025]{};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, bytes, wide, 1025);
    if (n == 0) return false;
    unsigned scalars = 0;
    for (int i = 0; i < n; ++i) {
        const unsigned ch = static_cast<unsigned>(wide[i]);
        if (ch < 0x20 || (ch >= 0x7f && ch <= 0x9f) || ch == 0x2028 || ch == 0x2029 ||
            ch == '<' || ch == '>') return false;
        if (ch < 0xdc00 || ch > 0xdfff) ++scalars;
    }
    return scalars <= 256;
}
bool Resolve(const Context& runtime, Binding& b, std::string& reason) {
    const auto& api = runtime.Functions();
    if (!api.il2cpp_gchandle_new || !api.il2cpp_gchandle_get_target ||
        !api.il2cpp_gchandle_free || !api.il2cpp_type_get_object ||
        !api.il2cpp_runtime_invoke || !api.il2cpp_object_unbox || !api.il2cpp_string_new ||
        !api.il2cpp_field_get_value || !api.il2cpp_class_is_assignable_from ||
        !api.il2cpp_array_length) {
        reason = "Required IL2CPP APIs unavailable";
        return false;
    }
    // This game has no Assembly-CSharp image at runtime. Resolve the exact
    // fully-qualified type across all loaded images instead of guessing which
    // Azur/Lens assembly owns it.
    b.view = runtime.FindClassAnyImage("Lens.Gameplay.UI.Watermark", "ModuleNetworkInfoView");
    b.tmp = runtime.FindClass("Unity.TextMeshPro.dll", "TMPro", "TMP_Text");
    auto* object = runtime.FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Object");
    if (!b.view || !b.tmp || !object) {
        reason = "ModuleNetworkInfoView / TMP_Text / UnityEngine.Object not loaded";
        return false;
    }
    b.uid = runtime.FindField(b.view, "m_txtUid");
    if (!b.uid || runtime.TypeName(api.il2cpp_field_get_type(b.uid)) != "TMPro.TMP_Text") {
        reason = "m_txtUid is absent or has an unexpected type; refusing access";
        return false;
    }
    auto* resources = runtime.FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Resources");
    b.find = resources
        ? runtime.FindMethodBySignature(resources, "FindObjectsOfTypeAll", {"System.Type"})
        : nullptr;
    b.alive = runtime.FindMethod(object, "op_Implicit", 1);
    if (!b.find || !b.alive ||
        runtime.TypeName(api.il2cpp_method_get_param(b.find, 0)) != "System.Type") {
        reason = "Unity object discovery overload unavailable";
        return false;
    }
    b.resolved = true;
    return true;
}
bool Alive(const Context& runtime, const Binding& b, Il2CppObject* object) {
    if (!object) return false;
    void* args[]{object};
    Il2CppObject* result = nullptr;
    if (!runtime.Invoke(b.alive, nullptr, args, &result) || !result) return false;
    const void* value = runtime.Functions().il2cpp_object_unbox(result);
    return value && *static_cast<const bool*>(value);
}
bool ReadText(const Context& runtime, const Target& t, Il2CppObject* object, std::string& text) {
    Il2CppObject* result = nullptr;
    if (!runtime.Invoke(t.get, object, nullptr, &result) || !result) return false;
    // Bound reads even if this exact widget is unexpectedly used for a large message.
    const auto& api = runtime.Functions();
    const auto size = api.il2cpp_string_length(result);
    if (size < 0 || size > 1152) return false;
    auto value = runtime.ReadString(result, 1152);
    if (!value) return false;
    text = std::move(*value);
    return true;
}
bool WriteText(const Context& runtime, const Target& t, Il2CppObject* object, const std::string& text) {
    auto* value = runtime.Functions().il2cpp_string_new(text.c_str());
    if (!value) return false;
    void* args[]{value};
    Il2CppObject* ignored = nullptr;
    return runtime.Invoke(t.set, object, args, &ignored);
}
void Release(const Context& runtime, Target& target) {
    auto& api = runtime.Functions();
    if (target.view) api.il2cpp_gchandle_free(target.view);
    if (target.text) api.il2cpp_gchandle_free(target.text);
    target.view = target.text = 0;
}
bool MatchesOriginal(const std::string& text, const std::string& uid, std::string& prefix) {
    // Only exact numeric value or an exact UID prefix; never replace substrings
    // in chat, account models, other widgets, longer IDs, or network payloads.
    auto begin = text.find_first_not_of(" \t");
    auto end = text.find_last_not_of(" \t");
    if (begin == std::string::npos) return false;
    const auto trimmed = text.substr(begin, end - begin + 1);
    if (trimmed == uid) { prefix = text.substr(0, begin); return true; }
    if (trimmed.size() < uid.size() || trimmed.compare(trimmed.size() - uid.size(), uid.size(), uid))
        return false;
    std::string head = trimmed.substr(0, trimmed.size() - uid.size());
    head.erase(std::remove_if(head.begin(), head.end(), [](char c) { return c == ' ' || c == '\t'; }), head.end());
    if (head != "UID" && head != "UID:" && head != "UID\xef\xbc\x9a") return false;
    prefix = text.substr(0, end + 1 - uid.size());
    return true;
}
bool ExtractOriginalUid(const std::string& text, std::string& uid, std::string& prefix) {
    // This is already the dedicated UID field. Find a contiguous decimal run
    // anywhere in its rendered text; labels, rich-text tags, punctuation and
    // trailing text must not prevent discovery.
    std::size_t best_begin = std::string::npos;
    std::size_t best_end = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] < '0' || text[i] > '9') { ++i; continue; }
        const auto begin = i++;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        if (best_begin == std::string::npos || i - begin > best_end - best_begin) {
            best_begin = begin;
            best_end = i;
        }
    }
    if (best_begin == std::string::npos || best_end - best_begin == 0 ||
        best_end - best_begin > 20) return false;
    uid = text.substr(best_begin, best_end - best_begin);
    prefix = text.substr(0, best_begin);
    return true;
}
bool Restore(const Context& runtime, State& s) {
    bool complete = true;
    for (auto it = s.targets.begin(); it != s.targets.end();) {
        auto* text = runtime.Functions().il2cpp_gchandle_get_target(it->text);
        std::string current;
        bool done = !Alive(runtime, s.binding, text) || it->last_written.empty();
        if (!done && ReadText(runtime, *it, text, current)) {
            // Do not overwrite the game's newer text (e.g. a different account).
            done = current != it->last_written || WriteText(runtime, *it, text, it->original);
        }
        if (done) {
            Release(runtime, *it);
            it = s.targets.erase(it);
        } else { complete = false; ++it; }
    }
    return complete;
}
void Publish(State& s, bool enabled, unsigned applied, std::string_view message) {
    Status next{};
    next.enabled = enabled;
    next.tracked = static_cast<std::uint32_t>(s.targets.size());
    next.applied = applied;
    next.writes = s.writes;
    if (!s.targets.empty())
        std::snprintf(next.original_text, sizeof(next.original_text), "%s", s.targets.front().original.c_str());
    if (!s.targets.empty())
        std::snprintf(next.original_uid, sizeof(next.original_uid), "%s", s.targets.front().original_uid.c_str());
    std::snprintf(next.message, sizeof(next.message), "%.*s",
        static_cast<int>(message.size()), message.data());
    std::scoped_lock lock(s.mutex);
    s.published = next;
}
void Discover(const Context& runtime, State& s, std::string& reason) {
    const auto& api = runtime.Functions();
    auto* type = api.il2cpp_type_get_object(api.il2cpp_class_get_type(s.binding.view));
    if (!type) { reason = "Could not resolve the watermark System.Type"; return; }
    void* args[]{type};
    Il2CppObject* array = nullptr;
    if (!runtime.Invoke(s.binding.find, nullptr, args, &array) || !array) {
        reason = "Watermark discovery failed"; return;
    }
    const auto array_handle = api.il2cpp_gchandle_new(array, false);
    if (!array_handle) { reason = "Could not retain discovery result"; return; }
    // Scoped GC root for the discovery result; all roots are released on this game thread.
    struct Root {
        const CabbirdIl2CppApiV1& api;
        Il2CppGCHandle handle;
        ~Root() { api.il2cpp_gchandle_free(handle); }
    } root{api, array_handle};
    if (!runtime.ArrayElementBase()) { reason = "Invalid runtime array header size"; return; }
    const auto count = api.il2cpp_array_length(array);
    if (count > 32) { reason = "More than 32 watermark views; refusing an unbounded scan"; return; }
    for (std::uint32_t i = 0; i < count && s.targets.size() < 8; ++i) {
        Il2CppObject* view = nullptr;
        if (!runtime.ArrayReferenceAt(array, i, &view) ||
            !Alive(runtime, s.binding, view)) continue;
        if (api.il2cpp_object_get_class(view) != s.binding.view) continue;
        Il2CppObject* text = nullptr;
        api.il2cpp_field_get_value(view, s.binding.uid, &text);
        if (!Alive(runtime, s.binding, text)) continue;
        if (std::any_of(s.targets.begin(), s.targets.end(), [&](const Target& t) {
            return api.il2cpp_gchandle_get_target(t.text) == text;
        })) continue;
        auto* cls = api.il2cpp_object_get_class(text);
        if (!api.il2cpp_class_is_assignable_from(s.binding.tmp, cls)) continue;
        Target target;
        target.get = runtime.FindMethod(cls, "get_text", 0);
        target.set = runtime.FindMethod(cls, "set_text", 1);
        if (!target.get || !target.set || !ReadText(runtime, target, text, target.original))
            continue;
        // The field itself is the identity boundary. Do not reject the target
        // because its rendered text uses a localized label, TMP markup, or a
        // non-decimal format. UID extraction is display-only.
        ExtractOriginalUid(target.original, target.original_uid, target.prefix);
        target.view = api.il2cpp_gchandle_new(view, false);
        target.text = api.il2cpp_gchandle_new(text, false);
        if (!target.view || !target.text) { Release(runtime, target); continue; }
        s.targets.push_back(std::move(target));
    }
    reason = s.targets.empty() ? "No matching watermark UID (waiting for HUD / check original UID)" : "";
}
} // namespace

bool Configure(const Settings& settings, std::string& error) {
    if (settings.enabled && !ValidDisplay(settings.display_uid, sizeof(settings.display_uid))) {
        error = "Use 1-256 single-line UTF-8 characters (no < > markup).";
        return false;
    }
    auto& s = GetState();
    std::scoped_lock lock(s.mutex);
    s.request = settings;
    error.clear();
    return true;
}
Status Snapshot() {
    auto& s = GetState();
    std::scoped_lock lock(s.mutex);
    return s.published;
}
bool Shutdown(const CabbirdIl2CppServiceV1* service) {
    // The host has drained ordinary callbacks. Release roots using the generic
    // lifecycle-safe entry; never call a Unity setter from the stop worker.
    auto& s = GetState();
    if (!s.targets.empty()) {
        if (!service || !service->release_handles) return false;
        std::array<Il2CppGCHandle, 16> handles{};
        std::size_t count = 0;
        for (const auto& target : s.targets) {
            if (target.view) handles[count++] = target.view;
            if (target.text) handles[count++] = target.text;
        }
        if (service->release_handles(service->user, handles.data(), count).code != CABBIRD_STATUS_V1_OK)
            return false;
        s.targets.clear();
    }
    std::scoped_lock lock(s.mutex);
    s.request.enabled = false;
    return true;
}
static void TickBound(const Context& runtime) {
    auto& s = GetState();
    const auto now = Clock::now();
    Settings request{};
    {
        std::scoped_lock lock(s.mutex);
        request = s.request;
    }
    if (!s.binding.resolved && now < s.next_check) return;
    // Poll retained widgets every game tick. A 100 ms gate left game-written
    // original text visible for several frames. Discovery remains throttled.
    // Keep next_check only for failed metadata resolution backoff below.
    s.next_check = {};
    std::string reason;
    if (!s.binding.resolved && !Resolve(runtime, s.binding, reason)) {
        // Avoid repeating failed metadata walks at the game tick rate.
        s.next_check = now + std::chrono::seconds(2);
        s.active_display = request.display_uid;
        s.active_hide = request.hide_prefix;
        Publish(s, request.enabled != 0, 0, reason);
        return;
    }
    if (!request.enabled) {
        const bool has_override = std::any_of(s.targets.begin(), s.targets.end(),
            [](const Target& target) { return !target.last_written.empty(); });
        if (has_override) {
            if (!Restore(runtime, s)) {
                Publish(s, false, 0, "Restore pending; unreadable widget will be retried");
                return;
            }
            s.next_discovery = {};
        }
        if (now >= s.next_discovery) {
            // A widget can exist before the game fills its UID. Refresh retained
            // originals without writing, and drop replaced/destroyed widgets.
            const auto& api = runtime.Functions();
            for (auto it = s.targets.begin(); it != s.targets.end();) {
                auto* view = api.il2cpp_gchandle_get_target(it->view);
                auto* text = api.il2cpp_gchandle_get_target(it->text);
                Il2CppObject* live_field = nullptr;
                if (Alive(runtime, s.binding, view))
                    api.il2cpp_field_get_value(view, s.binding.uid, &live_field);
                if (!Alive(runtime, s.binding, text) || live_field != text) {
                    Release(runtime, *it);
                    it = s.targets.erase(it);
                    continue;
                }
                std::string current;
                if (ReadText(runtime, *it, text, current)) {
                    it->original = std::move(current);
                    it->original_uid.clear();
                    it->prefix.clear();
                    ExtractOriginalUid(it->original, it->original_uid, it->prefix);
                }
                ++it;
            }
            Discover(runtime, s, reason);
            s.next_discovery = now + std::chrono::seconds(2);
        }
        Publish(s, false, 0, s.targets.empty()
            ? "Waiting for the game's watermark UID widget"
            : "Original UID captured");
        return;
    }
    s.active_display = request.display_uid;
    s.active_hide = request.hide_prefix;
    // Re-discovery is bounded and infrequent, not a full text-object scan per frame.
    if (now >= s.next_discovery) {
        Discover(runtime, s, reason);
        s.next_discovery = now + std::chrono::seconds(2);
    }
    unsigned applied = 0;
    for (auto it = s.targets.begin(); it != s.targets.end();) {
        const auto& api = runtime.Functions();
        auto* view = api.il2cpp_gchandle_get_target(it->view);
        auto* text = api.il2cpp_gchandle_get_target(it->text);
        if (!Alive(runtime, s.binding, text)) {
            Release(runtime, *it); it = s.targets.erase(it); continue;
        }
        Il2CppObject* live_field = nullptr;
        if (Alive(runtime, s.binding, view)) api.il2cpp_field_get_value(view, s.binding.uid, &live_field);
        std::string current;
        if (!ReadText(runtime, *it, text, current)) { reason = "UID text read failed"; ++it; continue; }
        if (live_field != text) {
            // A rebuilt HUD must not leave our text in a detached but live widget.
            if (current == it->last_written && !WriteText(runtime, *it, text, it->original)) {
                reason = "Detached UID widget restoration pending"; ++it; continue;
            }
            Release(runtime, *it); it = s.targets.erase(it); continue;
        }
        std::string prefix;
        if (current != it->last_written) {
            // The dedicated m_txtUid field is already the binding. Capture the
            // game's latest text before applying the display override; do not
            // require a guessed numeric format to keep the binding alive.
            it->original = current;
            it->original_uid.clear();
            it->prefix.clear();
            ExtractOriginalUid(current, it->original_uid, it->prefix);
        }
        const std::string desired = (request.hide_prefix ? "" : it->prefix) + request.display_uid;
        if (current == desired) { ++applied; ++it; continue; }
        // Set last_written only when a successful setter actually changed the text.
        if (!WriteText(runtime, *it, text, desired)) { reason = "TMP text setter failed"; ++it; continue; }
        it->last_written = desired;
        ++s.writes;
        std::string observed;
        if (ReadText(runtime, *it, text, observed) && observed == desired) ++applied;
        else reason = "Setter returned; text readback did not match";
        ++it;
    }
    if (reason.empty()) reason = applied ? "Applied to native UID text (readback matched)" :
        "Waiting for the game's watermark UID widget";
    Publish(s, true, applied, reason);
}
void Tick(const CabbirdIl2CppServiceV1* service) {
    auto& s = GetState();
    bool enabled = false;
    {
        std::scoped_lock lock(s.mutex);
        enabled = s.request.enabled;
    }
    // Reading Original UID must not depend on Apply. TickBound keeps discovery
    // throttled and only writes when applying an override or restoring one.
    const auto now = Clock::now();
    if (now < s.next_runtime_retry) return;
    if (!service || service->struct_size < sizeof(*service) || !service->with_runtime) {
        Publish(s, false, 0, "Generic cabbird.unity.il2cpp service unavailable; update the host");
        s.next_runtime_retry = now + std::chrono::seconds(2);
        return;
    }
    const auto result = service->with_runtime(service->user,
        [](void*, const CabbirdIl2CppApiV1* api) -> CabbirdStatusV1 {
            if (!api || api->struct_size < sizeof(*api) || api->api_version != 1)
                return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
            Context runtime(*api);
            TickBound(runtime);
            return {CABBIRD_STATUS_V1_OK, 0, {}};
        }, nullptr);
    if (result.code != CABBIRD_STATUS_V1_OK) {
        Publish(s, enabled, 0, result.message.data ?
            std::string_view(result.message.data, result.message.size) : "IL2CPP access failed");
        s.next_runtime_retry = now + std::chrono::seconds(2);
    }
}
} // namespace fake_uid
