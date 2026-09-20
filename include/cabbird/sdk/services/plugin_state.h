#pragma once
#include "cabbird/sdk/base.h"
#define CABBIRD_PLUGIN_STATE_SERVICE_V1_ID "cabbird.plugin-state"
#define CABBIRD_PLUGIN_STATE_SERVICE_V1_VERSION 1u
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CabbirdPluginStateServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *directory)(void* user, char* destination, size_t* inout_size);
} CabbirdPluginStateServiceV1;
#ifdef __cplusplus
}
#endif
