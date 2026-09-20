// Player Coordinates -- read the local player's world position, and nothing else.
//
// WHY THIS IS A PLUGIN AND WHICH HALF OF IT IS THE HOST'S
// ------------------------------------------------------
// Reading the player's position is the one part of "teleport like UE5's
// `K2_SetActorLocation`" that a plugin can do on its own, and it is deliberately split from
// the writing half (`plugins/player_teleport`):
//
//   * this plugin has NO write capability and cannot move anything, by construction.  A
//     manifest that lists `unity-player-snapshot` but never asks for a write is a plugin whose
//     worst failure is a wrong number on screen;
//   * the position itself comes from `cabbird.unity.player`, the host's bridge.  A plugin
//     could in principle walk the entity source's `entity_data` pointer chain with
//     `cabbird.core`'s `read_memory`, but the LAST hop -- the world position -- is
//     `Transform::get_position` on a native object, and a plugin cannot invoke a managed
//     method.  So the host answers, and this plugin formats.
//
// WHAT IT IS FOR, BEYOND SHOWING A NUMBER
// ---------------------------------------
// A teleport is only meaningful relative to where you are.  The common operations are
// "where am I", "move me up/forward by N", and "put me back where I was" -- all of which
// need a trustworthy reading with more precision than a HUD shows, plus a record of the
// last few readings.  That log is written to a file next to this plugin, because a plugin's
// window is not a place to do arithmetic with a mouse.
//
// NO SERVICE, NO WINDOW: when `cabbird.unity.player` is absent (a host build without the
// bridge) or answers NOT_FOUND (no live player: main menu, loading, no world scene), the
// window says exactly that instead of showing 0.000 -- a zeroed reading is indistinguishable
// from the world origin and this project has already shipped one "plausible wrong number"
// too many.

#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/services/core.h"
#include "cabbird/sdk/services/ui.h"
#include "cabbird/sdk/services/unity.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

using cabbird::sdk::Host;
using cabbird::sdk::Ok;
using cabbird::sdk::StringView;

const CabbirdUiServiceV1* g_ui{};
const CabbirdUnityPlayerServiceV1* g_player{};

int g_open = 1;
int g_watch = 1;
// Readings are cheap (a struct copy out of the host's cache), so this is a display refresh,
// not a poll of the game.  `--` in the window means "no reading", never "0.000".
CabbirdUnityPlayerSnapshotV1 g_snapshot{};
std::uint32_t g_reading{};
std::uint32_t g_missing{};
char g_status[160] = "no reading yet";

// Where the "save a reading" button writes.  Absolute, because the host process's working
// directory is the GAME's directory -- a relative path would land somewhere the user does
// not expect (the same trap the IL2CPP dump plugin records).
char g_log_path[260] = "C:\\AzurPromilia-player-position.txt";

bool PlayerAvailable() {
    return g_player != nullptr && g_player->snapshot != nullptr;
}

void Refresh() {
    if (!PlayerAvailable()) {
        std::memset(&g_snapshot, 0, sizeof(g_snapshot));
        ++g_missing;
        std::snprintf(g_status, sizeof(g_status),
                      "player service unavailable (host build without cabbird.unity.player)");
        return;
    }
    CabbirdUnityPlayerSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    const CabbirdStatusV1 status = g_player->snapshot(g_player->user, &snapshot);
    if (status.code == CABBIRD_STATUS_V1_OK) {
        g_snapshot = snapshot;
        ++g_reading;
        std::snprintf(g_status, sizeof(g_status), "%s%s",
                      (snapshot.flags & CABBIRD_UNITY_PLAYER_V1_IDENTITY_PROXIMITY) != 0
                          ? "live (identity by camera proximity)"
                          : "live",
                      snapshot.data_class[0] != 0 ? "" : " (class unknown)");
        return;
    }
    std::memset(&g_snapshot, 0, sizeof(g_snapshot));
    ++g_missing;
    std::snprintf(g_status, sizeof(g_status), "%s",
                  status.code == CABBIRD_STATUS_V1_NOT_FOUND
                      ? (snapshot.reason[0] != '\0' ? snapshot.reason
                                                     : "no live player (diagnostic unavailable)")
                      : "player snapshot failed");
}

void AppendReading(const CabbirdUnityPlayerSnapshotV1& snapshot) {
    // APPEND, and say when the file could not be opened.  A silent failure here is
    // indistinguishable from "the button does nothing" (`WriteIdentityMarker` in
    // plugins/entity_overlay/plugin.cpp records the same lesson at length).
    std::FILE* file = std::fopen(g_log_path, "ab");
    if (file == nullptr) {
        std::snprintf(g_status, sizeof(g_status), "could not open %s", g_log_path);
        return;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::fprintf(file,
                 "%04u-%02u-%02u %02u:%02u:%02u  id=%llu class=%s gen=%llu  "
                 "pos=(%.4f, %.4f, %.4f)  cam=%.2f%s\n",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                 static_cast<unsigned long long>(snapshot.entity_id),
                 snapshot.data_class[0] != 0 ? snapshot.data_class : "?",
                 static_cast<unsigned long long>(snapshot.generation), snapshot.position[0],
                 snapshot.position[1], snapshot.position[2], snapshot.distance_to_camera,
                 (snapshot.flags & CABBIRD_UNITY_PLAYER_V1_IDENTITY_PROXIMITY) != 0
                     ? "  [proximity]"
                     : "");
    std::fclose(file);
    std::snprintf(g_status, sizeof(g_status), "appended a reading to %s", g_log_path);
}

// `ui` is the host's own table -- the same pointer `on_load` queried.  The parameter is
// deliberately IGNORED in favour of `g_ui`, because a plugin generation can be drawn by a
// host whose table pointer is not the one it loaded against (see PluginScope's generation
// rule); `g_ui` is the table this plugin validated at load time.
void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1*) {
    const CabbirdUiServiceV1* const ui = g_ui;
    if (ui == nullptr) return;
    if (g_watch != 0) Refresh();

    cabbird::sdk::UiWindow window(ui, "Player Coordinates", &g_open);
    if (!window) return;

    ui->checkbox(ui->user, StringView("refresh every frame"), &g_watch);
    ui->text(ui->user, StringView(g_status));
    ui->separator(ui->user);

    // THE POSITION IS SHOWN TO THREE DECIMALS AND LABELLED AS THE PIVOT.  The number is
    // Unity's own `Transform.position` -- the character's feet, not their centre -- because
    // that is the value a teleport has to be expressed in, and showing anything else would
    // make this plugin's numbers unusable as a teleport input.
    char line[192]{};
    const bool live = (g_snapshot.flags & CABBIRD_UNITY_PLAYER_V1_VALID) != 0;
    if (live) {
        std::snprintf(line, sizeof(line), "world (pivot):  x = %.3f   y = %.3f   z = %.3f",
                      g_snapshot.position[0], g_snapshot.position[1], g_snapshot.position[2]);
        ui->text(ui->user, StringView(line));
        std::snprintf(line, sizeof(line), "box centre:     x = %.3f   y = %.3f   z = %.3f",
                      g_snapshot.bounds_center[0], g_snapshot.bounds_center[1],
                      g_snapshot.bounds_center[2]);
        ui->text(ui->user, StringView(line));
        std::snprintf(line, sizeof(line), "distance to camera: %.2f m", g_snapshot.distance_to_camera);
        ui->text(ui->user, StringView(line));
        std::snprintf(line, sizeof(line), "entity id: %llu   generation: %llu",
                      static_cast<unsigned long long>(g_snapshot.entity_id),
                      static_cast<unsigned long long>(g_snapshot.generation));
        ui->text(ui->user, StringView(line));
        std::snprintf(line, sizeof(line), "data class: %s",
                      g_snapshot.data_class[0] != 0 ? g_snapshot.data_class : "(unknown)");
        ui->text(ui->user, StringView(line));
    } else {
        ui->text(ui->user, StringView("world (pivot):  x = --   y = --   z = --"));
        ui->text(ui->user, StringView("box centre:     x = --   y = --   z = --"));
        ui->text(ui->user, StringView("no position: a missing reading is not the origin"));
    }

    ui->separator(ui->user);
    ui->text(ui->user, StringView("record"));
    ui->text(ui->user, StringView(g_log_path));
    const int can_write = live ? 1 : 0;
    if (ui->button_enabled(ui->user, StringView("append reading to file"), 0.0f, 0.0f,
                           can_write) != 0) {
        AppendReading(g_snapshot);
    }

    // NOTE WHAT IS *NOT* HERE: a "send to the teleport plugin" button.
    //
    // The obvious implementation is a shared file or a shared global, and both are wrong.
    // A file makes the two panels fail in a way that looks like neither of them (the write
    // works, the read is stale, and the user sees the OLD coordinates teleported to);
    // a shared in-process global is a hidden dependency between two independently reloaded
    // DLLs, which is precisely the coupling `PluginScope` exists to prevent.  The teleport
    // panel reads the same host service this one does, so it has its own "use current
    // position" button and needs nothing from here.

    // The counters are at the BOTTOM and are the only diagnostics: `reading` grows while
    // `missing` stays put when the bridge works, and the pair distinguishes "the host never
    // answers" from "the window stopped asking" -- which is not visible from one number.
    ui->separator(ui->user);
    std::snprintf(line, sizeof(line), "readings: %u   missing: %u", g_reading, g_missing);
    ui->text(ui->user, StringView(line));
}

// --- lifecycle -----------------------------------------------------------------

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *context = nullptr;
    const Host services(host);
    const auto ui =
        services.Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID, CABBIRD_UI_SERVICE_V1_VERSION);
    if (!ui) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    g_ui = ui.get();
    // OPTIONAL ON PURPOSE.  A host without the player bridge must still load this plugin:
    // taking the panel away would hide the diagnostic that says why (the requirement is
    // declared `optional` in the manifest for the same reason).
    g_player = services
                   .Query<CabbirdUnityPlayerServiceV1>(CABBIRD_UNITY_PLAYER_SERVICE_V1_ID,
                                                       CABBIRD_UNITY_PLAYER_SERVICE_V1_VERSION)
                   .get();
    Refresh();
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) { return Ok(); }
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) { return Ok(); }

void CABBIRD_CALL Unload(void*) {
    g_ui = nullptr;
    g_player = nullptr;
    std::memset(&g_snapshot, 0, sizeof(g_snapshot));
}

void CABBIRD_CALL Update(void*, double) {
    // NOTHING HERE ON PURPOSE.  The snapshot is a cached struct copy on the host side, so
    // the UI callback can read it directly without touching IL2CPP; doing the read in
    // Update as well would double the work for no new information.
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    // FILLED FIELD BY FIELD, not with a braced initialiser.  `CabbirdStringViewV1` is a C
    // struct with a pointer and a length and no converting constructor from
    // `std::string_view`, so an aggregate `= {..., StringView(...), ...}` does not compile --
    // the same reason `plugins/entity_overlay` assigns each view separately.
    descriptor->id = StringView("cabbird.player-coords");
    descriptor->name = StringView("Player Coordinates");
    descriptor->author = StringView("Cabbird");
    descriptor->version = StringView("0.1.0");
    descriptor->on_load = Load;
    descriptor->on_start = Start;
    descriptor->on_stop = Stop;
    descriptor->on_unload = Unload;
    descriptor->on_update = Update;
    descriptor->on_draw = Draw;
    return Ok();
}
