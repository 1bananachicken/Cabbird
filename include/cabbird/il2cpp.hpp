// IL2CPP runtime facade.
//
// WHY THIS EXISTS, AND WHY IT IS NOT A PATTERN SCANNER
// ---------------------------------------------------
// A UE5 target forces you to pattern-scan: GObjects / GNames / GWorld are not
// exported, so you locate them by signature and hope the build does not change.
//
// Unity/IL2CPP is the opposite situation.  `GameAssembly.dll` on this target
// exports 241 `il2cpp_*` functions, and they are a *supported C API* -- the same
// one MelonLoader/BepInEx use.  So symbol resolution goes through the export
// table, and `cabbird::mem::ScanExecutableSections` stays as the fallback for the
// few things the runtime does not export (e.g. `MethodInfo::methodPointer`).
//
// Two consequences drive the whole design:
//
//   1. We never read `global-metadata.dat`.  On this target it is encrypted
//      (sanity is 0x1357FEDA, not 0xFAB11BAF, and the magic appears nowhere in
//      the 48 MB file), but that does not matter: the runtime has already
//      decrypted it, and `il2cpp_class_from_name` / `il2cpp_class_get_fields`
//      walk the live tables for us.
//
//   2. We never hardcode the object model.  `Il2CppString::length` and the
//      array length slot move between Unity versions, so they are *queried*:
//      `il2cpp_offset_of_array_length_in_array_object_header()` and friends
//      return the real offsets for the runtime we are living inside.  Hardcoded
//      0x10/0x18/0x20 constants are how these projects rot.
//
// THREADING (the one thing that actually crashes people)
// -----------------------------------------------------
// IL2CPP objects belong to a domain.  A thread that the runtime does not know
// about must call `il2cpp_thread_attach` before it touches a managed object or
// the GC will walk a thread state that does not exist and tear the process
// down.  Our overlay creates its own worker threads, so every one of them needs
// a `ThreadScope`.  `ThreadScope` is also reference-counted, because attaching
// the same thread twice is allowed but detaching it once too early is not.
#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cabbird::il2cpp {

// ---------------------------------------------------------------------------
// Opaque runtime types.  We only ever hold pointers to these; the layout of
// most of them is deliberately unknown here, because anything we need to know
// is available through a function call instead of an offset guess.
// ---------------------------------------------------------------------------

struct Il2CppClass;
struct Il2CppImage;
struct Il2CppAssembly;
struct Il2CppDomain;
struct Il2CppThread;

// Spellings taken from Unity's il2cpp-api-types.h so that the Api table below
// can be compared against Unity's own il2cpp-api-functions.h mechanically
// instead of relying on anyone's memory.
using Il2CppChar = char16_t;
using Il2CppGCHandle = void*;
using Il2CppMethodPointer = void (*)();
struct Il2CppType;
struct FieldInfo;
struct MethodInfo;
struct PropertyInfo;
struct EventInfo;
struct Il2CppObject;

// The two layouts that are stable enough to model directly, and even these are
// only used for the header part -- `chars`/`vector` are reached via the queried
// offsets in `Layout`.
struct Il2CppString {
    Il2CppObject* object;  // klass + monitor
    std::int32_t length;
    char16_t chars[1];
};

// ---------------------------------------------------------------------------
// The resolved API.  Field names are the export names verbatim so that
// resolution is a single macro and stays self-documenting.
// ---------------------------------------------------------------------------

struct Api {
    HMODULE module{};

    // -- domain / image / assembly ------------------------------------------
    Il2CppDomain* (*il2cpp_domain_get)();
    const Il2CppAssembly** (*il2cpp_domain_get_assemblies)(const Il2CppDomain*, std::size_t*);
    Il2CppImage* (*il2cpp_assembly_get_image)(const Il2CppAssembly*);
    const char* (*il2cpp_image_get_name)(const Il2CppImage*);
    const char* (*il2cpp_image_get_filename)(const Il2CppImage*);
    std::size_t (*il2cpp_image_get_class_count)(const Il2CppImage*);
    Il2CppClass* (*il2cpp_image_get_class)(const Il2CppImage*, std::size_t);

    // -- class --------------------------------------------------------------
    Il2CppClass* (*il2cpp_class_from_name)(const Il2CppImage*, const char*, const char*);
    const char* (*il2cpp_class_get_name)(Il2CppClass*);
    const char* (*il2cpp_class_get_namespace)(Il2CppClass*);
    Il2CppClass* (*il2cpp_class_get_parent)(Il2CppClass*);
    Il2CppImage* (*il2cpp_class_get_image)(Il2CppClass*);
    Il2CppType* (*il2cpp_class_get_type)(Il2CppClass*);
    std::int32_t (*il2cpp_class_instance_size)(Il2CppClass*);
    std::int32_t (*il2cpp_class_value_size)(Il2CppClass*, std::uint32_t*);    bool (*il2cpp_class_is_valuetype)(const Il2CppClass*);
    bool (*il2cpp_class_is_enum)(const Il2CppClass*);
    bool (*il2cpp_class_is_interface)(const Il2CppClass*);
    bool (*il2cpp_class_is_abstract)(const Il2CppClass*);
    bool (*il2cpp_class_is_generic)(const Il2CppClass*);
    bool (*il2cpp_class_is_inflated)(const Il2CppClass*);
    bool (*il2cpp_class_is_subclass_of)(Il2CppClass*, Il2CppClass*, bool);
    bool (*il2cpp_class_is_assignable_from)(Il2CppClass*, Il2CppClass*);
    std::uint32_t (*il2cpp_class_get_flags)(const Il2CppClass*);
    void* (*il2cpp_class_get_static_field_data)(Il2CppClass*);
    const std::uint8_t* (*il2cpp_class_get_bitmap)(Il2CppClass*, std::size_t*);
    const MethodInfo* (*il2cpp_class_get_methods)(Il2CppClass*, void**);
    FieldInfo* (*il2cpp_class_get_fields)(Il2CppClass*, void**);
    FieldInfo* (*il2cpp_class_get_field_from_name)(Il2CppClass*, const char*);
    const MethodInfo* (*il2cpp_class_get_method_from_name)(Il2CppClass*, const char*, int);
    const PropertyInfo* (*il2cpp_class_get_properties)(Il2CppClass*, void**);
    const EventInfo* (*il2cpp_class_get_events)(Il2CppClass*, void**);
    Il2CppClass* (*il2cpp_class_get_interfaces)(Il2CppClass*, void**);
    Il2CppClass* (*il2cpp_class_get_nested_types)(Il2CppClass*, void**);
    Il2CppClass* (*il2cpp_class_get_declaring_type)(Il2CppClass*);
    Il2CppClass* (*il2cpp_class_get_element_class)(Il2CppClass*);
    int (*il2cpp_class_get_rank)(const Il2CppClass*);
    std::size_t (*il2cpp_class_num_fields)(const Il2CppClass*);
    Il2CppClass* (*il2cpp_class_from_type)(const Il2CppType*);

    // -- field --------------------------------------------------------------
    const char* (*il2cpp_field_get_name)(FieldInfo*);
    std::size_t (*il2cpp_field_get_offset)(FieldInfo*);
    Il2CppType* (*il2cpp_field_get_type)(FieldInfo*);
    Il2CppClass* (*il2cpp_field_get_parent)(FieldInfo*);
    std::uint32_t (*il2cpp_field_get_flags)(FieldInfo*);
    bool (*il2cpp_field_is_literal)(FieldInfo*);
    void (*il2cpp_field_get_value)(Il2CppObject*, FieldInfo*, void*);
    void (*il2cpp_field_set_value)(Il2CppObject*, FieldInfo*, void*);
    void (*il2cpp_field_static_get_value)(FieldInfo*, void*);
    void (*il2cpp_field_static_set_value)(FieldInfo*, void*);
    Il2CppObject* (*il2cpp_field_get_value_object)(FieldInfo*, Il2CppObject*);

    // -- method -------------------------------------------------------------
    const char* (*il2cpp_method_get_name)(const MethodInfo*);
    std::uint32_t (*il2cpp_method_get_param_count)(const MethodInfo*);
    const Il2CppType* (*il2cpp_method_get_param)(const MethodInfo*, std::uint32_t);
    const Il2CppType* (*il2cpp_method_get_return_type)(const MethodInfo*);
    Il2CppClass* (*il2cpp_method_get_class)(const MethodInfo*);
    std::uint32_t (*il2cpp_method_get_flags)(const MethodInfo*, std::uint32_t*);
    std::uint32_t (*il2cpp_method_get_token)(const MethodInfo*);

    // -- property -----------------------------------------------------------
    const char* (*il2cpp_property_get_name)(const PropertyInfo*);
    const MethodInfo* (*il2cpp_property_get_get_method)(const PropertyInfo*);
    const MethodInfo* (*il2cpp_property_get_set_method)(const PropertyInfo*);
    Il2CppObject* (*il2cpp_runtime_invoke)(const MethodInfo*, void*, void**, Il2CppObject**);

    // -- type ---------------------------------------------------------------
    const char* (*il2cpp_type_get_name)(const Il2CppType*);
    Il2CppClass* (*il2cpp_type_get_class_or_element_class)(const Il2CppType*);
    int (*il2cpp_type_get_type)(const Il2CppType*);
    Il2CppObject* (*il2cpp_type_get_object)(const Il2CppType*);

    // -- object / string / array -------------------------------------------
    Il2CppObject* (*il2cpp_object_new)(Il2CppClass*);
    Il2CppClass* (*il2cpp_object_get_class)(Il2CppObject*);
    std::uint32_t (*il2cpp_object_get_size)(Il2CppObject*);
    void* (*il2cpp_object_unbox)(Il2CppObject*);
    Il2CppObject* (*il2cpp_value_box)(Il2CppClass*, void*);
    Il2CppObject* (*il2cpp_string_new)(const char*);
    Il2CppObject* (*il2cpp_string_new_utf16)(const Il2CppChar*, std::int32_t);
    std::int32_t (*il2cpp_string_length)(Il2CppObject*);
    Il2CppChar* (*il2cpp_string_chars)(Il2CppObject*);
    Il2CppObject* (*il2cpp_array_new)(Il2CppClass*, std::size_t);
    // uint32_t, NOT size_t: declaring a 64-bit return for a 32-bit function
    // leaves the upper half of RAX undefined, so array lengths come back huge.
    std::uint32_t (*il2cpp_array_length)(Il2CppObject*);
    Il2CppClass* (*il2cpp_array_class_get)(Il2CppClass*, std::uint32_t);
    int (*il2cpp_array_element_size)(const Il2CppClass*);

    // -- threads / GC -------------------------------------------------------
    // Signatures here were wrong twice and both times it crashed the game rather
    // than failing loudly.  Every one of them is checked against Unity's own
    // il2cpp-api-functions.h -- re-check that file after touching this block.
    //
    //   attach: returns the THREAD, not the domain that was passed in.  The
    //           thread object is what detach needs.
    //   detach: takes the THREAD.  Passing the domain here made
    //           Thread::Detach() unregister the wrong thing, and the real game
    //           died with "Fatal error in GC: Collecting from unknown thread".
    Il2CppThread* (*il2cpp_thread_attach)(Il2CppDomain*);
    void (*il2cpp_thread_detach)(Il2CppThread*);
    Il2CppThread* (*il2cpp_thread_current)();
    // Also takes the thread OBJECT and returns the answer; it is not an
    // out-parameter function.
    bool (*il2cpp_is_vm_thread)(Il2CppThread*);
    void (*il2cpp_gc_disable)();
    void (*il2cpp_gc_enable)();
    bool (*il2cpp_gc_is_disabled)();

    // -- gc handles ---------------------------------------------------------
    Il2CppGCHandle (*il2cpp_gchandle_new)(Il2CppObject*, bool);
    Il2CppObject* (*il2cpp_gchandle_get_target)(Il2CppGCHandle);
    void (*il2cpp_gchandle_free)(Il2CppGCHandle);

    // -- misc ---------------------------------------------------------------
    Il2CppMethodPointer (*il2cpp_resolve_icall)(const char*);
    void (*il2cpp_free)(void*);
    // Returns the corlib IMAGE, not a class.  It was declared as Il2CppClass*
    // here, and DescribeRuntime() fed the result straight to
    // il2cpp_class_get_name -- i.e. it read an Il2CppImage as an Il2CppClass.
    Il2CppImage* (*il2cpp_get_corlib)();

    // -- layout queries: the reason we do not hardcode offsets --------------
    std::int32_t (*il2cpp_object_header_size)();
    std::int32_t (*il2cpp_array_object_header_size)();
    std::uint32_t (*il2cpp_offset_of_array_length_in_array_object_header)();
    std::uint32_t (*il2cpp_offset_of_array_bounds_in_array_object_header)();
};

// Runtime-provided layout constants, filled in by Initialize().
struct Layout {
    std::size_t object_header_size{};
    std::size_t array_object_header_size{};
    std::size_t array_length_offset{};
    std::size_t array_bounds_offset{};
    // Derived: where element 0 of a reference-type array starts.
    [[nodiscard]] std::size_t ArrayElementBase() const {
        return array_object_header_size != 0 ? array_object_header_size
                                             : array_length_offset + sizeof(std::int32_t);
    }
};

// Locates `GameAssembly.dll` in the current process and resolves the API.
// Safe to call more than once; the first successful call wins.
// `assembly_name` exists for games that rename the binary.
bool Initialize(std::wstring_view assembly_name = L"GameAssembly.dll");

[[nodiscard]] bool Ready();
[[nodiscard]] const Api& Functions();
[[nodiscard]] const Layout& Constants();
[[nodiscard]] const std::wstring& AssemblyPath();

// Human-readable reason for the last Initialize() failure.
[[nodiscard]] const std::string& LastError();

// ---------------------------------------------------------------------------
// RAII thread attachment.
//
// Reference-counted per thread: IL2CPP detaches a thread the moment
// `il2cpp_thread_detach` is called once, so nesting two scopes must not detach
// on the inner exit.  A thread that the runtime already knows (a Unity main
// thread, say) is left alone -- detaching it would be fatal.
// ---------------------------------------------------------------------------

class ThreadScope {
public:
    ThreadScope();
    ~ThreadScope();

    ThreadScope(const ThreadScope&) = delete;
    ThreadScope& operator=(const ThreadScope&) = delete;

    [[nodiscard]] bool attached() const noexcept { return attached_; }

private:
    bool attached_{};
    bool owns_{};
    // Opaque pointer to this thread's FLS bookkeeping block.  Kept as void* so
    // the header does not have to expose the FLS implementation, and so it is
    // obvious that nothing here may become a `thread_local` -- see the comment
    // in il2cpp.cpp for why that would break manual mapping.
    void* thread_state_{};
};

// ---------------------------------------------------------------------------
// Resolution helpers.  These are the "slow path": they walk runtime tables.
// Once a FieldInfo/offset is known, use cabbird::mem for the hot path.
// ---------------------------------------------------------------------------

[[nodiscard]] Il2CppImage* FindImage(std::string_view assembly_name);
[[nodiscard]] Il2CppClass* FindClass(
    std::string_view assembly_name,
    std::string_view namespaze,
    std::string_view name);

[[nodiscard]] std::string ClassName(const Il2CppClass* klass);
[[nodiscard]] std::string TypeName(const Il2CppType* type);

// Resolves a field's byte offset, walking base classes.
[[nodiscard]] std::optional<std::size_t> FieldOffset(
    Il2CppClass* klass,
    std::string_view field_name);
// Absolute address of a static field's storage (class static data + offset).
// Returns nullopt for instance fields, or before the class is initialised.
[[nodiscard]] std::optional<std::uintptr_t> StaticFieldAddress(
    Il2CppClass* klass,
    std::string_view field_name);
[[nodiscard]] FieldInfo* FindField(Il2CppClass* klass, std::string_view field_name);

// Reads a managed string into UTF-8.  Uses il2cpp_string_length/chars, so it
// does not depend on our struct layout guess.
[[nodiscard]] std::optional<std::string> ReadString(const Il2CppObject* string_object);

// Reads a managed string directly from an object's field at `offset`.
[[nodiscard]] std::optional<std::string> ReadStringField(
    const Il2CppObject* object,
    std::size_t offset);

// Dumps a one-line summary of the live type system into `out`, used by the
// in-game self-check so the next launch proves this layer works.
[[nodiscard]] std::string DescribeRuntime();

}  // namespace cabbird::il2cpp
