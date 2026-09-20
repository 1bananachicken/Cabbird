// Entity Overlay -- the first real Cabbird plugin.
//
// WHAT THIS DRAWS ON
// ------------------
// The request was "use the game's own drawing, to avoid stutter".  That needs answering,
// because Unity's own drawing surface is not the one the request assumes: the game's UGUI
// Canvas is a managed object graph, so drawing through it from a plugin means either
// building canvas elements per entity per frame (GC pressure -- exactly the stutter we
// were told to avoid) or calling icalls that heap-allocate what they return.  That
// decision rests on measurements rather than on opinion.
//
// So this plugin does NOT draw through the game's canvas, and it does NOT hook one.
// It draws on Cabbird's own overlay: `cabbird.unity.overlay`.  The host already
// presents on the game's swapchain (the same framebuffer, one Present later), so it
// owns a screen-space surface, and it hands the subscriber a frame carrying
// `measure_text` / `draw_text` / `draw_line` / `draw_rect`.  The host owns the
// surface and the batching.
//
// Notice what is absent from this file: no ImGui header, no D3D11, no struct
// offsets, no IL2CPP, no class names.  Those live in the host's validated profile
// binding, which is why a game update that moves a field breaks the binding instead
// of this plugin.
//
// The plugin's whole job is policy: which entities to box, in what colour, and with
// what label.

#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/services/platform.h"
#include "cabbird/sdk/services/ui.h"
#include "cabbird/sdk/services/unity.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t kMaximumEntities = 512;
constexpr float kMinimumThickness = 1.0f;
constexpr float kMaximumThickness = 6.0f;
// The label text magnification the slider spans.
//
// 1.0 is the host font's own size, which is 13 pixels.  THE UPPER BOUND IS 2.0 AND NOT HIGHER
// ON PURPOSE: the host bakes its font atlas at 1.00/1.25/1.50/1.75/2.00 times that size and
// draws a label with the smallest bake at least as large as the request, so every value up to
// 2.0 (26 pixels) is rendered by shrinking a bigger raster and stays crisp.  Above 2.0 there is
// no bigger bake to shrink, so the host would have to magnify the 26px bitmap -- which is what
// made Chinese names blurry in the first place ("中文字太模糊了，不只是大小的问题").
// Going past 26 pixels therefore needs another bake in the atlas, not a bigger number here.
constexpr float kMinimumLabelScale = 0.6f;
constexpr float kMaximumLabelScale = 2.0f;

// ---------------------------------------------------------------------------
// Settings.  Plain data, no allocation: the render callback must not touch the
// heap.  Anomaly's EntityOverlay keeps the same fields, so the two can be diffed.
//
// EVERY COLOUR IS PER CATEGORY AND OVERRIDABLE.  The first version hardcoded red for
// enemies and green for players and kept the user's colour only for "unclassified",
// which meant the colour picker appeared to do nothing: in a fight everything is
// classified, so every box took a hardcoded colour and the chosen one was never used.
// In game that reads as "the colour I set is not the colour shown".
// ---------------------------------------------------------------------------
struct EntityColor {
    float rgba[4];
};

struct Settings {
    int enabled{1};
    int draw_2d{1};
    // LABELS ON BY DEFAULT.  The user asked for names, repeatedly, and had to find a checkbox
    // OFF, because the host's name chain crashed the game the one time it was allowed to run.
    //
    // The user asked for names and is right to expect them; hiding the switch was wrong, and so
    // was defaulting it on before the chain had ever produced a string on the live game.  What
    // is left is the honest state: the feature is present, it is one checkbox away, and it has
    // a known crash that must be fixed before it can be defaulted on again.
    int draw_label{1};
    // HIDE ENTITIES THE HOST CANNOT NAME.  The label is the only "does this have a name" signal
    // the plugin gets, and an empty one is exactly that answer.  On by default: an ESP that shows
    // every fog patch and repair station is one whose actual resources you cannot find.
    int hide_unnamed{1};

    // FILTERS.  Each is "draw this category at all", which is the cheapest useful
    // filter: it needs no extra host call and it is applied before any geometry.
    //
    // ONE FLAG PER CATEGORY THE GAME ITSELF HAS, not per colour.  The host previously collapsed
    // `MonsterData` and `NpcData` into a single `kind`, so NPCs and monsters shared a colour and
    // could not be told apart or filtered independently.  The split is now the game's own
    // `EWorldBasicType` / `EWorldObjectType` / `EMonsterType`, and each bucket gets a flag here.
    int filter_player{1};
    int filter_npc{1};
    int filter_monster{1};
    int filter_world{1};

    // DISTANCE FILTER.  In metres, measured from the camera the host reported.
    // The maximum defaults to 0, meaning "no limit" rather than "zero metres" --
    // a default that hides everything is a much worse failure than one that draws
    // too much, because it looks like the ESP is broken.
    float max_distance{0.0f};
    float min_distance{0.0f};

    float thickness{1.5f};

    EntityColor enemy{{1.00f, 0.25f, 0.25f, 1.00f}};
    EntityColor player{{0.25f, 1.00f, 0.35f, 1.00f}};
    EntityColor other{{0.30f, 0.85f, 1.00f, 1.00f}};
    // The four live categories, each with its own colour so the split is visible at a glance.
    // `monster` deliberately starts at the old `enemy` red: monsters are what "enemy" meant, and
    // keeping the colour means an existing user's mental model still holds.
    EntityColor npc{{1.00f, 0.85f, 0.25f, 1.00f}};
    EntityColor monster{{1.00f, 0.25f, 0.25f, 1.00f}};
    EntityColor world{{0.65f, 0.65f, 0.70f, 1.00f}};
    EntityColor label_color{{1.00f, 1.00f, 1.00f, 1.00f}};
    // LABEL TEXT SCALE.  The label is drawn at this magnification and measured at it, so the two
    // can never disagree.  1.0 is the host font's own size, which is what made Chinese names hard
    // to read: the host's default text is sized for ASCII UI, and CJK glyphs need noticeably more
    // pixels before the strokes separate.
    float label_scale{1.6f};

    // HOW OFTEN THE HOST IS ASKED FOR THE ENTITY LIST, in game ticks.
    //
    // This is the knob that decides whether the ESP is affordable.  Every refresh walks the
    // entity list and makes two `il2cpp_runtime_invoke` calls per entity -- about 160 managed
    // calls for 80 entities -- and that work happens on the GAME thread, inside the game's own
    // frame.  Doing it on every tick drops the frame rate to roughly 7.6 Hz.
    //
    // 1 = every tick (smoothest), 2 = every other tick, 4 = quarter rate.  A box that updates
    // at 15-30 Hz is indistinguishable from one that updates at 60 while moving, and costs a
    // quarter as much.
    // EVERY TICK.  This was a user-facing slider and the default was 2, which is part of why the
    // box lagged the entity it was drawn around.  The frame cost was paid down at the host
    // instead -- the label cache, the position cache and the batched walk -- so the refresh no
    // longer needs rationing, and the knob could only ever make the ESP look broken.
    int refresh_divisor{1};
};

Settings g_settings;

// ---------------------------------------------------------------------------
// SETTINGS PERSISTENCE -- `cabbird.config`, the host's own durable store.
// ---------------------------------------------------------------------------
//
// The host owns the location and performs the write atomically against a registered schema, so
// the plugin never picks a path, never writes a partial file and never has to reconcile a
// half-written document.  The pattern is the one `examples/reliable_config` demonstrates:
// register the schema in every loaded generation, read with the two-call buffer protocol,
// write with `write_atomic`.
//
// THE FIELDS ARE A TABLE, NOT SIXTY LINES OF `if (key == ...)`.
//
// Forty-one values have to survive a round trip.  Writing one arm per key by hand is how a
// setting silently stops persisting the next time one is added -- the serializer is updated, the
// parser is not, and the only symptom is that one checkbox forgets itself.  One table drives
// both directions, so a field cannot be writable without also being readable.
struct SettingField {
    const char* name;
    int* integer;  // exactly one of these is set
    float* number;
};

#define CABBIRD_INT_FIELD(member) \
    SettingField { #member, &g_settings.member, nullptr }
#define CABBIRD_FLOAT_FIELD(member) \
    SettingField { #member, nullptr, &g_settings.member }
#define CABBIRD_COLOR_FIELD(member, channel, index) \
    SettingField { #member "_" #channel, nullptr, &g_settings.member.rgba[index] }

const SettingField kSettingFields[] = {
    CABBIRD_INT_FIELD(enabled),
    CABBIRD_INT_FIELD(draw_2d),
    CABBIRD_INT_FIELD(draw_label),
    CABBIRD_INT_FIELD(hide_unnamed),
    CABBIRD_INT_FIELD(filter_player),
    CABBIRD_INT_FIELD(filter_npc),
    CABBIRD_INT_FIELD(filter_monster),
    CABBIRD_INT_FIELD(filter_world),
    CABBIRD_INT_FIELD(refresh_divisor),
    CABBIRD_FLOAT_FIELD(label_scale),
    CABBIRD_FLOAT_FIELD(thickness),
    CABBIRD_FLOAT_FIELD(max_distance),
    CABBIRD_FLOAT_FIELD(min_distance),
    CABBIRD_COLOR_FIELD(player, r, 0), CABBIRD_COLOR_FIELD(player, g, 1),
    CABBIRD_COLOR_FIELD(player, b, 2), CABBIRD_COLOR_FIELD(player, a, 3),
    CABBIRD_COLOR_FIELD(npc, r, 0), CABBIRD_COLOR_FIELD(npc, g, 1),
    CABBIRD_COLOR_FIELD(npc, b, 2), CABBIRD_COLOR_FIELD(npc, a, 3),
    CABBIRD_COLOR_FIELD(monster, r, 0), CABBIRD_COLOR_FIELD(monster, g, 1),
    CABBIRD_COLOR_FIELD(monster, b, 2), CABBIRD_COLOR_FIELD(monster, a, 3),
    CABBIRD_COLOR_FIELD(world, r, 0), CABBIRD_COLOR_FIELD(world, g, 1),
    CABBIRD_COLOR_FIELD(world, b, 2), CABBIRD_COLOR_FIELD(world, a, 3),
    CABBIRD_COLOR_FIELD(other, r, 0), CABBIRD_COLOR_FIELD(other, g, 1),
    CABBIRD_COLOR_FIELD(other, b, 2), CABBIRD_COLOR_FIELD(other, a, 3),
    CABBIRD_COLOR_FIELD(label_color, r, 0), CABBIRD_COLOR_FIELD(label_color, g, 1),
    CABBIRD_COLOR_FIELD(label_color, b, 2), CABBIRD_COLOR_FIELD(label_color, a, 3),
};
constexpr std::size_t kSettingFieldCount = sizeof(kSettingFields) / sizeof(kSettingFields[0]);

#undef CABBIRD_INT_FIELD
#undef CABBIRD_FLOAT_FIELD
#undef CABBIRD_COLOR_FIELD

constexpr std::string_view kSettingsSchemaId = "entity-overlay-settings";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 4096;
// PERMISSIVE ON PURPOSE.  The document is written and read by this one table, so a strict schema
// would only be able to reject the plugin's own output -- and it would have to be edited in step
// with the field list, which is the coupling the table exists to remove.  What the host's
// validation must catch is a corrupt or truncated file, and `type: object` with numeric values
// does that.
constexpr std::string_view kSettingsSchema =
    R"json({"type":"object","additionalProperties":{"type":"number"}})json";

const CabbirdConfigServiceV1* g_config{};
CabbirdGenerationHandleV1 g_settings_schema{};
// The last state written, so a change is detected without instrumenting sixty controls.
Settings g_settings_saved;
bool g_settings_dirty{};
std::uint64_t g_settings_save_after{};

bool ConfigUsable() {
    return g_config != nullptr && g_config->struct_size >= offsetof(CabbirdConfigServiceV1, migrate) &&
        g_config->register_schema != nullptr && g_config->read != nullptr &&
        g_config->write_atomic != nullptr;
}

const SettingField* FindSettingField(const std::string_view name) {
    for (std::size_t index = 0; index < kSettingFieldCount; ++index) {
        const std::string_view candidate(kSettingFields[index].name);
        if (candidate.size() == name.size() && candidate == name) return &kSettingFields[index];
    }
    return nullptr;
}

std::string SerializeSettings() {
    std::string out;
    out.reserve(1024);
    out += '{';
    for (std::size_t index = 0; index < kSettingFieldCount; ++index) {
        if (index != 0) out += ',';
        out += '"';
        out += kSettingFields[index].name;
        out += "\":";
        if (kSettingFields[index].integer != nullptr) {
            out += std::to_string(*kSettingFields[index].integer);
        } else {
            char number[32]{};
            std::snprintf(number, sizeof(number), "%.6g",
                          static_cast<double>(*kSettingFields[index].number));
            out += number;
        }
    }
    out += '}';
    return out;
}

// A flat `{"name":number,...}` scanner.  Deliberately not a JSON parser: the document is the one
// this file wrote, the host has already validated it against the schema, and a general parser
// here would be more code with more ways to be wrong.
bool ApplySettingsDocument(const std::string_view document) {
    std::size_t position = 0;
    const auto skip_space = [&]() {
        while (position < document.size() &&
               (document[position] == ' ' || document[position] == '\n' ||
                document[position] == '\r' || document[position] == '\t')) {
            ++position;
        }
    };
    skip_space();
    if (position == document.size() || document[position] != '{') return false;
    ++position;
    skip_space();
    if (position < document.size() && document[position] == '}') return true;
    while (position < document.size()) {
        skip_space();
        if (position == document.size() || document[position] != '"') return false;
        ++position;
        const std::size_t key_begin = position;
        while (position < document.size() && document[position] != '"') ++position;
        if (position == document.size()) return false;
        const std::string_view key(document.data() + key_begin, position - key_begin);
        ++position;
        skip_space();
        if (position == document.size() || document[position] != ':') return false;
        ++position;
        skip_space();
        const std::size_t number_begin = position;
        if (position < document.size() && (document[position] == '-' || document[position] == '+')) {
            ++position;
        }
        while (position < document.size() &&
               ((document[position] >= '0' && document[position] <= '9') ||
                document[position] == '.' || document[position] == 'e' ||
                document[position] == 'E' || document[position] == '+' ||
                document[position] == '-')) {
            ++position;
        }
        if (position == number_begin) return false;
        const std::string text(document.substr(number_begin, position - number_begin));
        // AN UNKNOWN KEY IS SKIPPED, NOT AN ERROR.  A document written by a newer build must still
        // load in an older one; rejecting it would make downgrading lose every setting.
        if (const SettingField* const field = FindSettingField(key)) {
            try {
                if (field->integer != nullptr) {
                    *field->integer = std::stoi(text);
                } else {
                    *field->number = std::stof(text);
                }
            } catch (...) {
                // A single unreadable value keeps its default and the rest still load.
            }
        }
        skip_space();
        if (position < document.size() && document[position] == ',') {
            ++position;
            continue;
        }
        break;
    }
    return true;
}

void LoadSettings() {
    if (!ConfigUsable()) return;
    std::uint32_t schema_version = 0;
    std::size_t size = 0;
    const CabbirdStatusV1 probe = g_config->read(
        g_config->user, cabbird::sdk::StringView(kSettingsSchemaId), &schema_version, {nullptr, 0},
        &size);
    // NOT_FOUND is the ordinary first-run answer, not a failure: defaults stand.
    if (probe.code != CABBIRD_STATUS_V1_OK || size == 0 || size > kMaximumSettingsBytes) return;
    try {
        std::vector<std::uint8_t> document(size);
        std::size_t copied = document.size();
        const CabbirdStatusV1 read = g_config->read(
            g_config->user, cabbird::sdk::StringView(kSettingsSchemaId), &schema_version,
            {document.data(), document.size()}, &copied);
        if (read.code != CABBIRD_STATUS_V1_OK || copied == 0 || copied > document.size()) return;
        static_cast<void>(ApplySettingsDocument(
            {reinterpret_cast<const char*>(document.data()), copied}));
        g_settings_saved = g_settings;
        g_settings_dirty = false;
    } catch (...) {
        // A failed load leaves the defaults in place, which is the same state as a first run.
    }
}

void SaveSettings() {
    if (!g_settings_dirty || !ConfigUsable()) return;
    const std::string document = SerializeSettings();
    const CabbirdByteSpanV1 bytes{reinterpret_cast<const std::uint8_t*>(document.data()),
                                  document.size()};
    if (g_config->write_atomic(g_config->user,
                               cabbird::sdk::StringView(kSettingsSchemaId),
                               kSettingsSchemaVersion, bytes)
            .code != CABBIRD_STATUS_V1_OK) {
        return;  // marked dirty still, so the next attempt retries
    }
    g_settings_saved = g_settings;
    g_settings_dirty = false;
}


// ---------------------------------------------------------------------------
// Services.  Everything except `cabbird.ui` is optional: a build without a
// profile binding has no entities service, and an overlay consumer with no entity source
// must still open its window and say so rather than disappear.
// ---------------------------------------------------------------------------
const CabbirdUiServiceV1* g_ui{};
const CabbirdUnityOverlayServiceV1* g_overlay{};
const CabbirdUnityEntitiesServiceV1* g_entities{};

CabbirdGenerationHandleV1 g_overlay_subscription{};
bool g_overlay_frame_seen{};
int g_open{1};

// Entity cache, refreshed in Update (Game domain) and read in Draw (Render
// domain).  Copied by value so that the render callback never calls back into the
// entity service -- the two callbacks are on different threads and the host makes
// no promise that the entity list is stable during a render pass.
CabbirdUnityEntityV1 g_entities_cache[kMaximumEntities]{};
char g_label_cache[kMaximumEntities][64]{};
// The measured size of each cached label, so `measure_text` runs when a LABEL CHANGES rather
// than once per entity per frame.
//
// `measure_text` is ImGui's own text layout: a glyph lookup per character, and on a cache miss
// a font-atlas update.  Measured work per frame, at 60 entities: it is the most expensive thing
// in the draw callback -- more than all 240 outline lines together -- and the answer for a given
// string and scale is a CONSTANT.  Recomputing a constant sixty times a second per entity is
// most of why the ESP was reported as "single-digit fps".
//
// Invalidated by comparing the label bytes: an entity whose name resolves late, or whose slot is
// reused by a different entity, gets re-measured, and everything else is served from here.
float g_label_width[kMaximumEntities]{};

// The last value declared to the host through `set_want_labels`, so the call is made once per
// change instead of once per tick.
int g_declared_want_labels{-1};
float g_label_height[kMaximumEntities]{};
// The scale the cached width/height were measured at.  See the draw site: without this, moving
// the slider would re-centre text using a size measured at a different magnification.
float g_label_measured_scale[kMaximumEntities]{};
std::uint32_t g_label_measured_size[kMaximumEntities]{};
std::uint32_t g_cached_count{};
std::uint64_t g_cached_generation{};
std::uint64_t g_generation{};
std::uint32_t g_refresh_tick{};
bool g_camera_valid{};
double g_camera_position[3]{};

// Reported in the window.  Render-domain write, read by the render domain; the
// counts are the honest answer to "is this thing actually seeing anything".
std::uint64_t g_draws{};
std::uint64_t g_projected{};
std::uint64_t g_skipped_behind{};
std::uint64_t g_filtered{};
// Counted apart from `g_filtered`: "you asked not to see this category" and "this entity has no
// name to show" are different answers, and one number for both would hide which switch did it.
std::uint64_t g_unnamed_hidden{};
std::uint64_t g_labelled{};

std::uint32_t PackColor(const float rgba[4]) {
    const auto channel = [](float value) {
        if (value <= 0.0f) return std::uint32_t{0};
        if (value >= 1.0f) return std::uint32_t{255};
        return static_cast<std::uint32_t>(value * 255.0f + 0.5f);
    };
    // RGBA, in that byte order, because that is what the host's `ToImGuiColour` reads: it
    // takes the HIGH byte as red.  The comment here used to say "ABGR", which was wrong and
    // would have produced swapped channels the moment someone believed it and "fixed" the
    // constants to match.  The byte order is a contract with one reader; it is stated in the
    // reader's terms.
    return (channel(rgba[0]) << 24u) | (channel(rgba[1]) << 16u) | (channel(rgba[2]) << 8u) |
           channel(rgba[3]);
}

// The frame is host-owned and borrowed for the duration of one callback.  Validating
// it before use is not paranoia: a plugin DLL can outlive the host build it was
// compiled against, and a null function pointer here is a crash rather than an error.
bool FrameAvailable(const CabbirdUnityOverlayFrameV1* frame) noexcept {
    constexpr std::size_t kSize =
        offsetof(CabbirdUnityOverlayFrameV1, draw_rect) +
        sizeof(CabbirdUnityOverlayFrameV1::draw_rect);
    return frame != nullptr && frame->struct_size >= kSize && frame->draw_line != nullptr &&
           frame->draw_rect != nullptr && frame->draw_text != nullptr &&
           frame->measure_text != nullptr;
}

bool OverlayAvailable(const CabbirdUnityOverlayServiceV1* service) noexcept {
    constexpr std::size_t kSize =
        offsetof(CabbirdUnityOverlayServiceV1, unsubscribe) +
        sizeof(CabbirdUnityOverlayServiceV1::unsubscribe);
    return service != nullptr && service->struct_size >= kSize &&
           service->subscribe != nullptr && service->unsubscribe != nullptr;
}

// Whether the host's entity service is usable at all.
//
// The size is checked WITHOUT the newer `set_want_labels` entry, so a host that predates it is
// still usable -- labels are simply unavailable there.  Gating the whole service on the newest
// field would make an older host reject the plugin entirely, which is a much worse failure than
// a missing optional feature.
//
// TWO TABLES, TWO CHECKS, AND THE REASON THEY ARE SEPARATE is the entity split: enumerating the
// game's entities is `cabbird.unity.entities`, and the overlay's camera is
// `cabbird.unity.entities`.  A plugin that wants both must have both.
bool EntitySourceAvailable(const CabbirdUnityEntitiesServiceV1* service) noexcept {
    constexpr std::size_t kSize = offsetof(CabbirdUnityEntitiesServiceV1, entity_at) +
                                  sizeof(CabbirdUnityEntitiesServiceV1::entity_at);
    return service != nullptr && service->struct_size >= kSize &&
           service->entity_count != nullptr && service->entity_at != nullptr &&
           service->generation != nullptr;
}

bool EntityServiceAvailable(const CabbirdUnityEntitiesServiceV1* service) noexcept {
    constexpr std::size_t kSize =
        offsetof(CabbirdUnityEntitiesServiceV1, camera) + sizeof(CabbirdUnityEntitiesServiceV1::camera);
    return service != nullptr && service->struct_size >= kSize && service->camera != nullptr;
}

// Whether the HOST can fill labels at all.  A host built before `set_want_labels` exists still
// serves entities; it just cannot read names.  Saying so is better than a checkbox that appears
// to do nothing.
bool HostSupportsLabels() noexcept {
    return EntityServiceAvailable(g_entities) &&
           g_entities->struct_size >= offsetof(CabbirdUnityEntitiesServiceV1, set_want_labels) +
                                        sizeof(CabbirdUnityEntitiesServiceV1::set_want_labels) &&
           g_entities->set_want_labels != nullptr;
}

// ---------------------------------------------------------------------------
// The draw callback.  Runs in the Render domain during Present.
//
// Two rules it must obey, both of which are about not stalling the frame:
//   1. no allocation, no locking, no disk -- everything it needs is already in
//      the cache or on the stack;
//   2. no IL2CPP.  The host's headers say so explicitly: a lazy-initialising
//      il2cpp_* call from a thread the VM does not know has already killed this
//      process once in-game.
// ---------------------------------------------------------------------------
void CABBIRD_CALL DrawOverlay(void*, const CabbirdUnityOverlayFrameV1* frame) {
    if (!FrameAvailable(frame)) return;
    g_overlay_frame_seen = true;
    ++g_draws;
    if (g_settings.enabled == 0) return;

    const std::uint32_t count = g_cached_count;
    if (count == 0) return;

    const float thickness =
        g_settings.thickness < kMinimumThickness
            ? kMinimumThickness
            : (g_settings.thickness > kMaximumThickness ? kMaximumThickness
                                                        : g_settings.thickness);

    for (std::uint32_t index = 0; index < count; ++index) {
        const CabbirdUnityEntityV1& entity = g_entities_cache[index];

        if ((entity.flags & CABBIRD_UNITY_ENTITY_V1_VALID) == 0) continue;
        // HIDDEN WHEN IT HAS NO NAME.  The host returns zero length for an entity it cannot name,
        // and `entity.label_size` is the only signal; `label` may be null, so both are tested.
        if (g_settings.hide_unnamed != 0 &&
            (entity.label == nullptr || entity.label_size == 0)) {
            ++g_unnamed_hidden;
            continue;
        }
        // THE CATEGORY FILTER, applied before any geometry.  `kind` is the host's own
        // numbering; `cabbird/plugin/unity_entity.hpp` documents it, and it now carries the
        // game's four live categories rather than a collapsed enemy/player pair.
        std::uint32_t entity_color = PackColor(g_settings.world.rgba);
        int wanted = g_settings.filter_world;
        switch (entity.kind) {
            case CABBIRD_UNITY_ENTITY_V1_KIND_PLAYER:
                entity_color = PackColor(g_settings.player.rgba);
                wanted = g_settings.filter_player;
                break;
            case CABBIRD_UNITY_ENTITY_V1_KIND_NPC:
                entity_color = PackColor(g_settings.npc.rgba);
                wanted = g_settings.filter_npc;
                break;
            case CABBIRD_UNITY_ENTITY_V1_KIND_MONSTER:
                entity_color = PackColor(g_settings.monster.rgba);
                wanted = g_settings.filter_monster;
                break;
            // THE FOURTH BUCKET NEEDS ITS OWN CASE, and its absence shows up as
            // blue world resources even once the host has been fixed: with no `KIND_WORLD`
            // case, kind 4 fell through to `default` and was drawn in `g_settings.other.rgba`,
            // which is the blue that "unclassified" uses.  A named colour that no branch ever
            // selects looks exactly like a fixed bug that is not fixed.
            case CABBIRD_UNITY_ENTITY_V1_KIND_WORLD:
                entity_color = PackColor(g_settings.world.rgba);
                wanted = g_settings.filter_world;
                break;
            default:
                entity_color = PackColor(g_settings.other.rgba);
                wanted = g_settings.filter_world;
                break;
        }
        if (wanted == 0) {
            ++g_filtered;
            continue;
        }

        // THE DISTANCE FILTER, from the camera the host published.
        //
        // Measured against the camera position rather than the player object because the
        // camera IS the viewpoint: "distance to the thing I am looking through" is what a
        // player means by "too far away", and it is the only distance the host reports.
        if (g_camera_valid &&
            (g_settings.max_distance > 0.0f || g_settings.min_distance > 0.0f)) {
            const double dx = entity.bounds_center[0] - g_camera_position[0];
            const double dy = entity.bounds_center[1] - g_camera_position[1];
            const double dz = entity.bounds_center[2] - g_camera_position[2];
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (g_settings.max_distance > 0.0f &&
                distance > static_cast<double>(g_settings.max_distance)) {
                ++g_filtered;
                continue;
            }
            if (g_settings.min_distance > 0.0f &&
                distance < static_cast<double>(g_settings.min_distance)) {
                ++g_filtered;
                continue;
            }
        }

        // A corner the camera puts behind the near plane has no correct screen position, and
        // dividing by a negative depth mirrors it through the origin -- the classic "one giant
        // box across the whole screen".  The host drops those corners from the mask rather than
        // clamping them.
        //
        // ALL EIGHT ARE REQUIRED, AND RELAXING THAT WAS TRIED AND REVERTED.  Accepting any four
        // corners lets a box whose AABB straddles the near plane draw from its far face alone,
        // which is exactly the mirrored-extent bug this rule exists to prevent.  The cost is
        // that an entity the camera is practically inside draws
        // nothing, which is correct: there is no screen-space extent for it to have.
        if ((entity.flags & CABBIRD_UNITY_ENTITY_V1_SCREEN_POINTS) == 0) {
            ++g_skipped_behind;
            continue;
        }
        if (entity.screen_point_mask != 0xFFu) {
            ++g_skipped_behind;
            continue;
        }
        float minimum_x = static_cast<float>(entity.screen_points[0][0]);
        float maximum_x = minimum_x;
        float minimum_y = static_cast<float>(entity.screen_points[0][1]);
        float maximum_y = minimum_y;
        for (std::uint32_t corner = 1; corner < 8; ++corner) {
            const float screen_x = static_cast<float>(entity.screen_points[corner][0]);
            const float screen_y = static_cast<float>(entity.screen_points[corner][1]);
            if (screen_x < minimum_x) minimum_x = screen_x;
            if (screen_x > maximum_x) maximum_x = screen_x;
            if (screen_y < minimum_y) minimum_y = screen_y;
            if (screen_y > maximum_y) maximum_y = screen_y;
        }

        const float width = maximum_x - minimum_x;
        const float height = maximum_y - minimum_y;
        // A degenerate box is not worth a draw call and usually means the entity is far enough
        // away that it is sub-pixel.
        if (width < 1.0f || height < 1.0f) continue;

        // OFF-SCREEN REJECTION, in the render pass rather than the host.
        //
        // The host projects every entity it can read, including the ones behind the camera's
        // shoulders; a full list is the honest thing for it to publish and it does not know the
        // viewport's clip rectangle the way the overlay does.  Rejecting here means the draw
        // calls for off-screen boxes are never made, and the cost of a box that cannot be seen
        // is one comparison.
        {
            const float viewport_width = static_cast<float>(frame->viewport_width);
            const float viewport_height = static_cast<float>(frame->viewport_height);
            if (viewport_width > 0.0f && viewport_height > 0.0f) {
                if (maximum_x < 0.0f || maximum_y < 0.0f || minimum_x > viewport_width ||
                    minimum_y > viewport_height) {
                    ++g_skipped_behind;
                    continue;
                }
            }
        }

        ++g_projected;

        if (g_settings.draw_2d != 0) {
            // A HOLLOW FRAME, drawn as four `draw_line` calls because the SDK's `draw_rect`
            // FILLS.
            //
            // An earlier version called `draw_rect` and then drew four lines around it, with a
            // comment claiming the interior stayed clear -- the intent and the call disagreed,
            // so the ESP painted a solid colour block over every entity.  Reported from the real
            // machine as "why is it a filled matrix instead of a frame".  The interior must stay
            // clear: a filled box hides the thing it is pointing at, which makes an ESP worse
            // than useless in a fight.
            const float outline = thickness;
            frame->draw_line(frame->user, minimum_x - outline, minimum_y - outline,
                             maximum_x + outline, minimum_y - outline, entity_color, thickness);
            frame->draw_line(frame->user, maximum_x + outline, minimum_y - outline,
                             maximum_x + outline, maximum_y + outline, entity_color, thickness);
            frame->draw_line(frame->user, maximum_x + outline, maximum_y + outline,
                             minimum_x - outline, maximum_y + outline, entity_color, thickness);
            // The fourth line closes the frame on the LEFT.  Its start x must be the left edge
            // like its end x is -- starting at the RIGHT edge instead makes it a diagonal across
            // the box, which is the one error where every edge-extent check passes while the four
            // outline lines no longer form a closed frame: the signature of one line joining the
            // wrong two corners.
            frame->draw_line(frame->user, minimum_x - outline, maximum_y + outline,
                             minimum_x - outline, minimum_y - outline, entity_color, thickness);
        }

        if (g_settings.draw_label != 0 && entity.label_size != 0 && entity.label != nullptr) {
            // MEASURED ONCE PER LABEL, not once per label per frame.  See the cache's comment:
            // the size of a string at a fixed scale cannot change, and re-deriving it every
            // frame is the draw callback's dominant cost.
            //
            // THE CACHE IS KEYED ON THE SCALE TOO.  It was keyed on the label bytes alone, which
            // was correct only while the scale was the constant 1.0f.  Now that the user can move
            // a slider, a cache keyed on text alone would keep returning the width measured at the
            // OLD scale -- the text would grow while its centring box stayed the old size, so
            // dragging the slider would slide every label sideways.
            const std::uint32_t slot = index;
            const float scale = g_settings.label_scale;
            float text_width = 0.0f;
            float text_height = 0.0f;
            const bool cache_hit = g_label_measured_size[slot] == entity.label_size &&
                                   g_label_measured_scale[slot] == scale &&
                                   std::memcmp(g_label_cache[slot], entity.label,
                                               entity.label_size) == 0;
            if (cache_hit) {
                text_width = g_label_width[slot];
                text_height = g_label_height[slot];
            } else {
                const CabbirdStringViewV1 label{entity.label, entity.label_size};
                if (frame->measure_text(frame->user, label, scale, &text_width, &text_height) ==
                    0) {
                    continue;
                }
                g_label_width[slot] = text_width;
                g_label_height[slot] = text_height;
                g_label_measured_size[slot] = entity.label_size;
                g_label_measured_scale[slot] = scale;
            }
            const CabbirdStringViewV1 label{entity.label, entity.label_size};
            // Centred on the box and ABOVE it.  Drawn with the label's own colour rather than
            // the category colour: a red name on a red box is unreadable, and the category is
            // already said by the box.
            frame->draw_text(frame->user, label, minimum_x + (width - text_width) * 0.5f,
                             minimum_y - text_height - 2.0f,
                             PackColor(g_settings.label_color.rgba), scale);
            ++g_labelled;
        }
    }
}

CabbirdStatusV1 SubscribeOverlay() {
    if (!OverlayAvailable(g_overlay)) {
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    CabbirdGenerationHandleV1 handle{};
    const CabbirdStatusV1 status =
        g_overlay->subscribe(g_overlay->user, DrawOverlay, nullptr, &handle);
    if (status.code != CABBIRD_STATUS_V1_OK) return status;
    if (handle.id == 0 || handle.generation == 0) {
        return {CABBIRD_STATUS_V1_FAILED, 0, {}};
    }
    g_overlay_subscription = handle;
    return cabbird::sdk::Ok();
}

CabbirdStatusV1 UnsubscribeOverlay() {
    if (g_overlay_subscription.id == 0) return cabbird::sdk::Ok();
    const CabbirdGenerationHandleV1 handle = g_overlay_subscription;
    g_overlay_subscription = {};
    if (!OverlayAvailable(g_overlay)) {
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    const CabbirdStatusV1 status = g_overlay->unsubscribe(g_overlay->user, handle);
    // NOT_FOUND and UNAVAILABLE both mean "this subscription is already gone",
    // which is the outcome we wanted; only a real failure is reported.
    if (status.code == CABBIRD_STATUS_V1_OK || status.code == CABBIRD_STATUS_V1_NOT_FOUND ||
        status.code == CABBIRD_STATUS_V1_UNAVAILABLE) {
        return cabbird::sdk::Ok();
    }
    return status;
}

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *context = nullptr;

    const cabbird::sdk::Host services(host);

    // THE DURABLE SETTINGS STORE.  Optional: a host that does not publish it means the panel
    // works for the session and forgets everything on exit, which is a degradation and not a
    // failure -- refusing to load over it would take the ESP away entirely.
    g_config = services
                   .Query<CabbirdConfigServiceV1>(CABBIRD_CONFIG_SERVICE_V1_ID,
                                                  CABBIRD_CONFIG_SERVICE_V1_VERSION)
                   .get();
    if (ConfigUsable()) {
        const CabbirdStatusV1 schema = g_config->register_schema(
            g_config->user, cabbird::sdk::StringView(kSettingsSchemaId), kSettingsSchemaVersion,
            CabbirdByteSpanV1{reinterpret_cast<const std::uint8_t*>(kSettingsSchema.data()),
                              kSettingsSchema.size()},
            &g_settings_schema);
        if (schema.code != CABBIRD_STATUS_V1_OK) {
            // Registered once per generation, so a failure means no persistence this generation --
            // NOT a failed load.  The schema handle is scope-owned and the host re-registers on
            // reload, which is why the plugin does not try to unregister on the way out.
            g_config = nullptr;
        } else {
            LoadSettings();
        }
    }
    const auto ui =
        services.Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID, CABBIRD_UI_SERVICE_V1_VERSION);
    if (!ui) {
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_ui = ui.get();

    // Optional.  A missing overlay surface means this build has no render-time
    // drawing backend published; a missing entity service means no profile binding.
    // Both are normal states that the window reports instead of failing the load.
    g_overlay = services
                 .Query<CabbirdUnityOverlayServiceV1>(
                     CABBIRD_UNITY_OVERLAY_SERVICE_V1_ID, CABBIRD_UNITY_OVERLAY_SERVICE_V1_VERSION)
                 .get();
    g_entities = services
                     .Query<CabbirdUnityEntitiesServiceV1>(
                         CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID,
                         CABBIRD_UNITY_ENTITIES_SERVICE_V1_VERSION)
                     .get();


    return cabbird::sdk::Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) {
    // Subscribing in Start rather than Load is deliberate: on_load runs before the
    // host has finished wiring the render path, and a subscription registered
    // against a service that is about to be re-published would be dropped.
    const CabbirdStatusV1 status = SubscribeOverlay();
    if (status.code != CABBIRD_STATUS_V1_OK &&
        status.code != CABBIRD_STATUS_V1_UNAVAILABLE) {
        return status;
    }
    return cabbird::sdk::Ok();
}

CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) {
    // SAVE ON THE WAY OUT.  `Draw` already saves a second after the last change, so this only
    // matters for a change made in the final second of a session.
    SaveSettings();
    return UnsubscribeOverlay();
}

void CABBIRD_CALL Unload(void*) {
    // The host revokes the subscription on the way out, but doing it here as well
    // means the plugin is correct on its own terms rather than correct because the
    // host cleaned up after it.
    static_cast<void>(UnsubscribeOverlay());
    g_ui = nullptr;
    g_overlay = nullptr;
    g_entities = nullptr;
    // SAVED BEFORE THE SERVICE POINTER IS CLEARED, so a save can never run against a service the
    // host has already torn down.  The host owns the service table's lifetime, not this plugin.
    SaveSettings();
    g_config = nullptr;
    g_settings_schema = {};
    g_overlay_frame_seen = false;
    g_cached_count = 0;
    g_cached_generation = 0;
}

// Refresh the cache.  Runs in the Game domain, where calling into IL2CPP-backed
// host services is allowed -- that is the whole reason the split exists.
void CABBIRD_CALL Update(void*, double) {
    if (g_settings.enabled == 0) {
        g_cached_count = 0;
        g_camera_valid = false;
        g_refresh_tick = 0;
        return;
    }
    // Both consumer inputs are required: entities supplies the snapshot and overlay supplies the
    // render callback.  The plugin never owns or resolves either backend.
    if (!EntitySourceAvailable(g_entities) || !EntityServiceAvailable(g_entities)) {
        g_cached_count = 0;
        return;
    }

    // DECLARE THE LABEL REQUIREMENT BEFORE THE THROTTLE, so switching the checkbox takes effect
    // on the next tick rather than on the next REFRESH -- with a divisor of 8 that would be a
    // visible delay between ticking the box and seeing names.
    if (g_entities->struct_size >= offsetof(CabbirdUnityEntitiesServiceV1, set_want_labels) +
                                sizeof(CabbirdUnityEntitiesServiceV1::set_want_labels) &&
        g_entities->set_want_labels != nullptr) {
        const int want = g_settings.draw_label != 0 ? 1 : 0;
        if (want != g_declared_want_labels) {
            g_entities->set_want_labels(g_entities->user, want);
            g_declared_want_labels = want;
        }
    }

    // THE THROTTLE.  This runs on the game thread, inside the game's own frame, and every
    // refresh costs the game two managed calls per entity.  Skipping refreshes is the single
    // biggest saving available in this plugin, and the cost of it is a box that lags by a few
    // tens of milliseconds -- which is not visible while playing.
    //
    // THE FIRST REFRESH IS NEVER SKIPPED.  Doing so is not a small lag: with a divisor of 2 the
    // ESP shows nothing at all until the second tick, and with a large divisor it shows nothing
    // for a noticeable moment after being switched on -- an ESP that does not appear to work
    // when you enable it is worse than one that is a frame stale.  A caller that ticks the
    // "enabled" box and renders immediately is the normal case, not an edge case.
    //
    // Clamped to at least 1: a divisor of 0 would divide by zero on the modulus below, and a
    // plugin must not be able to crash the host through a settings value.
    std::uint32_t divisor = g_settings.refresh_divisor < 1 ? 1u
                            : static_cast<std::uint32_t>(g_settings.refresh_divisor);
    if (divisor > 16) divisor = 16;
    ++g_refresh_tick;
    if (divisor > 1 && g_refresh_tick != 1 && (g_refresh_tick % divisor) != 0) return;

    const std::uint64_t generation = g_entities->generation(g_entities->user);
    if (generation != g_generation) {
        g_generation = generation;
        g_cached_generation = generation;
    }

    // The camera, once per refresh: the distance filter needs a viewpoint and the whole list
    // shares one.  Reading it per entity would be 80 redundant host calls.
    {
        CabbirdEspCameraV1 camera{};
        camera.struct_size = sizeof(camera);
        g_camera_valid = false;
        if (g_entities->camera(g_entities->user, &camera).code == CABBIRD_STATUS_V1_OK) {
            g_camera_position[0] = camera.position[0];
            g_camera_position[1] = camera.position[1];
            g_camera_position[2] = camera.position[2];
            g_camera_valid = true;
        }
    }

    std::uint32_t count = g_entities->entity_count(g_entities->user);
    if (count > kMaximumEntities) count = kMaximumEntities;

    std::uint32_t written = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        CabbirdUnityEntityV1 entity{};
        entity.struct_size = sizeof(entity);
        if (g_entities->entity_at(g_entities->user, index, &entity).code !=
            CABBIRD_STATUS_V1_OK) {
            continue;
        }
        // The label pointer belongs to the host and is only guaranteed until the next
        // entity_at call, so copy it into plugin-owned storage.  Truncation is
        // deliberate: a label is decoration, and an entity with a 4 KB name is not
        // worth stalling the game thread for.
        if (entity.label != nullptr && entity.label_size != 0) {
            const std::size_t room = sizeof(g_label_cache[written]) - 1;
            const std::size_t length = entity.label_size < room ? entity.label_size : room;
            std::memcpy(g_label_cache[written], entity.label, length);
            g_label_cache[written][length] = '\0';
            entity.label = g_label_cache[written];
            entity.label_size = static_cast<std::uint32_t>(length);
        } else {
            entity.label = nullptr;
            entity.label_size = 0;
        }
        g_entities_cache[written++] = entity;
    }
    count = written;
    g_cached_count = written;


}

// ---------------------------------------------------------------------------
// Settings window.  Uses only CabbirdUiServiceV1, so it inherits the host theme
// and cannot be restyled by this plugin.
// ---------------------------------------------------------------------------


void Draw(void*, const CabbirdUiServiceV1* ui) {
    if (ui == nullptr) ui = g_ui;

    // PERSISTENCE IS CHECKED BEFORE THE WINDOW, so a change made and then closed is still saved.
    //
    // Change detection compares against the last written state rather than setting a dirty flag
    // from each control.  With sixty controls a flag per control is sixty places to forget, and
    // the symptom -- one setting silently stops persisting -- does not point at its own cause.
    // The comparison cannot be wrong.
    //
    // Throttled to a second apart, because dragging a slider changes the value on every frame and
    // each save is an atomic file replace.
    if (ConfigUsable()) {
        const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
        const std::uint64_t now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        if (std::memcmp(&g_settings, &g_settings_saved, sizeof(g_settings)) != 0) {
            g_settings_dirty = true;
            if (g_settings_save_after == 0) g_settings_save_after = now + 1000;
        } else {
            g_settings_dirty = false;
            g_settings_save_after = 0;
        }
        if (g_settings_dirty && now >= g_settings_save_after) {
            SaveSettings();
            g_settings_save_after = 0;
        }
    }

    if (ui == nullptr) return;
    cabbird::sdk::UiWindow window(ui, "Entity Overlay", &g_open);
    if (!window) return;

    // THE SECTION HEADINGS NAME THE GROUP, NOT THE SEPARATOR.  The `-- x --` decoration made the
    // headings read like comments in a config file; the separator above them already draws the
    // division, so the text only has to say which group it is.
    ui->text(ui->user, cabbird::sdk::StringView("general"));
    // THE FOUR SWITCHES FIRST, THEN THE TWO SLIDERS.  Interleaving them put `hide unnamed`
    // between `label size` and `thickness`, so the two numeric controls the eye reads as a pair
    // were split by an unrelated checkbox.  Grouped by KIND of control rather than by the order
    // the features were added.
    ui->checkbox(ui->user, cabbird::sdk::StringView("enabled"), &g_settings.enabled);
    ui->checkbox(ui->user, cabbird::sdk::StringView("2D box"), &g_settings.draw_2d);
    ui->checkbox(ui->user, cabbird::sdk::StringView("label"), &g_settings.draw_label);
    // HIDE WHAT HAS NO NAME.
    //
    // The host returns an empty label for a world object the game itself does not name, and the
    // ESP was still drawing a box for every one of them -- fog, repair stations, decoration --
    // which buried the resources that do have names.  This is the switch that keeps only the
    // answerable entities on screen.  It removes the BOX as well as the text, which is the point:
    // a box around an object with no name is still noise.
    ui->checkbox(ui->user, cabbird::sdk::StringView("hide unnamed"),
                 &g_settings.hide_unnamed);

    // LABEL SIZE, as a slider because the right value depends on the user's resolution, their
    // display, and how far they sit from it -- none of which this plugin can know.  Chinese
    // names needed noticeably more pixels than the host's default to be readable at all; Latin
    // ones do not need as much, so a fixed larger value would have been wrong for half the users.
    ui->slider_float(ui->user, cabbird::sdk::StringView("label size"), &g_settings.label_scale,
                     kMinimumLabelScale, kMaximumLabelScale);
    ui->slider_float(ui->user, cabbird::sdk::StringView("thickness"),
                     &g_settings.thickness, kMinimumThickness, kMaximumThickness);

    ui->separator(ui->user);
    ui->text(ui->user, cabbird::sdk::StringView("filter"));
    // One entry per category the game itself distinguishes.  NPCs used to be merged with
    // monsters, so there was no way to show one without the other.
    ui->checkbox(ui->user, cabbird::sdk::StringView("players"), &g_settings.filter_player);
    ui->checkbox(ui->user, cabbird::sdk::StringView("npc"), &g_settings.filter_npc);
    ui->checkbox(ui->user, cabbird::sdk::StringView("monsters"), &g_settings.filter_monster);
    ui->checkbox(ui->user, cabbird::sdk::StringView("world resources"),
                 &g_settings.filter_world);
    // 0 = no limit, and the slider says so.  A maximum distance of "0 metres" would hide
    // everything, which reads as a broken ESP rather than as a filter.
    ui->slider_float(ui->user, cabbird::sdk::StringView("max distance (m, 0 = off)"),
                     &g_settings.max_distance, 0.0f, 500.0f);
    ui->slider_float(ui->user, cabbird::sdk::StringView("min distance (m)"),
                     &g_settings.min_distance, 0.0f, 100.0f);

    ui->separator(ui->user);
    ui->text(ui->user, cabbird::sdk::StringView("color"));
    // ONE COLOUR PER CATEGORY, and each is the one actually used to draw that category -- see the
    // switch in the draw path.  Hardcoding red/green for the classified buckets and honouring
    // the chosen colour only for "unclassified" makes the picker look broken: in a fight
    // everything is classified, so the colour the user set is never the colour shown.
    ui->color_edit4(ui->user, cabbird::sdk::StringView("player"), g_settings.player.rgba);
    ui->color_edit4(ui->user, cabbird::sdk::StringView("npc"), g_settings.npc.rgba);
    ui->color_edit4(ui->user, cabbird::sdk::StringView("monster"), g_settings.monster.rgba);
    ui->color_edit4(ui->user, cabbird::sdk::StringView("world resource"), g_settings.world.rgba);
    ui->color_edit4(ui->user, cabbird::sdk::StringView("unclassified"), g_settings.other.rgba);
    ui->color_edit4(ui->user, cabbird::sdk::StringView("label"), g_settings.label_color.rgba);

    // NO DIAGNOSTIC FOOTER.  Every counter in it was a number the user cannot act on -- `cached`,
    // `pass(es)`, `box(es)`, `filtered`, `skipped`, `unnamed hidden` -- so the panel read like a
    // debug build and pushed the controls further down than they needed to be.  The counters still
    // increment; they are what the host's own `status` command publishes, which is where live
    // numbers belong.  A UI shows what can be changed.
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        cabbird::sdk::StringView("cabbird.entity-overlay"),
        cabbird::sdk::StringView("Entity Overlay"),
        cabbird::sdk::StringView("Cabbird"),
        // Version tracks Anomaly's EntityOverlay so the two can be compared, but the
        // major is 0: this is the first Cabbird-native build of it, and claiming 2.x
        // would imply a port that has been verified against a live game (it has not).
        cabbird::sdk::StringView("0.1.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return cabbird::sdk::Ok();
}
