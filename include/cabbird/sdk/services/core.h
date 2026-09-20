/* cabbird/sdk/services/core.h -- the always-available base service.
 *
 * Migrated from anomaly/sdk/services/core.h.  This is the smallest service that must
 * always resolve, so every plugin can log and read memory without depending on the
 * game-specific layers.
 *
 * The memory functions are thin C wrappers over the existing cabbird::mem pipeline,
 * which is already validated on the live target:
 * typed reads/writes, region enumeration, pattern scan, patch-vs-write rules.  A
 * plugin never gets a raw pointer to host internals -- it gets these four calls.
 *
 * NOTE ON WRITES: cabbird::mem distinguishes WriteMemory (refuses read-only pages)
 * from PatchMemory (changes protection).  The ABI exposes both, because a plugin
 * that only wants to mutate a data value should never be able to silently rewrite
 * code pages.
 */
#pragma once

#include "cabbird/sdk/base.h"

#define CABBIRD_CORE_SERVICE_V1_ID "cabbird.core"
#define CABBIRD_CORE_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CabbirdCoreLogLevelV1 {
    CABBIRD_CORE_LOG_LEVEL_V1_TRACE = 0,
    CABBIRD_CORE_LOG_LEVEL_V1_INFO = 1,
    CABBIRD_CORE_LOG_LEVEL_V1_WARNING = 2,
    CABBIRD_CORE_LOG_LEVEL_V1_ERROR = 3
} CabbirdCoreLogLevelV1;

typedef struct CabbirdCoreServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;

    void (CABBIRD_CALL *log)(void* user, uint32_t level, CabbirdStringViewV1 message);

    /* Reads `destination.size` bytes from `address`.  Returns
     * CABBIRD_STATUS_V1_UNAVAILABLE when the range is not committed or not readable,
     * rather than faulting. */
    CabbirdStatusV1 (CABBIRD_CALL *read_memory)(
        void* user, uintptr_t address, CabbirdMutableByteSpanV1 destination);

    /* Writes through only if every page in range is already writable.  Use
     * patch_memory when the intent is deliberately to modify code or const data. */
    CabbirdStatusV1 (CABBIRD_CALL *write_memory)(
        void* user, uintptr_t address, CabbirdByteSpanV1 source);

    /* Changes page protection as needed, then writes, then restores.  This is the
     * hook/patch path -- separate on purpose so it can be gated by capability
     * policy and audited independently. */
    CabbirdStatusV1 (CABBIRD_CALL *patch_memory)(
        void* user, uintptr_t address, CabbirdByteSpanV1 source);

    /* Writes the directory this plugin was loaded from into `destination` as a
     * NUL-terminated path, setting *inout_size to the required size (including the
     * terminator).  Returns CABBIRD_STATUS_V1_BUFFER_TOO_SMALL without writing when
     * the buffer is short, so a plugin can size the allocation itself. */
    CabbirdStatusV1 (CABBIRD_CALL *plugin_directory)(
        void* user, char* destination, size_t* inout_size);

    /* Base address of a loaded module by name (e.g. "GameAssembly.dll"), or 0 when
     * it is not loaded.  Unity's runtime and the game's own assembly are separate
     * modules, so plugins routinely need both. */
    uintptr_t (CABBIRD_CALL *module_base)(void* user, CabbirdStringViewV1 module_name);

    /* Convenience: reinterpret the last status returned on this thread.  Both
     * fields are borrowed and valid until the next host call on this thread. */
    CabbirdStatusV1 (CABBIRD_CALL *last_status)(void* user);
} CabbirdCoreServiceV1;

#ifdef __cplusplus
}
#endif
