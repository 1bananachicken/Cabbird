#include "log.hpp"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "cabbird/thread_local_value.hpp"

namespace cabbird {
namespace {

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
LogLevel g_level = LogLevel::kInfo;
bool g_mirror = true;
std::string g_buffer;
ULONGLONG g_last_flush = 0;
constexpr size_t kFlushThreshold = 4096;
constexpr ULONGLONG kFlushIntervalMs = 1000;

/* Re-entrancy guard, and it MUST be thread-local rather than a global flag.
 *
 * Why a guard is needed at all: OutputDebugStringA raises DBG_PRINTEXCEPTION_C, which an
 * attached debugger (or a vectored exception handler in this very process) handles.  A
 * handler that logs would re-enter LogWrite -- and because the outer call holds g_mutex,
 * and std::mutex is not recursive, that is a deadlock inside the game's Present path.
 *
 * Why thread-local and not a plain bool: logging happens on many threads at once.  A
 * global guard would make whichever thread is mid-write cause every OTHER thread's line to
 * be dropped -- turning a rare deadlock into constant, silent log loss.  A per-thread
 * guard blocks only the re-entrant caller, which is the one that is actually a problem.
 *
 * This is also the reason FLS exists in this project: a manually mapped image cannot use
 * `thread_local`, so the guard goes through FlsAlloc (see thread_local_value.hpp).  It is
 * the one place in cabbird_core that needs per-thread state, which makes it the proof that
 * the FLS path really links into the mapped image rather than merely compiling.
 */
ThreadLocalScalar<bool> g_in_log;

/* RAII so the guard is released on every early return, including the `n < 0` one. */
class LogGuard final {
public:
    LogGuard() {
        engaged_ = !g_in_log.Get();
        if (engaged_) {
            g_in_log.Set(true);
        }
    }
    ~LogGuard() {
        if (engaged_) {
            g_in_log.Set(false);
        }
    }
    LogGuard(const LogGuard&) = delete;
    LogGuard& operator=(const LogGuard&) = delete;

    /* False when this thread is already inside LogWrite: the caller must return at once.
     * Dropping the line is correct here -- the alternative is infinite recursion. */
    [[nodiscard]] bool engaged() const noexcept { return engaged_; }

private:
    bool engaged_{false};
};

const char* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::kError:
            return "E";
        case LogLevel::kInfo:
            return "I";
        case LogLevel::kDebug:
            return "D";
        default:
            return "?";
    }
}

void FlushLocked() {
    if (g_file == INVALID_HANDLE_VALUE || g_buffer.empty()) {
        return;
    }
    DWORD written = 0;
    WriteFile(g_file, g_buffer.data(), static_cast<DWORD>(g_buffer.size()), &written, nullptr);
    g_buffer.clear();
    g_last_flush = GetTickCount64();
}

}  // namespace

void EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) {
        return;
    }
    // CreateFileW does not create intermediate directories.  A caller may well
    // hand us a path that does not exist yet (the GUI derives its log directory
    // from its own executable directory), and the old behaviour there was the
    // worst possible one: CreateFileW failed, only OutputDebugStringA noticed,
    // and a run that genuinely succeeded left no evidence on disk at all.
    std::wstring partial;
    partial.reserve(dir.size());
    for (std::size_t i = 0; i < dir.size(); ++i) {
        const wchar_t ch = dir[i];
        const bool separator = (ch == L'\\' || ch == L'/');
        // Skip the root of a drive ("C:\") or a UNC prefix ("\\server\share"):
        // CreateDirectoryW cannot create those and would just fail.
        const bool at_root = partial.size() <= 3 &&
                             (partial == L"\\\\" || (partial.size() == 3 && partial[1] == L':'));
        if (separator && !at_root) {
            CreateDirectoryW(partial.c_str(), nullptr);
        }
        partial.push_back(ch);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

void LogOpen(const std::wstring& dir, const std::wstring& file_name, LogLevel level,
             bool mirror_to_debugger) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        return;
    }
    g_level = level;
    g_mirror = mirror_to_debugger;
    EnsureDirectory(dir);
    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path += file_name;
    // Truncate: each run should produce a self-contained log.
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        OutputDebugStringA("[cabbird] failed to open log file\n");
    }
}

void LogClose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    FlushLocked();
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void LogSetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

LogLevel LogGetLevel() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}

bool LogEnabled(LogLevel level) {
    return static_cast<int>(level) <= static_cast<int>(LogGetLevel());
}

void LogWrite(LogLevel level, const char* fmt, ...) {
    if (!LogEnabled(level)) {
        return;
    }

    // Entered via OutputDebugStringA below, or via a handler it triggers: drop the line
    // rather than recurse forever or deadlock on g_mutex.
    const LogGuard guard;
    if (!guard.engaged()) {
        return;
    }

    char message[2048];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[2304];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d.%03d][%s][t%05lu] %s\r\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, LevelTag(level),
                        static_cast<unsigned long>(GetCurrentThreadId()), message);
    if (n < 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file != INVALID_HANDLE_VALUE) {
            g_buffer.append(line, static_cast<size_t>(n));
            const ULONGLONG now = GetTickCount64();
            if (g_buffer.size() >= kFlushThreshold || now - g_last_flush >= kFlushIntervalMs) {
                FlushLocked();
            }
        }
    }

    // Outside the lock, deliberately.  OutputDebugStringA raises DBG_PRINTEXCEPTION_C and
    // blocks until the debugger has looked at it, so holding g_mutex across it means every
    // other thread's logging stalls behind the debugger -- and if the debugger's handler
    // logs, the guard above is the only thing standing between us and a deadlock.
    if (g_mirror) {
        OutputDebugStringA(line);
    }
}

void LogFlush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    FlushLocked();
}

}  // namespace cabbird
