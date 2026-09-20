// IL2CPP type-system dump -- a plugin, not an ini switch.
//
// WHY THIS IS A PLUGIN
// --------------------
// The dump already existed as code and already had SEH guards around every call that
// can fault.  What it did not have was a caller: the `il2cpp_dump*` keys in
// cabbird_overlay.ini were read by the manual-map probe, which is no longer part of this
// build, so the project shipped a dumper nothing could reach.
//
// The obvious fix would be "turn the ini switches back on", but that is the wrong shape:
// Cabbird hot-reloads plugins, so a dump should be an ACTION taken in
// the current session rather than a switch set before the game starts.  That is not a
// matter of taste:
//
//   * a dump is only useful if it was captured with the game in the right STATE.  The
//     dump this project has was taken at a login screen, contains no battle entities,
//     and is therefore the reason the Entity Overlay binding is blocked.  Re-running the
//     same switches would have reproduced the same gap;
//   * changing an ini costs a full relaunch, and the launch is the expensive,
//     user-owned step of the session;
//   * with hot reload one session can dump at the menu, walk into a fight, dump again,
//     and diff the two -- which is the only way to find out which classes a battle
//     actually instantiates.
//
// WHAT THIS PLUGIN DOES NOT DO
// ----------------------------
// It does not touch IL2CPP, does not read a `dump.cs`, and does not know what a class
// is.  It owns POLICY: which walks to request, over which images, and where the file
// lands.  The host owns the walk, the worker thread, the attachment to the runtime and
// the buffer.  That split is why a plugin can be reloaded between two dumps without the
// dumper being reloaded with it.
//
// WHY IT POLLS INSTEAD OF WAITING
// ------------------------------
// `run` NEVER blocks and NEVER dumps inline, from either thread domain (see the long
// note in src/game/unity/unity_adapter.cpp).  The first call returns PENDING and this
// plugin polls `state` from `on_update`, which the host calls every frame.  That is the
// design: a metadata walk can take seconds, and neither a render callback nor the game
// loop should be blocked by it.

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/services/ui.h"
#include "cabbird/sdk/services/unity.h"

namespace {

using cabbird::sdk::Ok;
using cabbird::sdk::StringView;
using cabbird::sdk::Succeeded;

const char* const kPluginId = "cabbird.unity-dump";
const char* const kPluginName = "IL2CPP Dump";

const CabbirdUiServiceV1* g_ui = nullptr;
const CabbirdUnityDumpServiceV1* g_dump = nullptr;
int g_open = 1;

struct Settings {
    int include_fields = 1;
    int include_methods = 1;
    // Recover the plaintext metadata so a file parser can see the WHOLE image.
    //
    // ON by default, and that default is the fix for a measured defect rather than a
    // preference: the previous artifact had 45,578 method lines and NOT ONE of them from
    // `Assembly-CSharp.dll`, which holds 24,960 of the game's classes -- because the walk
    // was filtered to images whose name contains "Azur" and the game's own main assembly
    // is not one of them.  The metadata blob is the only input that lets Il2CppDumper
    // enumerate everything, and it costs one scan (~6.6 s) that is then cached for the
    // session.
    int recover_metadata = 1;
    // Which images the METHOD walk covers.
    //
    // The default used to be "Azur", chosen when the game's code looked like five
    // `Azur*` assemblies.  It is not: this target also ships the game logic as
    // `Assembly-CSharp.dll` (24,960 classes) plus `Proxima*`/`Lens*`.  A substring list
    // covering all of them costs more minutes but produces the dump that was actually
    // asked for, and the metadata path above is what covers the images nobody listed.
    char image_filter[192] = "Azur,Assembly-CSharp,Proxima,Lens";
    // Absolute, because the host process's working directory is the game's directory,
    // not ours -- a relative path would land somewhere the user does not expect.
    char output_path[260] = "C:\\AzurPromilia-dump.cs";
    // Where the recovered metadata lands.  `global-metadata.decrypted.dat` is the name an
    // external Il2CppDumper run expects when it is handed a blob.
    char metadata_path[260] = "C:\\global-metadata.decrypted.dat";
    int poll_every_frames = 30;
};

Settings g_settings;

// The last outcome, copied out of the service rather than pointing into it: the service
// owns its result and may overwrite it under the plugin's feet.
CabbirdDumpResultV1 g_last_result{};
int g_have_result = 0;
// The last status message the service handed back, copied because the SDK borrows it.
char g_last_message[256] = "";
int g_frames_since_poll = 0;
// Stall detection was removed along with the note it drove -- see the note in `Draw`.
// Deleted rather than left as dead state, because a counter that nothing reads is how the
// next person concludes stall detection still exists.
unsigned long long g_runs_started = 0;
unsigned long long g_runs_finished = 0;

const char* StateName(std::uint32_t state) {
    switch (state) {
        // IDLE is the zero value and means "this service has never been asked to do anything".
        // Without it a first-frame poll of a fresh service read PENDING (then the zero value) and
        // the window announced a dump was running before the user had pressed anything.
        case CABBIRD_DUMP_V1_RESULT_IDLE: return "idle (nothing dumped yet)";
        case CABBIRD_DUMP_V1_RESULT_PENDING: return "running...";
        case CABBIRD_DUMP_V1_RESULT_COMPLETE: return "complete";
        case CABBIRD_DUMP_V1_RESULT_UNAVAILABLE: return "unavailable (runtime not ready)";
        case CABBIRD_DUMP_V1_RESULT_FAILED: return "failed";
        // The walk succeeded and its text is readable through `dump_data`, but no file was
        // written -- no `output_path` was supplied, or the path did not survive conversion.
        // Its own name rather than "complete", because a user who reads "complete" goes looking
        // for a file that does not exist.  See unity.h for why the state exists.
        case CABBIRD_DUMP_V1_RESULT_IN_MEMORY: return "complete (in memory, no file written)";
        default: return "unknown";
    }
}

bool DumpAvailable() {
    return g_dump != nullptr && g_dump->run != nullptr && g_dump->state != nullptr;
}

void StartDump() {
    if (!DumpAvailable()) return;

    CabbirdDumpRequestV1 request{};
    request.struct_size = sizeof(request);
    if (g_settings.include_fields != 0) request.flags |= CABBIRD_DUMP_V1_FIELDS;
    if (g_settings.include_methods != 0) request.flags |= CABBIRD_DUMP_V1_METHODS;
    if (g_settings.recover_metadata != 0) request.flags |= CABBIRD_DUMP_V1_METADATA;
    // 0 = no cap.  A cap is what a first attempt against an unknown build should use, but
    // it is deliberately not exposed as a knob here: the image filter is the bound that
    // matters, and two overlapping limits make "why is my dump short" ambiguous.
    request.max_classes_per_image = 0;
    request.max_methods_per_class = 0;
    request.image_filter = g_settings.image_filter;
    request.image_filter_size = static_cast<std::uint32_t>(std::strlen(g_settings.image_filter));

    // The path is UTF-16 on the service side because Windows paths are, and the field
    // here is UTF-8, so this is a real widening rather than a cast.  A path containing
    // non-ASCII survives it; one that does not convert leaves the request with no output
    // path, and the service then dumps into its own buffer without writing a file --
    // which the window reports as "written: 0 bytes" rather than silently.
    wchar_t wide[std::size(g_settings.output_path)]{};
    const int converted = MultiByteToWideChar(CP_UTF8, 0, g_settings.output_path, -1, wide,
                                              static_cast<int>(std::size(wide)));
    if (converted > 1) {
        request.output_path = wide;
        request.output_path_length = static_cast<std::uint32_t>(converted - 1);
    }

    // The metadata path, widened the same way and for the same reason.  Two separate
    // buffers: one `converted <= 1` must not silently drop the other path, which a shared
    // buffer would do.
    wchar_t wide_metadata[std::size(g_settings.metadata_path)]{};
    const int converted_metadata = MultiByteToWideChar(
        CP_UTF8, 0, g_settings.metadata_path, -1, wide_metadata,
        static_cast<int>(std::size(wide_metadata)));
    if (converted_metadata > 1) {
        request.metadata_path = wide_metadata;
        request.metadata_path_length = static_cast<std::uint32_t>(converted_metadata - 1);
    }

    CabbirdDumpResultV1 result{};
    result.struct_size = sizeof(result);
    // The status is the only place the RUNTIME's own explanation reaches us -- and the
    // one case that needs it most is UNAVAILABLE, where the answer is usually something
    // like "GameAssembly.dll is not loaded yet".  Copying it here (rather than holding
    // the view) is what the SDK's borrowed-lifetime contract requires, and it means a
    // refusal explains itself instead of just showing a state name.
    const CabbirdStatusV1 status = g_dump->run(g_dump->user, &request, &result);
    if (status.message.data != nullptr && status.message.size != 0) {
        const std::size_t count = status.message.size < sizeof(g_last_message) - 1
            ? status.message.size
            : sizeof(g_last_message) - 1;
        std::memcpy(g_last_message, status.message.data, count);
        g_last_message[count] = '\0';
    } else {
        g_last_message[0] = '\0';
    }
    ++g_runs_started;
    g_last_result = result;
    g_have_result = 1;
}

// Recover the metadata ALONE -- no dump.cs walk.
//
// WHY THIS EXISTS AS A SEPARATE BUTTON rather than only as a checkbox on "Dump now":
// the two artifacts answer different questions and have wildly different costs.  The
// blob is one scan of committed memory (~6.6 s, then cached for the session); a dump.cs
// with methods across every image is minutes of walking the live type system, and it can
// only be taken when the game is in a state worth capturing.  A user who wants to run
// Il2CppDumper right now should not have to pay for the walk, and a user mid-walk should
// not have to wait for it to get the blob.
//
// `CABBIRD_DUMP_V1_FIELDS` is deliberately NOT set: no walk is wanted, and the flag is
// what would start one.  The service treats a request with only METADATA as a legal one
// and returns COMPLETE with a near-empty dump text -- which is honest: the dump *is*
// empty because nobody asked for it.
void StartMetadataRecovery() {
    if (!DumpAvailable()) return;

    CabbirdDumpRequestV1 request{};
    request.struct_size = sizeof(request);
    request.flags = CABBIRD_DUMP_V1_METADATA;

    wchar_t wide_metadata[std::size(g_settings.metadata_path)]{};
    const int converted = MultiByteToWideChar(
        CP_UTF8, 0, g_settings.metadata_path, -1, wide_metadata,
        static_cast<int>(std::size(wide_metadata)));
    if (converted > 1) {
        request.metadata_path = wide_metadata;
        request.metadata_path_length = static_cast<std::uint32_t>(converted - 1);
    }

    CabbirdDumpResultV1 result{};
    result.struct_size = sizeof(result);
    const CabbirdStatusV1 status = g_dump->run(g_dump->user, &request, &result);
    if (status.message.data != nullptr && status.message.size != 0) {
        const std::size_t count = status.message.size < sizeof(g_last_message) - 1
            ? status.message.size
            : sizeof(g_last_message) - 1;
        std::memcpy(g_last_message, status.message.data, count);
        g_last_message[count] = '\0';
    } else {
        g_last_message[0] = '\0';
    }
    ++g_runs_started;
    g_last_result = result;
    g_have_result = 1;
}

void PollState() {    if (!DumpAvailable() || g_dump->state == nullptr) return;
    CabbirdDumpResultV1 result{};
    result.struct_size = sizeof(result);
    if (!Succeeded(g_dump->state(g_dump->user, &result))) return;

    // Count the TRANSITION, not the observation.  `state` is polled every
    // `poll_every_frames`, so counting every finished read would report the same dump
    // dozens of times and make the counter useless for telling "it finished" from "I
    // polled it again".
    const bool was_running = g_have_result != 0 &&
        g_last_result.state == CABBIRD_DUMP_V1_RESULT_PENDING;
    if (was_running && result.state != CABBIRD_DUMP_V1_RESULT_PENDING) ++g_runs_finished;

    g_last_result = result;
    g_have_result = 1;
}

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *context = nullptr;
    const cabbird::sdk::Host services(host);

    // REQUIRED.  A dump plugin that cannot report what happened is worse than one that
    // refuses to load: the user would press the button, see nothing, and have no way to
    // tell "it is running" from "it never started".
    const auto ui = services.Query<CabbirdUiServiceV1>(
        CABBIRD_UI_SERVICE_V1_ID, CABBIRD_UI_SERVICE_V1_VERSION);
    if (!ui) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    g_ui = ui.get();

    // OPTIONAL, and its absence is a normal state rather than a failure.  On a build
    // where the host did not publish the backend, the window says so and the plugin
    // still loads.  Failing the load would make an unbound build look broken.
    g_dump = services
                 .Query<CabbirdUnityDumpServiceV1>(
                     CABBIRD_UNITY_DUMP_SERVICE_V1_ID, CABBIRD_UNITY_DUMP_SERVICE_V1_VERSION)
                 .get();
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) { return Ok(); }
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) { return Ok(); }

void CABBIRD_CALL Unload(void*) {
    g_ui = nullptr;
    g_dump = nullptr;
    g_have_result = 0;
    g_frames_since_poll = 0;
}

void CABBIRD_CALL Update(void*, double) {
    // Deliberately empty.
    //
    // Polling used to live here, with this rationale: "`state` is safe from either domain,
    // but the render callback should contain nothing that is not drawing."  That rationale
    // is wrong for this host, and the service's own contract says so -- see the block on
    // `cabbird.unity.dump` in include/cabbird/sdk/services/unity.h, which states that a
    // render callback is "the only place a plugin is guaranteed to be called every frame",
    // and that from `on_draw` the call never blocks and never dumps inline.
    //
    // Without the host's Game-domain pump running, `PollState` is never called, so
    // `g_last_result` keeps its initial zeroes -- while the host's own diagnostics show the
    // dump walking 23,000+ classes.  "The dump is running but the window shows zero"
    // was exactly that: a correct producer, a live service, and a consumer that was never
    // invoked.  Moving the poll to `on_draw` makes the display depend only on the one pump
    // that is proven to run.
}

void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1* ui) {
    if (ui == nullptr) ui = g_ui;
    if (ui == nullptr) return;

    // POLL HERE, NOT IN `Update`.  The render domain is the one the host guarantees to call
    // every frame; see the note on `Update` above and the service contract in unity.h.
    // Throttled by the same `poll_every_frames` as before, because the reason for the
    // throttle is unchanged: the counter should report a transition, not every observation.
    //
    // `PollState` reads a result struct -- a couple of atomic loads and one mutex on the
    // host side -- and never blocks, never dumps inline, and cannot touch IL2CPP.  That is
    // what makes it safe inside a render callback, and it is the same call the contract
    // tells a plugin to make from here.
    if (g_settings.poll_every_frames <= 0) g_settings.poll_every_frames = 1;
    if (++g_frames_since_poll >= g_settings.poll_every_frames) {
        g_frames_since_poll = 0;
        PollState();
    }

    cabbird::sdk::UiWindow window(ui, kPluginName, &g_open);
    if (!window) return;

    char line[256]{};
    if (!DumpAvailable()) {
        ui->text(ui->user, StringView("dump service : UNAVAILABLE"));
        ui->separator(ui->user);
        ui->text(ui->user, StringView("The host has not published cabbird.unity.dump."));
        ui->text(ui->user, StringView("Nothing can be dumped from this build."));
        return;
    }

    ui->text(ui->user, StringView("dump service : available"));
    ui->checkbox(ui->user, StringView("fields (names, types, offsets)"),
                 &g_settings.include_fields);
    ui->checkbox(ui->user, StringView("methods (+ signatures, RVAs)"),
                 &g_settings.include_methods);
    // Labelled with what it BUYS, not with what it does: "recover metadata" means nothing
    // to a user who has not read the SDK header, while "for Il2CppDumper" names the tool
    // whose absence is the reason a dump comes back with no methods from the game's own
    // main assembly.
    ui->checkbox(ui->user, StringView("metadata blob (for Il2CppDumper, ~7 s)"),
                 &g_settings.recover_metadata);
    if (ui->input_text != nullptr) {
        static_cast<void>(ui->input_text(ui->user, StringView("image filter"),
                                         g_settings.image_filter,
                                         sizeof(g_settings.image_filter), 0));
        static_cast<void>(ui->input_text(ui->user, StringView("output file"),
                                         g_settings.output_path,
                                         sizeof(g_settings.output_path), 0));
        static_cast<void>(ui->input_text(ui->user, StringView("metadata file"),
                                         g_settings.metadata_path,
                                         sizeof(g_settings.metadata_path), 0));
    }
    ui->separator(ui->user);

    const bool running = g_have_result != 0 &&
        g_last_result.state == CABBIRD_DUMP_V1_RESULT_PENDING;
    // `g_have_result` is set by the FIRST successful poll, which happens on the first frame --
    // not by pressing the button.  So it means "the service answered", and the block below must
    // therefore key off the STATE, not off `g_have_result`.  It used to key off `g_have_result`
    // alone, which is how a window with no dump requested came to display a state line and an
    // elapsed timer.  `IDLE` is what makes the distinction expressible; see unity.h.
    const bool ever_requested =
        g_have_result != 0 && g_last_result.state != CABBIRD_DUMP_V1_RESULT_IDLE;
    // Disabled while one is running rather than hidden: the host rejects a second
    // concurrent dump anyway, and a button that silently does nothing is the failure
    // mode this plugin exists to avoid.
    if (ui->button_enabled != nullptr) {
        if (ui->button_enabled(ui->user, StringView("Dump now"), 0.0f, 0.0f, running ? 0 : 1)) {
            StartDump();
        }
        // The metadata-only path, next to the full one so the difference in cost is
        // visible at the point of decision rather than in a header.
        if (ui->button_enabled(ui->user, StringView("Metadata only"), 0.0f, 0.0f,
                               running ? 0 : 1)) {
            StartMetadataRecovery();
        }
    } else if (ui->button != nullptr &&
               ui->button(ui->user, StringView("Dump now"), 0.0f, 0.0f)) {
        StartDump();
    }
    if (running) ui->text(ui->user, StringView("a dump is already running"));

    ui->separator(ui->user);
    if (!ever_requested) {
        // Nothing has been asked of the service yet.  Saying so is the whole point of `IDLE`:
        // the alternative -- showing a state line, a timer and a class counter for a dump that
        // does not exist -- reads as a broken dump rather than as "nothing asked yet".
        ui->text(ui->user, StringView("no dump requested yet"));
        ui->text(ui->user, StringView("press \"Dump now\" to start one"));
        return;
    }
    if (g_have_result != 0) {
        std::snprintf(line, sizeof(line), "state     : %s", StateName(g_last_result.state));
        ui->text(ui->user, StringView(line));

        // THE HEARTBEAT IS SHOWN IN EVERY STATE, not only while PENDING.
        //
        // This is the second bug in this window, and the first one hid it: gating the block
        // on `state == PENDING` meant that the instant the walk finished, the window fell
        // back to the result fields -- which are populated only on completion.  A user who
        // opened the window after a 67-second walk (or whose first poll landed after it)
        // therefore saw ZEROES with no way to tell that a dump had just run successfully.
        //
        // The host-side counters are monotonic: they read 0 before a request, climb during
        // the walk, and RETAIN the final total afterwards.  So they are the right thing to
        // display unconditionally -- and the "is it still going" question is answered by
        // `state`, not by hiding the numbers.
        {
            // `running` is the one declared earlier in this function, from `g_have_result`
            // and the PENDING state.  It was re-declared here, shadowing that one with an
            // equivalent expression -- and the shadow is what made the window flicker: the
            // inner copy is recomputed every frame from a value that the poll updates, so
            // the two could disagree for a frame at a time and the note gated on it blinked.
            // One declaration, one meaning.
            const unsigned long long seconds = g_last_result.progress_elapsed_ms / 1000ULL;
            std::snprintf(line, sizeof(line), "elapsed   : %llu.%01llu s%s",
                          seconds, (g_last_result.progress_elapsed_ms / 100ULL) % 10ULL,
                          running ? "" : "  (finished)");
            ui->text(ui->user, StringView(line));

            // The "clock is advancing but no class has been read" note used to live here
            // and was removed on request: it flickered in and out as the class counter
            // alternated between advancing and not, which reads as a broken window rather
            // than as information.  The two counters it sat between (`classes so far` and
            // the elapsed clock) already show the same fact without anything blinking, and
            // `state` above already says whether the walk is still going.
            std::snprintf(line, sizeof(line), "classes   : %llu so far",
                          static_cast<unsigned long long>(g_last_result.progress_classes));
            ui->text(ui->user, StringView(line));
            std::snprintf(line, sizeof(line), "fields    : %llu so far",
                          static_cast<unsigned long long>(g_last_result.field_count));
            ui->text(ui->user, StringView(line));
            ui->separator(ui->user);
        }

        std::snprintf(line, sizeof(line), "classes   : %llu   fields : %llu   methods : %llu",
                      static_cast<unsigned long long>(g_last_result.class_count),
                      static_cast<unsigned long long>(g_last_result.field_count),
                      static_cast<unsigned long long>(g_last_result.method_count));
        ui->text(ui->user, StringView(line));
        std::snprintf(line, sizeof(line), "written   : %llu bytes",
                      static_cast<unsigned long long>(g_last_result.bytes_written));
        ui->text(ui->user, StringView(line));
        // These two lines are the reason the result carries them.  `faults` non-zero
        // means the text has holes and its own header says INCOMPLETE; a non-zero
        // "outside" means the MethodInfo offset is wrong and NO method address in the
        // dump should be believed, because it is not a translation of the offset --
        // it is a measurement of it.
        std::snprintf(line, sizeof(line), "faults    : %u", g_last_result.contained_faults);
        ui->text(ui->user, StringView(line));
        std::snprintf(line, sizeof(line), "method rva: %llu inside / %llu outside",
                      static_cast<unsigned long long>(g_last_result.method_rva_inside),
                      static_cast<unsigned long long>(g_last_result.method_rva_outside));
        ui->text(ui->user, StringView(line));

        // The metadata half.  Shown whenever it is non-zero, and shown as an explicit
        // "not recovered" line when the switch was on and nothing was found -- a missing
        // line would leave "the scan failed" and "nobody asked" looking identical, which
        // is the ambiguity this whole service keeps being rewritten to remove.
        if (g_last_result.metadata_size != 0) {
            std::snprintf(line, sizeof(line), "metadata  : %llu bytes, version %u, %llu ms",
                          static_cast<unsigned long long>(g_last_result.metadata_size),
                          g_last_result.metadata_version,
                          static_cast<unsigned long long>(g_last_result.metadata_scan_ms));
            ui->text(ui->user, StringView(line));
            std::snprintf(line, sizeof(line), "metadata  : %s   source 0x%llX",
                          g_last_result.metadata_written != 0
                              ? "written to disk"
                              : "in memory only (no path)",
                          static_cast<unsigned long long>(g_last_result.metadata_address));
            ui->text(ui->user, StringView(line));
        } else if (g_settings.recover_metadata != 0) {
            ui->text(ui->user, StringView("metadata  : NOT recovered (see reason below)"));
        }
    } else {
        ui->text(ui->user, StringView("state     : idle (nothing requested yet)"));
    }
    // Shown unconditionally when present: this is the runtime's own reason, and the case
    // that matters is "unavailable -- GameAssembly.dll not loaded yet", where a state
    // name alone would leave the user with no idea whether to wait or to give up.
    if (g_last_message[0] != '\0') {
        std::snprintf(line, sizeof(line), "reason    : %s", g_last_message);
        ui->text(ui->user, StringView(line));
    }
    std::snprintf(line, sizeof(line), "runs      : %llu started, %llu finished",
                  g_runs_started, g_runs_finished);
    ui->text(ui->user, StringView(line));
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        StringView(kPluginId),
        StringView(kPluginName),
        StringView("Cabbird"),
        StringView("0.1.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}
