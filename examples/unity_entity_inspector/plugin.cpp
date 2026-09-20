// Unity / IL2CPP reference plugin: the host's entity source.
//
// WHAT THIS FILE REPLACED, AND WHY
// --------------------------------
// This example used to be `unity_world_inspector`, and it queried three services --
// `cabbird.unity.build`, `cabbird.unity.objects`, `cabbird.unity.world` -- while its own
// header comment claimed "every service used here is one the host actually publishes".
// That claim was false: all three were declarations ported from the sibling UE5 project
// (`anomaly.ue5.build` / `.objects` / `.world`) that NOTHING in this repository ever
// published, so the window reported "unavailable" for all of them on every build, and a
// plugin author copying it would have copied a dependency on nothing.
//
// This one queries services the host publishes today:
//
//   cabbird.ui                  required -- it is the only way this plugin has output
//   cabbird.unity.entities      the live entity set, resolved by the host
//   cabbird.unity.player        the local player snapshot (optional)
//
// Both optional services report UNAVAILABLE until the host has a validated profile
// binding for the running build.  That is the normal state on an unbound build, not an
// error, so the plugin records presence and keeps drawing.
#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/services/ui.h"
#include "cabbird/sdk/services/unity.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

const CabbirdUiServiceV1* g_ui{};
const CabbirdUnityEntitiesServiceV1* g_entities{};
const CabbirdUnityPlayerServiceV1* g_player{};
int g_open{1};

// Service presence is reported rather than assumed: a missing optional service must
// not stop Draw from rendering, and the user needs to see what is missing.
struct ServicePresence {
    bool entities{};
    bool player{};
};

ServicePresence g_presence{};

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    const cabbird::sdk::Host services(host);

    // The window is the only hard requirement: without it this plugin has no output.
    const auto ui = services.Query<CabbirdUiServiceV1>(
        CABBIRD_UI_SERVICE_V1_ID, CABBIRD_UI_SERVICE_V1_VERSION);
    if (!ui) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    g_ui = ui.get();

    const auto entities = services.Query<CabbirdUnityEntitiesServiceV1>(
        CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID, CABBIRD_UNITY_ENTITIES_SERVICE_V1_VERSION);
    g_entities = entities.get();
    g_presence.entities = entities.get() != nullptr;

    const auto player = services.Query<CabbirdUnityPlayerServiceV1>(
        CABBIRD_UNITY_PLAYER_SERVICE_V1_ID, CABBIRD_UNITY_PLAYER_SERVICE_V1_VERSION);
    g_player = player.get();
    g_presence.player = player.get() != nullptr;

    *context = &g_presence;
    return cabbird::sdk::Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) { return cabbird::sdk::Ok(); }
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) { return cabbird::sdk::Ok(); }

void CABBIRD_CALL Unload(void*) {
    g_ui = nullptr;
    g_entities = nullptr;
    g_player = nullptr;
    g_presence = {};
}

void CABBIRD_CALL Update(void*, double) {}

const char* KindName(std::uint32_t kind) {
    switch (kind) {
    case CABBIRD_UNITY_ENTITY_V1_KIND_PLAYER: return "player";
    case CABBIRD_UNITY_ENTITY_V1_KIND_MONSTER: return "monster";
    case CABBIRD_UNITY_ENTITY_V1_KIND_NPC: return "npc";
    case CABBIRD_UNITY_ENTITY_V1_KIND_WORLD: return "world";
    default: return "unclassified";
    }
}

void DrawServicePresence(const CabbirdUiServiceV1* ui) {
    using cabbird::sdk::StringView;
    ui->text(ui->user, StringView(g_presence.entities ? "entities: available"
                                                     : "entities: unavailable"));
    ui->text(ui->user, StringView(g_presence.player ? "player  : available"
                                                   : "player  : unavailable"));
}

void DrawPlayer(const CabbirdUiServiceV1* ui) {
    using cabbird::sdk::StringView;
    if (g_player == nullptr) return;

    CabbirdUnityPlayerSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    if (g_player->snapshot(g_player->user, &snapshot).code != CABBIRD_STATUS_V1_OK) {
        // NOT_FOUND means "no live player", never "the player is at the origin" -- the
        // service zeroes the snapshot rather than reporting a fake position.
        ui->text(ui->user, StringView("player  : not found"));
        return;
    }

    char line[256]{};
    std::snprintf(line, sizeof(line), "player  : %s  (%.1f, %.1f, %.1f)",
                  snapshot.data_class, snapshot.position[0], snapshot.position[1],
                  snapshot.position[2]);
    ui->text(ui->user, StringView(line));
}

// Enumerates the entity set.  The list is capped because the host makes no promise about
// its size and a debug window must not stall the frame.
//
// `generation` is printed because a cached index is only valid within one generation:
// the entity set is rebuilt on a scene change or a profile rebind, and an index from the
// previous generation refers to a different entity (or none).
void DrawEntities(const CabbirdUiServiceV1* ui) {
    using cabbird::sdk::StringView;
    if (g_entities == nullptr) return;

    const std::uint32_t total = g_entities->entity_count(g_entities->user);
    char line[320]{};
    std::snprintf(line, sizeof(line), "entities: %u, generation %llu", total,
                  static_cast<unsigned long long>(g_entities->generation(g_entities->user)));
    ui->text(ui->user, StringView(line));
    ui->separator(ui->user);

    constexpr std::uint32_t kMaximumRows = 40;
    constexpr std::size_t kMaximumLabelBytes = 48;
    const std::uint32_t rows = total < kMaximumRows ? total : kMaximumRows;
    for (std::uint32_t index = 0; index < rows; ++index) {
        CabbirdUnityEntityV1 entity{};
        entity.struct_size = sizeof(entity);
        if (g_entities->entity_at(g_entities->user, index, &entity).code !=
            CABBIRD_STATUS_V1_OK) {
            continue;
        }
        // `label` is a borrowed, NOT NUL-terminated UTF-8 view -- print it by length.
        const std::size_t label_bytes =
            entity.label == nullptr ? 0
                                    : (entity.label_size < kMaximumLabelBytes
                                           ? entity.label_size
                                           : kMaximumLabelBytes);
        std::snprintf(line, sizeof(line), "[%u] %-12s %7.1fm  (%.0f, %.0f, %.0f)  %.*s",
                      index, KindName(entity.kind), entity.distance_meters,
                      entity.bounds_center[0], entity.bounds_center[1],
                      entity.bounds_center[2], static_cast<int>(label_bytes),
                      entity.label == nullptr ? "" : entity.label);
        ui->text(ui->user, StringView(line));
    }
    if (total > rows) {
        std::snprintf(line, sizeof(line), "... and %u more", total - rows);
        ui->text(ui->user, StringView(line));
    }
}

void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1* ui) {
    if (ui == nullptr) ui = g_ui;
    if (ui == nullptr) return;

    cabbird::sdk::UiWindow window(ui, "Unity Entity Inspector", &g_open);
    if (!window) return;

    DrawServicePresence(ui);
    ui->separator(ui->user);
    DrawPlayer(ui);
    ui->separator(ui->user);
    DrawEntities(ui);
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        cabbird::sdk::StringView("cabbird.example.unity-entity-inspector"),
        cabbird::sdk::StringView("Unity Entity Inspector"),
        cabbird::sdk::StringView("Cabbird"), cabbird::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return cabbird::sdk::Ok();
}
