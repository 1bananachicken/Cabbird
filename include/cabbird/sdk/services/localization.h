#pragma once

#include "cabbird/sdk/base.h"

#define CABBIRD_LOCALIZATION_SERVICE_V1_ID "cabbird.localization"
#define CABBIRD_LOCALIZATION_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CabbirdLocalizationServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *locale)(
        void* user, char* destination, size_t* inout_size);
    CabbirdStatusV1 (CABBIRD_CALL *translate)(
        void* user,
        CabbirdStringViewV1 key,
        CabbirdStringViewV1 english_fallback,
        const CabbirdStringViewV1* arguments,
        size_t argument_count,
        char* destination,
        size_t* inout_size);
} CabbirdLocalizationServiceV1;

#ifdef __cplusplus
}
#endif
