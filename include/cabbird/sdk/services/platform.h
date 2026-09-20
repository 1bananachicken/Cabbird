#pragma once

#include "cabbird/sdk/base.h"

#define CABBIRD_CONFIG_SERVICE_V1_ID "cabbird.config"
#define CABBIRD_CONFIG_SERVICE_V1_VERSION 1u
#define CABBIRD_STORAGE_SERVICE_V1_ID "cabbird.storage"
#define CABBIRD_STORAGE_SERVICE_V1_VERSION 1u
#define CABBIRD_RUNTIME_INFO_SERVICE_V1_ID "cabbird.runtime-info"
#define CABBIRD_RUNTIME_INFO_SERVICE_V1_VERSION 1u
#define CABBIRD_DIAGNOSTICS_SERVICE_V1_ID "cabbird.diagnostics"
#define CABBIRD_DIAGNOSTICS_SERVICE_V1_VERSION 1u
#define CABBIRD_SCHEDULER_SERVICE_V1_ID "cabbird.scheduler"
#define CABBIRD_SCHEDULER_SERVICE_V1_VERSION 1u
#define CABBIRD_COMMANDS_SERVICE_V1_ID "cabbird.commands"
#define CABBIRD_COMMANDS_SERVICE_V1_VERSION 1u
#define CABBIRD_NOTIFICATIONS_SERVICE_V1_ID "cabbird.notifications"
#define CABBIRD_NOTIFICATIONS_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CabbirdNotificationSeverityV1 {
    CABBIRD_NOTIFICATION_V1_INFO = 0,
    CABBIRD_NOTIFICATION_V1_WARNING = 1,
    CABBIRD_NOTIFICATION_V1_ERROR = 2
} CabbirdNotificationSeverityV1;

typedef struct CabbirdRuntimeInfoV1 {
    uint32_t struct_size;
    uint32_t runtime_version_major;
    uint32_t runtime_version_minor;
    uint32_t runtime_version_patch;
    uint32_t process_id;
    uint32_t thread_id;
    uint64_t uptime_milliseconds;
    uint64_t plugin_generation;
} CabbirdRuntimeInfoV1;

typedef CabbirdStatusV1 (CABBIRD_CALL *CabbirdConfigMigrationV1)(
    void* user,
    uint32_t source_schema_version,
    CabbirdByteSpanV1 source,
    CabbirdMutableByteSpanV1 destination,
    size_t* inout_size);

typedef CabbirdStatusV1 (CABBIRD_CALL *CabbirdDiagnosticSelfTestV1)(
    void* user, CabbirdMutableByteSpanV1 destination, size_t* inout_size);

typedef void (CABBIRD_CALL *CabbirdTaskCallbackV1)(
    void* user, CabbirdGenerationHandleV1 task);

typedef CabbirdStatusV1 (CABBIRD_CALL *CabbirdCommandCallbackV1)(
    void* user,
    CabbirdStringViewV1 arguments,
    CabbirdMutableByteSpanV1 destination,
    size_t* inout_size);

typedef struct CabbirdConfigServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // Register the JSON Schema for one plugin-local document. A plugin must
    // register its stable schema_id in every loaded generation before calling
    // read, write_atomic, or migrate. The returned handle is scope-owned.
    CabbirdStatusV1 (CABBIRD_CALL *register_schema)(
        void* user,
        CabbirdStringViewV1 schema_id,
        uint32_t schema_version,
        CabbirdByteSpanV1 schema_json,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *unregister_schema)(
        void* user, CabbirdGenerationHandleV1 handle);
    // Read the persisted JSON document through the standard two-call buffer
    // protocol. CABBIRD_STATUS_V1_NOT_FOUND means this plugin has no saved
    // document yet; the returned schema_version is the stored version.
    CabbirdStatusV1 (CABBIRD_CALL *read)(
        void* user,
        CabbirdStringViewV1 schema_id,
        uint32_t* schema_version,
        CabbirdMutableByteSpanV1 destination,
        size_t* inout_size);
    // Validate against the registered schema and atomically replace the
    // plugin-local document. The Host owns the durable state location.
    CabbirdStatusV1 (CABBIRD_CALL *write_atomic)(
        void* user,
        CabbirdStringViewV1 schema_id,
        uint32_t schema_version,
        CabbirdByteSpanV1 document);
    CabbirdStatusV1 (CABBIRD_CALL *migrate)(
        void* user,
        CabbirdStringViewV1 schema_id,
        CabbirdConfigMigrationV1 migration,
        void* migration_user);
} CabbirdConfigServiceV1;

typedef struct CabbirdStorageServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *read)(
        void* user,
        CabbirdStringViewV1 relative_path,
        CabbirdMutableByteSpanV1 destination,
        size_t* inout_size);
    CabbirdStatusV1 (CABBIRD_CALL *write_atomic)(
        void* user, CabbirdStringViewV1 relative_path, CabbirdByteSpanV1 source);
    CabbirdStatusV1 (CABBIRD_CALL *remove)(
        void* user, CabbirdStringViewV1 relative_path);
} CabbirdStorageServiceV1;

typedef struct CabbirdRuntimeInfoServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *snapshot)(
        void* user, CabbirdRuntimeInfoV1* snapshot);
    CabbirdStatusV1 (CABBIRD_CALL *runtime_version_utf8)(
        void* user, char* destination, size_t* inout_size);
} CabbirdRuntimeInfoServiceV1;

typedef struct CabbirdDiagnosticsServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *register_self_test)(
        void* user,
        CabbirdStringViewV1 id,
        CabbirdDiagnosticSelfTestV1 callback,
        void* callback_user,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *unregister_self_test)(
        void* user, CabbirdGenerationHandleV1 handle);
    CabbirdStatusV1 (CABBIRD_CALL *run_self_test)(
        void* user,
        CabbirdStringViewV1 id,
        CabbirdMutableByteSpanV1 destination,
        size_t* inout_size);
    CabbirdStatusV1 (CABBIRD_CALL *snapshot_json)(
        void* user, CabbirdMutableByteSpanV1 destination, size_t* inout_size);
} CabbirdDiagnosticsServiceV1;

typedef struct CabbirdSchedulerServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *schedule)(
        void* user,
        uint32_t delay_milliseconds,
        CabbirdTaskCallbackV1 callback,
        void* callback_user,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *cancel)(
        void* user, CabbirdGenerationHandleV1 handle);
} CabbirdSchedulerServiceV1;

typedef struct CabbirdCommandsServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *register_command)(
        void* user,
        CabbirdStringViewV1 name,
        CabbirdStringViewV1 description,
        CabbirdCommandCallbackV1 callback,
        void* callback_user,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *unregister_command)(
        void* user, CabbirdGenerationHandleV1 handle);
    CabbirdStatusV1 (CABBIRD_CALL *invoke)(
        void* user,
        CabbirdStringViewV1 name,
        CabbirdStringViewV1 arguments,
        CabbirdMutableByteSpanV1 destination,
        size_t* inout_size);
} CabbirdCommandsServiceV1;

typedef struct CabbirdNotificationsServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *post)(
        void* user,
        CabbirdNotificationSeverityV1 severity,
        CabbirdStringViewV1 title,
        CabbirdStringViewV1 body,
        uint32_t timeout_milliseconds,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *dismiss)(
        void* user, CabbirdGenerationHandleV1 handle);
} CabbirdNotificationsServiceV1;

#ifdef __cplusplus
}
#endif
