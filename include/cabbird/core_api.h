#pragma once

#include <Windows.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CABBIRD_BOOTSTRAP_ABI_VERSION 1u
#define CABBIRD_RUNTIME_STATE_INFO_VERSION 1u

#define CABBIRD_CORE_START_ENTRY "CabbirdStart"
#define CABBIRD_CORE_MANUAL_MAP_CXX_THROW_ENTRY "CabbirdManualMapCxxThrow"
#define CABBIRD_CORE_REQUEST_STOP_ENTRY "CabbirdRequestStop"
#define CABBIRD_CORE_GET_STATE_ENTRY "CabbirdGetState"
#define CABBIRD_CORE_WAIT_FOR_STOP_ENTRY "CabbirdWaitForStop"

typedef uint32_t CabbirdBootstrapType;
enum {
    CABBIRD_BOOTSTRAP_TYPE_UNKNOWN = 0u,
    CABBIRD_BOOTSTRAP_TYPE_DWMAPI_PROXY = 1u,
    CABBIRD_BOOTSTRAP_TYPE_EXTERNAL = 2u
};

typedef uint32_t CabbirdRuntimeState;
enum {
    CABBIRD_RUNTIME_STATE_DORMANT = 0u,
    CABBIRD_RUNTIME_STATE_BOOTSTRAPPING = 1u,
    CABBIRD_RUNTIME_STATE_STARTING_BLOCKING_SERVICES = 2u,
    CABBIRD_RUNTIME_STATE_STARTING_ASYNC_SERVICES = 3u,
    CABBIRD_RUNTIME_STATE_AWAITING_GAME_READINESS = 4u,
    CABBIRD_RUNTIME_STATE_RUNNING = 5u,
    CABBIRD_RUNTIME_STATE_STOP_REQUESTED = 6u,
    CABBIRD_RUNTIME_STATE_STOPPING_PLUGINS = 7u,
    CABBIRD_RUNTIME_STATE_STOPPING_SERVICES = 8u,
    CABBIRD_RUNTIME_STATE_FAILED = 9u,
    CABBIRD_RUNTIME_STATE_STOPPED = 10u
};

typedef struct CabbirdStartInfo {
    uint32_t struct_size;
    uint32_t bootstrap_abi_version;
    CabbirdBootstrapType bootstrap_type;
    uint32_t flags;
    HMODULE bootstrap_module;
    HMODULE game_module;
    const WCHAR* runtime_root;
    const WCHAR* log_directory;
    HANDLE external_stop_event;
} CabbirdStartInfo;

#define CABBIRD_START_INFO_V1_SIZE 56u

typedef struct CabbirdRuntimeStateInfo {
    uint32_t struct_size;
    uint32_t state_info_version;
    CabbirdRuntimeState state;
    uint32_t last_error;
    uint64_t session_generation;
} CabbirdRuntimeStateInfo;

#define CABBIRD_RUNTIME_STATE_INFO_V1_SIZE 24u

typedef DWORD(WINAPI* CabbirdStartFn)(const CabbirdStartInfo* start_info);
typedef DWORD(WINAPI* CabbirdRequestStopFn)(void);
typedef DWORD(WINAPI* CabbirdGetStateFn)(CabbirdRuntimeStateInfo* state_info);
typedef DWORD(WINAPI* CabbirdWaitForStopFn)(DWORD timeout_ms);

#ifdef __cplusplus
}
#endif
