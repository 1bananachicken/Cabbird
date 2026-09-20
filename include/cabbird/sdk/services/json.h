#pragma once
#include "cabbird/sdk/base.h"

#define CABBIRD_JSON_SERVICE_V1_ID "cabbird.json"
#define CABBIRD_JSON_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CabbirdJsonKindV1 {
    CABBIRD_JSON_V1_NULL = 0,
    CABBIRD_JSON_V1_BOOLEAN = 1,
    CABBIRD_JSON_V1_NUMBER = 2,
    CABBIRD_JSON_V1_STRING = 3,
    CABBIRD_JSON_V1_ARRAY = 4,
    CABBIRD_JSON_V1_OBJECT = 5
} CabbirdJsonKindV1;

// A lightweight, plugin-scoped DOM over nlohmann/json. Every handle returned by
// parse, array_item, and object_find is owned by the plugin scope and must be
// released before the DLL is unloaded. Handle tokens are invalidated when the
// owning plugin generation stops.
typedef struct CabbirdJsonServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *parse)(
        void* user, CabbirdStringViewV1 document,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *release)(
        void* user, CabbirdGenerationHandleV1 handle);
    CabbirdStatusV1 (CABBIRD_CALL *kind)(
        void* user, CabbirdGenerationHandleV1 handle, uint32_t* kind);
    CabbirdStatusV1 (CABBIRD_CALL *boolean_value)(
        void* user, CabbirdGenerationHandleV1 handle, int32_t* value);
    CabbirdStatusV1 (CABBIRD_CALL *number_value)(
        void* user, CabbirdGenerationHandleV1 handle, double* value);
    CabbirdStatusV1 (CABBIRD_CALL *string_value)(
        void* user, CabbirdGenerationHandleV1 handle,
        char* destination, size_t* inout_size);
    CabbirdStatusV1 (CABBIRD_CALL *array_size)(
        void* user, CabbirdGenerationHandleV1 handle, size_t* size);
    CabbirdStatusV1 (CABBIRD_CALL *array_item)(
        void* user, CabbirdGenerationHandleV1 handle, size_t index,
        CabbirdGenerationHandleV1* child);
    CabbirdStatusV1 (CABBIRD_CALL *object_size)(
        void* user, CabbirdGenerationHandleV1 handle, size_t* size);
    CabbirdStatusV1 (CABBIRD_CALL *object_key_at)(
        void* user, CabbirdGenerationHandleV1 handle, size_t index,
        char* destination, size_t* inout_size);
    CabbirdStatusV1 (CABBIRD_CALL *object_find)(
        void* user, CabbirdGenerationHandleV1 handle, CabbirdStringViewV1 key,
        CabbirdGenerationHandleV1* child);
    CabbirdStatusV1 (CABBIRD_CALL *serialize)(
        void* user, CabbirdGenerationHandleV1 handle,
        char* destination, size_t* inout_size);
} CabbirdJsonServiceV1;

#ifdef __cplusplus
}
#endif
