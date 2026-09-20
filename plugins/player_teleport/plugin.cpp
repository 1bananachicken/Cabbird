// Player Teleport -- move the local player to arbitrary world coordinates.
//
// THE UE5 SENTENCE THIS REPLACES
// ------------------------------
// In the sibling UE5 project a teleport is `K2_SetActorLocation(actor, location, sweep,
// teleport)`, reached through `ProcessEvent`, from the plugin, directly.  Everything that
// makes that short also makes it impossible here:
//
//   * UE5 has ONE funnel for every UFunction call, so a plugin that knows an object pointer
//     can call anything on it;
//   * the actor's location is a `UPROPERTY` the game keeps in the object, so the call is
//     just a way of writing it.
//
// Unity/IL2CPP has neither.  There is no funnel, and the position is
// not in managed memory at all -- it is inside Unity's native `Transform` behind
// `m_CachedPtr`, an undocumented layout this project refuses to guess.  So the write is a
// managed CALL, and a call needs the runtime's invocation entry, a `MethodInfo*` and an
// attached game thread: three things only the host has.  This plugin therefore asks
// `cabbird.unity.player` to move the player and reports what actually happened.
//
// WHY THE RESULT IS NOT JUST "OK"
// -------------------------------
// The single most common way a Unity teleport fails is not an error -- it is a
// `CharacterController`, a `Rigidbody` or a movement state machine putting the character
// back on the next frame.  The call succeeds, the position is right, and the player never
// moves.  The host therefore reads the position again a few ticks later and reports
// REVERTED when it found the old position back, with both positions attached.  This window
// shows that verdict verbatim, because "the teleport did nothing" and "the teleport was
// refused" have completely different next steps.
//
// WHAT THIS PLUGIN DELIBERATELY DOES NOT DO
// -----------------------------------------
//   * no offsets, no IL2CPP, no memory writes of its own: it cannot corrupt anything the
//     host has not already validated;
//   * no navigation, no pathing, no "teleport to marker": only the coordinates the user
//     typed.  Anything cleverer needs the game's own map data, which is a different feature
//     with a different data source (the sibling project's map-landmark service).
//
// THE HIDDEN-ROLE LIST IS THE ONE THING HERE THAT COSTS THE GAME FRAME TIME, AND IT IS
// THEREFORE ASKED FOR, NOT POLLED
// --------------------------------------------------------------------------------------
// `cabbird.unity.entities` is not a free lookup: it is a cache the HOST builds on the GAME
// thread, and it only builds it while a consumer keeps asking.  Every `entity_count` call
// renews a 250 ms lease, and for as long as that lease is alive the host walks the whole
// live entity set on EVERY game tick -- `il2cpp_runtime_invoke` per entity, measured by the
// host at 213 ms of work per tick across ~80 entities.  The host's own comment calls the
// walk "the frame rate", and it is.
//
// Version 0.2.0 refreshed the hidden-role list every 0.25 s unconditionally, from `on_update`,
// whether or not the window was open.  A 0.25 s poll against a 250 ms lease is not a 4 Hz
// cost: it is a PERMANENT lease, so the game paid the full entity walk on every tick forever,
// which presents as the plugin making the game stutter.  `player_coords` never stuttered
// beside it because it never touches this service at all -- the difference between the two
// plugins was never the player snapshot, it was this list.
//
// So the rule this file now follows: THIS PLUGIN ASKS FOR ENTITIES ONLY WHILE THE USER IS
// ASKING FOR ROLES.
//
//   * nothing at all while the window is closed -- the lease lapses and the host's walk
//     switches itself off;
//   * one press of "rescan retained roles" opens a short SCAN WINDOW (see `ServiceEntityScans`)
//     and then stops.  The window exists because the host's cache is rebuilt on ITS next game
//     tick, not on the call that asks for it, so a single call after an idle period would show
//     the previous cache -- or nothing at all after a scene change;
//   * live tracking is available and OFF BY DEFAULT.  Ticking it is the old behaviour, at a
//     fraction of the rate and with its cost stated in the window instead of hidden.

#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/services/core.h"
#include "cabbird/sdk/services/ui.h"
#include "cabbird/sdk/services/unity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

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
// THE WRITE IS A SECOND SERVICE, and that is the point of the split: this plugin needs BOTH --
// the snapshot to show where the player is now, and `player-teleport` to move them.  A
// coordinate display needs only the first.
const CabbirdUnityPlayerTeleportServiceV1* g_teleport{};
const CabbirdUnityEntitiesServiceV1* g_entities{};
const CabbirdUnityTransformServiceV1* g_transform{};

int g_open = 1;
// Position as TEXT, not as three floats.  A teleport target is typed (usually pasted from
// somewhere -- a guide, a previous reading, a chat message), and `input_double`'s
// drag-to-change behaviour is actively harmful for coordinates: a stray drag across a
// 1e5-wide slider is a teleport to the other side of the map.  Text is also the only way to
// accept "-1234.5678" without a precision argument.
char g_position_text[3][32] = {"0.000", "0.000", "0.000"};
// Rotation, in Unity's euler convention.  `input` here is degrees with the SIGN WARNING of
// ui.h: POSITIVE x pitches DOWN.  Off by default -- a teleport that also spins the camera is
// surprising, and the two are separate properties on the engine side anyway.
char g_rotation_text[3][32] = {"0.000", "0.000", "0.000"};
int g_set_rotation = 0;
int g_relative = 0;

CabbirdUnityPlayerSnapshotV1 g_snapshot{};
CabbirdUnityPlayerWriteResultV1 g_result{};
int g_have_result = 0;
char g_status[220] = "no request made yet";

struct HiddenRoleSlot {
    bool valid{};
    bool position_valid{};
    std::uint64_t entity_id{};
    std::uint64_t generation{};
    std::uint64_t data{};
    char data_class[64]{};
    double position[3]{};
};

struct HiddenRoleRequest {
    bool queued{};
    int slot{-1};
    std::uint64_t entity_id{};
    std::uint64_t data{};
    double position[3]{};
    double euler_degrees[3]{};
    std::uint32_t flags{};
};

// The retained-role list is bounded, and the bound is stated in the window when it bites.
// Unbounded was the previous behaviour and it is not affordable: the list is copied for the
// panel on every drawn frame, and every entry is a Transform resolve plus a
// `Transform::get_position` call on the game thread during a scan.  Eight is far more than a
// party can hold, and "N more not shown" is a better answer than an unbounded window.
constexpr std::size_t kMaximumHiddenRoles = 8;

std::vector<HiddenRoleSlot> g_hidden_roles;
HiddenRoleRequest g_hidden_request{};
std::mutex g_hidden_mutex;
char g_hidden_status[220] = "not scanned yet -- press rescan";

// --- the entity-service budget -------------------------------------------------
//
// See the file header.  The only place `entity_count` may be called from is
// `RefreshHiddenRoles`, and the only place that may be called from is `ServiceEntityScans`,
// which is gated on the window being open AND on the user having asked.
//
// THE WINDOW IS MEASURED IN GAME TIME (`on_update`'s delta), NOT WALL TIME, and that is a
// deliberate choice rather than an accident: the cost this window is sized against is paid by
// the host on its own game ticks, so "long enough to contain the host's next walk" is a
// statement about ticks.  It also makes the schedule reproducible offline -- a harness that
// drives 600 ticks in a millisecond gets 10 seconds of game time, not 1 millisecond.
constexpr double kScanStepSeconds = 0.4;       // between the scans inside one window
constexpr double kScanWindowSeconds = 0.8;     // long enough to contain a host walk
constexpr double kTrackIntervalSeconds = 5.0;  // opt-in live tracking: one window per 5 s

int g_track_roles = 0;             // OFF by default; this is the old 0.2.0 behaviour
int g_rescan_requested = 0;        // set by the button, consumed on the game thread
double g_scan_window_seconds = 0.0;  // remaining game time in the current window
double g_scan_step_seconds = 0.0;
double g_track_seconds = 0.0;
std::uint64_t g_scan_count = 0;
double g_last_scan_millis = 0.0;

double NowSeconds() {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    ::QueryPerformanceFrequency(&frequency);
    ::QueryPerformanceCounter(&counter);
    if (frequency.QuadPart == 0) return 0.0;
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

bool PlayerAvailable() {
    // `snapshot` is the READ (the panel shows where the player is) and the teleport table is the
    // WRITE.  Both are required here; neither is required by `player_coords`, which is exactly the
    // distinction the two grants exist to make.
    return g_player != nullptr && g_player->snapshot != nullptr && g_teleport != nullptr &&
           g_teleport->teleport != nullptr && g_teleport->state != nullptr;
}

bool HiddenRoleAvailable() {
    return g_entities != nullptr && g_entities->entity_count != nullptr &&
           g_entities->entity_at != nullptr && g_transform != nullptr &&
           g_transform->resolve != nullptr && g_transform->position != nullptr &&
           g_transform->write != nullptr;
}

// Parse one component.  `strtod` and an explicit end check rather than `atof`: a typo that
// leaves trailing garbage ("12.3x") must not silently become 12.3, because the user would
// then be teleported somewhere plausible and wrong.
bool ParseComponent(const char* text, double* value) {
    if (text == nullptr || value == nullptr) return false;
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (end == text) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end != '\0') return false;
    if (!std::isfinite(parsed)) return false;
    // The same bound the host enforces, checked HERE so the user gets a message about the
    // field they typed instead of a refusal from a service they cannot see.
    if (std::fabs(parsed) > 1.0e6) return false;
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

void SetHiddenStatus(const char* text) {
    std::scoped_lock lock(g_hidden_mutex);
    std::snprintf(g_hidden_status, sizeof(g_hidden_status), "%s", text != nullptr ? text : "");
}

double SquaredDistance(const double left[3], const double right[3]) {
    const double dx = left[0] - right[0];
    const double dy = left[1] - right[1];
    const double dz = left[2] - right[2];
    return dx * dx + dy * dy + dz * dz;
}

bool LabelMatchesClass(const char* label, const char* data_class) {
    if (label == nullptr || data_class == nullptr || data_class[0] == '\0') return false;
    const std::size_t length = std::strlen(data_class);
    return std::strncmp(label, data_class, length) == 0 &&
           (label[length] == '\0' || label[length] == '#');
}

void RefreshHiddenRoles() {
    if (!HiddenRoleAvailable()) return;

    // TIMED, because this function is the only thing in this plugin that spends the game's
    // frame time and the user is entitled to see what a scan cost.  It is the plugin's own
    // half only -- the host's entity walk is the larger half and happens on the host's tick,
    // not inside this call.
    const double scan_started = NowSeconds();

    CabbirdUnityPlayerSnapshotV1 current{};
    current.struct_size = sizeof(current);
    const bool have_current =
        g_player != nullptr && g_player->snapshot != nullptr &&
        g_player->snapshot(g_player->user, &current).code == CABBIRD_STATUS_V1_OK &&
        (current.flags & CABBIRD_UNITY_PLAYER_V1_VALID) != 0;

    std::vector<HiddenRoleSlot> previous;
    {
        std::scoped_lock lock(g_hidden_mutex);
        previous = g_hidden_roles;
    }

    // Every player-shaped entity that is not the controlled one gets a slot.  The old
    // "first two records" bug -- where role 1 was consumed by `PlayerData#...` and the second
    // hidden role was never reached -- is fixed by identifying the active entity rather than
    // by stopping after N candidates.
    std::vector<HiddenRoleSlot> candidates;
    const std::uint32_t count = g_entities->entity_count(g_entities->user);
    candidates.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        CabbirdUnityEntityV1 entity{};
        entity.struct_size = sizeof(entity);
        if (g_entities->entity_at(g_entities->user, index, &entity).code !=
            CABBIRD_STATUS_V1_OK) {
            continue;
        }
        if (entity.kind != CABBIRD_UNITY_ENTITY_V1_KIND_PLAYER || entity.entity_data == 0) {
            continue;
        }

        candidates.emplace_back();
        HiddenRoleSlot& slot = candidates.back();
        slot.valid = true;
        slot.entity_id = entity.entity_id;
        slot.generation = g_entities->generation != nullptr
            ? g_entities->generation(g_entities->user)
            : 0;
        slot.data = entity.entity_data;
        if (entity.label != nullptr && entity.label_size != 0) {
            const std::size_t size =
                std::min<std::size_t>(entity.label_size, sizeof(slot.data_class) - 1);
            std::memcpy(slot.data_class, entity.label, size);
            slot.data_class[size] = '\0';
        } else {
            std::snprintf(slot.data_class, sizeof(slot.data_class), "%s", "PlayerData");
        }

        CabbirdUnityTransformResolveRequestV1 resolve{};
        resolve.struct_size = sizeof(resolve);
        resolve.data = slot.data;
        CabbirdUnityTransformHandleV1 handle{};
        handle.struct_size = sizeof(handle);
        if (g_transform->resolve(g_transform->user, &resolve, &handle).code ==
                CABBIRD_STATUS_V1_OK &&
            handle.transform != 0) {
            CabbirdUnityTransformPositionV1 position{};
            position.struct_size = sizeof(position);
            position.transform = handle.transform;
            if (g_transform->position(g_transform->user, &handle, &position).code ==
                CABBIRD_STATUS_V1_OK) {
                std::memcpy(slot.position, position.position, sizeof(slot.position));
                slot.position_valid = true;
            }
        }
    }

    std::size_t active = candidates.size();
    if (have_current) {
        // Prefer the literal id when both services use the same identity.
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (candidates[index].entity_id == current.entity_id) {
                active = index;
                break;
            }
        }
        // The authoritative player service currently publishes m_entityId while the entity
        // cache publishes the managed data address.  In that case identify the controlled
        // PlayerData by class label and exact Transform position.
        if (active == candidates.size()) {
            double best = 4.0;  // two metres squared; the live pair is normally identical.
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                if (!candidates[index].position_valid ||
                    !LabelMatchesClass(candidates[index].data_class, current.data_class)) {
                    continue;
                }
                const double distance = SquaredDistance(candidates[index].position,
                                                        current.position);
                if (distance < best) {
                    best = distance;
                    active = index;
                }
            }
        }
        // Last resort for a localized label: position is still a stronger identity signal than
        // enumeration order, and prevents the active entity occupying role 1.
        if (active == candidates.size()) {
            double best = 4.0;
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                if (!candidates[index].position_valid) continue;
                const double distance = SquaredDistance(candidates[index].position,
                                                        current.position);
                if (distance < best) {
                    best = distance;
                    active = index;
                }
            }
        }
    }

    std::vector<HiddenRoleSlot> refreshed;
    std::vector<bool> used(candidates.size(), false);
    // Preserve the previous order while an entity remains hidden, then append newly discovered
    // entities. Slots remain stable without hiding any additional role entities.
    for (const HiddenRoleSlot& old_slot : previous) {
        if (!old_slot.valid) continue;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (index == active || used[index] ||
                candidates[index].entity_id != old_slot.entity_id) {
                continue;
            }
            refreshed.push_back(candidates[index]);
            used[index] = true;
            break;
        }
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (index == active || used[index]) continue;
        refreshed.push_back(candidates[index]);
        used[index] = true;
    }

    // BOUNDED, and the overflow is reported rather than silently dropped: a role the window
    // does not show is a role the user cannot teleport to, and "role 9 exists but is not
    // listed" is a fact they need.
    const std::size_t found = refreshed.size();
    if (refreshed.size() > kMaximumHiddenRoles) refreshed.resize(kMaximumHiddenRoles);

    char status[220]{};
    if (found == 0) {
        std::snprintf(status, sizeof(status),
                      "no retained role entities in %u live entities%s", count,
                      count == 0 ? " (the host has not built its entity cache yet)" : "");
    } else if (found > kMaximumHiddenRoles) {
        std::snprintf(status, sizeof(status),
                      "%zu retained roles in %u entities; showing the first %zu (%zu not shown)",
                      found, count, kMaximumHiddenRoles, found - kMaximumHiddenRoles);
    } else {
        std::snprintf(status, sizeof(status), "%zu retained role(s) in %u live entities", found,
                      count);
    }

    {
        std::scoped_lock lock(g_hidden_mutex);
        g_hidden_roles = std::move(refreshed);
        std::snprintf(g_hidden_status, sizeof(g_hidden_status), "%s", status);
        ++g_scan_count;
        g_last_scan_millis = (NowSeconds() - scan_started) * 1000.0;
    }
}

void ApplyHiddenRoleRequest() {
    HiddenRoleRequest request{};
    {
        std::scoped_lock lock(g_hidden_mutex);
        if (!g_hidden_request.queued) return;
        request = g_hidden_request;
        g_hidden_request.queued = false;
    }
    if (!HiddenRoleAvailable()) {
        SetHiddenStatus("hidden role teleport service unavailable");
        return;
    }

    // THE SLOT'S ADDRESS IS RE-CHECKED AGAINST THE LIVE CACHE FIRST, and the check matters more
    // now than it did at 4 Hz: the list is scanned on demand, so a slot can be minutes old and
    // its `data` address can name an object the game has since destroyed.  `lookup` answers
    // "is this id still in the current entity set, and is it still the same object" out of the
    // published cache -- no IL2CPP, no dereference, safe from any domain -- which is exactly
    // the question.  It is optional: a host table that predates `lookup` simply gets the
    // resolve below, which requires a live `m_CachedPtr` and refuses a dead wrapper.
    if (g_entities->struct_size >=
            offsetof(CabbirdUnityEntitiesServiceV1, lookup) +
                sizeof(CabbirdUnityEntitiesServiceV1::lookup) &&
        g_entities->lookup != nullptr) {
        CabbirdUnityEntitiesLookupV1 found{};
        found.struct_size = sizeof(found);
        if (g_entities->lookup(g_entities->user, request.entity_id, &found).code !=
                CABBIRD_STATUS_V1_OK ||
            found.data != request.data) {
            SetHiddenStatus("that retained role is gone from the live entity set; press rescan");
            g_rescan_requested = 1;
            return;
        }
    }

    CabbirdUnityTransformResolveRequestV1 resolve{};
    resolve.struct_size = sizeof(resolve);
    resolve.data = request.data;
    CabbirdUnityTransformHandleV1 handle{};
    handle.struct_size = sizeof(handle);
    if (g_transform->resolve(g_transform->user, &resolve, &handle).code !=
            CABBIRD_STATUS_V1_OK ||
        handle.transform == 0) {
        SetHiddenStatus("hidden role is no longer live; press rescan");
        g_rescan_requested = 1;
        return;
    }

    CabbirdUnityTransformWriteV1 write{};
    write.struct_size = sizeof(write);
    write.flags = CABBIRD_UNITY_TRANSFORM_WRITE_V1_SET_POSITION;
    write.transform = handle.transform;
    std::memcpy(write.position, request.position, sizeof(write.position));
    if ((request.flags & CABBIRD_UNITY_PLAYER_WRITE_V1_SET_ROTATION) != 0) {
        write.flags |= CABBIRD_UNITY_TRANSFORM_WRITE_V1_SET_ROTATION;
        std::memcpy(write.euler_degrees, request.euler_degrees, sizeof(write.euler_degrees));
    }
    const CabbirdStatusV1 status = g_transform->write(g_transform->user, &write);
    if (status.code != CABBIRD_STATUS_V1_OK) {
        SetHiddenStatus("hidden role transform write was refused");
        return;
    }
    char line[220]{};
    std::snprintf(line, sizeof(line), "hidden role %d moved (entity %llu)",
                  request.slot + 1,
                  static_cast<unsigned long long>(request.entity_id));
    SetHiddenStatus(line);
}

void QueueHiddenRoleTeleport(int slot) {
    HiddenRoleSlot role{};
    bool available = false;
    {
        std::scoped_lock lock(g_hidden_mutex);
        if (slot >= 0 && static_cast<std::size_t>(slot) < g_hidden_roles.size()) {
            role = g_hidden_roles[static_cast<std::size_t>(slot)];
            available = role.valid;
        }
    }
    if (!available) {
        SetHiddenStatus("hidden role is not available");
        return;
    }

    double target[3]{};
    if (!ParseVector(g_position_text, target)) {
        SetHiddenStatus("one of the coordinates is not a number (or is outside +/-1e6)");
        return;
    }
    if (g_relative != 0) {
        if (!role.position_valid) {
            SetHiddenStatus("relative teleport needs a readable hidden role position");
            return;
        }
        for (int index = 0; index < 3; ++index) target[index] += role.position[index];
    }
    HiddenRoleRequest request{};
    request.queued = true;
    request.slot = slot;
    request.entity_id = role.entity_id;
    request.data = role.data;
    std::memcpy(request.position, target, sizeof(request.position));
    if (g_set_rotation != 0) {
        if (!ParseVector(g_rotation_text, request.euler_degrees)) {
            SetHiddenStatus("one of the rotation values is not a number");
            return;
        }
        request.flags |= CABBIRD_UNITY_PLAYER_WRITE_V1_SET_ROTATION;
    }
    {
        std::scoped_lock lock(g_hidden_mutex);
        if (!g_hidden_request.queued) {
            g_hidden_request = request;
        } else {
            request.queued = false;
        }
    }
    if (!request.queued) {
        SetHiddenStatus("a hidden role teleport is still being applied");
        return;
    }
    // The role has moved and the set may have changed shape, so re-read it once the write has
    // been applied.  One scan window, then the entity service goes quiet again.
    g_rescan_requested = 1;
    SetHiddenStatus("queued hidden role teleport for the next game tick");
}

void ReadPlayer() {
    if (!PlayerAvailable()) {
        std::memset(&g_snapshot, 0, sizeof(g_snapshot));
        return;
    }
    CabbirdUnityPlayerSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    const CabbirdStatusV1 status = g_player->snapshot(g_player->user, &snapshot);
    if (status.code == CABBIRD_STATUS_V1_OK) {
        g_snapshot = snapshot;
    } else {
        // NOT zeroed into "a player at the origin": the validity flag is what every reader
        // below tests.  Preserve the host's binding diagnostic even though the snapshot is
        // invalid, so "no live player" names the exact failed resolution step.
        g_snapshot = snapshot;
        g_snapshot.flags = CABBIRD_UNITY_PLAYER_V1_NONE;
    }
}

void PollResult() {
    if (!PlayerAvailable()) return;
    CabbirdUnityPlayerWriteResultV1 result{};
    result.struct_size = sizeof(result);
    const CabbirdStatusV1 status = g_teleport->state(g_teleport->user, &result);
    if (status.code != CABBIRD_STATUS_V1_OK) return;
    const bool was_pending =
        g_have_result != 0 &&
        g_result.request_sequence == result.request_sequence &&
        g_result.result_code == CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_PENDING;
    g_result = result;
    g_have_result = 1;
    // The status line is updated only when the verdict CHANGES, so a running commentary does
    // not overwrite a message about the last action with the same sentence every frame.
    if (!was_pending) return;
    switch (result.result_code) {
        case CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_OK:
            std::snprintf(g_status, sizeof(g_status),
                          "teleport #%u applied: error %.3f m", result.request_sequence,
                          result.position_error);
            break;
        case CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REVERTED:
            std::snprintf(g_status, sizeof(g_status),
                          "teleport #%u REVERTED by the game's own movement: the player is "
                          "%.3f m from the target again",
                          result.request_sequence, result.position_error);
            break;
        case CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REFUSED:
            std::snprintf(g_status, sizeof(g_status), "teleport #%u refused (error %.3f m)",
                          result.request_sequence, result.position_error);
            break;
        default: break;
    }
}

void IssueTeleport() {
    if (!PlayerAvailable()) {
        std::snprintf(g_status, sizeof(g_status),
                      "player service unavailable (host build without cabbird.unity.player)");
        return;
    }
    double target[3]{};
    if (!ParseVector(g_position_text, target)) {
        std::snprintf(g_status, sizeof(g_status),
                      "one of the coordinates is not a number (or is outside +/-1e6)");
        return;
    }
    if (g_relative != 0) {
        // RELATIVE MODE NEEDS A CURRENT READING, and refuses without one rather than
        // treating a missing player as the origin.  "Teleport +5 on y" from an unknown
        // position is not a smaller teleport; it is a teleport to (5, 5, 5).
        ReadPlayer();
        if ((g_snapshot.flags & CABBIRD_UNITY_PLAYER_V1_VALID) == 0) {
            std::snprintf(g_status, sizeof(g_status),
                          "relative teleport needs a live reading, and there is none");
            return;
        }
        for (int index = 0; index < 3; ++index) {
            target[index] = g_snapshot.position[index] + target[index];
        }
    }
    CabbirdUnityPlayerWriteRequestV1 request{};
    request.struct_size = sizeof(request);
    request.entity_id = (g_snapshot.flags & CABBIRD_UNITY_PLAYER_V1_VALID) != 0
        ? g_snapshot.entity_id
        : 0;
    std::memcpy(request.position, target, sizeof(request.position));
    if (g_set_rotation != 0) {
        double euler[3]{};
        if (!ParseVector(g_rotation_text, euler)) {
            std::snprintf(g_status, sizeof(g_status), "one of the rotation values is not a number");
            return;
        }
        std::memcpy(request.euler_degrees, euler, sizeof(request.euler_degrees));
        request.flags |= CABBIRD_UNITY_PLAYER_WRITE_V1_SET_ROTATION;
    }
    const CabbirdStatusV1 status = g_teleport->teleport(g_teleport->user, &request);
    switch (status.code) {
        case CABBIRD_STATUS_V1_OK:
            std::snprintf(g_status, sizeof(g_status),
                          "queued a teleport to (%.3f, %.3f, %.3f); waiting for the game tick",
                          target[0], target[1], target[2]);
            break;
        case CABBIRD_STATUS_V1_CONFLICT:
            std::snprintf(g_status, sizeof(g_status),
                          "a teleport is still being applied; wait a moment");
            break;
        case CABBIRD_STATUS_V1_NOT_FOUND:
            std::snprintf(g_status, sizeof(g_status), "there is no live player to move");
            break;
        default:
            std::snprintf(g_status, sizeof(g_status),
                          "the host refused the request (status %u)", status.code);
            break;
    }
}

// See the note in plugins/player_coords: the table drawn with is `g_ui`, the one validated
// at load time, not the parameter.
void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1*) {
    const CabbirdUiServiceV1* const ui = g_ui;
    if (ui == nullptr) return;
    ReadPlayer();
    PollResult();

    cabbird::sdk::UiWindow window(ui, "Player Teleport", &g_open);
    if (!window) return;

    const bool live = (g_snapshot.flags & CABBIRD_UNITY_PLAYER_V1_VALID) != 0;
    char line[220]{};
    if (live) {
        std::snprintf(line, sizeof(line), "current: (%.3f, %.3f, %.3f)  %s",
                      g_snapshot.position[0], g_snapshot.position[1], g_snapshot.position[2],
                      g_snapshot.data_class);
    } else {
        std::snprintf(line, sizeof(line),
                      "current: -- (no live player: menu, loading, or no world scene)");
    }
    ui->text(ui->user, StringView(line));
    if (!live && g_snapshot.reason[0] != '\0') {
        std::snprintf(line, sizeof(line), "binding diagnostic: %s", g_snapshot.reason);
        ui->text(ui->user, StringView(line));
    }
    ui->text(ui->user, StringView(g_status));

    ui->separator(ui->user);
    ui->checkbox(ui->user, StringView("relative to the current position"), &g_relative);
    if (g_relative != 0) {
        ui->text(ui->user, StringView("offsets, in metres (applied to the live reading)"));
        ui->input_text(ui->user, StringView("dx"), g_position_text[0],
                       sizeof(g_position_text[0]), 0);
        ui->input_text(ui->user, StringView("dy"), g_position_text[1],
                       sizeof(g_position_text[1]), 0);
        ui->input_text(ui->user, StringView("dz"), g_position_text[2],
                       sizeof(g_position_text[2]), 0);
    } else {
        ui->text(ui->user, StringView("absolute world coordinates"));
        ui->input_text(ui->user, StringView("x"), g_position_text[0],
                       sizeof(g_position_text[0]), 0);
        ui->input_text(ui->user, StringView("y"), g_position_text[1],
                       sizeof(g_position_text[1]), 0);
        ui->input_text(ui->user, StringView("z"), g_position_text[2],
                       sizeof(g_position_text[2]), 0);
    }
    // "USE CURRENT" IS A BUTTON, NOT A LIVE MIRROR.  Mirroring the reading into the fields
    // would make the fields impossible to type into while the player moves (the value would
    // be overwritten every frame), and the whole point of this panel is typing a target.
    if (ui->button_enabled(ui->user, StringView("use current position"), 0.0f, 0.0f,
                           live ? 1 : 0) != 0) {
        if (g_relative != 0) {
            // In relative mode the fields ARE the offsets, so "use current" means the
            // zero offset -- filling them with the player's absolute position would turn
            // the next click into a teleport to twice the coordinates.
            for (int index = 0; index < 3; ++index) {
                std::snprintf(g_position_text[index], sizeof(g_position_text[index]), "0.000");
            }
        } else {
            FormatVector(g_snapshot.position, g_position_text);
        }
        std::snprintf(g_status, sizeof(g_status), "filled the fields from the live reading");
    }

    if (ui->button(ui->user, StringView("teleport"), 0.0f, 0.0f) != 0) {
        IssueTeleport();
    }

    ui->separator(ui->user);
    ui->checkbox(ui->user, StringView("also set rotation"), &g_set_rotation);
    if (g_set_rotation != 0) {
        ui->text(ui->user, StringView("euler degrees; POSITIVE x pitches DOWN (Unity)"));
        ui->input_text(ui->user, StringView("rx"), g_rotation_text[0],
                       sizeof(g_rotation_text[0]), 0);
        ui->input_text(ui->user, StringView("ry"), g_rotation_text[1],
                       sizeof(g_rotation_text[1]), 0);
        ui->input_text(ui->user, StringView("rz"), g_rotation_text[2],
                       sizeof(g_rotation_text[2]), 0);
    }

    ui->separator(ui->user);
    ui->text(ui->user, StringView("retained hidden role entities"));

    // THE COST OF THIS LIST IS IN THE WINDOW, because it is the only part of this plugin that
    // spends the game's frame time and the previous version hid that completely: it asked the
    // host for the whole entity set four times a second forever, which kept the host's entity
    // walk running on every game tick whether or not anyone was looking at this panel.
    ui->text(ui->user, StringView("scanned on demand: the entity walk costs the game frame time"));
    if (ui->button(ui->user, StringView("rescan retained roles"), 0.0f, 0.0f) != 0) {
        g_rescan_requested = 1;
    }
    ui->checkbox(ui->user, StringView("track retained roles live (costs frame time)"),
                 &g_track_roles);
    if (g_track_roles != 0) {
        ui->text(ui->user,
                 StringView("tracking: one short scan every 5 s while this window is open"));
    }

    // A FIXED-SIZE COPY, not a per-frame heap allocation: the previous version copied the
    // vector on every drawn frame, and the list is now bounded at `kMaximumHiddenRoles` so the
    // copy is a memcpy with no allocator behind it.
    HiddenRoleSlot hidden_roles[kMaximumHiddenRoles]{};
    std::size_t hidden_role_count = 0;
    char hidden_status[sizeof(g_hidden_status)]{};
    std::uint64_t scan_count = 0;
    double last_scan_millis = 0.0;
    {
        std::scoped_lock lock(g_hidden_mutex);
        hidden_role_count = std::min(g_hidden_roles.size(), kMaximumHiddenRoles);
        if (hidden_role_count != 0) {
            std::memcpy(hidden_roles, g_hidden_roles.data(),
                        hidden_role_count * sizeof(HiddenRoleSlot));
        }
        std::memcpy(hidden_status, g_hidden_status, sizeof(hidden_status));
        scan_count = g_scan_count;
        last_scan_millis = g_last_scan_millis;
    }
    ui->text(ui->user, StringView(hidden_status));
    if (scan_count != 0) {
        char scan_line[160]{};
        std::snprintf(scan_line, sizeof(scan_line),
                      "scans: %llu   last one: %.2f ms of the game thread (host walk excluded)",
                      static_cast<unsigned long long>(scan_count), last_scan_millis);
        ui->text(ui->user, StringView(scan_line));
    }
    for (std::size_t index = 0; index < hidden_role_count; ++index) {
        char role_line[220]{};
        if (hidden_roles[index].valid && hidden_roles[index].position_valid) {
            std::snprintf(role_line, sizeof(role_line), "role %d: %s  (%.3f, %.3f, %.3f)",
                          static_cast<int>(index + 1), hidden_roles[index].data_class,
                          hidden_roles[index].position[0], hidden_roles[index].position[1],
                          hidden_roles[index].position[2]);
        } else if (hidden_roles[index].valid) {
            std::snprintf(role_line, sizeof(role_line), "role %d: %s  (position unreadable)",
                          static_cast<int>(index + 1), hidden_roles[index].data_class);
        } else {
            std::snprintf(role_line, sizeof(role_line), "role %d: --",
                          static_cast<int>(index + 1));
        }
        ui->text(ui->user, StringView(role_line));
        char button_label[64]{};
        std::snprintf(button_label, sizeof(button_label), "teleport hidden role %d",
                      static_cast<int>(index + 1));
        if (ui->button_enabled(ui->user, StringView(button_label), 0.0f, 0.0f,
                               hidden_roles[index].valid ? 1 : 0) != 0) {
            QueueHiddenRoleTeleport(static_cast<int>(index));
        }
    }

    // The verdict, verbatim and last.  This is the part that makes the window worth having
    // over a console command: `measured` vs `applied` is the only place the difference
    // between "the call was made" and "the player is there" is visible.
    ui->separator(ui->user);
    if (g_have_result != 0) {
        const char* verdict = "?";
        switch (g_result.result_code) {
            case CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_PENDING: verdict = "pending"; break;
            case CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_OK: verdict = "ok"; break;
            case CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REVERTED: verdict = "reverted"; break;
            case CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REFUSED: verdict = "refused"; break;
            default: break;
        }
        std::snprintf(line, sizeof(line), "last teleport #%u: %s", g_result.request_sequence,
                      verdict);
        ui->text(ui->user, StringView(line));
        std::snprintf(line, sizeof(line), "asked for:  (%.3f, %.3f, %.3f)",
                      g_result.applied_position[0], g_result.applied_position[1],
                      g_result.applied_position[2]);
        ui->text(ui->user, StringView(line));
        std::snprintf(line, sizeof(line), "game now reports: (%.3f, %.3f, %.3f)  error %.3f m",
                      g_result.measured_position[0], g_result.measured_position[1],
                      g_result.measured_position[2], g_result.position_error);
        ui->text(ui->user, StringView(line));
        std::snprintf(line, sizeof(line), "write call: %.0f us on the game thread",
                      g_result.apply_micros);
        ui->text(ui->user, StringView(line));
    } else {
        ui->text(ui->user, StringView("no teleport has been applied this session"));
    }
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
    g_player = services
                   .Query<CabbirdUnityPlayerServiceV1>(CABBIRD_UNITY_PLAYER_SERVICE_V1_ID,
                                                       CABBIRD_UNITY_PLAYER_SERVICE_V1_VERSION)
                   .get();
    // The WRITE.  Queried separately because it is a separate grant: a host build (or a manifest)
    // that publishes the snapshot without the teleport leaves this plugin's controls inert and
    // says so, rather than offering a button that cannot work.
    g_teleport = services
                     .Query<CabbirdUnityPlayerTeleportServiceV1>(
                         CABBIRD_UNITY_PLAYER_TELEPORT_SERVICE_V1_ID,
                         CABBIRD_UNITY_PLAYER_TELEPORT_SERVICE_V1_VERSION)
                     .get();
    g_entities = services
                     .Query<CabbirdUnityEntitiesServiceV1>(
                         CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID,
                         CABBIRD_UNITY_ENTITIES_SERVICE_V1_VERSION)
                     .get();
    g_transform = services
                      .Query<CabbirdUnityTransformServiceV1>(
                          CABBIRD_UNITY_TRANSFORM_SERVICE_V1_ID,
                          CABBIRD_UNITY_TRANSFORM_SERVICE_V1_VERSION)
                      .get();
    // NO SCAN AT LOAD, AND THAT IS THE FIX.  Version 0.2.0 scanned the whole entity set four
    // times a second from the moment it loaded -- with the window closed, whether or not the
    // user ever looked at the role list -- and every one of those calls renewed the host's
    // 250 ms entity-consumer lease, so the host's entity walk (its own measurement: 213 ms per
    // game tick) never switched off.  The status line says what to do instead of doing it.
    SetHiddenStatus(HiddenRoleAvailable()
                        ? "not scanned yet -- press rescan (the entity walk costs frame time)"
                        : "hidden role teleport unavailable (entity/transform service missing)");
    std::snprintf(g_status, sizeof(g_status),
                  PlayerAvailable()
                      ? "ready; the host applies the write on its next game tick"
                      : "player teleport service unavailable (host build without "
                        "cabbird.unity.player-teleport)");
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) { return Ok(); }
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) { return Ok(); }

void CABBIRD_CALL Unload(void*) {
    g_ui = nullptr;
    g_player = nullptr;
    g_teleport = nullptr;
    g_entities = nullptr;
    g_transform = nullptr;
    {
        std::scoped_lock lock(g_hidden_mutex);
        g_hidden_roles = {};
        g_hidden_request = {};
        std::snprintf(g_hidden_status, sizeof(g_hidden_status),
                      "hidden role teleport unavailable");
    }
    std::memset(&g_snapshot, 0, sizeof(g_snapshot));
    std::memset(&g_result, 0, sizeof(g_result));
    g_have_result = 0;
    g_rescan_requested = 0;
    g_scan_window_seconds = 0.0;
    g_scan_step_seconds = 0.0;
    g_track_seconds = 0.0;
    g_scan_count = 0;
    g_last_scan_millis = 0.0;
}

// THE ONLY CALLER OF `RefreshHiddenRoles`, AND THE REASON THE PLUGIN NO LONGER STUTTERS.
//
// This is a scan WINDOW, not a poll.  It is open only when the user asked for it (the rescan
// button, a queued hidden-role teleport, or -- if they ticked the box -- the live-tracking
// cadence), and it closes itself again.  With the box unticked and nothing pressed, this
// function returns without touching `cabbird.unity.entities` at all, which is what lets the
// host's 250 ms consumer lease lapse and its entity walk switch itself off.
//
// WHY A WINDOW RATHER THAN ONE CALL: the host's entity cache is rebuilt on the host's own
// game tick, and only after a consumer has asked for it.  A single `entity_count` after an
// idle period therefore answers with the cache from the last walk -- or with nothing at all
// after a scene change cleared it -- so one press of "rescan" would produce an empty list and
// look broken.  The window is long enough to contain the host's next walk, so the second and
// third scans inside it see the set the walk just built.  It is the difference between a
// feature that works on the first press and one that needs pressing twice.
void ServiceEntityScans(double delta_seconds) {
    if (g_open == 0) {
        // THE WINDOW IS CLOSED, SO THE PLUGIN GOES QUIET.  Dropping the pending request here
        // rather than leaving it armed is deliberate: a rescan queued while the panel was
        // visible must not fire the moment the user reopens it and cost them a hitch they did
        // not ask for -- reopening shows the last list, and the button is right there.
        g_rescan_requested = 0;
        g_scan_window_seconds = 0.0;
        g_scan_step_seconds = 0.0;
        g_track_seconds = 0.0;
        return;
    }
    if (!HiddenRoleAvailable()) return;

    if (g_rescan_requested != 0) {
        g_rescan_requested = 0;
        g_scan_window_seconds = kScanWindowSeconds;
        g_scan_step_seconds = kScanStepSeconds;  // scan on this tick, not after a delay
    }
    if (g_scan_window_seconds <= 0.0) {
        // IDLE.  `g_track_roles == 0` is the default and means: no entity traffic whatsoever
        // until the user asks again.
        if (g_track_roles == 0) return;
        g_track_seconds += delta_seconds;
        if (g_track_seconds < kTrackIntervalSeconds) return;
        g_track_seconds = 0.0;
        g_scan_window_seconds = kScanWindowSeconds;
        g_scan_step_seconds = kScanStepSeconds;
    }
    g_scan_window_seconds -= delta_seconds;
    g_scan_step_seconds += delta_seconds;
    if (g_scan_step_seconds < kScanStepSeconds) return;
    g_scan_step_seconds = 0.0;
    RefreshHiddenRoles();
}

void CABBIRD_CALL Update(void*, double delta_seconds) {
    // Entity enumeration and Transform calls are game-domain-only.  The render callback merely
    // queues a request, so the actual writes happen here without violating the Unity threading
    // contract.
    //
    // THE WRITE IS APPLIED WHETHER OR NOT THE WINDOW IS OPEN: a click that was queued just
    // before the user closed the panel must still land, and this call touches no entity list.
    ApplyHiddenRoleRequest();
    ServiceEntityScans(delta_seconds);
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    // Field by field: `CabbirdStringViewV1` has no converting constructor, so a braced
    // aggregate initialiser with `StringView(...)` elements does not compile.
    descriptor->id = StringView("cabbird.player-teleport");
    descriptor->name = StringView("Player Teleport");
    descriptor->author = StringView("Cabbird");
    descriptor->version = StringView("0.3.0");
    descriptor->on_load = Load;
    descriptor->on_start = Start;
    descriptor->on_stop = Stop;
    descriptor->on_unload = Unload;
    descriptor->on_update = Update;
    descriptor->on_draw = Draw;
    return Ok();
}
