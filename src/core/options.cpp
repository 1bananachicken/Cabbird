#include "options.hpp"

#include <Windows.h>

namespace cabbird {
namespace {

int ReadInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
}

bool ReadBool(const std::wstring& ini, const wchar_t* section, const wchar_t* key, bool fallback) {
    return ReadInt(ini, section, key, fallback ? 1 : 0) != 0;
}

// Reads a string value out of the ini.  GetPrivateProfileStringW needs a caller
// buffer and silently truncates when it is too small, so grow once and report
// honestly rather than returning a half-value.  A missing key yields an empty
// string, which every caller here treats as "not set".
std::wstring ReadString(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    std::wstring buffer(256, L'\0');
    for (;;) {
        const DWORD copied = ::GetPrivateProfileStringW(section, key, L"", buffer.data(),
                                                        static_cast<DWORD>(buffer.size()),
                                                        ini.c_str());
        // A full buffer means the value may have been cut short; retry bigger.
        if (copied + 1 < buffer.size()) {
            buffer.resize(copied);
            return buffer;
        }
        if (buffer.size() >= 4096) {
            buffer.resize(copied);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring Join(const std::wstring& dir, const wchar_t* name) {
    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path += name;
    return path;
}

}  // namespace

LogLevel ToLogLevel(int value) {
    switch (value) {
        case 0:
            return LogLevel::kOff;
        case 1:
            return LogLevel::kError;
        case 3:
            return LogLevel::kDebug;
        default:
            return LogLevel::kInfo;
    }
}

Options LoadOptions(const std::wstring& dll_dir) {
    Options options;
    options.dll_dir = dll_dir;

    const std::wstring ini = Join(dll_dir, L"cabbird_overlay.ini");
    const DWORD attrs = GetFileAttributesW(ini.c_str());
    const bool ini_exists = attrs != INVALID_FILE_ATTRIBUTES;

    options.log_level = ReadInt(ini, L"logging", L"level", options.log_level);
    options.log_to_debugger = ReadBool(ini, L"logging", L"debugger", options.log_to_debugger);

    options.enable_hooks = ReadBool(ini, L"runtime", L"enable_hooks", options.enable_hooks);
    options.enable_overlay = ReadBool(ini, L"runtime", L"enable_overlay", options.enable_overlay);
    options.init_delay_ms = ReadInt(ini, L"runtime", L"init_delay_ms", options.init_delay_ms);
    options.retry_interval_ms =
        ReadInt(ini, L"runtime", L"retry_interval_ms", options.retry_interval_ms);
    options.retry_attempts = ReadInt(ini, L"runtime", L"retry_attempts", options.retry_attempts);

    options.wndproc_hook = ReadBool(ini, L"overlay", L"wndproc_hook", options.wndproc_hook);
    options.show_status_text = ReadBool(ini, L"overlay", L"status_text", options.show_status_text);

    // Manual-map only.  `il2cpp_probe=0` restores the overlay to the exact
    // configuration that has already run inside the real game for minutes, so a
    // suspected probe interaction can be ruled out without a rebuild.
    options.il2cpp_probe = ReadBool(ini, L"manual_map", L"il2cpp_probe", options.il2cpp_probe);
    options.il2cpp_probe_after_frames = ReadInt(ini, L"manual_map", L"il2cpp_probe_after_frames",
                                                options.il2cpp_probe_after_frames);
    options.il2cpp_dump = ReadBool(ini, L"manual_map", L"il2cpp_dump", options.il2cpp_dump);
    options.il2cpp_dump_methods =
        ReadBool(ini, L"manual_map", L"il2cpp_dump_methods", options.il2cpp_dump_methods);
    options.il2cpp_dump_properties =
        ReadBool(ini, L"manual_map", L"il2cpp_dump_properties", options.il2cpp_dump_properties);
    options.il2cpp_dump_interfaces =
        ReadBool(ini, L"manual_map", L"il2cpp_dump_interfaces", options.il2cpp_dump_interfaces);
    options.il2cpp_dump_methods_images =
        ReadString(ini, L"manual_map", L"il2cpp_dump_methods_images");
    options.il2cpp_dump_max_classes =
        ReadInt(ini, L"manual_map", L"il2cpp_dump_max_classes", options.il2cpp_dump_max_classes);
    options.il2cpp_dump_max_methods_per_class =
        ReadInt(ini, L"manual_map", L"il2cpp_dump_max_methods_per_class",
                options.il2cpp_dump_max_methods_per_class);
    options.il2cpp_dump_metadata =
        ReadBool(ini, L"manual_map", L"il2cpp_dump_metadata", options.il2cpp_dump_metadata);
    if (options.il2cpp_dump_max_classes < 0) {
        options.il2cpp_dump_max_classes = 0;
    }
    if (options.il2cpp_dump_max_methods_per_class < 0) {
        options.il2cpp_dump_max_methods_per_class = 0;
    }
    if (options.il2cpp_probe_after_frames < 0) {
        options.il2cpp_probe_after_frames = 0;
    }

    if (options.init_delay_ms < 0) {
        options.init_delay_ms = 0;
    }
    if (options.retry_interval_ms < 50) {
        options.retry_interval_ms = 50;
    }

    // Remember whether we had an ini at all: a fresh deployment writes one.
    options.ini_present = ini_exists;
    options.ini_path = ini;
    return options;
}

}  // namespace cabbird
