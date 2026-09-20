// Entity Teleport -- move ANY entity to coordinates, or bring it to the local player.
//
// WHAT THIS ADDS THAT THE TREE DID NOT HAVE
// -----------------------------------------
// `plugins/player_teleport` can move exactly one thing: the local player, through
// `cabbird.unity.player-teleport`, which resolves the player itself and therefore cannot be
// pointed at anything else.  There was no way to move a crate, an NPC, a monster or a second
// character.
//
// The missing half is not a missing service -- it is a missing COMPOSITION.  Two services
// already exist and neither alone is enough:
//
//   * `cabbird.unity.entities` says WHICH entities exist and publishes each one's managed
//     `BaseData` address (`entity_data`) plus a display name.  It says nothing about where the
//     entity is in a form that can be written.
//   * `cabbird.unity.transform` turns a `BaseData` address into a live `UnityEngine.Transform`,
//     reads its position and writes it.
//
// `resolve(entity_data) -> handle`, then `write(handle, position)`, and an arbitrary entity is
// movable.  That is the whole plugin, and it is exactly the composition the SDK header for
// `cabbird.unity.transform` describes ("a consumer with an entity id asks
// `cabbird.unity.entities` for the data address and then asks this service about the
// transform -- two questions, two owners, no coupling").
//
// WHY EVERYTHING HAPPENS ON A DIFFERENT THREAD THAN THE BUTTON
// ------------------------------------------------------------
// The plugin UI (`on_draw`) runs in the RENDER domain, inside Present; `on_update` runs in the
// GAME domain.  `cabbird.unity.transform` is documented GAME DOMAIN ONLY -- it calls
// `il2cpp_runtime_invoke`, and doing that from the presenter's thread is a crash.  So a button
// press does not move anything: it COPIES its arguments into a command slot and the next game
// tick performs the work.  The copy happens before the slot is
// published, so the game thread never reads a half-written command.
//
// WHY THE RESULT IS A VERDICT AND NOT "OK"
// ----------------------------------------
// `cabbird.unity.transform`'s `write` reports only that a managed call returned without an
// exception (the SDK header says so in those words).  Unity's `CharacterController`,
// `Rigidbody` or a movement state machine can put the object back on the very next frame, so
// "the call succeeded" and "the entity is there" are different facts and the gap between them
// is the single most common way a Unity teleport fails.  This plugin therefore re-resolves the
// entity's transform in a LATER tick and re-reads the position, and prints three numbers that
// cannot hide the difference: asked for, immediately after, and a few ticks later.  When the
// third does not match the first, it says REVERTED, because that is what happened.
//
// THERE IS NO SERVER CONFIRMATION HERE, AND THE WINDOW SAYS SO
// -----------------------------------------------------------
// The player teleport is server-authoritative in this game: the server owns the player's
// position and corrects it.  Moving a MONSTER or an NPC locally is a client-side transform
// write, so the entity may be snapped back by the server a moment later -- the same REVERTED
// verdict, arriving later than this window's four ticks can observe.  A window that claimed
// otherwise would be lying about the one thing the user wants to know.
//
// WHAT THIS DELIBERATELY DOES NOT DO
// ----------------------------------
//   * no memory offsets, no pointer walking, no `read_memory`/`write_memory`.  `entity_data` is
//     passed to the host's own service as an opaque token; this file never dereferences it;
//   * no name resolution of its own.  The label comes from the entity service;
//   * no "teleport everything" convenience button.  A bulk write of 80 transforms on one game
//     tick is how a plugin turns a typo into a crash.

#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/services/core.h"
#include "cabbird/sdk/services/ui.h"
#include "cabbird/sdk/services/unity.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

using cabbird::sdk::Host;
using cabbird::sdk::Ok;
using cabbird::sdk::StringView;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr std::uint32_t kMaximumEntities = 256;
constexpr std::size_t kLabelBytes = 64;
constexpr std::size_t kMaximumCommandsPerTick = 4;
constexpr float kMinimumCoordinateMagnitude = 1.0e6f;
// How far a written position may be from the request and still be called "there".
//
// Unity stores floats, so a metre-scale error is never a rounding artefact -- it is the game
// having moved the object.  The same tolerance the host's own teleport policy uses, for the same
// reason: two different definitions of "arrived" in one product is one too many.
constexpr double kAcceptanceToleranceMeters = 0.75;
// Ticks between the write and the re-read.  Not 1: the frame the call happens in is not a frame
// in which the game's own movement has run, so a one-tick check would pass every write including
// the ones the game undoes.
constexpr std::uint64_t kVerifyDelayTicks = 4;

// ---------------------------------------------------------------------------
// Services
// ---------------------------------------------------------------------------

const CabbirdUiServiceV1* g_ui{};
const CabbirdUnityEntitiesServiceV1* g_entities{};
const CabbirdUnityTransformServiceV1* g_transform{};
// The player SNAPSHOT, not the player teleport: this plugin reads where the player is
// ("bring it to me") and writes through the transform service like any other entity.  It never
// asks `cabbird.unity.player-teleport` to move the player, because that service exists to move
// ONE object and would bypass the entity the user selected.
const CabbirdUnityPlayerServiceV1* g_player{};

bool EntitiesUsable() {
    if (g_entities == nullptr || g_entities->entity_count == nullptr ||
        g_entities->entity_at == nullptr || g_entities->generation == nullptr) {
        return false;
    }
    return g_entities->struct_size >=
           offsetof(CabbirdUnityEntitiesServiceV1, entity_at) +
               sizeof(CabbirdUnityEntitiesServiceV1::entity_at);
}

bool TransformUsable() {
    if (g_transform == nullptr) return false;
    constexpr std::size_t kSize =
        offsetof(CabbirdUnityTransformServiceV1, write) +
        sizeof(CabbirdUnityTransformServiceV1::write);
    return g_transform->struct_size >= kSize && g_transform->resolve != nullptr &&
           g_transform->position != nullptr && g_transform->write != nullptr;
}

bool PlayerUsable() {
    return g_player != nullptr && g_player->snapshot != nullptr;
}

// ---------------------------------------------------------------------------
// The entity snapshot, copied into plugin-owned storage
// ---------------------------------------------------------------------------
//
// COPIED, AND WITH A DEEP COPY OF THE NAME, for two separate reasons:
//
//   * `entity_at`'s `label` is a borrowed view that "stays valid only until the next
//     `entity_at` call" (SDK header).  The list is drawn on the render thread a whole frame
//     after it was read, so a held pointer would be a dangling read;
//   * the entity set is replaced by every walk, so anything read a frame ago may describe an
//     entity that no longer exists.  The stored `entity_id` is what makes that detectable:
//     the write path looks the entity up by id before touching it, and a stale row fails
//     cleanly instead of moving whatever now occupies the old address.
struct EntityRow {
    std::uint64_t entity_id;
    std::uint64_t entity_data;
    std::uint32_t kind;
    double bounds_center[3];
    float distance;
    bool valid;
    char label[kLabelBytes];
};

EntityRow g_rows[kMaximumEntities]{};
std::atomic<std::uint32_t> g_row_count{0};
std::atomic<std::uint64_t> g_generation{0};
std::atomic<std::uint32_t> g_rows_total{0};
std::atomic<std::uint32_t> g_rows_labelled{0};
std::atomic<std::uint32_t> g_rows_no_transform{0};

int g_open = 1;
// The row the user picked.  An INDEX into a list that is rebuilt every tick, so the STABLE
// identity is kept beside it: `g_selected_id`.  Without it, selecting row 7 and then having a
// monster die would silently retarget the next entity that slid into slot 7 -- a teleport
// aimed at the wrong object, which is exactly the class of failure this project keeps
// refusing.
int g_selected_row = -1;
std::uint64_t g_selected_id{};
char g_selected_name[kLabelBytes] = "(nothing selected)";

// Filters and inputs.  Text, not sliders: a coordinate is typed or pasted, and `input_double`'s
// drag-to-change behaviour turns a stray drag across a 1e5-wide slider into a teleport to the
// other side of the map.
char g_target_text[3][32] = {"0.000", "0.000", "0.000"};
int g_offset_mode = 0;
float g_bring_offset_y = 0.0f;
int g_search_enabled = 0;
// ONE SWITCH PER BUCKET THE GAME HAS, INCLUDING THE EMPTY ONE.
//
// `g_filter_unclassified` exists because its absence is what made this panel look broken: the
// host returns `kind == 0` for every entity whose class or camp it could not decide, and a
// filter table without a zero case hides all of them silently -- the panel then reads as
// "the entity list is empty, but the header says there are 81 entities".
int g_filter_player = 1;
int g_filter_monster = 1;
int g_filter_npc = 1;
int g_filter_world = 1;
int g_filter_unclassified = 1;
char g_search[64] = {};

// Which page of the entity table is shown.  PAGINATION AND NOT A SCROLLING REGION: a scrolling
// region is a `begin_child`, and a `begin_child` that comes back collapsed hides its contents
// without saying anything -- which is exactly how this list went blank twice.  A page is a range
// of rows drawn as plain items, and there is nothing that can collapse.
std::uint32_t g_page{};
// The entity id typed into the coordinate action's text box.  Kept in plugin state because the
// buffer is rebuilt from it on every draw (and `input_text` needs a buffer it can edit).
std::uint64_t g_last_target_id{};

// ---------------------------------------------------------------------------
// Command queue (render domain -> game domain)
// ---------------------------------------------------------------------------
//
// Deliberately NOT a general queue: four slots, filled from the tail, drained from the head.
// A button can be pressed at most once per frame, and the game tick drains everything queued,
// so the queue can only overflow if the tick thread has stopped -- in which case dropping the
// command and SAYING SO is correct, and blocking the render callback while waiting for a game
// tick would deadlock the overlay (the reason `teleport` itself is queued rather than applied).
enum class CommandKind : std::uint32_t {
    None = 0,
    /* `position` is the absolute target. */
    MoveToCoordinates = 1,
    /* `reference` is the player's pivot as read at the moment of the click. */
    BringToPlayer = 2,
    /* No coordinates: read the entity's `Transform.position` back to the window. */
    ReadPosition = 3,
    /* `reference` is a (dx, dy, dz) added to the entity's OWN position, which is read on the
     * game thread -- the render thread cannot read it. */
    MoveByOffset = 4
};

// ---------------------------------------------------------------------------
// WHY SELECTION IS A COMBO BOX AND NOT A LIST OF CLICKABLE NAMES
// ---------------------------------------------------------------------------
// The first two versions drew one clickable row per entity, through `ui->text_link`.  That was
// WRONG, and the host's own implementation says why (src/ui/cabbird_ui_service.cpp, TextLink):
//
//     const bool allowed = target.starts_with("http://") || target.starts_with("https://");
//     if (!allowed) {
//         // Drawn as plain text rather than as a link, so the UI does not promise something
//         // it will refuse to do.
//         ImGui::TextUnformatted(text.c_str());
//         return 0;
//     }
//
// The host validates the URL SCHEME because the callback it queues opens a browser.  A plugin
// cannot use `text_link` as a generic "clickable row": with a non-http URL the text is drawn as
// PLAIN TEXT, `text_link` returns 0 forever, and the panel shows a list of names that cannot be
// selected.  On screen that is indistinguishable from an empty list -- which is exactly how it
// was reported, twice.
//
// `combo` is the control that exists for this.  It is a real ImGui combo owned by the host, it
// returns the chosen index, and it needs no URL, no click routing and no agreement about what a
// "link" means.
constexpr int kMaximumComboItems = 96;

struct Command {
    std::uint32_t kind;
    std::uint32_t reserved;
    std::uint64_t entity_id;
    double position[3];
    /* Meaning depends on `kind`: the player's pivot for `BringToPlayer`, the offset for
     * `MoveByOffset`.  Carried in the command rather than re-read on the game thread so
     * "bring it to me" lands where the user was looking when they clicked, and so the game
     * thread makes one fewer host call. */
    double reference[3];
    char label[kLabelBytes];
};

Command g_commands[kMaximumCommandsPerTick]{};
std::atomic<std::uint32_t> g_command_write{0};
std::atomic<std::uint32_t> g_command_read{0};
std::atomic<std::uint32_t> g_commands_dropped{0};
// Set by the game thread, cleared by the render thread: "something changed, redraw with the new
// answer".  Only a hint -- the window reads the result structs themselves.
std::atomic<bool> g_result_dirty{false};

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------
//
// TWO SLOTS RATHER THAN A LOG.  Slot 0 is what the game thread did (the write returned), slot 1
// is what the game reported a few ticks later (the verdict).  A log would need allocation and
// eviction in the game domain; two slots say everything the window needs and cannot grow.
struct WriteSlot {
    bool used;
    std::uint32_t kind;
    std::uint32_t status;
    std::uint64_t entity_id;
    char label[kLabelBytes];
    double requested[3];
    double before[3];
    double after[3];
    double later[3];
    bool have_before;
    bool have_after;
    bool have_later;
    double error_now;
    double error_later;
    std::uint32_t sequence;
};

WriteSlot g_applied{};   // written by the game thread when the call returns
WriteSlot g_verified{};  // written by the game thread when the later re-read lands

// The reading a "read position" command produced, so the user can copy an entity's exact
// `Transform.position` (as opposed to the entity set's approximate box centre).
struct PositionRead {
    bool used;
    std::uint64_t entity_id;
    double position[3];
};
PositionRead g_reading{};

// Status text, written by whichever domain produced it.  A char buffer rather than a
// std::string because the game thread must not allocate while the game's frame is in progress.
char g_status[256] = "idle";
std::atomic<std::uint32_t> g_pending_verify{0};
std::uint64_t g_pending_verify_tick{};
std::uint64_t g_pending_verify_id{};
double g_pending_verify_target[3]{};
char g_pending_verify_label[kLabelBytes]{};
std::uint32_t g_sequence{};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void SetStatus(const char* text) {
    std::snprintf(g_status, sizeof(g_status), "%s", text == nullptr ? "" : text);
}

bool ParseComponent(const char* text, double* value) {
    if (text == nullptr || value == nullptr) return false;
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (end == text) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end != '\0') return false;
    if (!std::isfinite(parsed)) return false;
    if (std::fabs(parsed) > static_cast<double>(kMinimumCoordinateMagnitude)) return false;
    *value = parsed;
    return true;
}

bool ParseVector(const char text[3][32], double out[3]) {
    for (int index = 0; index < 3; ++index) {
        if (!ParseComponent(text[index], &out[index])) return false;
    }
    return true;
}

void FormatVector(const double value[3], char text[3][32]) {
    for (int index = 0; index < 3; ++index) {
        std::snprintf(text[index], sizeof(text[index]), "%.4f", value[index]);
    }
}

// Substring search, case-insensitive for ASCII.  `ui->filter_match` exists but is a UI-side
// ImGui filter helper and nothing here needs ImGui's matching rules; a name filter that is
// case-sensitive in a list of Chinese and Latin names would be worse than useless.
bool NameMatches(const char* name, const char* needle) {
    if (needle == nullptr || needle[0] == '\0') return true;
    if (name == nullptr) return false;
    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    const std::size_t needle_length = std::strlen(needle);
    const std::size_t name_length = std::strlen(name);
    if (needle_length > name_length) return false;
    for (std::size_t start = 0; start + needle_length <= name_length; ++start) {
        std::size_t index = 0;
        for (; index < needle_length; ++index) {
            if (lower(name[start + index]) != lower(needle[index])) break;
        }
        if (index == needle_length) return true;
    }
    return false;
}

bool KindWanted(std::uint32_t kind) {
    switch (kind) {
        case CABBIRD_UNITY_ENTITY_V1_KIND_PLAYER:
            return g_filter_player != 0;
        case CABBIRD_UNITY_ENTITY_V1_KIND_MONSTER:
            return g_filter_monster != 0;
        case CABBIRD_UNITY_ENTITY_V1_KIND_NPC:
            return g_filter_npc != 0;
        case CABBIRD_UNITY_ENTITY_V1_KIND_WORLD:
            return g_filter_world != 0;
        // THE ZERO CASE IS NAMED RATHER THAN LEFT TO `default`.  `KIND_UNCLASSIFIED` is a
        // bucket the host fills on purpose (every entity it cannot classify), it is the bucket
        // this panel was reported empty because of, and it has its own switch.  Folding it into
        // `default` would make it invisible to anyone reading this function and impossible to
        // switch off -- which is how it went unnoticed the first time.
        case CABBIRD_UNITY_ENTITY_V1_KIND_UNCLASSIFIED:
            return g_filter_unclassified != 0;
        default:
            // A kind number from a NEWER host than this plugin was built against.  Shown under
            // the unclassified switch rather than hidden: an unknown bucket is a reason to look,
            // not a reason to draw nothing.
            return g_filter_unclassified != 0;
    }
}

const char* KindName(std::uint32_t kind) {
    switch (kind) {
        case CABBIRD_UNITY_ENTITY_V1_KIND_PLAYER: return "player";
        case CABBIRD_UNITY_ENTITY_V1_KIND_MONSTER: return "monster";
        case CABBIRD_UNITY_ENTITY_V1_KIND_NPC: return "npc";
        case CABBIRD_UNITY_ENTITY_V1_KIND_WORLD: return "world";
        default: return "unclassified";
    }
}

// Enqueue one command.  Returns false when the queue is full, which the caller turns into a
// visible message rather than a silent drop.
bool Enqueue(const Command& command) {
    const std::uint32_t write = g_command_write.load(std::memory_order_relaxed);
    const std::uint32_t read = g_command_read.load(std::memory_order_acquire);
    if (write - read >= kMaximumCommandsPerTick) {
        g_commands_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_commands[write % kMaximumCommandsPerTick] = command;
    // RELEASE: the payload above must be visible before the game thread sees the new head.
    g_command_write.store(write + 1, std::memory_order_release);
    return true;
}

// Whether any class switch is on at all.  Every switch off means the user has filtered the list
// down to nothing, and the honest response is to show everything and say so -- not to render a
// blank panel that looks exactly like a broken plugin.
bool AnyKindEnabled() {
    return g_filter_player != 0 || g_filter_monster != 0 || g_filter_npc != 0 ||
           g_filter_world != 0 || g_filter_unclassified != 0;
}

// Whether a row passes the class switches and the name filter.  ONE definition, because the
// empty-list message and the list itself must agree about what "hidden" means.
bool RowWanted(const EntityRow& row) {
    if (row.entity_id == 0) return false;
    if (AnyKindEnabled() && !KindWanted(row.kind)) return false;
    if (g_search_enabled != 0 && !NameMatches(row.label, g_search)) return false;
    return true;
}

double DistanceBetween(const double a[3], const double b[3]) {
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---------------------------------------------------------------------------
// The game-domain work
// ---------------------------------------------------------------------------

// Refresh the entity snapshot.  Runs in the GAME domain (see on_update): `entity_at` is
// documented as render-safe, and it is, but the row's `entity_data` is only useful to a caller
// that can call `cabbird.unity.transform`, and that caller is here.
void RefreshEntities() {
    if (!EntitiesUsable()) {
        g_row_count.store(0, std::memory_order_release);
        return;
    }
    std::uint32_t count = g_entities->entity_count(g_entities->user);
    if (count > kMaximumEntities) count = kMaximumEntities;

    std::uint32_t written = 0;
    std::uint32_t labelled = 0;
    std::uint32_t no_transform = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        CabbirdUnityEntityV1 entity{};
        entity.struct_size = sizeof(entity);
        if (g_entities->entity_at(g_entities->user, index, &entity).code !=
            CABBIRD_STATUS_V1_OK) {
            continue;
        }
        EntityRow row{};
        row.entity_id = entity.entity_id;
        row.entity_data = entity.entity_data;
        row.kind = entity.kind;
        row.valid = (entity.flags & CABBIRD_UNITY_ENTITY_V1_VALID) != 0;
        row.bounds_center[0] = entity.bounds_center[0];
        row.bounds_center[1] = entity.bounds_center[1];
        row.bounds_center[2] = entity.bounds_center[2];
        row.distance = static_cast<float>(entity.distance_meters);
        if (entity.label != nullptr && entity.label_size != 0) {
            const std::size_t room = sizeof(row.label) - 1;
            const std::size_t length = entity.label_size < room ? entity.label_size : room;
            std::memcpy(row.label, entity.label, length);
            row.label[length] = '\0';
            ++labelled;
        } else {
            std::snprintf(row.label, sizeof(row.label), "#%llu",
                          static_cast<unsigned long long>(entity.entity_id));
        }
        // A row without a data address cannot be moved, and saying so in the LIST is better than
        // a move that fails after the user picked it.  `entity_data` is present only when the
        // host's struct covers the field; a host that predates it hands over zero.
        if (row.entity_data == 0) ++no_transform;
        g_rows[written++] = row;
    }

    g_rows_total.store(written, std::memory_order_relaxed);
    g_rows_labelled.store(labelled, std::memory_order_relaxed);
    g_rows_no_transform.store(no_transform, std::memory_order_relaxed);
    g_generation.store(g_entities->generation(g_entities->user), std::memory_order_relaxed);
    // RELEASE: the rows above must be complete before the render thread reads the count.
    g_row_count.store(written, std::memory_order_release);
}

// The entity's exact `Transform.position`, and the transform handle it came from.  ONE
// resolution per call, and the caller keeps the handle so the write does not resolve again.
bool ResolveSelected(std::uint64_t entity_id, std::uintptr_t* transform, double position[3],
                     char* reason, std::size_t reason_bytes) {
    if (!TransformUsable()) {
        std::snprintf(reason, reason_bytes,
                      "the host does not publish cabbird.unity.transform (build without it?)");
        return false;
    }
    // THE DATA ADDRESS IS RE-READ BY ID, never taken from the row the user clicked.
    //
    // `entity_at`'s index is only meaningful within one generation and `entity_data` is an
    // address that the game frees and reuses.  Writing through an address read two seconds ago
    // is how a teleport lands on an unrelated object while reporting success, so the id is
    // turned back into a CURRENT address here, and an entity that has gone away is reported as
    // gone.
    CabbirdUnityEntitiesLookupV1 lookup{};
    lookup.struct_size = sizeof(lookup);
    if (g_entities->lookup == nullptr ||
        g_entities->struct_size <
            offsetof(CabbirdUnityEntitiesServiceV1, lookup) +
                sizeof(CabbirdUnityEntitiesServiceV1::lookup)) {
        std::snprintf(reason, reason_bytes, "this host's entity service cannot look up by id");
        return false;
    }
    if (g_entities->lookup(g_entities->user, entity_id, &lookup).code != CABBIRD_STATUS_V1_OK) {
        std::snprintf(reason, reason_bytes,
                      "entity %llu is gone from the current entity set",
                      static_cast<unsigned long long>(entity_id));
        return false;
    }
    if (lookup.data == 0) {
        std::snprintf(reason, reason_bytes, "entity %llu has no data address this generation",
                      static_cast<unsigned long long>(entity_id));
        return false;
    }
    CabbirdUnityTransformResolveRequestV1 request{};
    request.struct_size = sizeof(request);
    request.data = lookup.data;
    CabbirdUnityTransformHandleV1 handle{};
    handle.struct_size = sizeof(handle);
    const CabbirdStatusV1 resolved = g_transform->resolve(g_transform->user, &request, &handle);
    if (resolved.code != CABBIRD_STATUS_V1_OK || handle.transform == 0) {
        std::snprintf(reason, reason_bytes,
                      "the entity's Transform could not be resolved (%s, status %u)",
                      lookup.data_class[0] != '\0' ? lookup.data_class : "class unknown",
                      resolved.code);
        return false;
    }
    CabbirdUnityTransformPositionV1 current{};
    current.struct_size = sizeof(current);
    if (g_transform->position(g_transform->user, &handle, &current).code !=
        CABBIRD_STATUS_V1_OK) {
        std::snprintf(reason, reason_bytes, "the entity's position could not be read");
        return false;
    }
    *transform = static_cast<std::uintptr_t>(handle.transform);
    position[0] = current.position[0];
    position[1] = current.position[1];
    position[2] = current.position[2];
    return true;
}

// Perform one move.  `slot` receives everything the window needs to judge it, including the
// position read BEFORE the write -- without it, "the write did not take" and "the game put it
// back" are indistinguishable afterwards.
void ApplyMove(const Command& command, const double target[3]) {
    WriteSlot slot{};
    slot.used = true;
    slot.kind = command.kind;
    slot.entity_id = command.entity_id;
    std::snprintf(slot.label, sizeof(slot.label), "%s", command.label);
    std::memcpy(slot.requested, target, sizeof(slot.requested));
    slot.sequence = ++g_sequence;

    char reason[192]{};
    std::uintptr_t transform = 0;
    double before[3]{};
    if (!ResolveSelected(command.entity_id, &transform, before, reason, sizeof(reason))) {
        slot.status = CABBIRD_STATUS_V1_NOT_FOUND;
        std::snprintf(g_status, sizeof(g_status), "%s", reason);
        g_applied = slot;
        g_result_dirty.store(true, std::memory_order_release);
        return;
    }
    std::memcpy(slot.before, before, sizeof(slot.before));
    slot.have_before = true;

    CabbirdUnityTransformWriteV1 write{};
    write.struct_size = sizeof(write);
    write.flags = CABBIRD_UNITY_TRANSFORM_WRITE_V1_SET_POSITION;
    write.transform = static_cast<std::uint64_t>(transform);
    std::memcpy(write.position, target, sizeof(write.position));
    slot.status = g_transform->write(g_transform->user, &write).code;

    if (slot.status == CABBIRD_STATUS_V1_OK) {
        // IMMEDIATE RE-READ.  This is not the verdict -- it cannot be, see kVerifyDelayTicks --
        // but it separates "the managed call did nothing" from "the game undid it", and those
        // two have completely different next steps.
        CabbirdUnityTransformPositionV1 now{};
        now.struct_size = sizeof(now);
        CabbirdUnityTransformHandleV1 handle{};
        handle.struct_size = sizeof(handle);
        handle.transform = static_cast<std::uint64_t>(transform);
        if (g_transform->position(g_transform->user, &handle, &now).code ==
            CABBIRD_STATUS_V1_OK) {
            std::memcpy(slot.after, now.position, sizeof(slot.after));
            slot.have_after = true;
            slot.error_now = DistanceBetween(now.position, target);
        }
        // The later verdict.  One pending verification at a time: two overlapping writes to the
        // same entity would make the second re-read meaningless.
        std::snprintf(g_pending_verify_label, sizeof(g_pending_verify_label), "%s",
                      command.label);
        g_pending_verify_id = command.entity_id;
        g_pending_verify_target[0] = target[0];
        g_pending_verify_target[1] = target[1];
        g_pending_verify_target[2] = target[2];
        g_pending_verify_tick = 0;
        g_pending_verify.store(1, std::memory_order_release);
        std::snprintf(g_status, sizeof(g_status),
                      "wrote %s to (%.3f, %.3f, %.3f); re-reading in %llu ticks",
                      command.label, target[0], target[1], target[2],
                      static_cast<unsigned long long>(kVerifyDelayTicks));
    } else {
        std::snprintf(g_status, sizeof(g_status),
                      "the transform write was refused (status %u) for %s", slot.status,
                      command.label);
    }
    g_applied = slot;
    g_result_dirty.store(true, std::memory_order_release);
}

// The delayed re-read: did the entity stay where it was put?
void TickVerify() {
    if (g_pending_verify.load(std::memory_order_acquire) == 0) return;
    ++g_pending_verify_tick;
    if (g_pending_verify_tick < kVerifyDelayTicks) return;
    g_pending_verify.store(0, std::memory_order_release);

    WriteSlot slot{};
    slot.used = true;
    slot.kind = g_applied.kind;
    slot.entity_id = g_pending_verify_id;
    slot.sequence = g_applied.sequence;
    std::snprintf(slot.label, sizeof(slot.label), "%s", g_pending_verify_label);
    std::memcpy(slot.requested, g_pending_verify_target, sizeof(slot.requested));
    std::memcpy(slot.before, g_applied.before, sizeof(slot.before));
    slot.have_before = g_applied.have_before;
    std::memcpy(slot.after, g_applied.after, sizeof(slot.after));
    slot.have_after = g_applied.have_after;
    slot.error_now = g_applied.error_now;

    char reason[192]{};
    std::uintptr_t transform = 0;
    double later[3]{};
    if (!ResolveSelected(g_pending_verify_id, &transform, later, reason, sizeof(reason))) {
        // "IT IS GONE" IS AN ANSWER, and a common honest one: a monster that died, or a scene
        // change.  Reported as such rather than as a failed teleport.
        std::snprintf(g_status, sizeof(g_status),
                      "%s -- the teleport cannot be confirmed (%s)", slot.label, reason);
        g_result_dirty.store(true, std::memory_order_release);
        return;
    }
    std::memcpy(slot.later, later, sizeof(slot.later));
    slot.have_later = true;
    slot.error_later = DistanceBetween(later, slot.requested);
    slot.status = CABBIRD_STATUS_V1_OK;
    if (slot.error_later <= kAcceptanceToleranceMeters) {
        std::snprintf(g_status, sizeof(g_status), "%s is there: %.3f m from the target", slot.label,
                      slot.error_later);
    } else {
        std::snprintf(g_status, sizeof(g_status),
                      "%s REVERTED: the game has it %.3f m from the target again (it was %.3f m "
                      "right after the write)",
                      slot.label, slot.error_later, slot.error_now);
    }
    g_verified = slot;
    g_result_dirty.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// on_update -- the GAME domain.  Everything that touches IL2CPP happens here.
// ---------------------------------------------------------------------------
void CABBIRD_CALL Update(void*, double) {
    // Verification first: it is the oldest outstanding work, and doing it before this tick's
    // commands keeps the "asked for / now / later" triple ordered in time.
    TickVerify();
    RefreshEntities();

    for (;;) {
        const std::uint32_t read = g_command_read.load(std::memory_order_relaxed);
        const std::uint32_t write = g_command_write.load(std::memory_order_acquire);
        if (read == write) break;
        const Command& command = g_commands[read % kMaximumCommandsPerTick];
        const std::uint32_t kind = command.kind;
        // CONSUMED BEFORE IT IS ACTED ON: `g_command_read` is advanced after the copy but the
        // slot is not reused until the head passes it, so a second `on_update` inside one frame
        // cannot see the same command twice.
        g_command_read.store(read + 1, std::memory_order_release);

        if (kind == static_cast<std::uint32_t>(CommandKind::ReadPosition)) {
            char reason[192]{};
            std::uintptr_t transform = 0;
            double position[3]{};
            PositionRead reading{};
            reading.entity_id = command.entity_id;
            if (ResolveSelected(command.entity_id, &transform, position, reason, sizeof(reason))) {
                reading.used = true;
                std::memcpy(reading.position, position, sizeof(reading.position));
                std::snprintf(g_status, sizeof(g_status), "%s at (%.4f, %.4f, %.4f)", command.label,
                              position[0], position[1], position[2]);
            } else {
                std::snprintf(g_status, sizeof(g_status), "%s", reason);
            }
            g_reading = reading;
            g_result_dirty.store(true, std::memory_order_release);
            continue;
        }

        if (kind == static_cast<std::uint32_t>(CommandKind::BringToPlayer)) {
            double target[3] = {command.reference[0], command.reference[1], command.reference[2]};
            ApplyMove(command, target);
            continue;
        }

        if (kind == static_cast<std::uint32_t>(CommandKind::MoveByOffset)) {
            // THE OFFSET IS ADDED TO A POSITION READ **HERE**, on the game thread.  Adding it in
            // the UI would require the entity's live position on the render thread, which is the
            // `il2cpp_runtime_invoke` call this plugin must never make there.
            char reason[192]{};
            std::uintptr_t transform = 0;
            double current[3]{};
            if (!ResolveSelected(command.entity_id, &transform, current, reason, sizeof(reason))) {
                WriteSlot slot{};
                slot.used = true;
                slot.kind = kind;
                slot.entity_id = command.entity_id;
                slot.status = CABBIRD_STATUS_V1_NOT_FOUND;
                slot.sequence = ++g_sequence;
                std::snprintf(slot.label, sizeof(slot.label), "%s", command.label);
                g_applied = slot;
                std::snprintf(g_status, sizeof(g_status), "%s", reason);
                g_result_dirty.store(true, std::memory_order_release);
                continue;
            }
            const double target[3] = {
                current[0] + command.reference[0],
                current[1] + command.reference[1],
                current[2] + command.reference[2],
            };
            ApplyMove(command, target);
            continue;
        }

        if (kind == static_cast<std::uint32_t>(CommandKind::MoveToCoordinates)) {
            ApplyMove(command, command.position);
            continue;
        }
    }
}

// ---------------------------------------------------------------------------
// on_draw -- the RENDER domain.  Buttons enqueue; nothing here calls the engine.
// ---------------------------------------------------------------------------

// The player's pivot, read through the snapshot service (cheap, cached, render-safe).
bool ReadPlayerPivot(CabbirdUnityPlayerSnapshotV1* snapshot) {
    if (!PlayerUsable()) return false;
    CabbirdUnityPlayerSnapshotV1 local{};
    local.struct_size = sizeof(local);
    if (g_player->snapshot(g_player->user, &local).code != CABBIRD_STATUS_V1_OK) return false;
    if ((local.flags & CABBIRD_UNITY_PLAYER_V1_VALID) == 0) return false;
    *snapshot = local;
    return true;
}

// ---------------------------------------------------------------------------
// on_draw -- the RENDER domain.  Buttons enqueue; nothing here calls the engine.
//
// THE SHAPE OF THIS PANEL IS COPIED FROM A WORKING PLUGIN, and that is the point: after three
// failed attempts at "a list of entities" (clickable `text_link` rows the host refuses to draw as
// links, a `combo` popup that crashed the game when the GUI opened, and a `begin_child` region
// that can collapse and hide its contents) the answer was already in the tree next door --
// `Anomaly/plugins/PinkPawHeistESP`, which draws its loot list as:
//
//   * a TABLE (`begin_table` / `table_next_row` / `table_next_column` / `end_table`) with one
//     column per value and ONE ACTION BUTTON PER ROW, so nothing has to be selected first;
//   * PAGINATION (First/Previous/Next/Last) with a fixed rows-per-page, so a list of 81 rows is
//     never asked to fit in one window;
//   * a guarded call for every optional UI entry (`HasUiField`), with a plain-text fallback when
//     the host build does not publish it;
//   * a `begin_child` that is ALWAYS matched by `end_child`, whether or not the child is visible.
//
// That is the design here.  Picking an entity and then pressing a button was the wrong shape for
// this host: it needs a selection to survive frames and to be rendered by a control that can be
// missing or refused.  Acting per row needs neither.
// ---------------------------------------------------------------------------

// Whether the host's UI table carries a given field.  `struct_size` is the plugin ABI's own
// versioning mechanism: a host built before a field simply has a shorter table, and reading past
// it is a read of another function pointer.  PinkPawHeistESP guards every optional entry this way
// and this plugin now does too.
template <typename Field>
bool HasUiField(const CabbirdUiServiceV1* ui, std::size_t offset) noexcept {
    if (ui == nullptr) return false;
    const std::size_t needed = offset + sizeof(Field);
    return ui->struct_size >= needed;
}

bool TableUiAvailable(const CabbirdUiServiceV1* ui) noexcept {
    return HasUiField<decltype(CabbirdUiServiceV1::end_table)>(
               ui, offsetof(CabbirdUiServiceV1, end_table)) &&
           ui->begin_table != nullptr && ui->table_next_row != nullptr &&
           ui->table_next_column != nullptr && ui->end_table != nullptr;
}

bool ChildUiAvailable(const CabbirdUiServiceV1* ui) noexcept {
    return HasUiField<decltype(CabbirdUiServiceV1::end_child)>(
               ui, offsetof(CabbirdUiServiceV1, end_child)) &&
           ui->begin_child != nullptr && ui->end_child != nullptr;
}

// The label a row's action button carries.  UNIQUE PER ROW, because ImGui identifies a widget by
// its label: two rows both labelled "bring to me" are ONE button to ImGui, and only one of them
// can ever be pressed.  The entity id goes in the label, which is also what makes the pressed
// row's target unambiguous.
int FormatBringLabel(char* buffer, std::size_t bytes, std::uint64_t entity_id) {
    return std::snprintf(buffer, bytes, "bring to me##%llu",
                         static_cast<unsigned long long>(entity_id));
}

// Queue "move this entity to me" for one row.  Shared by the row button and the "bring the
// nearest" shortcut so both build the same request.
void QueueBringToPlayer(std::uint64_t entity_id, const char* label,
                        const CabbirdUnityPlayerSnapshotV1& player_snapshot) {
    Command command{};
    command.kind = static_cast<std::uint32_t>(CommandKind::BringToPlayer);
    command.entity_id = entity_id;
    command.reference[0] = player_snapshot.position[0];
    // The offset is applied along Y, which in Unity is up -- the axis a user means by "put it
    // next to me" rather than "inside me".
    command.reference[1] = player_snapshot.position[1] + static_cast<double>(g_bring_offset_y);
    command.reference[2] = player_snapshot.position[2];
    std::snprintf(command.label, sizeof(command.label), "%s", label);
    if (!Enqueue(command)) {
        SetStatus("the command queue is full -- the game tick is not draining it");
    } else {
        std::snprintf(g_status, sizeof(g_status), "queued: bring %s to the player", label);
    }
}

void Draw(void*, const CabbirdUiServiceV1*) {
    // The table validated at load time, not the parameter: a generation can be drawn by a host
    // whose table pointer is not the one it loaded against.
    const CabbirdUiServiceV1* const ui = g_ui;
    if (ui == nullptr) return;

    cabbird::sdk::UiWindow window(ui, "Entity Teleport", &g_open);
    if (!window) return;

    const bool entities_ok = EntitiesUsable();
    const bool transform_ok = TransformUsable();
    const bool player_ok = PlayerUsable();

    char line[256]{};
    if (!entities_ok || !transform_ok) {
        ui->text(ui->user, StringView("this host build cannot move entities:"));
        if (!entities_ok) {
            ui->text(ui->user,
                     StringView("  - cabbird.unity.entities is not published (no entity list)"));
        }
        if (!transform_ok) {
            ui->text(ui->user, StringView("  - cabbird.unity.transform is not published (no write "
                                          "path: resolve/position/write)"));
        }
    }

    const std::uint32_t count = g_row_count.load(std::memory_order_acquire);
    const std::uint32_t total = g_rows_total.load(std::memory_order_relaxed);
    const std::uint32_t no_transform = g_rows_no_transform.load(std::memory_order_relaxed);
    std::snprintf(line, sizeof(line), "%u entities this generation (%u without a data address)",
                  total, no_transform);
    ui->text(ui->user, StringView(line));

    CabbirdUnityPlayerSnapshotV1 player_snapshot{};
    const bool player_live = player_ok && ReadPlayerPivot(&player_snapshot);
    if (player_live) {
        std::snprintf(line, sizeof(line), "player (pivot): (%.3f, %.3f, %.3f)",
                      player_snapshot.position[0], player_snapshot.position[1],
                      player_snapshot.position[2]);
        ui->text(ui->user, StringView(line));
    } else {
        ui->text(ui->user,
                 StringView("player (pivot): -- (no live player; 'bring to me' cannot be used)"));
    }

    // THE CLASS BREAKDOWN, in the words the host's own diagnostic uses.  "81 entities" and
    // "0 rows" are two facts that do not explain each other; "unclassified=81" does.
    std::uint32_t kind_counts[5]{};
    std::uint32_t wanted_rows = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        const EntityRow& row = g_rows[index];
        if (row.entity_id == 0) continue;
        if (row.kind < 5) ++kind_counts[row.kind];
        if (RowWanted(row)) ++wanted_rows;
    }
    std::snprintf(line, sizeof(line),
                  "by class: unclassified=%u player=%u monster=%u npc=%u world=%u", kind_counts[0],
                  kind_counts[1], kind_counts[2], kind_counts[3], kind_counts[4]);
    ui->text(ui->user, StringView(line));

    // --- filters, above the table they filter ----------------------------------------------
    ui->separator(ui->user);
    ui->checkbox(ui->user, StringView("players"), &g_filter_player);
    ui->same_line(ui->user, 0.0f, 12.0f);
    ui->checkbox(ui->user, StringView("monsters"), &g_filter_monster);
    ui->same_line(ui->user, 0.0f, 12.0f);
    ui->checkbox(ui->user, StringView("npcs"), &g_filter_npc);
    ui->same_line(ui->user, 0.0f, 12.0f);
    ui->checkbox(ui->user, StringView("world"), &g_filter_world);
    ui->same_line(ui->user, 0.0f, 12.0f);
    ui->checkbox(ui->user, StringView("unclassified"), &g_filter_unclassified);
    if (!AnyKindEnabled()) {
        ui->text(ui->user, StringView("all class switches off: every entity is shown"));
    }
    if (ui->checkbox(ui->user, StringView("filter by name"), &g_search_enabled) != 0) {
        if (g_search_enabled == 0) g_search[0] = '\0';
    }
    if (g_search_enabled != 0) {
        ui->input_text(ui->user, StringView("contains"), g_search, sizeof(g_search), 0);
    }

    // --- the landing height, which is a property of the ONLY action that has one ------------
    ui->slider_float(ui->user, StringView("landing height above the player (m)"), &g_bring_offset_y,
                     -2.0f, 5.0f);

    // --- the table --------------------------------------------------------------------------
    ui->separator(ui->user);
    constexpr std::uint32_t kRowsPerPage = 12;
    if (wanted_rows == 0) {
        if (!entities_ok) {
            ui->text(ui->user, StringView("the host does not publish cabbird.unity.entities"));
        } else if (total == 0) {
            ui->text(ui->user, StringView("the entity set is empty (no world scene, or the host has "
                                          "not walked it yet)"));
        } else if (g_search_enabled != 0 && g_search[0] != '\0') {
            ui->text(ui->user, StringView("nothing matches the name filter"));
        } else {
            ui->text(ui->user, StringView("nothing passes the class switches above"));
        }
    } else {
        const std::uint32_t page_count = (wanted_rows + kRowsPerPage - 1) / kRowsPerPage;
        if (g_page >= page_count) g_page = page_count - 1;
        const std::uint32_t first = g_page * kRowsPerPage;
        const std::uint32_t last = (first + kRowsPerPage < wanted_rows) ? first + kRowsPerPage
                                                                       : wanted_rows;
        std::snprintf(line, sizeof(line),
                      "%u entities match; rows %u-%u of %u   (page %u/%u)", wanted_rows, first + 1,
                      last, wanted_rows, g_page + 1, page_count);
        ui->text(ui->user, StringView(line));

        const bool table_ok = TableUiAvailable(ui);
        const int table_open = table_ok ? ui->begin_table(ui->user, StringView("entities"), 4,
                                                          CABBIRD_UI_TABLE_V1_NONE, 0.0f,
                                                          260.0f)
                                        : 0;
        if (table_open != 0) {
            ui->table_next_row(ui->user);
            static_cast<void>(ui->table_next_column(ui->user));
            ui->text(ui->user, StringView("entity"));
            static_cast<void>(ui->table_next_column(ui->user));
            ui->text(ui->user, StringView("class"));
            static_cast<void>(ui->table_next_column(ui->user));
            ui->text(ui->user, StringView("box centre"));
            static_cast<void>(ui->table_next_column(ui->user));
            ui->text(ui->user, StringView("action"));
        }

        // One pass over the filtered rows, drawing `[first, last)` of them.  A row is drawn as
        // table cells when the table opened and as plain lines otherwise -- the same FAILURE
        // TOLERANCE PinkPawHeistESP uses, and the reason its list cannot go blank because one
        // optional entry is missing.
        std::uint32_t position = 0;
        std::uint32_t drawn = 0;
        for (std::uint32_t index = 0; index < count && position < last; ++index) {
            const EntityRow& row = g_rows[index];
            if (!RowWanted(row)) continue;
            const std::uint32_t slot = position++;
            if (slot < first) continue;

            char coordinates[64]{};
            std::snprintf(coordinates, sizeof(coordinates), "%.1f, %.1f, %.1f",
                          row.bounds_center[0], row.bounds_center[1], row.bounds_center[2]);
            char bring_label[64]{};
            FormatBringLabel(bring_label, sizeof(bring_label), row.entity_id);

            if (table_open != 0) {
                ui->table_next_row(ui->user);
                static_cast<void>(ui->table_next_column(ui->user));
                ui->text(ui->user, StringView(row.label));
                static_cast<void>(ui->table_next_column(ui->user));
                ui->text(ui->user, StringView(KindName(row.kind)));
                static_cast<void>(ui->table_next_column(ui->user));
                ui->text(ui->user, StringView(coordinates));
                static_cast<void>(ui->table_next_column(ui->user));
                // THE ACTION IS PER ROW, and its enabled state is the honest answer to "can this
                // one be moved": a row whose data address the host did not publish cannot be.
                if (ui->button_enabled(ui->user, StringView(bring_label), 0.0f, 0.0f,
                                       (row.entity_data != 0 && transform_ok && player_live) ? 1
                                                                                            : 0) !=
                    0) {
                    QueueBringToPlayer(row.entity_id, row.label, player_snapshot);
                }
            } else {
                std::snprintf(line, sizeof(line), "%s  [%s]  %s", row.label, KindName(row.kind),
                              coordinates);
                ui->text(ui->user, StringView(line));
                if (ui->button_enabled(ui->user, StringView(bring_label), 0.0f, 0.0f,
                                       (row.entity_data != 0 && transform_ok && player_live) ? 1
                                                                                            : 0) !=
                    0) {
                    QueueBringToPlayer(row.entity_id, row.label, player_snapshot);
                }
            }
            ++drawn;
        }
        if (table_open != 0) ui->end_table(ui->user);
        if (drawn == 0) {
            ui->text(ui->user, StringView("no row fell on this page -- the list changed underneath "
                                          "the page number"));
        }

        // --- pagination, the way PinkPawHeistESP draws it: buttons with a scope guard ---------
        ui->separator(ui->user);
        std::snprintf(line, sizeof(line), "page %u / %u", g_page + 1, page_count);
        ui->text(ui->user, StringView(line));
        const auto page_button = [&](const char* label, bool enabled, std::uint32_t destination) {
            if (ui->button_enabled(ui->user, StringView(label), 0.0f, 0.0f, enabled ? 1 : 0) != 0) {
                g_page = destination;
            }
        };
        const bool has_previous = g_page > 0;
        const bool has_next = g_page + 1 < page_count;
        page_button("first page", has_previous, 0);
        ui->same_line(ui->user, 0.0f, 6.0f);
        page_button("previous", has_previous, has_previous ? g_page - 1 : 0);
        ui->same_line(ui->user, 0.0f, 6.0f);
        page_button("next", has_next, has_next ? g_page + 1 : page_count - 1);
        ui->same_line(ui->user, 0.0f, 6.0f);
        page_button("last page", has_next, page_count - 1);
    }

    // --- the two coordinate actions, which are not per row ----------------------------------
    //
    // "Move the nearest matching entity to me" is the shortcut the feature is actually for: with
    // 81 entities in the list, the fastest path is not to find the row but to ask.  The coordinate
    // teleport is the generic one and it takes an entity id from the text box, because a coordinate
    // action for a row would need a second text box per row.
    ui->separator(ui->user);
    ui->text(ui->user, StringView("shortcuts"));
    if (ui->button_enabled(ui->user, StringView("bring the nearest matching entity to me"), 0.0f,
                           0.0f, (wanted_rows != 0 && player_live && transform_ok) ? 1 : 0) != 0) {
        // "Nearest" is measured from the player's pivot, against the entity set's box centre --
        // the only distance the host publishes without a per-entity engine call.
        std::uint64_t best_id = 0;
        char best_label[kLabelBytes]{};
        double best_distance = 0.0;
        if (player_live) {
            for (std::uint32_t index = 0; index < count; ++index) {
                const EntityRow& row = g_rows[index];
                if (!RowWanted(row) || row.entity_data == 0) continue;
                const double distance = DistanceBetween(row.bounds_center, player_snapshot.position);
                if (best_id == 0 || distance < best_distance) {
                    best_id = row.entity_id;
                    best_distance = distance;
                    std::snprintf(best_label, sizeof(best_label), "%s", row.label);
                }
            }
        }
        if (best_id == 0) {
            SetStatus("no entity with a published data address passes the filters");
        } else {
            std::snprintf(line, sizeof(line), "nearest: %s at %.1f m", best_label, best_distance);
            SetStatus(line);
            QueueBringToPlayer(best_id, best_label, player_snapshot);
        }
    }

    char target_id_text[32]{"0"};
    std::snprintf(target_id_text, sizeof(target_id_text), "%llu",
                  static_cast<unsigned long long>(g_last_target_id));
    ui->text(ui->user, StringView("move one entity to typed coordinates"));
    ui->input_text(ui->user, StringView("entity id"), target_id_text, sizeof(target_id_text), 0);
    g_last_target_id = std::strtoull(target_id_text, nullptr, 10);
    ui->input_text(ui->user, StringView("x"), g_target_text[0], sizeof(g_target_text[0]), 0);
    ui->input_text(ui->user, StringView("y"), g_target_text[1], sizeof(g_target_text[1]), 0);
    ui->input_text(ui->user, StringView("z"), g_target_text[2], sizeof(g_target_text[2]), 0);
    if (ui->button_enabled(ui->user, StringView("teleport that entity to those coordinates"), 0.0f,
                           0.0f, (g_last_target_id != 0 && transform_ok) ? 1 : 0) != 0) {
        double values[3]{};
        if (!ParseVector(g_target_text, values)) {
            SetStatus("one of the coordinates is not a number (or is outside +/-1e6)");
        } else {
            Command command{};
            command.kind = static_cast<std::uint32_t>(CommandKind::MoveToCoordinates);
            command.entity_id = g_last_target_id;
            command.position[0] = values[0];
            command.position[1] = values[1];
            command.position[2] = values[2];
            std::snprintf(command.label, sizeof(command.label), "entity %llu",
                          static_cast<unsigned long long>(g_last_target_id));
            if (!Enqueue(command)) {
                SetStatus("the command queue is full -- the game tick is not draining it");
            } else {
                std::snprintf(g_status, sizeof(g_status),
                              "queued: move entity %llu to (%.2f, %.2f, %.2f)",
                              static_cast<unsigned long long>(g_last_target_id), values[0],
                              values[1], values[2]);
            }
        }
    }

    // --- the verdict ------------------------------------------------------------------------
    ui->separator(ui->user);
    ui->text(ui->user, StringView(g_status));

    if (g_applied.used) {
        const WriteSlot& slot = g_applied;
        std::snprintf(line, sizeof(line), "#%u %s: asked for (%.3f, %.3f, %.3f)", slot.sequence,
                      slot.label, slot.requested[0], slot.requested[1], slot.requested[2]);
        ui->text(ui->user, StringView(line));
        if (slot.have_before) {
            std::snprintf(line, sizeof(line), "was       (%.3f, %.3f, %.3f)", slot.before[0],
                          slot.before[1], slot.before[2]);
            ui->text(ui->user, StringView(line));
        }
        if (slot.have_after) {
            std::snprintf(line, sizeof(line), "right after (%.3f, %.3f, %.3f)  error %.3f m",
                          slot.after[0], slot.after[1], slot.after[2], slot.error_now);
            ui->text(ui->user, StringView(line));
        } else if (slot.status != CABBIRD_STATUS_V1_OK) {
            std::snprintf(line, sizeof(line), "the write was refused (status %u)", slot.status);
            ui->text(ui->user, StringView(line));
        }
    }
    if (g_verified.used && g_verified.have_later) {
        const WriteSlot& slot = g_verified;
        std::snprintf(line, sizeof(line),
                      "4 ticks later (%.3f, %.3f, %.3f)  error %.3f m  ->  %s", slot.later[0],
                      slot.later[1], slot.later[2], slot.error_later,
                      slot.error_later <= kAcceptanceToleranceMeters ? "HELD" : "REVERTED");
        ui->text(ui->user, StringView(line));
    }

    // THE PART THAT KEEPS THE WINDOW HONEST.  A local transform write is not a server command;
    // an entity the server owns can be put back after this window stops looking.
    ui->separator(ui->user);
    ui->text(ui->user, StringView("client-side transform write: a server-owned entity can be put "
                                  "back after the check above."));
    ui->text(ui->user, StringView("Moving YOURSELF this way does not move you on the server -- use "
                                  "the Player Teleport plugin for that."));

    std::snprintf(line, sizeof(line), "generation %llu   queued commands dropped: %u",
                  static_cast<unsigned long long>(g_generation.load(std::memory_order_relaxed)),
                  g_commands_dropped.load(std::memory_order_relaxed));
    ui->text(ui->user, StringView(line));

    g_result_dirty.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

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

    // ALL THREE ARE OPTIONAL, and deliberately so.  A missing service must leave a window that
    // says which one is missing -- taking the panel away would hide the diagnostic, and a host
    // build without the transform service is a normal state (it is published by the game
    // adapter, which needs a validated profile binding).
    g_entities = services
                     .Query<CabbirdUnityEntitiesServiceV1>(CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID,
                                                           CABBIRD_UNITY_ENTITIES_SERVICE_V1_VERSION)
                     .get();
    g_transform = services
                      .Query<CabbirdUnityTransformServiceV1>(
                          CABBIRD_UNITY_TRANSFORM_SERVICE_V1_ID,
                          CABBIRD_UNITY_TRANSFORM_SERVICE_V1_VERSION)
                      .get();
    g_player = services
                   .Query<CabbirdUnityPlayerServiceV1>(CABBIRD_UNITY_PLAYER_SERVICE_V1_ID,
                                                       CABBIRD_UNITY_PLAYER_SERVICE_V1_VERSION)
                   .get();

    SetStatus(TransformUsable() && EntitiesUsable()
                  ? "ready; a move is performed by the host on its next game tick"
                  : "a required service is missing -- see the lines above");
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) { return Ok(); }
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) { return Ok(); }

void CABBIRD_CALL Unload(void*) {
    // THE SERVICE POINTERS ARE DELIBERATELY **NOT** CLEARED HERE.
    //
    // They used to be (`g_ui = nullptr; g_entities = nullptr; ...`) as a tidiness measure, and
    // that was a crash waiting to happen: a hot reload calls `on_unload` on the load thread while
    // the render thread may be a few instructions from entering `on_draw` or the game thread from
    // entering `on_update`.  Nulling the tables under those callbacks turns "a stale call" into a
    // null dereference, and the crash signature is exactly that shape -- a faulting instruction
    // pointer of 0x2 with a read of address 0, on the render thread, seconds after a plugin
    // reload.
    //
    // The host's service tables are process-lifetime objects owned by the host (the SDK says so:
    // "service tables reached through query_service are host-owned and outlive the plugin"), so
    // leaving the pointers alone costs nothing.  Only the plugin's OWN state is reset, and a late
    // callback then finds a consistent, empty world instead of a half-emptied one.
    g_command_write.store(0, std::memory_order_relaxed);
    g_command_read.store(0, std::memory_order_relaxed);
    g_pending_verify.store(0, std::memory_order_relaxed);
    g_row_count.store(0, std::memory_order_relaxed);
    g_rows_total.store(0, std::memory_order_relaxed);
    g_applied = WriteSlot{};
    g_verified = WriteSlot{};
    g_reading = PositionRead{};
    g_selected_id = 0;
    g_selected_row = -1;
    g_page = 0;
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    // Field by field: `CabbirdStringViewV1` has no converting constructor, so an aggregate
    // initialiser with `StringView(...)` elements does not compile.
    descriptor->id = StringView("cabbird.entity-teleport");
    descriptor->name = StringView("Entity Teleport");
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
