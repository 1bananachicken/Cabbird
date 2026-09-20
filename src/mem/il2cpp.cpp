#include "cabbird/il2cpp.hpp"

#include "cabbird/memory.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

namespace cabbird::il2cpp {
namespace {

Api g_api{};
Layout g_layout{};
std::wstring g_assembly_path;
std::string g_last_error;
std::mutex g_init_mutex;
bool g_ready{};

// Resolve one export into a like-named Api member.  The field names are the
// export names verbatim precisely so this stays a one-liner.
#define CABBIRD_RESOLVE_IL2CPP(field)                                                        \
    do {                                                                                 \
        g_api.field = reinterpret_cast<decltype(Api::field)>(                            \
            ::GetProcAddress(g_api.module, #field));                                     \
        if (g_api.field == nullptr) {                                                    \
            missing.emplace_back(#field);                                                \
        }                                                                                \
    } while (false)

void ResolveAll() {
    std::vector<const char*> missing;

    // domain / image / assembly
    CABBIRD_RESOLVE_IL2CPP(il2cpp_domain_get);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_domain_get_assemblies);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_assembly_get_image);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_image_get_name);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_image_get_filename);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_image_get_class_count);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_image_get_class);
    // class
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_from_name);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_name);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_namespace);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_parent);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_image);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_type);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_instance_size);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_value_size);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_is_valuetype);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_is_enum);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_is_interface);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_is_abstract);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_is_generic);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_is_inflated);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_is_subclass_of);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_is_assignable_from);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_flags);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_static_field_data);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_bitmap);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_methods);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_fields);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_field_from_name);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_method_from_name);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_properties);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_events);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_interfaces);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_nested_types);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_declaring_type);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_element_class);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_get_rank);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_num_fields);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_class_from_type);
    // field
    CABBIRD_RESOLVE_IL2CPP(il2cpp_field_get_name);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_field_get_offset);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_field_get_type);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_field_get_parent);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_field_get_flags);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_field_is_literal);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_field_get_value);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_field_set_value);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_field_static_get_value);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_field_static_set_value);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_field_get_value_object);
    // method
    CABBIRD_RESOLVE_IL2CPP(il2cpp_method_get_name);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_method_get_param_count);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_method_get_param);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_method_get_return_type);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_method_get_class);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_method_get_flags);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_method_get_token);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_property_get_name);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_property_get_get_method);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_property_get_set_method);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_runtime_invoke);
    // type
    CABBIRD_RESOLVE_IL2CPP(il2cpp_type_get_name);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_type_get_class_or_element_class);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_type_get_type);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_type_get_object);
    // object / string / array
    CABBIRD_RESOLVE_IL2CPP(il2cpp_object_new);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_object_get_class);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_object_get_size);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_object_unbox);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_value_box);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_string_new);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_string_new_utf16);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_string_length);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_string_chars);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_array_new);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_array_length);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_array_class_get);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_array_element_size);
    // threads / GC
    CABBIRD_RESOLVE_IL2CPP(il2cpp_thread_attach);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_thread_detach);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_thread_current);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_is_vm_thread);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_gc_disable);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_gc_enable);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_gc_is_disabled);
    // gc handles
    CABBIRD_RESOLVE_IL2CPP(il2cpp_gchandle_new);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_gchandle_get_target);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_gchandle_free);
    // misc
    CABBIRD_RESOLVE_IL2CPP(il2cpp_resolve_icall);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_free);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_get_corlib);
    // layout queries
    CABBIRD_RESOLVE_IL2CPP(il2cpp_object_header_size);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_array_object_header_size);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_offset_of_array_length_in_array_object_header);
    CABBIRD_RESOLVE_IL2CPP(il2cpp_offset_of_array_bounds_in_array_object_header);

    if (!missing.empty()) {
        std::ostringstream message;
        message << "GameAssembly.dll is missing " << missing.size()
                << " required export(s): ";
        for (std::size_t index = 0; index < missing.size(); ++index) {
            if (index != 0) {
                message << ", ";
            }
            message << missing[index];
        }
        // A handful are genuinely optional, so only the ones we cannot work
        // without are fatal.
        g_last_error = message.str();
    }
}

}  // namespace

namespace {

// One resolution attempt.  Runs under g_init_mutex and never latches a failure.
void AttemptInitialize(std::wstring_view assembly_name) {
        // GetModuleHandleW, never LoadLibraryW: GameAssembly.dll is already
        // mapped by the runtime and loading a second copy would be catastrophic.
        HMODULE module = ::GetModuleHandleW(std::wstring(assembly_name).c_str());
        if (module == nullptr) {
            std::ostringstream message;
            message << "module not found in this process: ";
            for (const wchar_t ch : assembly_name) {
                message << static_cast<char>(ch);
            }
            g_last_error = message.str();
            return;
        }
        g_api.module = module;

        std::array<wchar_t, 32768> path{};
        const DWORD length = ::GetModuleFileNameW(module, path.data(),
                                                  static_cast<DWORD>(path.size()));
        g_assembly_path.assign(path.data(), length);

        ResolveAll();

        // The irreplaceable subset.  If any of these is absent the export table
        // is stripped or renamed and the whole approach has to change.
        if (g_api.il2cpp_domain_get == nullptr || g_api.il2cpp_class_from_name == nullptr ||
            g_api.il2cpp_thread_attach == nullptr || g_api.il2cpp_image_get_class == nullptr) {
            if (g_last_error.empty()) {
                g_last_error = "core il2cpp exports unavailable";
            }
            return;
        }

        if (g_api.il2cpp_object_header_size != nullptr) {
            g_layout.object_header_size =
                static_cast<std::size_t>(g_api.il2cpp_object_header_size());
        }
        if (g_api.il2cpp_array_object_header_size != nullptr) {
            g_layout.array_object_header_size =
                static_cast<std::size_t>(g_api.il2cpp_array_object_header_size());
        }
        if (g_api.il2cpp_offset_of_array_length_in_array_object_header != nullptr) {
            g_layout.array_length_offset =
                static_cast<std::size_t>(g_api.il2cpp_offset_of_array_length_in_array_object_header());
        }
        if (g_api.il2cpp_offset_of_array_bounds_in_array_object_header != nullptr) {
            g_layout.array_bounds_offset =
                static_cast<std::size_t>(g_api.il2cpp_offset_of_array_bounds_in_array_object_header());
        }

        g_ready = true;
}

}  // namespace

bool Initialize(std::wstring_view assembly_name) {
    // Deliberately NOT std::call_once.  The injector maps this image in as soon
    // as the target process exists, which is usually well before IL2CPP has
    // finished coming up, so the first attempt legitimately fails.  A one-shot
    // latch would turn that early failure into a permanent one; retrying is
    // cheap and is the whole point of a probe that polls for the runtime.
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_ready) {
        return true;
    }
    // Clear per-attempt state so a retry cannot leave a stale module handle or
    // a half-resolved table behind.
    g_api = Api{};
    g_last_error.clear();
    AttemptInitialize(assembly_name);
    return g_ready;
}

bool Ready() { return g_ready; }
const Api& Functions() { return g_api; }
const Layout& Constants() { return g_layout; }
const std::wstring& AssemblyPath() { return g_assembly_path; }
const std::string& LastError() { return g_last_error; }

// ---------------------------------------------------------------------------
// ThreadScope
// ---------------------------------------------------------------------------

namespace {

// Per-thread attach bookkeeping.
//
// NOT `thread_local`.  MSVC implements that as `__declspec(thread)`, which
// emits a static-TLS directory -- and a manual-mapped image is rejected
// outright if it has one, because there is no loader to fill the slot per
// thread.  Doing it the obvious way here cost the whole delivery route until the
// mapper rejected the image.  FLS gives the same per-thread semantics with
// no TLS directory at all, and is what the UCRT itself uses internally.
//
// IL2CPP's own attach is idempotent but its detach is not, so we must only
// detach on the outermost scope exit -- hence the depth counter.
struct ThreadState final {
    int depth{};
    bool attached_here{};
    // The thread object the runtime returned from il2cpp_thread_attach.  It is
    // what il2cpp_thread_detach must be given.
    Il2CppThread* attached_thread{};
};

DWORD g_fls_index = FLS_OUT_OF_INDEXES;
std::once_flag g_fls_once;

void WINAPI FreeThreadState(void* value) {
    delete static_cast<ThreadState*>(value);
}

ThreadState* ThreadStateForCurrentThread() {
    std::call_once(g_fls_once, []() { g_fls_index = ::FlsAlloc(&FreeThreadState); });
    if (g_fls_index == FLS_OUT_OF_INDEXES) {
        // Cannot happen in practice; stay inert rather than share state across
        // threads, which would make the depth counter meaningless.
        return nullptr;
    }
    auto* state = static_cast<ThreadState*>(::FlsGetValue(g_fls_index));
    if (state == nullptr) {
        state = new ThreadState{};
        ::FlsSetValue(g_fls_index, state);
    }
    return state;
}

bool IsCurrentThreadVmThread() {
    // `il2cpp_is_vm_thread` takes the thread object and returns the answer; it
    // does not take an out-parameter.  Asking for the current thread first is
    // what makes an unattached thread answer "no" instead of being dereferenced
    // as if it were a real Il2CppThread.
    if (g_api.il2cpp_thread_current == nullptr || g_api.il2cpp_is_vm_thread == nullptr) {
        return false;
    }
    Il2CppThread* current = g_api.il2cpp_thread_current();
    if (current == nullptr) {
        return false;
    }
    return g_api.il2cpp_is_vm_thread(current);
}

}  // namespace

ThreadScope::ThreadScope() {
    if (!g_ready) {
        return;
    }
    ThreadState* state = ThreadStateForCurrentThread();
    if (state == nullptr) {
        return;
    }
    thread_state_ = state;
    if (state->depth++ > 0) {
        // Already attached by an outer scope on this thread.
        attached_ = state->attached_here;
        owns_ = false;
        return;
    }
    if (IsCurrentThreadVmThread()) {
        // A thread the runtime already knows (Unity's main thread, a job thread).
        // Detaching it would be fatal, so leave it strictly alone.
        attached_ = true;
        owns_ = false;
        state->attached_here = false;
        return;
    }
    // CRITICAL: the domain is null until IL2CPP finishes initialising.  Calling
    // il2cpp_thread_attach(nullptr) at that point is a hard crash, and we are
    // routinely running before the runtime is up.  Report "not attached" so the
    // caller can retry instead of taking the game down.
    Il2CppDomain* domain =
        g_api.il2cpp_domain_get != nullptr ? g_api.il2cpp_domain_get() : nullptr;
    if (domain == nullptr) {
        --state->depth;
        attached_ = false;
        owns_ = false;
        state->attached_here = false;
        return;
    }
    g_api.il2cpp_thread_attach(domain);
    // Record the THREAD the runtime handed back.  detach() needs it; passing the
    // domain instead is what produced "Collecting from unknown thread".
    state->attached_thread = g_api.il2cpp_thread_current != nullptr
        ? g_api.il2cpp_thread_current()
        : nullptr;
    // ...and then REQUIRE it, because "attach was called" and "this thread is attached"
    // are different facts and only the second one is what the rest of the facade relies on.
    //
    // `il2cpp_thread_attach` CAN return null (the runtime does it while the domain is null,
    // i.e. before IL2CPP finishes initialising).  Ignoring the return value and setting
    // `attached_ = true; owns_ = true` unconditionally means a scope over a thread the
    // runtime never accepted reports itself as attached, and its destructor then hands
    // a null thread to `il2cpp_thread_detach`.  The destructor guards against the null, so
    // this is not a crash -- it is a lie, and a lie of exactly the kind this file
    // keeps writing notes about: a caller reads `attached()` and believes it may now touch
    // managed objects.
    if (state->attached_thread == nullptr) {
        --state->depth;
        attached_ = false;
        owns_ = false;
        state->attached_here = false;
        return;
    }
    attached_ = true;
    owns_ = true;
    state->attached_here = true;
}

ThreadScope::~ThreadScope() {
    auto* const state = static_cast<ThreadState*>(thread_state_);
    if (state == nullptr) {
        return;
    }
    thread_state_ = nullptr;
    if (state->depth > 0) {
        --state->depth;
    }
    if (state->depth == 0 && owns_ && state->attached_here &&
        g_api.il2cpp_thread_detach != nullptr && state->attached_thread != nullptr) {
        // The THREAD OBJECT, never the domain.  See the note in the header.
        g_api.il2cpp_thread_detach(state->attached_thread);
        state->attached_thread = nullptr;
        state->attached_here = false;
    }
}

// ---------------------------------------------------------------------------
// Resolution helpers
// ---------------------------------------------------------------------------

Il2CppImage* FindImage(std::string_view assembly_name) {
    if (!g_ready) {
        return nullptr;
    }
    std::size_t count = 0;
    const Il2CppAssembly** assemblies =
        g_api.il2cpp_domain_get_assemblies(g_api.il2cpp_domain_get(), &count);
    if (assemblies == nullptr) {
        return nullptr;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (assemblies[index] == nullptr) {
            continue;
        }
        Il2CppImage* image = g_api.il2cpp_assembly_get_image(assemblies[index]);
        if (image == nullptr) {
            continue;
        }
        const char* name = g_api.il2cpp_image_get_name(image);
        if (name == nullptr) {
            continue;
        }
        std::string_view candidate{name};
        // "Assembly-CSharp.dll" and "Assembly-CSharp" both have to match.
        if (candidate == assembly_name) {
            return image;
        }
        const auto dot = candidate.rfind('.');
        if (dot != std::string_view::npos && candidate.substr(0, dot) == assembly_name) {
            return image;
        }
    }
    return nullptr;
}

Il2CppClass* FindClass(
    std::string_view assembly_name,
    std::string_view namespaze,
    std::string_view name) {
    Il2CppImage* image = FindImage(assembly_name);
    if (image == nullptr) {
        return nullptr;
    }
    const std::string namespace_buffer{namespaze};
    const std::string name_buffer{name};
    return g_api.il2cpp_class_from_name(image, namespace_buffer.c_str(), name_buffer.c_str());
}

std::string ClassName(const Il2CppClass* klass) {
    if (klass == nullptr) {
        return "<null>";
    }
    const char* name = g_api.il2cpp_class_get_name(const_cast<Il2CppClass*>(klass));
    const char* namespaze = g_api.il2cpp_class_get_namespace(const_cast<Il2CppClass*>(klass));
    std::string result;
    if (namespaze != nullptr && namespaze[0] != '\0') {
        result += namespaze;
        result += '.';
    }
    result += (name != nullptr ? name : "<anonymous>");
    return result;
}

std::string TypeName(const Il2CppType* type) {
    if (type == nullptr) {
        return "<null>";
    }
    const char* name = g_api.il2cpp_type_get_name(type);
    return name != nullptr ? name : "<unnamed>";
}

FieldInfo* FindField(Il2CppClass* klass, std::string_view field_name) {
    if (!g_ready || klass == nullptr) {
        return nullptr;
    }
    const std::string name{field_name};
    // il2cpp_class_get_field_from_name only looks at this class, so walk up.
    for (Il2CppClass* cursor = klass; cursor != nullptr;
         cursor = g_api.il2cpp_class_get_parent(cursor)) {
        if (FieldInfo* field = g_api.il2cpp_class_get_field_from_name(cursor, name.c_str())) {
            return field;
        }
    }
    return nullptr;
}

std::optional<std::size_t> FieldOffset(Il2CppClass* klass, std::string_view field_name) {
    FieldInfo* field = FindField(klass, field_name);
    if (field == nullptr) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(g_api.il2cpp_field_get_offset(field));
}

std::optional<std::uintptr_t> StaticFieldAddress(Il2CppClass* klass, std::string_view field_name) {
    if (!g_ready || klass == nullptr || g_api.il2cpp_class_get_static_field_data == nullptr) {
        return std::nullopt;
    }
    const std::string name{field_name};
    for (Il2CppClass* cursor = klass; cursor != nullptr;
         cursor = g_api.il2cpp_class_get_parent(cursor)) {
        FieldInfo* field = g_api.il2cpp_class_get_field_from_name(cursor, name.c_str());
        if (field == nullptr) {
            continue;
        }
        void* static_data = g_api.il2cpp_class_get_static_field_data(cursor);
        if (static_data == nullptr) {
            // The class has not been initialised by the runtime yet.
            return std::nullopt;
        }
        return reinterpret_cast<std::uintptr_t>(static_data) +
               static_cast<std::size_t>(g_api.il2cpp_field_get_offset(field));
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

namespace {

std::string Utf16ToUtf8(const char16_t* text, std::size_t length) {
    if (text == nullptr || length == 0) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, reinterpret_cast<const wchar_t*>(text), static_cast<int>(length), nullptr, 0,
        nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(text),
                          static_cast<int>(length), result.data(), needed, nullptr, nullptr);
    return result;
}

}  // namespace

std::optional<std::string> ReadString(const Il2CppObject* string_object) {
    if (!g_ready || string_object == nullptr) {
        return std::nullopt;
    }
    // Prefer the runtime accessors: they know the layout, we do not have to.
    if (g_api.il2cpp_string_length != nullptr && g_api.il2cpp_string_chars != nullptr) {
        const std::int32_t length =
            g_api.il2cpp_string_length(const_cast<Il2CppObject*>(string_object));
        if (length < 0 || length > (1 << 22)) {
            return std::nullopt;
        }
        const char16_t* chars =
            g_api.il2cpp_string_chars(const_cast<Il2CppObject*>(string_object));
        return Utf16ToUtf8(chars, static_cast<std::size_t>(length));
    }
    // Fallback: read the layout directly.  `length` is the second field of
    // Il2CppString and has been at +0x10 on every x64 IL2CPP build to date.
    const auto address = reinterpret_cast<std::uintptr_t>(string_object);
    const auto length = cabbird::mem::Read<std::int32_t>(address + 0x10);
    if (!length || *length < 0 || *length > (1 << 22)) {
        return std::nullopt;
    }
    std::vector<char16_t> buffer(static_cast<std::size_t>(*length));
    if (!cabbird::mem::ReadMemoryInto(address + 0x14, buffer.data(),
                                  buffer.size() * sizeof(char16_t))) {
        return std::nullopt;
    }
    return Utf16ToUtf8(buffer.data(), buffer.size());
}

std::optional<std::string> ReadStringField(const Il2CppObject* object, std::size_t offset) {
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto field = cabbird::mem::Read<std::uintptr_t>(
        reinterpret_cast<std::uintptr_t>(object) + offset);
    if (!field || *field == 0) {
        return std::nullopt;
    }
    return ReadString(reinterpret_cast<const Il2CppObject*>(*field));
}

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------

std::string DescribeRuntime() {
    std::ostringstream out;
    if (!g_ready) {
        out << "il2cpp=unavailable (" << g_last_error << ')';
        return out.str();
    }
    ThreadScope scope;
    Il2CppDomain* domain = g_api.il2cpp_domain_get();
    std::size_t assembly_count = 0;
    const Il2CppAssembly** assemblies = g_api.il2cpp_domain_get_assemblies(domain, &assembly_count);
    out << "il2cpp=ready domain=" << static_cast<const void*>(domain)
        << " assemblies=" << assembly_count
        << " layout{objhdr=" << g_layout.object_header_size
        << " arrhdr=" << g_layout.array_object_header_size
        << " arrlen=" << g_layout.array_length_offset
        << " arrbounds=" << g_layout.array_bounds_offset << '}';

    // Prove enum/class/image walking actually works, not just that the pointers
    // resolved: count the classes the runtime reports.
    std::size_t class_total = 0;
    for (std::size_t index = 0; index < assembly_count && index < 512; ++index) {
        if (assemblies == nullptr || assemblies[index] == nullptr) {
            continue;
        }
        Il2CppImage* image = g_api.il2cpp_assembly_get_image(assemblies[index]);
        if (image == nullptr) {
            continue;
        }
        class_total += g_api.il2cpp_image_get_class_count(image);
    }
    out << " classes_scanned=" << class_total;
    if (g_api.il2cpp_get_corlib != nullptr) {
        // get_corlib returns an IMAGE.  Calling ClassName() on it (as an earlier
        // version did) reads an Il2CppImage as an Il2CppClass.
        Il2CppImage* corlib = g_api.il2cpp_get_corlib();
        const char* corlib_name =
            (corlib != nullptr && g_api.il2cpp_image_get_name != nullptr)
                ? g_api.il2cpp_image_get_name(corlib)
                : nullptr;
        out << " corlib=" << (corlib_name != nullptr ? corlib_name : "<unnamed>");
    }
    return out.str();
}

}  // namespace cabbird::il2cpp
