#pragma once

#include "cabbird/sdk/base.h"

#define CABBIRD_SIGNATURE_SERVICE_V1_ID "cabbird.interop.signature"
#define CABBIRD_SIGNATURE_SERVICE_V1_VERSION 1u
#define CABBIRD_HOOK_SERVICE_V1_ID "cabbird.interop.hook"
#define CABBIRD_HOOK_SERVICE_V1_VERSION 1u
#define CABBIRD_PATCH_SERVICE_V1_ID "cabbird.interop.patch"
#define CABBIRD_PATCH_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CabbirdHookKindV1 {
    CABBIRD_HOOK_V1_FUNCTION = 1,
    CABBIRD_HOOK_V1_IAT = 2,
    CABBIRD_HOOK_V1_EXPORT = 3,
    CABBIRD_HOOK_V1_VTABLE = 4
} CabbirdHookKindV1;

typedef struct CabbirdHookRequestV1 {
    uint32_t struct_size;
    uint32_t kind;
    uintptr_t target;
    void* detour;
    CabbirdStringViewV1 label;
} CabbirdHookRequestV1;

typedef struct CabbirdSignatureServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *resolve)(
        void* user,
        CabbirdStringViewV1 module_name,
        CabbirdStringViewV1 section_name,
        CabbirdStringViewV1 pattern,
        uintptr_t* address);
} CabbirdSignatureServiceV1;

typedef struct CabbirdHookServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *create)(
        void* user,
        const CabbirdHookRequestV1* request,
        uintptr_t* original,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *release)(
        void* user, CabbirdGenerationHandleV1 handle);
    CabbirdStatusV1 (CABBIRD_CALL *begin_callback)(
        void* user,
        CabbirdGenerationHandleV1 hook,
        CabbirdGenerationHandleV1* callback_lease);
    CabbirdStatusV1 (CABBIRD_CALL *end_callback)(
        void* user, CabbirdGenerationHandleV1 callback_lease);
} CabbirdHookServiceV1;

typedef struct CabbirdPatchServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *apply)(
        void* user,
        uintptr_t address,
        CabbirdByteSpanV1 replacement,
        CabbirdStringViewV1 label,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *release)(
        void* user, CabbirdGenerationHandleV1 handle);
} CabbirdPatchServiceV1;

#ifdef __cplusplus
}
#endif
