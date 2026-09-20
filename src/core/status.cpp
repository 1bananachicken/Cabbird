#include "status.hpp"

#include <Windows.h>

#include <map>
#include <mutex>

#include "log.hpp"

namespace cabbird {
namespace {

std::mutex g_mutex;
std::wstring g_path;
std::map<std::wstring, std::wstring> g_values;
bool g_open = false;

void RewriteLocked() {
    if (!g_open) {
        return;
    }
    std::wstring text;
    for (const auto& item : g_values) {
        text += item.first;
        text += L"=";
        text += item.second;
        text += L"\r\n";
    }
    HANDLE file = CreateFileW(g_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const size_t bytes = text.size() * sizeof(wchar_t);
    DWORD written = 0;
    if (bytes > 0 && bytes <= MAXDWORD) {
        // UTF-16LE with a BOM so PowerShell's Get-Content decodes it without flags.
        const wchar_t bom = 0xFEFF;
        DWORD bom_written = 0;
        WriteFile(file, &bom, sizeof(bom), &bom_written, nullptr);
        WriteFile(file, text.data(), static_cast<DWORD>(bytes), &written, nullptr);
    }
    CloseHandle(file);
}

}  // namespace

void StatusOpen(const std::wstring& dll_dir, const std::wstring& file_name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureDirectory(dll_dir);
    g_path = dll_dir;
    if (!g_path.empty() && g_path.back() != L'\\' && g_path.back() != L'/') {
        g_path.push_back(L'\\');
    }
    g_path += file_name;
    g_values.clear();
    g_open = true;
    RewriteLocked();
}

void StatusSet(const std::wstring& key, const std::wstring& value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_values[key] = value;
    RewriteLocked();
}

void StatusSet(const std::wstring& key, const char* value) {
    std::wstring wide;
    if (value != nullptr) {
        const int length = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
        if (length > 0) {
            wide.resize(static_cast<size_t>(length - 1));
            MultiByteToWideChar(CP_UTF8, 0, value, -1, wide.data(), length);
        }
    }
    StatusSet(key, wide);
}

void StatusSetInt(const std::wstring& key, long long value) {
    wchar_t buffer[32];
    _snwprintf_s(buffer, _TRUNCATE, L"%lld", value);
    StatusSet(key, std::wstring(buffer));
}

void StatusClose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    RewriteLocked();
    g_open = false;
}

}  // namespace cabbird
