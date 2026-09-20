/* APPEND-ONLY FILE.  Field order is the ABI: plugin_manager.cpp validates every
 * service table by (struct_size, service_version), so nothing may be reordered,
 * retyped or removed.  New services go at the END behind a new id + version.
 *
 * WHY THE OVERLAY SERVICE IS NOT NAMED AFTER AN ENGINE HUD
 * --------------------------------------------------------
 * This facade was ported from Anomaly, which targets UE5, and it arrived named after
 * Unreal's own HUD class.  That name was wrong twice over:
 *   1. A reader who knows that engine takes such a name as a claim about WHAT IS BEING
 *      HOOKED, and would go looking for a canvas binding that does not exist here.
 *   2. Worse, it is not the game's canvas at all.  This service is the HOST's own
 *      screen-space drawing surface: the host owns the framebuffer (it presents on
 *      the game's swapchain), and a plugin subscribes to be handed a frame with
 *      project / draw_line / draw_rect / draw_text on it.  A HUD-class name hides
 *      the fact that Cabbird draws it, not the game.
 *
 * It is now `CabbirdUnityOverlayServiceV1` / `cabbird.unity.overlay`, which matches what
 * the rest of this repository calls it ("the overlay's Present hook", "the overlay draws
 * ESP full-screen").  Nothing is published under the old id -- ProcessAdapterServices()
 * has no producer for it -- so no plugin can depend on it.
 *
 * The engine nouns that remain in this repository are the ones that are genuinely about
 * the engine and should stay: a comment that cites the sibling UE5 host, or a UE concept
 * that keeps its UE name so a reader can look it up.
 *
 * WHAT WAS APPENDED
 *
 * CabbirdUnityObjectsServiceV1 enumerates live UnityEngine.Objects but carries no
 * world transform and no bounding box, so a plugin cannot make a box out of it
 * without knowing the game's own component layout.  That layout belongs in a host
 * Profile (a signature-validated binding), not in a plugin.
 *
 * CabbirdUnityEntitiesServiceV1 is therefore appended as the host-owned source of
 * "these are the boxable entities in the world right now".  The plugin stays
 * engine-agnostic and game-agnostic: it consumes world-space positions and
 * extents and knows nothing about class names, field offsets or IL2CPP.
 *
 * WHAT WAS REMOVED, AND WHY THAT DOES NOT BREAK THE RULE AT THE TOP
 * -----------------------------------------------------------------
 * Six service families were declared here and published by NOTHING -- no Publish() call
 * in this repository ever named them, so no plugin could obtain one:
 *
 *   cabbird.unity.build          <- anomaly.ue5.build / anomaly.nte.build
 *   cabbird.unity.framework      <- anomaly.ue5.framework
 *   cabbird.unity.process-event  <- anomaly.ue5.process-event
 *   cabbird.unity.names          <- anomaly.ue5.names
 *   cabbird.unity.objects        <- anomaly.ue5.objects
 *   cabbird.unity.world          <- anomaly.ue5.world
 *
 * The lineage is the point: these are the sibling UE5 project's services with the
 * namespace rewritten, and the nouns left alone.  UE5's single `UObject::ProcessEvent`
 * funnel, its `FName`/`FText` pools, its chunked `GObjects` array and its `UWorld` do not
 * exist in Unity + IL2CPP, and nothing here implemented them; `CabbirdUnityObjectsServiceV1`
 * even carried the `name_id`/`find_exact` shape of a GObjects walk.  Two of the six
 * (`build`, `framework`) had engine-agnostic payloads the host does hold -- a build id and
 * a game-thread tick -- but they were unpublished too, so a plugin could not reach them
 * either; if that surface is wanted it should come back as a Unity-native table, not as
 * this one.
 *
 * The append-only rule protects PUBLISHED tables: their field offsets are what an
 * already-built plugin reads.  A declaration with no producer has no reader, so removing
 * it costs nothing and leaving it costs a documented service that lies.  Every table that
 * IS published kept its position, its shape and its version.
 *
 * The replacement for the objects/world pair is `cabbird.unity.entities`, with
 * `cabbird.unity.transform` for the engine facts a consumer of it needs.
 */
#pragma once
#include "cabbird/sdk/base.h"
/* CabbirdEspCameraV1 lives in the UI facade.  unity.h needs it for the entity overlay
 * camera entry, and ui.h does not include unity.h, so this is not a cycle. */
#include "cabbird/sdk/services/ui.h"
/* The host's screen-space drawing surface.  See the header comment: this is
 * Cabbird's overlay layer: the host's own surface, not the game's UGUI canvas. */
#define CABBIRD_UNITY_OVERLAY_SERVICE_V1_ID "cabbird.unity.overlay"
#define CABBIRD_UNITY_OVERLAY_SERVICE_V1_VERSION 1u
#ifdef __cplusplus
extern "C" {
#endif
typedef enum CabbirdUnityOverlayFrameFlagsV1 {
    CABBIRD_UNITY_OVERLAY_FRAME_V1_NONE = 0
} CabbirdUnityOverlayFrameFlagsV1;
typedef struct CabbirdUnityOverlayFrameV1 {
    uint32_t struct_size; uint32_t flags; void* user;
    uint32_t viewport_width; uint32_t viewport_height;
    int (CABBIRD_CALL *project)(
        void* user, const double world[3], float screen[2], double* depth);
    int (CABBIRD_CALL *measure_text)(
        void* user, CabbirdStringViewV1 text, float scale, float* width, float* height);
    int (CABBIRD_CALL *draw_text)(
        void* user, CabbirdStringViewV1 text, float x, float y,
        uint32_t color_rgba, float scale);
    int (CABBIRD_CALL *draw_line)(
        void* user, float start_x, float start_y, float end_x, float end_y,
        uint32_t color_rgba, float thickness);
    int (CABBIRD_CALL *draw_rect)(
        void* user, float x, float y, float width, float height,
        uint32_t color_rgba);
} CabbirdUnityOverlayFrameV1;
// The host calls every subscriber on one thread for the frame, in subscription
// order, from inside Present.  The frame and every function in it are valid only
// until the callback returns; nothing in it may be retained.
//
// This is NOT the UE Game thread, and it is NOT a Unity UGUI canvas hook.  Cabbird's
// overlay presents on the game's swapchain, so the callback runs on whatever thread
// calls Present -- which is why the render-domain rules in plugin.h apply here: no
// allocation, no blocking, no IL2CPP.
typedef void (CABBIRD_CALL *CabbirdUnityOverlayDrawCallbackV1)(
    void* user, const CabbirdUnityOverlayFrameV1* frame);
typedef struct CabbirdUnityOverlayServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    CabbirdStatusV1 (CABBIRD_CALL *subscribe)(
        void* user, CabbirdUnityOverlayDrawCallbackV1 callback, void* callback_user,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *unsubscribe)(
        void* user, CabbirdGenerationHandleV1 handle);
} CabbirdUnityOverlayServiceV1;
// A successful unsubscribe prevents future callback admission and normally
// drains an already-entered callback before returning. Self-unsubscribe from
// that callback only closes future admission; callback_user must remain valid
// until the current callback returns.

/* ---------------------------------------------------------------------------
 * APPENDED.  Everything below this line is new; nothing above it moved.
 * ------------------------------------------------------------------------- */

typedef enum CabbirdUnityEntityFlagsV1 {
    CABBIRD_UNITY_ENTITY_V1_NONE = 0,
    /* The local player.  The plugin's own default hides it, for the same reason
     * Anomaly hides it: a box on your own character is noise. */
    CABBIRD_UNITY_ENTITY_V1_LOCAL_PLAYER = 1u << 0u,
    /* Read from cached state rather than the live object this frame.  A stale box is
     * still drawable; the flag exists so "stale" is information the plugin receives
     * rather than has to guess. */
    CABBIRD_UNITY_ENTITY_V1_STALE = 1u << 1u,
    /* Bounds are trustworthy.  Absent means the host knows the entity exists but could
     * not resolve its extents this frame; the plugin must skip it rather than draw a
     * degenerate box at the origin. */
    CABBIRD_UNITY_ENTITY_V1_VALID = 1u << 2u,
    /* `screen_points` / `screen_point_mask` are populated for this entity.  Absent means the
     * host could not project it this frame (no camera, or the entity is entirely behind the
     * near plane), which is a DIFFERENT condition from "the entity does not exist" and is why
     * it is a flag rather than an empty array the plugin would have to interpret. */
    CABBIRD_UNITY_ENTITY_V1_SCREEN_POINTS = 1u << 3u
} CabbirdUnityEntityFlagsV1;

/* `CabbirdUnityEntityV1.kind`, the host's category numbering.
 *
 * APPENDED, AND THE TWO OLD VALUES KEPT.  Three buckets would have to put `MonsterData` and
 * `NpcData` in the same one, and they are different things in the game's own model --
 * monster-ness and NPC-ness are separate facts -- so they are separate buckets here.
 *
 * Values 1 and 2 keep their original meaning so a plugin built against the old header still
 * colours enemies and players correctly; only the collapsed "other" bucket was split. */
typedef enum CabbirdUnityEntityKindV1 {
    CABBIRD_UNITY_ENTITY_V1_KIND_UNCLASSIFIED = 0,
    CABBIRD_UNITY_ENTITY_V1_KIND_PLAYER = 1,
    CABBIRD_UNITY_ENTITY_V1_KIND_MONSTER = 2,
    CABBIRD_UNITY_ENTITY_V1_KIND_NPC = 3,
    CABBIRD_UNITY_ENTITY_V1_KIND_WORLD = 4
} CabbirdUnityEntityKindV1;

/* One boxable entity, resolved by the HOST from the game's own object model.
 *
 * `bounds_center` and `bounds_extent` are WORLD-space, in the space the host's
 * projection consumes: center is the box midpoint, extent is the half-size along
 * each axis.  An AABB in that space is what the 2D and 3D box paths need, and it is
 * the only shape that stays correct when the entity rotates.
 *
 * Everything is copied by value.  `label` is borrowed and stays alive at least until
 * the next `entity_at` call; a plugin that needs it longer must copy it (UTF-8, not
 * NUL-terminated -- use `label_size`). */
typedef struct CabbirdUnityEntityV1 {
    uint32_t struct_size;
    uint32_t flags;               /* CabbirdUnityEntityFlagsV1 */
    uint64_t entity_id;           /* stable per entity for the session */
    uint32_t kind;                /* host-assigned category; 0 = unclassified */
    uint32_t label_size;          /* bytes, excluding any NUL */
    const char* label;            /* UTF-8, borrowed, may be null when label_size == 0 */
    double bounds_center[3];
    double bounds_extent[3];
    double distance_meters;       /* from the active camera; 0 when unknown */
    /* The eight AABB corners ALREADY PROJECTED by the game, in viewport pixels.
     *
     * APPENDED.  `screen_points[corner]` is corner `corner` of the world AABB, with the same
     * bit convention as `bounds_extent` uses (bit 0 = +x, bit 1 = +y, bit 2 = +z), and
     * `screen_point_mask` has bit `corner` set when that corner is in front of the camera.
     *
     * WHY THE HOST PUBLISHES PIXELS RATHER THAN A MATRIX: `WorldToScreenPoint` is only legal on
     * the game thread, and the render callback runs on the presenter's, so a plugin cannot make
     * this call itself.  The alternatives are to call it on the game thread and hand over the
     * answer (this), or to reconstruct the matrix on the render side (which four separate
     * derivations failed at, because the inputs available on that side are not the ones the
     * game used).
     *
     * This mirrors the shipped UE5 design, whose host answers `project` by calling the game's
     * own projection function instead of rebuilding UE's matrices (Anomaly's
     * `project` entry).  A plugin that prefers its own projection may still call the
     * frame's `project`; this exists so it never has to.
     *
     * When `CABBIRD_UNITY_ENTITY_V1_SCREEN_POINTS` is absent from `flags` the array is zeroed and
     * must not be used. */
    double screen_points[8][2];
    uint32_t screen_point_mask;
    uint32_t reserved_screen_points;
/* THE ADDRESS OF THE ENTITY'S MANAGED DATA OBJECT.
 *
 * APPENDED.  Present only when `struct_size` covers the field; a host that predates it leaves
 * it absent, which is exactly the "not available" answer.  It is still a plain data field, so
 * a plugin must not dereference it -- use `cabbird.core`'s `read_memory`.
 *
 * WHY THE HOST PUBLISHES A RAW ADDRESS TO A PLUGIN AT ALL:
 *
 * The name of an entity lives in the game's configuration tables, not in the entity's data
 * object, and reaching it means walking a chain of managed pointers.  That walk has to run on
 * the game thread, and every iteration of it has cost a rebuild of the host and
 * therefore a full game restart.  The plugin, by contrast, hot-reloads: dropping a DLL into
 * `plugins' is enough.
 *
 * Publishing the address moves the ENTIRE investigation to the side that reloads for free.  The
 * host keeps doing what only it can do (find the entities on the game thread, project them), and
 * the plugin gets the starting address it needs to explore on its own.
 *
 * Not a capability leak: the plugin already declares `cabbird.core` and can read any address in
 * the process with `read_memory`.  What it lacked was not permission but a PLACE TO START, and
 * that is all this is. */
uint64_t entity_data;
} CabbirdUnityEntityV1;

/* The entity source.  Same two-call pattern as every other Cabbird buffer entry:
 *
 *     count = entity_count(user);            // may be 0
 *     for each index: entity_at(user, index, &entity)
 *
 * There is deliberately NO subscribe.  Entities change every frame, and a push model
 * would either queue an unbounded number of updates or force the host to guess a
 * frame boundary.  Pull makes the host's work proportional to what a plugin asks
 * for, and an ESP asks once per render pass.
 *
 * `generation` increments whenever the host's entity set changes shape (new
 * snapshot, scene transition, profile rebind).  A plugin caching per-entity work must
 * key it on (generation, entity_id): entity_id is stable, index only within one
 * generation.
 *
 * Availability: UNAVAILABLE until the host has a validated profile binding for the
 * running build.  That is the NORMAL state on an unbound build, not an error; a
 * plugin degrades rather than fails. */


/* ---------------------------------------------------------------------------
 * IL2CPP type-system dump -- `cabbird.unity.dump`
 *
 * The dump already existed, but as a CONFIGURATION SWITCH read at startup by the host's
 * own probe code.  Making it a service turns it from "a switch you must set before
 * launching the game" into "an action a plugin can take, in this session, at this moment".
 *
 * WHY IT MATTERS, concretely: dumping has to happen with the game in the right STATE
 * (a dump taken at a login screen contains no battle entities), and changing an ini file
 * costs a full relaunch.  With hot reload a plugin can dump at the main menu,
 * walk into a fight, dump again, and diff the two -- in one session.
 *
 * WHY A TWIN FILE API AND NOT "return the dump as a string": a full dump is ~9 MB.
 * Copying that across the ABI on every call would be worse than the dump itself, and
 * the host would have to allocate it in the render domain's address space for no
 * reason.  The host therefore keeps the text and writes it out itself; a plugin that
 * genuinely wants the bytes can walk them in chunks with `dump_data` without ever
 * holding the whole thing.
 *
 * THREADING -- this is the part that is easy to get wrong.  Both entry points may be
 * called from EITHER domain, and both do the right thing:
 *
 *   * From the GAME domain (`on_update`), the call blocks and returns the finished
 *     dump.  That is the natural place: the game thread is already attached to the
 *     IL2CPP runtime, and a multi-second metadata walk does not stall a frame that a
 *     player is looking at.  It DOES stall the game loop, so a plugin should do it at
 *     a moment where that is acceptable.
 *
 *   * From the RENDER domain (`on_draw`), the call NEVER blocks and NEVER dumps
 *     inline.  It queues the request and returns `*result` with
 *     `CABBIRD_DUMP_V1_RESULT_PENDING`.  The walk runs on a worker thread that the
 *     host attaches to the IL2CPP domain first (`il2cpp_thread_attach`), which is the
 *     fix for the crash that made `include_methods` opt-in in the first place --
 *     "Fatal error in GC: Collecting from unknown thread".  Poll `state()` until it
 *     leaves PENDING.
 *
 * The second path is not a convenience.  A render callback is the only place a plugin
 * is guaranteed to be called every frame, and dumping from it directly would both
 * stall Present and call `il2cpp_class_get_methods` from a thread the runtime does not
 * know about.
 * ------------------------------------------------------------------------- */

#define CABBIRD_UNITY_DUMP_SERVICE_V1_ID "cabbird.unity.dump"
#define CABBIRD_UNITY_DUMP_SERVICE_V1_VERSION 1u

typedef enum CabbirdDumpFlagsV1 {
    CABBIRD_DUMP_V1_NONE = 0u,
    /* Walk `il2cpp_class_get_fields` and emit field names, types and OFFSETS.  A pure
     * metadata read -- the cheap half, and on by default in the ini-based dumper. */
    CABBIRD_DUMP_V1_FIELDS = 1u << 0u,
    /* Walk `il2cpp_class_get_methods` and emit signatures plus method RVAs.
     *
     * OFF in the ini's default configuration and left to the caller here, because it
     * forces lazy metadata initialisation and allocates managed memory across the
     * whole image.  It is also the ONLY way to answer "can the host call a managed
     * method, and at what address", which is the question the Entity Overlay binding is
     * blocked on.  Pair it with `image_filter`. */
    CABBIRD_DUMP_V1_METHODS = 1u << 1u,
    /* `Class::SetupProperties` / `SetupInterfaces`.  These WRITE to the class
     * structure and can allocate.  Enabling both of these at once is what produced
     * 0xC0000005 in GameAssembly.dll + 0x13E15C2 on an earlier run; they are exposed
     * because the worker thread now attaches to the domain first, but they remain
     * separately opt-in so that "which walk faulted" stays answerable. */
    CABBIRD_DUMP_V1_PROPERTIES = 1u << 2u,
    CABBIRD_DUMP_V1_INTERFACES = 1u << 3u,
    /* Recover the process's PLAINTEXT IL2CPP metadata blob, and (when `metadata_path`
     * is set) write it out.
     *
     * WHY THIS BELONGS TO THE DUMP AND IS NOT A SEPARATE SERVICE.  The dump this
     * service produces is a walk of the LIVE type system: names, field offsets, and --
     * when asked -- method RVAs.  What it cannot see is everything a FILE PARSER sees,
     * because the file parser reads the decrypted metadata instead of the runtime's
     * view of it.  The two are complementary, and the pairing is not optional:
     *
     *   * `global-metadata.dat` on disk is ENCRYPTED on this target (the sanity word
     *     reads back as 0x1357FEDA instead of 0xFAB11BAF, and the plaintext magic
     *     appears zero times in 50 MB).  So Il2CppDumper -- a pure file parser with no
     *     live-process mode -- cannot be pointed at the installed game at all.
     *   * The runtime has ALREADY decrypted it in order to run, and the plaintext
     *     lives in the process at a MEM_MAPPED region (type 0x40000, 50,476,024
     *     bytes, version 31).  Recovering it is a memory scan, not a cryptanalysis.
     *
     * What that buys: a dump taken WITHOUT this flag has no method lines from
     * `Assembly-CSharp.dll` -- the
     * assembly holding the game's own logic -- because the metadata never reaches the
     * parser that could enumerate the whole image.  With the blob in hand Il2CppDumper
     * produced 402,409 method declarations, DummyDll for 171 assemblies and a
     * name-to-RVA table of 1,006,620 entries.
     *
     * COST, so a caller can decide: the scan takes ~6.6 s and walks 2,356 MiB across
     * 2,044 regions of every committed type (private, mapped, image).  The result is
     * CACHED per process -- the second request, and every later one, returns the same
     * bytes with no rescan -- so a plugin may ask freely after the first. */
    CABBIRD_DUMP_V1_METADATA = 1u << 4u
} CabbirdDumpFlagsV1;

/* What the caller asks for.  Fields map 1:1 onto the dumper's own options; nothing
 * here is interpreted by the plugin layer.
 *
 * `image_filter` and `image_filter_size` restrict the METHOD walk to images whose name
 * contains the substring -- "Azur" covers this game's own assemblies and bounds the
 * heaviest operation to the part that matters.  Substring rather than exact match on
 * purpose: the target splits its code across AzurFrameworkRuntime / AzurShell /
 * AzurEngine / AzurCore / AzurPrecompile, so an exact list would need editing every
 * patch.  Empty (size 0) means every image, which is expensive and rarely wanted. */
typedef struct CabbirdDumpRequestV1 {
    uint32_t struct_size;
    uint32_t flags;                     /* CabbirdDumpFlagsV1 */
    uint32_t max_classes_per_image;     /* 0 = no limit */
    uint32_t max_methods_per_class;     /* 0 = no limit */
    const char* image_filter;           /* UTF-8 substring, may be null */
    uint32_t image_filter_size;         /* bytes, excluding any NUL */
    /* The host writes the finished text here itself.  Absolute or relative to the
     * host process's working directory -- the caller should pass an absolute path.
     * May be null, in which case nothing is written (use `dump_data`/`dump_size`). */
    const wchar_t* output_path;
    uint32_t output_path_length;        /* UTF-16 code units, excluding any NUL */

    /* APPENDED -- where the recovered metadata blob is written when
     * CABBIRD_DUMP_V1_METADATA is set.  Same conventions as `output_path`.
     *
     * MAY BE NULL, and that is a supported request rather than an omission: the bytes
     * are then in memory only and reachable through `metadata_data`/`metadata_size`.
     * A plugin that feeds a parser it hosts itself wants exactly that and should not
     * have to nominate a path it will never read. */
    const wchar_t* metadata_path;
    uint32_t metadata_path_length;
} CabbirdDumpRequestV1;

typedef enum CabbirdDumpStateV1 {
    /* NOTHING HAS EVER BEEN ASKED FOR.
     *
     * This is the zero value, and it replaces `PENDING` in that position for a reason that cost
     * real time to find: a zero-initialised `CabbirdDumpResultV1` meant "a dump is running", so
     * every consumer that polled the service before anything had run -- which is what a plugin
     * does on its first frame -- was told a dump was in flight.  The window displayed "a dump is
     * already running" and a live-looking `running...` state with a running timer, on a service
     * that had never been asked to do anything.
     *
     * A sensor must be able to say "no reading", and an enum whose zero value is a transient
     * state cannot. */
    CABBIRD_DUMP_V1_RESULT_IDLE = 0,
    /* The walk is in flight.  Assigned by `run`, and only ever observed after one. */
    CABBIRD_DUMP_V1_RESULT_PENDING = 1,
    CABBIRD_DUMP_V1_RESULT_COMPLETE = 2,
    /* The runtime never became ready, or the request was rejected before it started. */
    CABBIRD_DUMP_V1_RESULT_UNAVAILABLE = 3,
    CABBIRD_DUMP_V1_RESULT_FAILED = 4,
    /* The walk finished and its text is available through `dump_data`/`dump_size`, but no file
     * was written -- either because the request carried no `output_path` (documented as valid
     * above) or because the path did not survive conversion to UTF-16.
     *
     * Passed as a literal rather than renumbered with the rest: the enumerators above were
     * renumbered once already, and a second silent renumber is how an out-of-tree plugin ends up
     * comparing against the wrong value. */
    CABBIRD_DUMP_V1_RESULT_IN_MEMORY = 5
} CabbirdDumpStateV1;

/* The outcome of a dump, copied by value.
 *
 * `contained_faults` is the number of SEH-guarded calls that FAULTED and were caught.
 * It is not cosmetic: a non-zero value means the dump has holes, and the text itself
 * says INCOMPLETE in its header.  A caller that reports success without checking this
 * is reporting something it did not verify.
 *
 * `method_rva_inside` / `method_rva_outside` are the measurement that says whether the
 * `MethodInfo::methodPointer` offset is right.  There is no exported getter for it, so
 * it is read as a raw pointer at offset 0 -- an offset taken from a DIFFERENT Unity
 * version than this target runs.  A pointer inside GameAssembly.dll's range is real
 * code; one outside is reported as unknown rather than printed as a confident wrong
 * address.  If `method_rva_outside` is not near zero, the offset is wrong and the
 * methods in the dump must not be trusted. */
typedef struct CabbirdDumpResultV1 {
    uint32_t struct_size;
    uint32_t state;                     /* CabbirdDumpStateV1 */
    uint32_t contained_faults;
    uint32_t reserved;
    uint64_t bytes_written;             /* 0 when the request had no output_path */
    uint64_t class_count;
    uint64_t field_count;
    uint64_t method_count;
    uint64_t method_rva_inside;
    uint64_t method_rva_outside;

    /* LIVE PROGRESS -- appended, so the struct stays append-only and an older caller that
     * passes a smaller `struct_size` is still correct.
     *
     * These exist because the first real-machine dump produced a complete 30 MB artifact
     * while the window still read "running...": until the worker publishes its final
     * result there was nothing to show but the word "pending", so a 40-second walk and a
     * dead thread looked exactly alike.
     *
     * `progress_elapsed_ms` is the witness that matters most.  A class counter can stall
     * legitimately (a huge image with no classes past the last interval); a monotonic
     * clock cannot.  A caller deciding "wait" versus "give up" should read the clock.
     *
     * Semantics while state == PENDING: `class_count` / `field_count` are "visited so far"
     * and these two are the live clock and the live class count.  In any terminal state
     * `class_count` is the TOTAL and `progress_elapsed_ms` is the walk's total duration.
     * A caller wanting the final count must read it after the state leaves PENDING. */
    uint64_t progress_elapsed_ms;
    uint64_t progress_classes;

    /* APPENDED -- the metadata half of the same request.
     *
     * `metadata_size` is 0 in exactly three cases: the request did not set
     * CABBIRD_DUMP_V1_METADATA, recovery was asked for and found nothing, or the walk
     * never got far enough to try.  The three are told apart by the dump's own header
     * line ("metadata: ..."), which states which of them happened.
     *
     * `metadata_written` is the honest completion signal for the FILE half: a caller
     * that asked for a path and reads this as false knows the artifact is not on disk,
     * rather than inferring success from `state == COMPLETE`.  This mirrors
     * `bytes_written` for the dump text, for the same reason -- a green check on a path
     * nobody wrote is the failure mode this project keeps finding.
     *
     * `metadata_address` is where the blob was recovered FROM, for the log.  It is an
     * address in the host process and is meaningless to a caller outside it; it is
     * published because "recovered from 0x000000000C4F0150 in a MEM_MAPPED region" is
     * what makes the next run's log comparable with this one. */
    uint64_t metadata_size;
    uint64_t metadata_written;
    uint64_t metadata_address;
    uint64_t metadata_scan_ms;
    uint32_t metadata_version;
    uint32_t metadata_reserved;
} CabbirdDumpResultV1;

typedef struct CabbirdUnityDumpServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;

    /* Start a dump.
     *
     * From the GAME domain this blocks until the dump is finished and fills `*result`
     * with COMPLETE / FAILED / UNAVAILABLE.  From the RENDER domain it queues the work
     * and fills `*result` with PENDING, having done no metadata work on this thread.
     *
     * A second call while one is running is rejected with FAILED rather than queued:
     * two concurrent walks of the same metadata is the kind of thing that produces a
     * crash with no useful evidence. */
    CabbirdStatusV1 (CABBIRD_CALL *run)(
        void* user, const CabbirdDumpRequestV1* request, CabbirdDumpResultV1* result);

    /* Cheap, non-blocking.  Safe from both domains, safe to call every frame. */
    CabbirdStatusV1 (CABBIRD_CALL *state)(void* user, CabbirdDumpResultV1* result);

    /* Read the LAST COMPLETED dump out of the host's buffer.  `offset` is a byte
     * offset; the call copies min(capacity, size - offset) bytes and reports how many.
     * Returns UNAVAILABLE when there is no completed dump. */
    CabbirdStatusV1 (CABBIRD_CALL *dump_data)(
        void* user, uint64_t offset, char* buffer, uint64_t capacity, uint64_t* copied);

    uint64_t (CABBIRD_CALL *dump_size)(void* user);

    /* APPENDED -- the recovered plaintext metadata, in the same chunked shape as the
     * dump text, and for the same reason: it is 50 MB, so handing it across the ABI by
     * value is not an option and a plugin that only wants to write it somewhere should
     * never have to hold it.
     *
     * `metadata_data` copies min(capacity, size - offset) bytes.  UNAVAILABLE means no
     * blob has been recovered in this session; call `run` with CABBIRD_DUMP_V1_METADATA
     * once and poll `state()` -- after which every read is a memcpy, because the blob is
     * cached in the host.
     *
     * `metadata_size` returns 0 when there is nothing recovered.  Both are safe from
     * either domain, hold one lock, and do no runtime work: they serve a copy that was
     * already made, so unlike the dump walk they cannot fault inside IL2CPP. */
    CabbirdStatusV1 (CABBIRD_CALL *metadata_data)(
        void* user, uint64_t offset, char* buffer, uint64_t capacity, uint64_t* copied);

    uint64_t (CABBIRD_CALL *metadata_size)(void* user);
} CabbirdUnityDumpServiceV1;

/* ---------------------------------------------------------------------------
 * The local player: where it is, and moving it.  `cabbird.unity.player`
 *
 * APPENDED.  Nothing above this line moved.
 *
 * WHY THIS IS A HOST SERVICE AND NOT A PLUGIN
 * -------------------------------------------
 * The ask was two plugins: one that reports the player's coordinates, one that
 * teleports to arbitrary coordinates -- the Unity counterpart of calling
 * `K2_SetActorLocation` from the sibling UE5 project.
 *
 * The READ half could be a plugin on its own: `cabbird.unity.entities` already
 * publishes `entity_data` and world bounds for every live entity, and a plugin can
 * walk from there with `cabbird.core`'s `read_memory`.  The WRITE half cannot.
 *
 * In UE5 a teleport is `ProcessEvent(actor, K2_SetActorLocation, params)`: one
 * function pointer, callable from anywhere in the process.  IL2CPP has no such
 * funnel.  The position lives in Unity's NATIVE Transform behind
 * `m_CachedPtr`, which is not in `dump.cs` and whose layout this project refuses to
 * guess -- so "write the position" means CALLING a managed
 * method, and the only entity in this process that holds the runtime's invocation
 * entry, the `MethodInfo*` and an attached game thread is the HOST.
 *
 * So the split is the same one `cabbird.unity.dump` uses, and for the same reason:
 * the host owns the engine-facing half (resolve the transform, call the runtime,
 * attach to the domain), the plugin owns policy (which coordinates, from where, with
 * what UI).
 *
 * WHY A SEPARATE SERVICE FROM `cabbird.unity.entities`
 * -----------------------------------------------------
 * The entity source is a PULL surface read from the RENDER domain.  A teleport is a
 * MUTATION issued from the plugin UI that must run on the GAME thread.  Folding the
 * two together would put a mutating entry in a table whose contract is "read the
 * cached set", and would change the meaning of a table plugins already depend on.
 * Append a new id instead.
 *
 * THE WRITE IS POST-CHECKED, AND THAT IS THE POINT
 * ------------------------------------------------
 * `set_position` returning without an exception is not evidence that the player
 * moved.  Unity's own character controllers (`CharacterController`, `Rigidbody`)
 * own the movement and can overwrite a transform write on the very next frame, so
 * the host re-reads the position and only reports OK when the player is actually
 * within tolerance of the request.  A mutation service that reports success because
 * a function returned is a service that lies about the one thing it exists to do.
 * ------------------------------------------------------------------------- */

#define CABBIRD_UNITY_PLAYER_SERVICE_V1_ID "cabbird.unity.player"
#define CABBIRD_UNITY_PLAYER_SERVICE_V1_VERSION 1u

typedef enum CabbirdUnityPlayerFlagsV1 {
    CABBIRD_UNITY_PLAYER_V1_NONE = 0u,
    /* The identity and position below were read off a live player this generation.
     * Absent means no player was found -- not that the player is at the origin. */
    CABBIRD_UNITY_PLAYER_V1_VALID = 1u << 0u,
    /* `bounds_center` carries the approximate AABB centre from the entity source.
     * `position` is always the exact transform position; this flag describes the
     * extra field only. */
    CABBIRD_UNITY_PLAYER_V1_HAS_BOUNDS = 1u << 1u,
    /* The host resolved the player by proximity to the active camera because
     * `cabbird.unity.entities` does not yet identify the local player itself
     * (its `LOCAL_PLAYER` flag is off; see unity_adapter.cpp).  Published
     * because a heuristic that is invisible is one nobody can correct. */
    CABBIRD_UNITY_PLAYER_V1_IDENTITY_PROXIMITY = 1u << 2u
} CabbirdUnityPlayerFlagsV1;

/* One reading of the local player, copied by value.
 *
 * `entity_id` is the SAME identity `CabbirdUnityEntityV1::entity_id` carries --
 * the managed data object's address via the entity source -- so a plugin can match
 * a player reading against its own entity list without a translation table.
 *
 * `position` is a WORLD-SPACE pivot (Unity's transform position), NOT the box
 * centre: the entity source lifts its box by one half-height, and a teleport must
 * use the value Unity itself would return from `Transform.position`. */
typedef struct CabbirdUnityPlayerSnapshotV1 {
    uint32_t struct_size;
    uint32_t flags;                 /* CabbirdUnityPlayerFlagsV1 */
    uint64_t generation;            /* entity-source generation this reading came from */
    uint64_t entity_id;             /* 0 when no player was found */
    char data_class[32];            /* live C# class of the player's data object, NUL-terminated */
    double position[3];
    double bounds_center[3];
    double distance_to_camera;
    /* Host-side binding diagnostic.  On NOT_FOUND this identifies the exact failed
     * MainPlayer/controlling-entity/Transform step instead of collapsing every cause
     * into "no live player".  Empty means no additional diagnostic is available. */
    char reason[192];
} CabbirdUnityPlayerSnapshotV1;

typedef enum CabbirdUnityPlayerWriteFlagsV1 {
    CABBIRD_UNITY_PLAYER_WRITE_V1_NONE = 0u,
    /* ALSO WRITE THE ROTATION, as euler degrees in Unity's convention (positive x
     * pitches DOWN -- see the SIGN WARNING in ui.h).  Absent means `euler_degrees`
     * must be ignored and the player keeps facing where it faced. */
    CABBIRD_UNITY_PLAYER_WRITE_V1_SET_ROTATION = 1u << 0u
} CabbirdUnityPlayerWriteFlagsV1;

/* A teleport request.  The counterpart of the UE5 host's
 * `AnomalyNtePlayerTeleportRequestV1`, with the engine-specific parts replaced:
 * UE needed `world` + `player` handles and `bSweep`/`bTeleport` flags; IL2CPP needs
 * neither a world handle nor a sweep flag, and `entity_id` alone identifies the
 * target because it IS the live data object's identity. */
typedef struct CabbirdUnityPlayerWriteRequestV1 {
    uint32_t struct_size;
    uint32_t flags;                 /* CabbirdUnityPlayerWriteFlagsV1 */
    uint64_t entity_id;             /* from a snapshot; 0 = resolve the local player */
    double position[3];
    double euler_degrees[3];        /* only when SET_ROTATION */
} CabbirdUnityPlayerWriteRequestV1;

typedef enum CabbirdUnityPlayerWriteResultCodeV1 {
    /* Queued or mid-verification.  The write may already have happened; the verdict is not
     * in yet, so reporting success here would be a guess. */
    CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_PENDING = 0,
    /* The player is within `position_error` of the request AFTER a later tick confirmed it
     * was still there. */
    CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_OK = 1,
    /* The write call succeeded and the position was correct immediately afterwards, but a
     * later tick read the OLD position back: the game's own controller (a
     * `CharacterController`, a `Rigidbody`, a movement state machine, or a server
     * correction) owns the transform and overwrote us.  THIS IS THE CASE THAT MAKES THE
     * POST-CHECK WORTH ITS COST -- without it this would be reported as a successful
     * teleport that visibly did nothing. */
    CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REVERTED = 2,
    /* Refused before or during the call: bad coordinates, an entity that is no longer the
     * player, a missing `set_position`, or a runtime exception.  `position_error` is not
     * meaningful. */
    CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_REFUSED = 3
} CabbirdUnityPlayerWriteResultCodeV1;

/* What actually happened, copied by value.  A status code alone cannot report this:
 * "the call was made" and "the player is there" are different facts and the whole
 * failure mode of a teleport is the gap between them. */
typedef struct CabbirdUnityPlayerWriteResultV1 {
    uint32_t struct_size;
    uint32_t flags;                 /* CabbirdUnityPlayerWriteFlagsV1 of the request */
    uint64_t entity_id;             /* the entity actually written */
    double applied_position[3];     /* requested target, echoed back */
    double measured_position[3];    /* re-read AFTER the write -- what the game reports now */
    double position_error;          /* metres between the two */
    double apply_micros;            /* game-thread cost of the write call itself */
    uint32_t request_sequence;      /* the sequence this result belongs to */
    uint32_t result_code;           /* CabbirdUnityPlayerWriteResultCodeV1 */
} CabbirdUnityPlayerWriteResultV1;

typedef struct CabbirdUnityPlayerServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    /* Cheap, read-only, non-blocking.  Safe from the RENDER domain (the ESP window)
     * and from the GAME domain.  Returns NOT_FOUND when no player is live, having
     * zeroed the snapshot -- a zeroed snapshot means "nobody", never "the origin". */
    CabbirdStatusV1 (CABBIRD_CALL *snapshot)(
        void* user, CabbirdUnityPlayerSnapshotV1* snapshot);
    /* ======================================================================================
     * THE TWO ENTRIES BELOW ARE THE v1 COMPATIBILITY PATH, AND THEY COST A SECOND CAPABILITY
     * ======================================================================================
     *
     * This table was designed as the READ half of the player bridge and the write half was split
     * out into `cabbird.unity.player-teleport` (see the section below).  These two slots are what
     * the split could not remove: the SDK surface is append-only, so the offsets an already-built
     * plugin reads are frozen, and the host keeps them WORKING rather than nulling them --
     * `include/cabbird/unity_services.hpp` states the rule ("the compatibility path and the
     * documented path are the SAME code").
     *
     * The consequence for a PLUGIN AUTHOR is the part that matters:
     *
     *   * the capability that grants THIS table is `unity-player-snapshot` -- a READ.  Declaring it
     *     does NOT authorize these two entries;
     *   * they are authorized per call against `unity-player-teleport`, because authorization is
     *     otherwise resolved once per service at query time and cannot tell which entry is called.
     *     A plugin without that capability gets `PERMISSION_DENIED` from them, with the missing
     *     capability named in the message;
     *   * so the supported way to move the player is `cabbird.unity.player-teleport` +
     *     `unity-player-teleport`, and this pair exists for plugins built against the older shape.
     *     `plugins/player_teleport` uses the supported path; nothing in this tree calls these.
     *
     * The host applies the same shape to `cabbird.core`, whose single table carries memory reads
     * and writes: there, the named pair is `memory-read` / `memory-write`.  These two services are
     * the only mixed-character tables in the ABI, and both are resolved in
     * `src/plugin/plugin_capability_policy.cpp` (`AuthorizeRawMemory`, `AuthorizePlayerWrite`).
     * ====================================================================================== */
    /* Request a teleport.
     *
     * THE CALL NEVER MOVES THE PLAYER ITSELF.  The transform write is a managed call
     * and therefore legal only on the game thread, so this records the request and
     * returns OK ("accepted and queued"); the host applies it on its next game-domain
     * tick and the outcome appears in `write_state`.  Blocking a render callback on a
     * game tick would deadlock the overlay, and calling `il2cpp_runtime_invoke` from the
     * presenter's thread is a crash this project has already recorded.
     *
     * OK here does NOT mean the player moved.  Whether it moved is `write_state`'s
     * answer, and the two are deliberately different questions.
     *
     * Requires `unity-player-teleport`.  Returns PERMISSION_DENIED without it, NOT_FOUND when
     * there is no live player to move, INVALID_ARGUMENT for a non-finite or absurd coordinate (or
     * a short `struct_size`), and CONFLICT when a previous request is still awaiting its game tick.
     * Prefer `cabbird.unity.player-teleport`'s `teleport`. */
    CabbirdStatusV1 (CABBIRD_CALL *write_position)(
        void* user, const CabbirdUnityPlayerWriteRequestV1* request);
    /* The outcome of the LAST-APPLIED request, or NOT_FOUND when nothing has been
     * applied yet in this session.  `request_sequence` tells the caller whether the
     * result it is reading belongs to the request it made.
     *
     * Requires `unity-player-teleport`, like the entry above: reading the verdict of a write is
     * part of the write policy.  Prefer `cabbird.unity.player-teleport`'s `state`. */
    CabbirdStatusV1 (CABBIRD_CALL *write_state)(
        void* user, CabbirdUnityPlayerWriteResultV1* result);
} CabbirdUnityPlayerServiceV1;

/* ==========================================================================================
 * cabbird.unity.transform -- the ENGINE-side half of "move an object"
 * ==========================================================================================
 *
 * WHY THIS EXISTS AS ITS OWN TABLE, AND WHY IT IS NOT PART OF entity-overlay
 *
 * The two offset hops that reach a `UnityEngine.Transform` (`BaseData::<transform>`, then
 * `RelativeTransform::m_transform`) and the IL2CPP handles needed to call it are ENGINE FACTS.
 * They are true regardless of any entity list, any camera, or any ESP overlay.
 *
 * They used to live only in `cabbird.unity.entities`, so the player service had to borrow
 * them by including that service's header -- a header whose contract is "publish the cached
 * entity set", documented as read-only, but which ended up exporting a function that changes
 * game world state.  Both halves of that were wrong: a read-only service published a write,
 * and two services with nothing to say to each other were coupled by an include.
 *
 * So the fact moved to the ADAPTER (where Anomaly keeps its engine-facing service), and both
 * `entity-overlay` and `player` became CONSUMERS of it.  `player` reaches it through the service
 * registry, so it no longer includes anything from the game layer at all.
 *
 * DOMAIN AND THREADING
 *
 * Every entry here calls into IL2CPP and is GAME DOMAIN ONLY.  Calling `il2cpp_runtime_invoke`
 * from the presenter's thread is a crash this project has already recorded, and this table does
 * not pretend otherwise by offering a "safe from any thread" entry.
 *
 * WHAT THIS TABLE DELIBERATELY DOES NOT DO
 *
 * It does not know what a "player" is, does not take entity ids, and does not decide whether a
 * position is acceptable.  Those are policy, they belong to the player service, and keeping
 * them out is what makes this table an honest description of the engine rather than a guess
 * about the game's intent.
 * ========================================================================================== */

#define CABBIRD_UNITY_TRANSFORM_SERVICE_V1_VERSION 1u
#define CABBIRD_UNITY_TRANSFORM_SERVICE_V1_ID "cabbird.unity.transform"

/* Resolve `data` (a managed `BaseData` object address that the CALLER already read) into the
 * `UnityEngine.Transform` it owns, with the liveness check applied: a destroyed managed
 * wrapper still has a reachable address but a null native pointer, and calling a method on one
 * faults inside the runtime.
 *
 * `data` is taken rather than an entity id because entity ids are the entity source's
 * vocabulary, not the engine's.  A consumer with an entity id asks `cabbird.unity.entities`
 * for the data address and then asks this service about the transform -- two questions, two
 * owners, no coupling.
 *
 * Returns OK and writes `*transform` (non-zero), or NOT_FOUND when the object is unreadable,
 * the chain is null, the wrapper is dead, or the binding has not been resolved yet.
 * INVALID_ARGUMENT for a null out-parameter.  GAME DOMAIN ONLY. */
typedef struct CabbirdUnityTransformResolveRequestV1 {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t data;              /* managed BaseData object address */
} CabbirdUnityTransformResolveRequestV1;

typedef struct CabbirdUnityTransformHandleV1 {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t transform;         /* UnityEngine.Transform* */
    uint64_t data;              /* echoed back, so a caller can detect a stale reading */
} CabbirdUnityTransformHandleV1;

/* Read `Transform::get_position`.  `position` receives three doubles.
 *
 * MEASURED COST: about 2.5 ms per call on the live game.  A consumer that reads
 * many objects per frame should sample rather than read every frame -- the entity overlay does
 * exactly that, and this number is the reason. */
typedef struct CabbirdUnityTransformPositionV1 {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t transform;
    double position[3];
} CabbirdUnityTransformPositionV1;

/* Move the object, and/or rotate it.
 *
 * A WRITE IS NOT VERIFIED BY THIS ENTRY.  It reports only whether the managed call returned
 * without an exception.  Unity's movement controllers routinely undo a position write on the
 * very next frame, so "the call succeeded" and "the object is there now" are different
 * questions -- which is precisely why `cabbird.unity.player` re-reads the position afterwards
 * and reports REVERTED when the game put it back, instead of this table claiming success. */
typedef struct CabbirdUnityTransformWriteV1 {
    uint32_t struct_size;
    uint32_t flags;             /* CabbirdUnityTransformWriteFlagsV1 */
    uint64_t transform;
    double position[3];         /* used when SET_POSITION is set */
    double euler_degrees[3];    /* used when SET_ROTATION is set */
} CabbirdUnityTransformWriteV1;

enum {
    CABBIRD_UNITY_TRANSFORM_WRITE_V1_NONE = 0u,
    CABBIRD_UNITY_TRANSFORM_WRITE_V1_SET_POSITION = 1u << 0u,
    CABBIRD_UNITY_TRANSFORM_WRITE_V1_SET_ROTATION = 1u << 1u
};

typedef struct CabbirdUnityTransformServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    /* `data` -> a live `Transform`.  GAME DOMAIN ONLY. */
    CabbirdStatusV1 (CABBIRD_CALL *resolve)(
        void* user, const CabbirdUnityTransformResolveRequestV1* request,
        CabbirdUnityTransformHandleV1* handle);
    /* `Transform::get_position`.  GAME DOMAIN ONLY. */
    CabbirdStatusV1 (CABBIRD_CALL *position)(
        void* user, const CabbirdUnityTransformHandleV1* handle,
        CabbirdUnityTransformPositionV1* position);
    /* `set_position` / `set_eulerAngles`, per `flags`.  GAME DOMAIN ONLY.
     *
     * Non-finite or absurd coordinates are INVALID_ARGUMENT; a missing
     * `set_eulerAngles` on this build is UNAVAILABLE for a rotation-only request and does
     * NOT make a position request fail. */
    CabbirdStatusV1 (CABBIRD_CALL *write)(
        void* user, const CabbirdUnityTransformWriteV1* write);
} CabbirdUnityTransformServiceV1;

/* ==========================================================================================
 * cabbird.unity.player-teleport -- the WRITE policy, split away from the player SNAPSHOT
 * ==========================================================================================
 *
 * WHY THIS IS NOT PART OF `cabbird.unity.player`
 *
 * It used to be.  `cabbird.unity.player` carried `snapshot` (read the local player) alongside
 * `write_position` / `write_state` (move it), which made one table answer two questions with
 * opposite characters: a read-only snapshot a plugin may poll every frame, and a queued MUTATION
 * that changes the world.
 *
 * The sibling project does not do that, and its manifests show why it matters.  Its
 * `NtePosition` ("Coordinate Display") declares `anomaly.nte.player` only -- a read.  Its
 * `NteTeleport` declares `anomaly.nte.player` AND `anomaly.nte.player-teleport`.  The grant a
 * coordinate HUD needs and the grant a teleporter needs are visibly different capabilities, and
 * a single table forces them to be the same one.
 *
 * So the split here mirrors it exactly, and the two plugins in this tree landed on the same
 * line without being told: `player_coords` reads the snapshot and has no control that writes;
 * `player_teleport` drives the write.
 *
 * THE REQUEST AND RESULT TYPES ARE SHARED, DELIBERATELY
 *
 * `CabbirdUnityPlayerWriteRequestV1` and `CabbirdUnityPlayerWriteResultV1` are declared above
 * and are NOT redefined here.  They are the vocabulary of a write, and both halves of this
 * split need to speak it; declaring a second, identically-shaped pair would create two types
 * that mean the same thing and can drift apart.
 *
 * WHY `teleport` DOES NOT REPORT THE VERDICT (the one place this diverges from the sibling)
 *
 * The sibling's `teleport` is synchronous: it is called from the Game callback domain and
 * returns FAILED when the post-call location check does not reach the requested position.  That
 * host has a `scheduler` service, so a plugin UI can hand a request to a game-thread callback
 * and get an answer in the same breath.
 *
 * This host has no scheduler, and its plugin UI runs on the RENDER domain -- where calling
 * `il2cpp_runtime_invoke` is a crash this project has already recorded.  Blocking a render
 * callback on a game tick would deadlock the overlay.  So `teleport` RECORDS the request and
 * answers OK ("accepted and queued"), and `state` carries the verdict once the game thread has
 * applied and re-read it.  OK here does NOT mean the player moved; `state` is what answers that,
 * and the two are deliberately different questions.
 *
 * The VERDICT itself is unchanged from the sibling's intent: `RESULT_REVERTED` exists because
 * Unity's `CharacterController` / `Rigidbody` can undo a transform write on the very next frame,
 * and a mutation service that reports success because a function returned is a service that lies
 * about the one thing it exists to do.
 * ========================================================================================== */

#define CABBIRD_UNITY_PLAYER_TELEPORT_SERVICE_V1_VERSION 1u
#define CABBIRD_UNITY_PLAYER_TELEPORT_SERVICE_V1_ID "cabbird.unity.player-teleport"

typedef struct CabbirdUnityPlayerTeleportServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    /* Request a teleport.  NEVER moves the player itself: see the header note above for why the
     * write is queued rather than performed in this call.
     *
     * Returns NOT_FOUND when there is no live player to move, INVALID_ARGUMENT for a non-finite
     * or absurd coordinate (or a short `struct_size`), and CONFLICT when a previous request is
     * still awaiting its game tick.
     *
     * Fails with UNAVAILABLE when the engine half (`cabbird.unity.transform`) could not be
     * resolved -- an honest answer, because in that state no write can ever succeed. */
    CabbirdStatusV1 (CABBIRD_CALL *teleport)(
        void* user, const CabbirdUnityPlayerWriteRequestV1* request);
    /* The outcome of the LAST-APPLIED request, or NOT_FOUND when nothing has been applied yet
     * in this session.  `request_sequence` tells the caller whether the result it is reading
     * belongs to the request it made. */
    CabbirdStatusV1 (CABBIRD_CALL *state)(
        void* user, CabbirdUnityPlayerWriteResultV1* result);
} CabbirdUnityPlayerTeleportServiceV1;

/* ==========================================================================================
 * cabbird.unity.entities -- THE ENTITY SET, with no opinion about drawing
 * ==========================================================================================
 *
 * WHY THIS EXISTS SEPARATELY FROM `cabbird.unity.entities`
 *
 * `cabbird.unity.entities` was doing two jobs under one id: enumerating the live entities, and
 * answering the questions an OVERLAY needs (where are they on screen, what colour, what does the
 * label say).  The second job is a rendering concern with no reader outside an ESP plugin; the
 * first is the thing every other consumer actually wants.
 *
 * Two consumers said so.  The player service only needs "which entity is this id, where is its
 * transform" and had to reach the entity set THROUGH the ESP service.  A project-wide entity
 * service is what the sibling project has (`anomaly.nte.entities`), and it is the shape the
 * consumers imply.
 *
 * WHAT THIS TABLE ANSWERS: which entities exist, what class each is, where its world box is, and
 * how far it is from the camera.  That is a faithful description of the game's object model.
 *
 * WHAT IT DELIBERATELY DOES NOT ANSWER: nothing about pixels, nothing about colours, nothing
 * about labels.  A consumer that wants a box drawn on a screen is an overlay's business, and
 * keeping those fields out is what lets this table be useful to a teleporter.
 *
 * `cabbird.unity.entities` remains the overlay-facing table and delegates its entity
 * enumeration here, so the two cannot disagree about what is alive.
 * ========================================================================================== */

#define CABBIRD_UNITY_ENTITIES_SERVICE_V1_VERSION 1u
#define CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID "cabbird.unity.entities"

/* One entity, re-read off the live cache by its stable id.
 *
 * WHY THIS EXISTS AS WELL AS `entity_at`: `entity_at` is addressed by INDEX, which is only
 * meaningful within one generation, so a consumer holding an id across a scene transition has no
 * way to ask about it.  The player service is exactly that consumer -- it resolves a player once
 * and must re-ask afterwards to find out whether the entity is still there rather than reading
 * through a pointer whose address may have been recycled.
 */
typedef struct CabbirdUnityEntitiesLookupV1 {
    uint32_t struct_size;
    uint32_t kind;                  /* CabbirdUnityEntityKindV1 */
    uint64_t entity_id;
    uint64_t generation;            /* the generation this reading belongs to */
    /* The managed `BaseData` object's address -- the same value `entity_at` publishes as
     * `entity_data`, and therefore a starting point a consumer can use with
     * `cabbird.unity.transform`'s `resolve`, or re-walk itself through `cabbird.core`. */
    uint64_t data;
    /* The live C# class of that object, NUL-terminated.
     *
     * A FIXED BUFFER RATHER THAN A `const char*`, because the string it names lives inside the
     * entity set's own storage and the next refresh frees it: a pointer would dangle the moment
     * the caller kept it past the call. */
    char data_class[40];
} CabbirdUnityEntitiesLookupV1;

typedef struct CabbirdUnityEntitiesServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    /* Increments whenever the set changes shape (new snapshot, scene transition, profile
     * rebind).  A consumer caching per-entity work keys it on (generation, entity_id):
     * entity_id is stable, an INDEX is only meaningful within one generation. */
    uint64_t (CABBIRD_CALL *generation)(void* user);
    /* The number of entities in the CURRENT generation.  Safe from the RENDER domain: it answers
     * a count out of the published cache and touches no IL2CPP. */
    uint32_t (CABBIRD_CALL *entity_count)(void* user);
    /* One entity by index, valid only for the generation it was read in.
     *
     * `entity->label` is a UTF-8 view that is NOT NUL-terminated -- use `label_size` -- and it
     * stays valid only until the next `entity_at` call; copy it if it must outlive that.  This is
     * the same borrowed-view contract `cabbird.unity.entities` publishes, because it is the
     * same buffer.
     *
     * NOT_FOUND for a stale index, INVALID_ARGUMENT for a null out-parameter or a short
     * `struct_size`. */
    CabbirdStatusV1 (CABBIRD_CALL *entity_at)(
        void* user, uint32_t index, CabbirdUnityEntityV1* entity);
    /* One entity by its stable id, re-read off the live cache.  No IL2CPP and no dereference of
     * the target, so it is safe from any domain; NOT_FOUND for an id the current generation does
     * not contain, which is how a caller tells "it is gone" from "the whole set was rebuilt". */
    CabbirdStatusV1 (CABBIRD_CALL *lookup)(
        void* user, uint64_t entity_id, CabbirdUnityEntitiesLookupV1* entity);
    /* The active camera's world position, as the game last reported it (Unity's
     * `Camera.transform.position`, read through the engine and never derived).
     *
     * WHY IT IS HERE AND NOT IN THE PLAYER SERVICE: in a third-person game the active camera is
     * AT the local player, which is how the player service decides WHICH of the player-shaped
     * entities is the one being driven.  So the camera is an input to that decision rather than
     * an output of it, and the service that publishes the entity set is the one that can offer
     * it.  UNAVAILABLE when no camera has been read this session (before the first walk, or
     * outside a world scene). */
    CabbirdStatusV1 (CABBIRD_CALL *camera_position)(void* user, double position[3]);
    /* Camera snapshot used by consumers that draw entities. */
    CabbirdStatusV1 (CABBIRD_CALL *camera)(void* user, CabbirdEspCameraV1* camera);
    /* Enables/disables optional label population in entity_at results. */
    CabbirdStatusV1 (CABBIRD_CALL *set_want_labels)(void* user, int want);
} CabbirdUnityEntitiesServiceV1;

#ifdef __cplusplus
}
#endif
