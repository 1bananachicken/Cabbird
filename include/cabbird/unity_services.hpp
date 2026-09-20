#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

struct ImFont;

// Host-facing capability helpers and diagnostic views.
// Start/Stop, refresh and scene invalidation are private to UnityAdapter;
// SDK service tables remain in cabbird/sdk/services, not lifecycle APIs here.

// Host backend for the `cabbird.unity.dump` service declared in
// `include/cabbird/sdk/services/unity.h`.
//
// State and asynchronous execution belong to UnityAdapter's private Dump component.
// The host reads progress here; it does not publish or stop the component separately.


namespace cabbird {

// The dump service's internal progress, for host diagnostics.
//
// This exists because a plugin CANNOT distinguish two very different failures from the
// outside: the walk is stalled before its first progress report, or the plugin's polling
// is broken.  Both present as "the counters stay at zero".  Reading the producer directly
// separates them -- `elapsed_ms` advancing while `classes` stays 0 puts the fault in the
// walk, and `claimed == false` puts it in the request path instead.
struct HostDumpStats {
    // The published `CabbirdDumpResultV1.state` (0 idle, 1 pending, ...), so a caller does
    // not have to know the enum ordering to tell "nothing is running" from "something is".
    std::uint64_t state{};
    // True while the worker slot is taken, i.e. a dump has been accepted and not finished.
    // Distinct from `state` on purpose: the slot is what actually rejects a second request.
    bool claimed{};
    std::uint64_t classes{};
    std::uint64_t fields{};
    std::uint64_t images{};
    std::uint64_t elapsed_ms{};
};

[[nodiscard]] HostDumpStats SnapshotHostUnityDump();

// Split a method-image filter string into needles, on commas and whitespace.
//
// Exported so it can be TESTED OFFLINE, which is the whole reason it is not a static
// function inside unity_adapter.cpp: the defect it exists to prevent is invisible from
// every other vantage point.  The caller used to push the whole filter string as ONE needle
// of `DumpOptions::method_image_filter`, so `image_name.find(needle)` required an image name
// containing the entire comma-separated list -- a test no image can pass.  A single-word
// filter ("Azur") hid it because then the whole string WAS the needle.  The symptom is not
// an error but an absence: `stats.methods == 0` on a dump whose own header says the method
// walk was requested, which reads like a coverage or configuration problem.
//
// `text` is a borrowed, non-NUL-terminated view; `size` is its length in bytes.  A null
// pointer or a zero size yields an empty vector, which the dumper reads as "every image".
[[nodiscard]] std::vector<std::string> SplitMethodImageFilter(const char* text,
                                                             std::uint32_t size);

}  // namespace cabbird

// Host backend for the `cabbird.unity.overlay` service -- see unity_adapter.cpp
// for the long form: why the surface is Cabbird's own rather than the game's, what the
// subscription model buys, and why an absent camera makes `project` return 0 instead of
// inventing a matrix.


#include "cabbird/sdk/services/unity.h"

namespace cabbird {

// Called once per render pass AFTER ImGui::NewFrame() and BEFORE ImGui::Render(), while
// the frame is still open.  Drawing goes to ImGui's background draw list, i.e. underneath
// every host window.  Safe to call with no subscribers (it returns immediately) and safe
// to call every frame.
//
// `imgui_context` is deliberately untyped here rather than `ImGuiContext*`: this header is
// reachable from the render layer, which is the only caller, and keeping ImGui out of it
// means the caller does not have to agree with this file about ImGui's include order.
// Font selection is supplied by the render backend; the adapter must not own or
// depend on the product UI theme. The selector is borrowed for this frame only.
using OverlayFontSelector = ::ImFont* (*)(float size_pixels) noexcept;
void RunHostOverlayFrame(void* imgui_context, OverlayFontSelector select_font = nullptr);

// The camera seam.
//
// `project` fills a ROW-MAJOR 4x4 world->clip matrix for the current frame and reports the
// point's view depth; it returns false when it cannot, which makes `frame->project` return
// 0 so the plugin skips the entity instead of drawing it somewhere wrong.
//
// There is NO PRODUCTION PRODUCER of this yet, and that is a statement about the tree rather
// than an omission: getting the active camera needs an IL2CPP binding or the camera's native
// struct layout, and no profile in this repository carries either yet.  Tests install one; a
// profile binding will later.
void SetOverlayCameraSource(
    bool (*project)(void* user, const double world[3], double view_projection[16],
                    double* depth),
    void* user);

struct HostOverlayStats {
    std::uint64_t subscribers{};
    std::uint64_t frames{};
    std::uint64_t draw_calls{};
    std::uint64_t subscriptions_total{};
    std::uint64_t projection_failures{};
    bool camera_source_present{};
};

// Diagnostics for the host UI and for the offline test.  Cheap and non-blocking.
[[nodiscard]] HostOverlayStats SnapshotHostOverlay();
[[nodiscard]] std::uint64_t HostOverlayFrameCount();

}  // namespace cabbird

// ============================================================================
// `cabbird.unity.transform` -- the ENGINE-side half of "move an object".
//
// WHY THIS SERVICE EXISTS AT ALL, SEPARATELY FROM `cabbird.unity.entity-overlay`
//
// The entity ESP service used to be the only place in the tree that had resolved the two
// offset hops that reach a `UnityEngine.Transform` (`BaseData::<transform>`, then
// `RelativeTransform::m_transform`) and the IL2CPP handles needed to CALL it.  The player
// service therefore had to `#include "cabbird/unity_services.hpp"` to borrow
// them -- and that header, whose stated contract is "publish the cached entity set" and which
// is documented as read-only, ended up exporting `ApplyUnityTransformPosition`: a function
// that CHANGES GAME WORLD STATE.
//
// THAT WAS TWO PROBLEMS IN ONE FILE:
//
//   1. a read-only service's header exposed a write;
//   2. two services that have no business knowing about each other were coupled by an include.
//
// The user's correction was to the point: offsets and resolution belong in the ADAPTER, as a
// general service.  They are engine facts -- "where is the transform of this BaseData" and
// "call `Transform::set_position` on it" -- and they are true independent of any entity list,
// any camera, and any ESP overlay.  Both `entity-overlay` and `player` are CONSUMERS of that fact,
// not owners of it.
//
// WHY THIS FILE IS UNDER `game/` AND NOT `plugin/`
//
// The organisation rule is Anomaly's, and it is unambiguous there: `plugins/` holds only real,
// loadable plugin packages (its `plugins/EntityOverlay`, `plugins/NteTeleport`, ... each with a
// manifest), and `src/plugin/` holds plugin INFRASTRUCTURE only -- its seventeen
// `plugin_*.cpp` files contain no service implementation at all.  Anomaly has no host-side ESP
// service because its `EntityOverlay` IS a plugin that calls the engine itself.
//
// Cabbird keeps that host capability in the single Unity adapter implementation rather than
// creating one public service implementation file per capability. It is private Adapter state;
// this header describes only the SDK-facing helper view.
//
// THE DEPENDENCY DIRECTION THIS ESTABLISHES, AND WHY IT IS THE HONEST ONE
//
//   entity-overlay service  --include-->  this header        (game/unity, same module family)
//   player service      --registry--> this service        (NO include of any game/ header)
//   player service      --registry--> entity-overlay service  (for "which entity is live")
//
// The player service's `plugin -> game` include edge DISAPPEARS: it reaches both services
// through `AdapterServiceRegistry`, which is the mechanism the services themselves already use.
// And with the write gone from `entity-overlay`, that service's published surface is read-only
// again, which is what its own documentation promises.
//
// WHAT IS DELIBERATELY *NOT* HERE
//
// This service does not know what a "player" is, does not know entity ids, and does not decide
// whether a position is acceptable.  Identity is a policy question (answered in the player
// service, which has the camera) and validation is a policy question (answered there too).
// Keeping those out is what makes this table safe to expose as an SDK capability: it is a
// faithful description of the engine, and it makes no claims about the game's intent.
//
// ============================================================================

#include <cstddef>


namespace cabbird {

// The published id and version.  Registered in `plugin_capability_policy.cpp` as the
// capability `unity-transform`.
inline constexpr const char* kUnityTransformServiceId = CABBIRD_UNITY_TRANSFORM_SERVICE_V1_ID;
inline constexpr std::uint32_t kUnityTransformServiceVersion =
    CABBIRD_UNITY_TRANSFORM_SERVICE_V1_VERSION;

// Resolve everything this service needs: the `m_CachedPtr` field that proves a managed wrapper
// is still alive, the two offset hops from a `BaseData` to its `Transform`, and the IL2CPP
// handles for `Transform::get_position` / `set_position` / `set_eulerAngles` plus `Vector3`.
//
// GAME DOMAIN ONLY, and safe to call repeatedly: the handles are cached and a later call is a
// no-op once resolution has succeeded.  Returns false when the runtime is not initialised or a
// class/method is missing, leaving the reason readable through `LastUnityTransformError()`.
[[nodiscard]] bool ResolveUnityTransformBinding();

// Why the last resolution attempt failed.  A borrowed view, valid until the next attempt; the
// caller copies what it needs.  A fixed buffer rather than a `thread_local` because this image
// is MANUALLY MAPPED and a static TLS directory is rejected by the mapper -- the lesson
// `unity_adapter.cpp` records at length and `unity_adapter.cpp` learnt twice.
[[nodiscard]] const char* LastUnityTransformError();

// The two offset hops, resolved from the runtime with the dump-verified values as documented
// fallbacks and ALWAYS reported: a `confirmed == false` field means the runtime lookup failed
// and the dump constant was used, which is a named, visible diff rather than a silent one.
struct UnityTransformOffsets {
    std::size_t data_transform{};  // BaseData::<transform>            (dump: 0xA0)
    std::size_t m_transform{};     // RelativeTransform::m_transform   (dump: 0x10)
    bool confirmed{};
};

// Hands the entity walk the SAME two offsets it would otherwise resolve itself, so there is
// exactly one implementation of this chain in the tree.  GAME DOMAIN ONLY.
[[nodiscard]] bool UnityEntityTransformOffsets(UnityTransformOffsets* offsets);

// The offset of `m_CachedPtr` inside a managed wrapper, and the liveness test built on it: a
// destroyed managed wrapper still has a reachable address but a null native pointer, and
// calling a method on one faults inside the runtime.
inline constexpr std::size_t kUnityCachedPtrOffset = 0x10;
[[nodiscard]] std::uintptr_t ReadUnityNativePointer(std::uintptr_t object);
[[nodiscard]] bool UnityObjectAlive(std::uintptr_t object);

// `BaseData` object address -> its `UnityEngine.Transform`, with the liveness check applied.
// The caller supplies an address the entity walk already read, so this does NOT re-walk the
// entity list.  GAME DOMAIN ONLY.
[[nodiscard]] bool ResolveUnityTransformFromData(std::uintptr_t data, std::uintptr_t* transform);

// `Transform::get_position`.  GAME DOMAIN ONLY.  Measured at ~2.5 ms per call on the live
// game, so a caller that reads many objects should sample rather than read every frame.
[[nodiscard]] bool ReadUnityTransformPosition(std::uintptr_t transform, double position[3]);

// `Transform::set_position`.  GAME DOMAIN ONLY, null-safe: a missing method or a failing
// runtime call returns false instead of faulting.
//
// THE ARGUMENT IS A MANAGED `Vector3` AND IS ALLOCATED AS ONE.  `il2cpp_runtime_invoke`
// unboxes each entry of the argument array, so a stack `float[3]` would have the runtime read
// a managed object header off the stack and then copy from wherever the garbage header
// pointed.
[[nodiscard]] bool WriteUnityTransformPosition(std::uintptr_t transform,
                                               const double position[3]);

// `Transform::set_eulerAngles`, the optional rotation half.  Absence is not fatal to the
// position half: a build without this method rejects only rotation requests.
[[nodiscard]] bool WriteUnityTransformEuler(std::uintptr_t transform,
                                            const double euler_degrees[3]);

// --- the published table ----------------------------------------------------

// The service table, published under `CABBIRD_UNITY_TRANSFORM_SERVICE_V1_ID`.  The typedef the
// SDK declares is the only thing that keeps the two halves -- definition and use -- describing
// the same layout.
[[nodiscard]] const CabbirdUnityTransformServiceV1* UnityTransformServiceTable();

}  // namespace cabbird

// Host backend for `cabbird.unity.entity-overlay` -- see unity_adapter.cpp for the
// long form: the verified offsets and their provenance, why this port has no signature
// profile where the UE5 one needs one, and which single step is still unverified.



namespace cabbird {

// The overlay's camera source, registered with the overlay service at startup.
//
// RENDER DOMAIN ONLY.  Reads the matrix the game thread published; touches no IL2CPP and
// allocates nothing, so the render-domain guarantee ("never blocks") holds.
//
// Returns false when no valid matrix exists, which makes the overlay's `project` return 0 and
// the plugin skip the entity -- never a guessed projection.
bool CABBIRD_CALL ProvideEspCameraMatrix(
    void* user, const double world[3], double view_projection[16], double* depth) noexcept;

struct HostEntityStats {
    std::uint64_t entities{};
    std::uint64_t generation{};
    bool class_resolved{};
    // Why the entity set is empty.  Borrowed: valid until the next call to any entry point
    // in this file, which is what the SDK's borrowed-lifetime contract promises and what a
    // plugin that wants to keep it must copy.
    const char* unavailable_reason{};
    // --- the camera probe -------------------------------------------------------------
    //
    // THE CAMERA IS PROBED FROM THIS SERVICE, ON THE GAME THREAD, AND THAT PLACEMENT IS THE
    // WHOLE POINT.
    //
    // A live IL2CPP probe was first written into `cabbird-cli` as a `unity` command, run from
    // the diagnostic pipe's thread.  It took the game down on its first use: `il2cpp_class_get_methods`
    // forces LAZY METADATA INITIALISATION and allocates across the image, and an
    // `il2cpp::ThreadScope` does not make that safe -- the scope registers the thread with the
    // GC, it does not license managed allocation or metadata work from an arbitrary thread.
    // The dump service already encoded exactly this lesson by making the method walk an
    // opt-in flag; the command ignored it.
    //
    // On the GAME thread the same reads are routine.  So the probe lives here, where it is
    // both safe and free: this already runs every tick.
    //
    // The anchor is a STATIC field, which is why no scene search is needed:
    //
    //     Lens.Framework.Managers.CameraManager::<mainCamera>k__BackingField
    //
    // `CameraManager : SingletonMono\`1`, and Unity's own `Camera.main` is backed by a static
    // cache the same way -- so this is the game's own answer to "which camera is the player
    // seeing", not a heuristic.
    bool camera_anchor_resolved{};
    bool main_camera_present{};
    // The managed `UnityEngine.Camera*` and its native `m_CachedPtr`.  Kept because a live
    // object address is what a future probe has to be developed against.
    std::uint64_t main_camera_object{};
    std::uint64_t main_camera_native{};
    // Whether `ProvideEspCameraMatrix` currently has a solved projection to hand out.  This is
    // the single field that separates "the entities are found but nothing draws" from
    // "everything is wired up", so it is reported rather than inferred from the box count.
    bool camera_matrix_valid{};
    // The camera self-check.  check_ok is false only until the first successful solve;
    // check_error_pixels is how far a point the solve did NOT use lands from where the
    // camera itself puts it.  Published because it is the one camera number that can
    // contradict the others -- a matrix that is "valid" and a projection that is wrong is
    // exactly the failure that shipped once.
    bool camera_check_ok{};
    double camera_check_error_pixels{};
    // Why the camera anchor or its matrix is not usable.  Same borrowed lifetime as
    // `unavailable_reason`.
    const char* camera_reason{};
    // The camera solve's RAW inputs -- camera position, the two half-angle tangents, the
    // reconstructed basis lengths and the four sampled screen points.
    //
    // Published because `camera_reason` alone could not diagnose a wrong projection: it named
    // the symptom ("half-angle tangent came out non-positive") while the cause was in the
    // samples, which no amount of re-reading the algebra reveals.  Same borrowed lifetime as
    // `unavailable_reason`.
    const char* camera_raw{};
    // Where the game itself says a few entities are, next to where our matrix says they are.
    // The only independent witness the projection has; see `CameraBinding::sample_pixels`.
    const char* camera_cross_check{};
    // Entities the game answered a screen position for, entities that got a COMPLETE set of
    // eight corners, and how many were read at all.
    //
    // The three together are what make "no boxes drawn" diagnosable: the plugin needs all eight
    // corners to form an AABB, so a large gap between `projected_entities` and
    // `full_mask_entities` means the completeness requirement is the cause, not the projection.
    std::size_t projected_entities{};
    std::size_t full_mask_entities{};
    std::size_t total_entities{};
    // Game-thread cost of one refresh, in microseconds, smoothed over 64 ticks.
    //
    // Measured because "the ESP made the game slow" is not actionable on its own.  The two
    // halves are five metadata lookups plus five `runtime_invoke` calls for the camera, against
    // two `runtime_invoke` calls per entity for the walk -- different fixes, and a tick rate
    // alone cannot say which one is paying.
    const char* kind_histogram{};
    const char* data_class_histogram{};
    const char* label_stage_summary{};
    const char* label_samples{};
    bool camps_known{};
    std::int32_t camp_monster{};
    std::int32_t camp_player{};
    bool labels_resolved{};
    const char* label_reason{};
    const char* camp_stage{};
    // The one-entity name probe's result, verbatim.  Empty until it runs.
    const char* name_probe{};
    std::int64_t tick_micros{};
    std::int64_t camera_micros{};
    std::int64_t walk_micros{};
    std::uint64_t ticks_measured{};
    std::int64_t position_reads{};
    std::int64_t position_failures{};
    std::int64_t position_micros{};
    // THE DIRECT POSITION CALL, and whether it is actually running.
    //
    // `direct_position` is the verdict verbatim: "adopted via metadata" when the fast path is
    // live, otherwise the reason it was refused.  It is published because a performance path that
    // is silently not running is indistinguishable from one that is, and this one has several
    // independent ways to be refused (no profile, a prologue mismatch, a fault, a disagreement
    // with the reflection route).  `position_micros`/`position_reads` measure the OTHER route, so
    // the two numbers together are the whole story.
    const char* direct_position{};
    std::int64_t direct_position_reads{};
    std::int64_t direct_position_faults{};
    std::int64_t direct_position_micros{};
    std::int64_t position_cache_hits{};
    std::int64_t position_cache_rejects{};
    std::int64_t label_reads{};
    std::int64_t label_cache_hits{};
    std::int64_t label_micros{};
    std::int64_t label_cache_rejects{};
};

// Diagnostics for the host UI and for the offline test.
[[nodiscard]] HostEntityStats SnapshotHostEntities();

// --- the entity source, as a source of LIVE ENTITIES ------------------------
//
// THE TRANSFORM IS NOT THIS SERVICE'S ANY MORE.
//
// This paragraph used to read: the player service "must move the player, which means it needs
// the entity's `UnityEngine.Transform*` -- and this file is the only place in the tree that has
// resolved the offset chain that reaches it (`BaseData::<transform>` ->
// `RelativeTransform::m_transform`) ... THE ALTERNATIVE WAS A SECOND COPY OF THOSE OFFSETS."
//
// The worry was right and the conclusion was wrong.  Duplicating a layout implementation is
// indeed how a game patch gets confirmed in one file and silently wrong in another -- but the
// answer is not to let a READ-ONLY entity service own an ENGINE capability, it is to give that
// capability its own owner.  Which is what happened: the chain, the liveness rule and the
// managed `Transform` calls are `cabbird.unity.transform` now
// (`unity_services.hpp`, a sibling in this directory), and this service CONSUMES it
// for its own walk instead of publishing it to everybody else.
//
// What remains here is what an entity source should answer: WHICH entity an id names, what C#
// class it is, whether it is player-shaped, and where the camera was.  No offsets, no
// `MethodInfo*`, and nothing that writes.

// One entity, re-read off the LIVE cache by its published id.
struct UnityEntityLookup {
    bool found{};
    std::uint64_t entity_id{};
    // The managed `BaseData` object's address -- the same value `entity_at` publishes
    // as `entity_data`, and therefore a starting point a consumer can re-walk itself.
    std::uintptr_t data{};
    // Live C# class of that object, as the walk recorded it.
    //
    // A FIXED BUFFER IN THE RESULT, NOT A `const char*`.  The string it names lives inside
    // the entity source's own vector, and the next refresh moves that vector -- so a pointer
    // would be a pointer into freed memory the moment the caller kept it past the call.
    // Copying is not an optimisation question: this image is MANUALLY MAPPED, and a
    // `thread_local` scratch buffer (the first attempt) injected a TLS directory the mapper
    // rejects outright (see include/cabbird/thread_local_value.hpp for the same lesson).
    char data_class[40]{};
    std::uint32_t kind{};
    // Entity-source generation the reading belongs to, so a caller can tell "this
    // entity is gone" from "the whole set was rebuilt".
    std::uint64_t generation{};
    // GAME domain only: true when this entity is the one the entity source published as
    // a player-shaped entity (see the file for which classes that covers).  NOT a claim
    // to know which player is local; the identity question is answered in the player
    // service, which has the camera.
    bool player_shaped{};
    std::uint32_t error{};
};

// Reads the cached record for `entity_id`.  No IL2CPP, no dereference of the target:
// safe from any domain, and returns `found = false` for an id the current generation
// does not contain.
[[nodiscard]] UnityEntityLookup LookupUnityEntityById(std::uint64_t entity_id);

// The transform-facing half of this service MOVED OUT, and the reason is the point of this
// refactor: `ResolveUnityEntityTransform`, `ReadUnityTransformPosition` and
// `ApplyUnityTransformPosition` used to be declared here, and the third of them CHANGES GAME
// WORLD STATE -- exported by a service whose contract is "publish the cached entity set" and
// which is documented as read-only.  They now live in `unity_services.hpp`
// (`cabbird.unity.transform`), in this same adapter directory, where the offset chain and the
// managed calls are one implementation instead of two.
//
// The note that USED to justify their presence here -- "this file is the only place in the tree
// that has resolved the offset chain" -- was true and was the actual defect: an engine fact
// owned by a consumer.  `unity_adapter.cpp` reached it by including this header, which
// is exactly the coupling the refactor removed.  Nothing here changes world state now.

// The world-space half-height of the published AABB, so a consumer holding a
// `bounds_center` can convert it back to the pivot Unity would report as `position`.
// Exposed instead of copied: the entity source owns this constant.
[[nodiscard]] double UnityEntityBoxBaseOffset();

// The active camera's world position, as the entity walk last read it (Unity's
// `Camera.transform.position`, read through the engine, never derived).
//
// Used by the player service to answer "which of the player-shaped entities is the one
// the player is driving" -- in a third-person game the active camera is AT the local
// player, and there is exactly one of it.  Returns false when no camera has been read
// this session, which is the state before the first walk or outside a world scene.
[[nodiscard]] bool ProvideEspCameraPosition(double position[3]);

// Renew the low-rate entity walk lease for the player snapshot consumer.
void RequestUnityPlayerEntityRefresh();

}  // namespace cabbird

// Host backend for `cabbird.unity.player` -- WHERE the local player is, and which entity it is.
//
// THIS IS THE READ-ONLY HALF.  The write policy -- accept, queue, apply, verify a teleport --
// is `unity_adapter.cpp` (`cabbird.unity.player-teleport`), a separate service
// with its own grant.  The split is the sibling project's: its `NtePosition` declares
// `anomaly.nte.player` alone, its `NteTeleport` declares that PLUS
// `anomaly.nte.player-teleport`, and the two capabilities are separate so that a coordinate HUD
// does not silently gain the ability to move anything.
//
// The long form -- why this is a host service rather than plugin logic, and why the write is
// post-checked -- is in the SDK header (`cabbird/sdk/services/unity.h`) and at the top of each
// .cpp.
//
// SHORT FORM: in UE5 a teleport is `ProcessEvent(actor, K2_SetActorLocation)`.  IL2CPP
// has no such funnel, the position lives in a native Transform whose layout this project
// refuses to guess, so "move the player" means CALLING a managed method -- and the only
// thing in this process holding the runtime's invoke entry, a `MethodInfo*` and an
// attached game thread is the host.


namespace cabbird {

// The resolved player, including the `Transform` pointer, handed to the teleport service.
//
// WHY THIS EXISTS AS AN INTERNAL EXPORT RATHER THAN A SECOND LOOKUP
//
// The teleport service needs "which entity is the player" and "where is its transform", and it
// must not re-derive either: a second copy of the camera-proximity heuristic could disagree with
// the first, and the disagreement would surface as a teleport landing on the wrong character.
// So the service that OWNS the decision hands it over, through this process-internal function
// rather than through the SDK table -- plugins get the snapshot, which has no pointer in it.
//
// GAME DOMAIN ONLY, and copy-out: the result is a value, so the caller owns it.
struct HostResolvedPlayer {
    bool valid{};
    std::uint64_t generation{};
    std::uint64_t entity_id{};
    std::uintptr_t transform{};
    char data_class[32]{};
    double position[3]{};
    double distance_to_camera{};
    bool identity_from_proximity{};
};
[[nodiscard]] HostResolvedPlayer ResolvedHostPlayer();

struct HostUnityPlayerStats {
    bool live{};
    // Which entity the host decided is the local player, and how.
    std::uint64_t entity_id{};
    char data_class[32]{};
    bool identity_from_proximity{};
    double position[3]{};
    double distance_to_camera{};
    // Why the identity is not resolved, when it is not.  Borrowed: valid until the next
    // call into this service.
    const char* reason{};
    // NOTE: the write bookkeeping (`writesApplied`, the last verdict, the last error) moved to
    // `HostUnityPlayerTeleportStats`.  Those counters describe the teleport service's work, and
    // leaving them here would keep the two services looking like one.
};
[[nodiscard]] HostUnityPlayerStats SnapshotHostUnityPlayer();

}  // namespace cabbird

// Host backend for `cabbird.unity.player-teleport` -- the WRITE policy for the local player.
//
// WHY THIS IS A SECOND SERVICE AND NOT PART OF `cabbird.unity.player`
//
// They used to be one table, and it answered two questions with opposite characters: `snapshot`
// is a read-only poll a plugin may make every frame, `write_position` is a queued MUTATION.  A
// single grant therefore could not express "may look at the player" without also granting "may
// move the player".
//
// The sibling project splits them, and its manifests make the reason visible: its
// `NtePosition` declares `anomaly.nte.player` alone, while `NteTeleport` declares
// `anomaly.nte.player` AND `anomaly.nte.player-teleport`.  This tree's two plugins landed on the
// same line by themselves -- `player_coords` has no control that writes, `player_teleport` has
// nothing else -- so the split here is the one the plugins already implied.
//
// THE DIVISION OF LABOUR, STATED PRECISELY
//
//   `cabbird.unity.player`           decides WHICH entity is the local player (the camera
//                                    proximity heuristic), owns that decision, and publishes it.
//   `cabbird.unity.transform`        owns the engine mechanics: offset chain, `set_position`,
//                                    `set_eulerAngles`, `get_position`.
//   `cabbird.unity.player-teleport`  owns the write POLICY: accept and validate a request, queue
//                                    it, apply it on the game thread, re-read to see whether the
//                                    game undid it, and publish a verdict.
//
// This service therefore holds NO identity heuristic of its own.  It asks the player service
// which entity is the player and gets the transform that service resolved, because a second
// copy of "which one is the player" could disagree with the first, and the disagreement would
// show up as a teleport that lands on the wrong character.



namespace cabbird {

// Write bookkeeping, for the host UI and for the offline tests.  The identity fields live in
// `HostUnityPlayerStats` (`unity_services.hpp`) because identity is the player service's
// answer, not this one's.
struct HostUnityPlayerTeleportStats {
    std::uint64_t writes_requested{};
    std::uint64_t writes_applied{};
    std::uint64_t writes_refused{};
    std::uint64_t writes_reverted{};
    std::uint32_t last_request_sequence{};
    std::uint32_t last_result_code{};
    bool last_result_applied{};
    double last_position_error{};
    double last_apply_micros{};
    // Why the last request was refused, when it was.  Borrowed: valid until the next call into
    // this service.
    const char* reason{};
};
[[nodiscard]] HostUnityPlayerTeleportStats SnapshotHostUnityPlayerTeleport();

// --- the two entries `cabbird.unity.player` forwards to ----------------------
//
// `cabbird.unity.player` v1 shipped with `write_position` / `write_state`, and the SDK surface is
// append-only: dropping them from that table would change the offsets every already-built plugin
// reads.  So they stay there and DELEGATE to these, which are the real implementation.
//
// They are process-internal rather than part of the SDK table: a plugin should reach the teleport
// service through `cabbird.unity.player-teleport`, whose table is what the SDK documents.  This
// pair exists so the compatibility path and the documented path are the SAME code.
//
// Note the signatures: no `CABBIRD_CALL` and no leading `void* user`.  Those belong to the ABI
// shape of a SERVICE TABLE entry; these are plain C++ functions called from inside the process,
// and giving them the ABI decoration would make the header's declaration and the definition
// disagree about linkage -- which is exactly how the first version of this split failed to link.
CabbirdStatusV1 PlayerTeleportSubmit(
    const CabbirdUnityPlayerWriteRequestV1* request) noexcept;
CabbirdStatusV1 PlayerTeleportQuery(CabbirdUnityPlayerWriteResultV1* result) noexcept;

}  // namespace cabbird

#include <string>
#include <string_view>
namespace cabbird {
// Synchronous diagnostic request from a CLI/worker caller. The actual walk is
// executed by the private UnityAdapter IL2CPP component on the accepted game tick.
[[nodiscard]] std::string RequestUnityIl2CppObjectScan(
    std::string_view namespaze, std::string_view class_name,
    std::string_view field_name, std::size_t limit);
}
