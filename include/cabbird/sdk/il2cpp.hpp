#pragma once
#include "cabbird/sdk/services/il2cpp.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <optional>
#include <initializer_list>
#include <string>

namespace cabbird::sdk::il2cpp {
using Il2CppObject = CabbirdIl2CppObjectV1;
using Il2CppClass = CabbirdIl2CppClassV1;
using FieldInfo = CabbirdFieldInfoV1;
using MethodInfo = CabbirdMethodInfoV1;
using Il2CppType = CabbirdIl2CppTypeV1;
using Il2CppGCHandle = CabbirdIl2CppGCHandleV1;

// Borrow this view inside CabbirdIl2CppServiceV1::with_runtime only.
// No globals, private loader, thread attachment, or game-specific policy.
class Context {
public:
    explicit Context(const CabbirdIl2CppApiV1& api) : api_(api) {}
    const CabbirdIl2CppApiV1& Functions() const { return api_; }
    Il2CppClass* FindClass(const char* assembly, const char* namespaze, const char* name) const {
        std::size_t count{};
        auto* domain = api_.il2cpp_domain_get();
        if (!domain) return nullptr;
        const auto** assemblies = api_.il2cpp_domain_get_assemblies(domain, &count);
        if (!assemblies || count > 4096) return nullptr;
        for (std::size_t i = 0; i < count; ++i) {
            auto* image = api_.il2cpp_assembly_get_image(assemblies[i]);
            if (!image) continue;
            const char* image_name = api_.il2cpp_image_get_name(image);
            if (image_name && std::strcmp(image_name, assembly) == 0)
                return api_.il2cpp_class_from_name(image, namespaze, name);
        }
        return nullptr;
    }
    Il2CppClass* FindClassAnyImage(const char* namespaze, const char* name) const {
        std::size_t count{};
        auto* domain = api_.il2cpp_domain_get();
        if (!domain) return nullptr;
        const auto** assemblies = api_.il2cpp_domain_get_assemblies(domain, &count);
        if (!assemblies || count > 4096) return nullptr;
        for (std::size_t i = 0; i < count; ++i) {
            auto* image = api_.il2cpp_assembly_get_image(assemblies[i]);
            if (!image) continue;
            if (auto* klass = api_.il2cpp_class_from_name(image, namespaze, name))
                return klass;
        }
        return nullptr;
    }
    FieldInfo* FindField(Il2CppClass* cls, const char* name) const {
        for (; cls; cls = api_.il2cpp_class_get_parent(cls))
            if (auto* field = api_.il2cpp_class_get_field_from_name(cls, name)) return field;
        return nullptr;
    }
    const MethodInfo* FindMethod(Il2CppClass* cls, const char* name, int argc) const {
        for (; cls; cls = api_.il2cpp_class_get_parent(cls))
            if (const auto* method = api_.il2cpp_class_get_method_from_name(cls, name, argc)) return method;
        return nullptr;
    }
    const MethodInfo* FindMethodBySignature(Il2CppClass* cls, const char* name,
                                            std::initializer_list<const char*> parameters) const {
        for (; cls; cls = api_.il2cpp_class_get_parent(cls)) {
            void* iterator = nullptr;
            while (const auto* method = api_.il2cpp_class_get_methods(cls, &iterator)) {
                const char* candidate = api_.il2cpp_method_get_name(method);
                if (!candidate || std::strcmp(candidate, name) != 0 ||
                    api_.il2cpp_method_get_param_count(method) != parameters.size()) continue;
                std::uint32_t index = 0;
                bool matches = true;
                for (const auto* expected : parameters) {
                    if (TypeName(api_.il2cpp_method_get_param(method, index++)) != expected) {
                        matches = false;
                        break;
                    }
                }
                if (matches) return method;
            }
        }
        return nullptr;
    }
    std::string TypeName(const Il2CppType* type) const {
        if (!type) return {};
        const char* name = api_.il2cpp_type_get_name(type);
        if (!name) return {};
        struct Name {
            const CabbirdIl2CppApiV1& api;
            const char* value;
            ~Name() { api.il2cpp_free(const_cast<char*>(value)); }
        } owned{api_, name};
        return name;
    }
    std::optional<std::string> ReadString(Il2CppObject* object,
                                        std::size_t maximum_utf16_units = 4096) const {
        if (!object) return std::nullopt;
        const auto length = api_.il2cpp_string_length(object);
        if (length < 0 || static_cast<std::size_t>(length) > maximum_utf16_units) return std::nullopt;
        if (length == 0) return std::string{};
        const auto* chars = api_.il2cpp_string_chars(object);
        if (!chars) return std::nullopt;
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            reinterpret_cast<const wchar_t*>(chars), length, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return std::nullopt;
        std::string result(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            reinterpret_cast<const wchar_t*>(chars), length, result.data(), size, nullptr, nullptr) != size)
            return std::nullopt;
        return result;
    }
    std::size_t ArrayElementBase() const {
        const auto size = api_.il2cpp_array_object_header_size();
        return size >= 16 && size <= 128 ? static_cast<std::size_t>(size) : 0;
    }
    bool ArrayReferenceAt(Il2CppObject* array, std::uint32_t index, Il2CppObject** out) const {
        if (!out) return false;
        *out = nullptr;
        if (!array || !ArrayElementBase() || index >= api_.il2cpp_array_length(array)) return false;
        return ReadReference(reinterpret_cast<std::uintptr_t>(array) +
            ArrayElementBase() + static_cast<std::size_t>(index) * sizeof(void*), out);
    }
    // Managed exceptions and native access faults are failures, not successful void calls.
    // Keep this a POD-only SEH leaf. The caller can optionally retain the managed exception.
    bool Invoke(const MethodInfo* method, Il2CppObject* instance, void** args,
                Il2CppObject** result, Il2CppObject** exception_out = nullptr) const {
        if (!result) return false;
        *result = nullptr;
        if (exception_out) *exception_out = nullptr;
        if (!method) return false;
        __try {
            Il2CppObject* exception = nullptr;
            *result = api_.il2cpp_runtime_invoke(method, instance, args, &exception);
            if (exception_out) *exception_out = exception;
            return exception == nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *result = nullptr;
            return false;
        }
    }
private:
    static bool ReadReference(std::uintptr_t address, Il2CppObject** out) {
        __try {
            std::memcpy(out, reinterpret_cast<const void*>(address), sizeof(*out));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *out = nullptr;
            return false;
        }
    }
    const CabbirdIl2CppApiV1& api_;
};
}
