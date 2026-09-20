#pragma once
#include "cabbird/sdk/base.h"
#include <stdbool.h>

#define CABBIRD_IL2CPP_SERVICE_V1_ID "cabbird.unity.il2cpp"
#define CABBIRD_IL2CPP_SERVICE_V1_VERSION 1u

/* Opaque identities only; no engine structure layouts are public. C++ aliases
 * share the host runtime facade's existing opaque types, not its implementation. */
#ifdef __cplusplus
namespace cabbird::il2cpp {
struct Il2CppObject;
struct Il2CppClass;
struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppType;
struct FieldInfo;
struct MethodInfo;
}
typedef cabbird::il2cpp::Il2CppObject CabbirdIl2CppObjectV1;
typedef cabbird::il2cpp::Il2CppClass CabbirdIl2CppClassV1;
typedef cabbird::il2cpp::Il2CppDomain CabbirdIl2CppDomainV1;
typedef cabbird::il2cpp::Il2CppAssembly CabbirdIl2CppAssemblyV1;
typedef cabbird::il2cpp::Il2CppImage CabbirdIl2CppImageV1;
typedef cabbird::il2cpp::Il2CppType CabbirdIl2CppTypeV1;
typedef cabbird::il2cpp::FieldInfo CabbirdFieldInfoV1;
typedef cabbird::il2cpp::MethodInfo CabbirdMethodInfoV1;
typedef char16_t CabbirdIl2CppCharV1;
extern "C" {
#else
typedef struct CabbirdIl2CppObjectV1 CabbirdIl2CppObjectV1;
typedef struct CabbirdIl2CppClassV1 CabbirdIl2CppClassV1;
typedef struct CabbirdIl2CppDomainV1 CabbirdIl2CppDomainV1;
typedef struct CabbirdIl2CppAssemblyV1 CabbirdIl2CppAssemblyV1;
typedef struct CabbirdIl2CppImageV1 CabbirdIl2CppImageV1;
typedef struct CabbirdIl2CppTypeV1 CabbirdIl2CppTypeV1;
typedef struct CabbirdFieldInfoV1 CabbirdFieldInfoV1;
typedef struct CabbirdMethodInfoV1 CabbirdMethodInfoV1;
typedef uint16_t CabbirdIl2CppCharV1;
#endif
typedef void* CabbirdIl2CppGCHandleV1;

/* Borrowed only for the synchronous with_runtime callback. Never retain the
 * table or invoke its entries from render/lifecycle threads. Object references
 * kept after the callback require a GC handle. Metadata handles are runtime-owned.
 * This is an invocation capability, NOT a read-only grant or a native sandbox. */
typedef struct CabbirdIl2CppApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    CabbirdIl2CppDomainV1* (CABBIRD_CALL *il2cpp_domain_get)();
    const CabbirdIl2CppAssemblyV1** (CABBIRD_CALL *il2cpp_domain_get_assemblies)(const CabbirdIl2CppDomainV1*, size_t*);
    CabbirdIl2CppImageV1* (CABBIRD_CALL *il2cpp_assembly_get_image)(const CabbirdIl2CppAssemblyV1*);
    const char* (CABBIRD_CALL *il2cpp_image_get_name)(const CabbirdIl2CppImageV1*);
    CabbirdIl2CppClassV1* (CABBIRD_CALL *il2cpp_class_from_name)(const CabbirdIl2CppImageV1*, const char*, const char*);
    CabbirdIl2CppClassV1* (CABBIRD_CALL *il2cpp_class_get_parent)(CabbirdIl2CppClassV1*);
    CabbirdIl2CppTypeV1* (CABBIRD_CALL *il2cpp_class_get_type)(CabbirdIl2CppClassV1*);
    CabbirdFieldInfoV1* (CABBIRD_CALL *il2cpp_class_get_field_from_name)(CabbirdIl2CppClassV1*, const char*);
    const CabbirdMethodInfoV1* (CABBIRD_CALL *il2cpp_class_get_method_from_name)(CabbirdIl2CppClassV1*, const char*, int);
    bool (CABBIRD_CALL *il2cpp_class_is_assignable_from)(CabbirdIl2CppClassV1*, CabbirdIl2CppClassV1*);
    CabbirdIl2CppTypeV1* (CABBIRD_CALL *il2cpp_field_get_type)(CabbirdFieldInfoV1*);
    void (CABBIRD_CALL *il2cpp_field_get_value)(CabbirdIl2CppObjectV1*, CabbirdFieldInfoV1*, void*);
    const CabbirdIl2CppTypeV1* (CABBIRD_CALL *il2cpp_method_get_param)(const CabbirdMethodInfoV1*, uint32_t);
    const char* (CABBIRD_CALL *il2cpp_type_get_name)(const CabbirdIl2CppTypeV1*);
    CabbirdIl2CppObjectV1* (CABBIRD_CALL *il2cpp_type_get_object)(const CabbirdIl2CppTypeV1*);
    CabbirdIl2CppClassV1* (CABBIRD_CALL *il2cpp_object_get_class)(CabbirdIl2CppObjectV1*);
    CabbirdIl2CppObjectV1* (CABBIRD_CALL *il2cpp_runtime_invoke)(const CabbirdMethodInfoV1*, void*, void**, CabbirdIl2CppObjectV1**);
    void* (CABBIRD_CALL *il2cpp_object_unbox)(CabbirdIl2CppObjectV1*);
    CabbirdIl2CppObjectV1* (CABBIRD_CALL *il2cpp_string_new)(const char*);
    int32_t (CABBIRD_CALL *il2cpp_string_length)(CabbirdIl2CppObjectV1*);
    CabbirdIl2CppCharV1* (CABBIRD_CALL *il2cpp_string_chars)(CabbirdIl2CppObjectV1*);
    uint32_t (CABBIRD_CALL *il2cpp_array_length)(CabbirdIl2CppObjectV1*);
    int32_t (CABBIRD_CALL *il2cpp_array_object_header_size)();
    CabbirdIl2CppGCHandleV1 (CABBIRD_CALL *il2cpp_gchandle_new)(CabbirdIl2CppObjectV1*, bool);
    CabbirdIl2CppObjectV1* (CABBIRD_CALL *il2cpp_gchandle_get_target)(CabbirdIl2CppGCHandleV1);
    void (CABBIRD_CALL *il2cpp_gchandle_free)(CabbirdIl2CppGCHandleV1);
    void (CABBIRD_CALL *il2cpp_free)(void*);
    const CabbirdMethodInfoV1* (CABBIRD_CALL *il2cpp_class_get_methods)(CabbirdIl2CppClassV1*, void**);
    const char* (CABBIRD_CALL *il2cpp_method_get_name)(const CabbirdMethodInfoV1*);
    uint32_t (CABBIRD_CALL *il2cpp_method_get_param_count)(const CabbirdMethodInfoV1*);
} CabbirdIl2CppApiV1;

typedef CabbirdStatusV1 (CABBIRD_CALL *CabbirdIl2CppCallbackV1)(
    void* user, const CabbirdIl2CppApiV1* api);

typedef struct CabbirdIl2CppServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    /* Synchronous, only from on_update on the observed game thread. The host
     * reuses cabbird::il2cpp initialization/ThreadScope. Never dispatches or waits. */
    CabbirdStatusV1 (CABBIRD_CALL *with_runtime)(
        void* user, CabbirdIl2CppCallbackV1 callback, void* callback_user);
    /* GC cleanup only; usable in on_stop after callbacks have drained. The caller
     * supplies valid, distinct handles it owns and clears them after success.
     * No Unity methods are invoked here. */
    CabbirdStatusV1 (CABBIRD_CALL *release_handles)(
        void* user, const CabbirdIl2CppGCHandleV1* handles, size_t count);
} CabbirdIl2CppServiceV1;
#ifdef __cplusplus
}
#endif
