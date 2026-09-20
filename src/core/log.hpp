// Buffered, low-overhead logger for the injected overlay.
//
// Rationale: this module runs inside a shipping game process. Writing + flushing a
// file on every line is a measurable I/O stall, so lines are accumulated in memory
// and flushed in batches (size threshold, explicit
// Flush() at milestones, and once per second from the render thread).
#pragma once

#include <string>

namespace cabbird {

enum class LogLevel : int {
    kOff = 0,
    kError = 1,
    kInfo = 2,
    kDebug = 3,
};

// Creates `dir` and every missing parent.  Safe to call on an existing path and
// on a UNC path; failures are ignored because the subsequent CreateFileW is the
// real check.
void EnsureDirectory(const std::wstring& dir);

// `dir` must be the directory the DLL itself lives in; the log is written next
// to it so the file is easy to find during a test run.
void LogOpen(const std::wstring& dir, const std::wstring& file_name, LogLevel level,
             bool mirror_to_debugger);
void LogClose();

void LogWrite(LogLevel level, const char* fmt, ...);
void LogFlush();

bool LogEnabled(LogLevel level);

void LogSetLevel(LogLevel level);
LogLevel LogGetLevel();

}  // namespace cabbird

#define CABBIRD_LOG_ERROR(...) ::cabbird::LogWrite(::cabbird::LogLevel::kError, __VA_ARGS__)
#define CABBIRD_LOG_INFO(...) ::cabbird::LogWrite(::cabbird::LogLevel::kInfo, __VA_ARGS__)
#define CABBIRD_LOG_DEBUG(...) ::cabbird::LogWrite(::cabbird::LogLevel::kDebug, __VA_ARGS__)
