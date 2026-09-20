/* cabbird/sdk/base.h -- versioned foundation types for the public C ABI.
 *
 * Migrated from the sibling Anomaly project (include/anomaly/sdk/base.h) as part of
 * the UE -> Unity port.  The naming convention, the struct layout and the
 * extensibility rules are kept identical on purpose: a plugin author who has read
 * Anomaly's SDK should recognise every construct here, and the two ABIs should be
 * diffable side by side.
 *
 * THE RULES, straight from the upstream design:
 *
 *   1. Everything crossing the host/plugin boundary is a struct, is versioned with
 *      a `V1` suffix, and starts with `struct_size` (and, where the struct can grow
 *      in the middle, `reserved`).  The host writes `struct_size` before the call,
 *      so a newer host and an older plugin can both be correct.
 *   2. Strings and buffers are (pointer, size) pairs -- never NUL-terminated
 *      assumptions, never a length the receiving side has to compute.
 *   3. Allocation does NOT cross the boundary.  CabbirdAllocatorV1 is supplied BY
 *      the host; a plugin that returns memory the host will free, or frees memory
 *      the host allocated, is a bug the ABI prevents rather than documents.
 *   4. Errors are values (CabbirdStatusV1), not exceptions and not errno.
 *
 * WHY THESE RULES EXIST FOR US SPECIFICALLY: Cabbird's worst failure so far was a
 * component killing the host process (0xC0000005, in-game, from a dumper that read
 * a field it was not allowed to read).  A plug-in architecture multiplies the
 * number of components that can do that.  The ABI is the place where the blast
 * radius gets bounded, so it is worth getting right before there are plugins.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define CABBIRD_CALL __cdecl
#if defined(__cplusplus)
#define CABBIRD_SDK_EXPORT extern "C" __declspec(dllexport)
#else
#define CABBIRD_SDK_EXPORT __declspec(dllexport)
#endif
#else
#define CABBIRD_CALL
#define CABBIRD_SDK_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Borrowed views.  The callee must not retain either past the call that received
 * it unless the service contract for that call says otherwise. */
typedef struct CabbirdStringViewV1 { const char* data; size_t size; } CabbirdStringViewV1;
typedef struct CabbirdByteSpanV1 { const uint8_t* data; size_t size; } CabbirdByteSpanV1;
typedef struct CabbirdMutableByteSpanV1 { uint8_t* data; size_t size; } CabbirdMutableByteSpanV1;

typedef enum CabbirdStatusCodeV1 {
    CABBIRD_STATUS_V1_OK = 0,
    CABBIRD_STATUS_V1_INVALID_ARGUMENT = 1,
    CABBIRD_STATUS_V1_UNAVAILABLE = 2,
    CABBIRD_STATUS_V1_NOT_FOUND = 3,
    CABBIRD_STATUS_V1_BUFFER_TOO_SMALL = 4,
    CABBIRD_STATUS_V1_FAILED = 5,
    CABBIRD_STATUS_V1_TIMEOUT = 6,
    CABBIRD_STATUS_V1_PERMISSION_DENIED = 7,
    CABBIRD_STATUS_V1_CONFLICT = 8,
    CABBIRD_STATUS_V1_CANCELLED = 9
} CabbirdStatusCodeV1;

/* `message` is a borrowed view owned by the host, valid until the next call on the
 * same thread.  A plugin must copy it if it wants to keep it. */
typedef struct CabbirdStatusV1 {
    uint32_t code;
    uint32_t reserved;
    CabbirdStringViewV1 message;
} CabbirdStatusV1;

/* Supplied by the HOST, used by the plugin.  This is what makes it safe for
 * plugin-allocated memory to outlive a single call: both sides agree on who may
 * release it, and the host's allocator is the only one involved. */
typedef struct CabbirdAllocatorV1 {
    uint32_t struct_size;
    uint32_t reserved;
    void* user;
    void* (CABBIRD_CALL *allocate)(void* user, size_t size, size_t alignment);
    void* (CABBIRD_CALL *reallocate)(void* user, void* memory, size_t size, size_t alignment);
    void (CABBIRD_CALL *release)(void* user, void* memory, size_t alignment);
} CabbirdAllocatorV1;

/* A handle that is only valid for one "generation" of a plugin or service.
 *
 * The generation is not decoration: it is how a task, hook or subscription that was
 * queued before a hot-reload is recognised as stale after it.  Anything the host
 * holds on a plugin's behalf is tagged with (id, generation) so that cancel and
 * drain can be exact -- CancelOwnerGeneration() rather than "cancel everything and
 * hope".
 */
typedef struct CabbirdGenerationHandleV1 { uint64_t id; uint64_t generation; } CabbirdGenerationHandleV1;

/* Availability gating.  A service is never simply absent: it is either AVAILABLE or
 * UNAVAILABLE, and a plugin is expected to degrade rather than fail when it asks for
 * something the current game build does not expose.
 *
 * This matters more for Cabbird than it did upstream.  Our target is a live shipping
 * build whose layout is discovered at runtime; "the game patch
 * moved this and the service is unavailable" is the normal case, not an error case,
 * and the framework has to make that expressible without a crash.
 *
 * `CabbirdFeatureStateV1` (UNAVAILABLE / AVAILABLE) used to live here, for the
 * `feature_state` entry of `cabbird.unity.build`.  Both are gone: that service was a
 * UE5 import that nothing published, so the enum had no reader anywhere in the tree.
 * Per-feature state is reported today by the Unity compatibility page, whose rows come
 * from the profile document rather than from a hand-written id list. */

#ifdef __cplusplus
}
#endif
