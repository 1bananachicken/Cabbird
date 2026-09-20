#pragma once

#include "cabbird/sdk/base.h"

#define CABBIRD_IPC_SERVICE_V1_ID "cabbird.ipc"
#define CABBIRD_IPC_SERVICE_V1_VERSION 1u
#define CABBIRD_IPC_SCHEMA_HASH_V1_SIZE 32u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CabbirdIpcModeV1 {
    CABBIRD_IPC_MODE_V1_SYNC_REQUEST = 1u << 0u,
    CABBIRD_IPC_MODE_V1_ASYNC_REQUEST = 1u << 1u,
    CABBIRD_IPC_MODE_V1_EVENT = 1u << 2u
} CabbirdIpcModeV1;

typedef enum CabbirdIpcAffinityV1 {
    CABBIRD_IPC_AFFINITY_V1_CALLER = 0,
    CABBIRD_IPC_AFFINITY_V1_WORKER = 1,
    CABBIRD_IPC_AFFINITY_V1_LIFECYCLE = 2,
    CABBIRD_IPC_AFFINITY_V1_GAME = 3,
    CABBIRD_IPC_AFFINITY_V1_RENDER = 4
} CabbirdIpcAffinityV1;

typedef enum CabbirdIpcReentrancyV1 {
    CABBIRD_IPC_REENTRANCY_V1_REJECT = 0,
    CABBIRD_IPC_REENTRANCY_V1_ALLOW = 1
} CabbirdIpcReentrancyV1;

typedef enum CabbirdIpcErrorV1 {
    CABBIRD_IPC_ERROR_V1_NONE = 0,
    CABBIRD_IPC_ERROR_V1_PROVIDER_MISSING = 1,
    CABBIRD_IPC_ERROR_V1_VERSION_MISMATCH = 2,
    CABBIRD_IPC_ERROR_V1_SCHEMA_MISMATCH = 3,
    CABBIRD_IPC_ERROR_V1_MODE_UNAVAILABLE = 4,
    CABBIRD_IPC_ERROR_V1_TIMEOUT = 5,
    CABBIRD_IPC_ERROR_V1_REENTRANT_CYCLE = 6,
    CABBIRD_IPC_ERROR_V1_QUEUE_FULL = 7,
    CABBIRD_IPC_ERROR_V1_STALE_GENERATION = 8,
    CABBIRD_IPC_ERROR_V1_DEPENDENCY_REQUIRED = 9
} CabbirdIpcErrorV1;

typedef struct CabbirdIpcSchemaHashV1 {
    uint8_t bytes[CABBIRD_IPC_SCHEMA_HASH_V1_SIZE];
} CabbirdIpcSchemaHashV1;

typedef struct CabbirdIpcEndpointDescriptorV1 {
    uint32_t struct_size;
    CabbirdStringViewV1 endpoint_id;
    uint32_t major_version;
    uint32_t minor_version;
    CabbirdIpcSchemaHashV1 request_schema;
    CabbirdIpcSchemaHashV1 response_schema;
    CabbirdIpcSchemaHashV1 event_schema;
    uint32_t modes;
    uint32_t affinity;
    uint32_t timeout_milliseconds;
    uint32_t reentrancy;
    uint32_t maximum_request_bytes;
    uint32_t maximum_response_bytes;
    uint32_t maximum_event_bytes;
    uint32_t maximum_queue_depth;
} CabbirdIpcEndpointDescriptorV1;

typedef struct CabbirdIpcEndpointSelectorV1 {
    uint32_t struct_size;
    CabbirdStringViewV1 endpoint_id;
    uint32_t major_version;
    uint32_t minimum_minor_version;
    CabbirdIpcSchemaHashV1 request_schema;
    CabbirdIpcSchemaHashV1 response_schema;
    CabbirdIpcSchemaHashV1 event_schema;
} CabbirdIpcEndpointSelectorV1;

typedef struct CabbirdIpcRequestContextV1 {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t request_id;
    CabbirdStringViewV1 caller_plugin_id;
} CabbirdIpcRequestContextV1;

typedef CabbirdStatusV1 (CABBIRD_CALL *CabbirdIpcRequestHandlerV1)(
    void* user,
    const CabbirdIpcRequestContextV1* context,
    CabbirdByteSpanV1 request,
    CabbirdMutableByteSpanV1 response,
    size_t* response_size);

typedef void (CABBIRD_CALL *CabbirdIpcCompletionCallbackV1)(
    void* user,
    CabbirdGenerationHandleV1 pending_call,
    CabbirdStatusV1 status,
    CabbirdByteSpanV1 response);

typedef void (CABBIRD_CALL *CabbirdIpcEventCallbackV1)(
    void* user,
    CabbirdStringViewV1 endpoint_id,
    CabbirdByteSpanV1 event);

typedef struct CabbirdIpcServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *register_endpoint)(
        void* user,
        const CabbirdIpcEndpointDescriptorV1* descriptor,
        CabbirdIpcRequestHandlerV1 request_handler,
        void* callback_user,
        CabbirdGenerationHandleV1* endpoint);
    CabbirdStatusV1 (CABBIRD_CALL *unregister_endpoint)(
        void* user, CabbirdGenerationHandleV1 endpoint);
    CabbirdStatusV1 (CABBIRD_CALL *invoke)(
        void* user,
        const CabbirdIpcEndpointSelectorV1* selector,
        CabbirdByteSpanV1 request,
        CabbirdMutableByteSpanV1 response,
        size_t* response_size);
    CabbirdStatusV1 (CABBIRD_CALL *invoke_async)(
        void* user,
        const CabbirdIpcEndpointSelectorV1* selector,
        CabbirdByteSpanV1 request,
        CabbirdIpcCompletionCallbackV1 completion,
        void* completion_user,
        CabbirdGenerationHandleV1* pending_call);
    CabbirdStatusV1 (CABBIRD_CALL *cancel)(
        void* user, CabbirdGenerationHandleV1 pending_call);
    CabbirdStatusV1 (CABBIRD_CALL *subscribe)(
        void* user,
        const CabbirdIpcEndpointSelectorV1* selector,
        CabbirdIpcEventCallbackV1 callback,
        void* callback_user,
        CabbirdGenerationHandleV1* subscription);
    CabbirdStatusV1 (CABBIRD_CALL *unsubscribe)(
        void* user, CabbirdGenerationHandleV1 subscription);
    CabbirdStatusV1 (CABBIRD_CALL *publish)(
        void* user,
        CabbirdGenerationHandleV1 endpoint,
        CabbirdByteSpanV1 event);
} CabbirdIpcServiceV1;

#ifdef __cplusplus
}
#endif
