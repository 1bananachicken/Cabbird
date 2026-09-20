// cabbird_inject -- launch the game via its launcher and manually map the overlay
// image into the resulting process.
//
// Why this route exists:
//   The proxy DLL route proved the rendering path works end to end -- a blank
//   ImGui window really did render inside AzurPromilia -- but the process is
//   then terminated with the shell's 0xDEADC0DE code, while a stock game
//   directory exits with code 0.  The leading hypotheses all concern module
//   *discovery*: the proxy name, the anomaly of a non-System32 module, the
//   module appearing in the PEB loader lists, or the unsigned binary.
//
//   Manual mapping removes the first three outright.  Nothing calls
//   LoadLibrary, so no loader data structure ever learns about us, and the DLL
//   can live outside the game directory entirely.
//
// Usage:
//   cabbird_inject.exe --dll <Cabbird.Core.dll> [--level 1]
//                  [--launcher <path>] [--target AzurPromilia.exe]
//                  [--pid <n>] [--no-launch] [--wait <seconds>]
//
// Defaults: launcher D:\AzurPromilia\0.6.2.2\launcher.exe,
//           target AzurPromilia.exe,
//           dll <injector dir>\Cabbird.Core.dll,
//           log dir <injector dir>\logs.

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "cabbird/launcher/manual_map.hpp"

namespace {

constexpr wchar_t kDefaultLauncher[] = L"D:\\AzurPromilia\\0.6.2.2\\launcher.exe";
constexpr wchar_t kDefaultTarget[] = L"AzurPromilia.exe";

std::filesystem::path ExecutableDirectory() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    if (length == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

void Print(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fflush(stdout);
}

const char* ErrorName(cabbird::launcher::ManualMapError error) {
    using cabbird::launcher::ManualMapError;
    switch (error) {
        case ManualMapError::None: return "None";
        case ManualMapError::ProcessLaunchFailure: return "ProcessLaunchFailure";
        case ManualMapError::ProcessControlFailure: return "ProcessControlFailure";
        case ManualMapError::ProcessUnavailable: return "ProcessUnavailable";
        case ManualMapError::AccessDenied: return "AccessDenied";
        case ManualMapError::DifferentUser: return "DifferentUser";
        case ManualMapError::IncompatibleArchitecture: return "IncompatibleArchitecture";
        case ManualMapError::ImageUnavailable: return "ImageUnavailable";
        case ManualMapError::ImageInvalid: return "ImageInvalid";
        case ManualMapError::AlreadyAttached: return "AlreadyAttached";
        case ManualMapError::DependencyFailure: return "DependencyFailure";
        case ManualMapError::AllocationFailure: return "AllocationFailure";
        case ManualMapError::WriteFailure: return "WriteFailure";
        case ManualMapError::ProtectionFailure: return "ProtectionFailure";
        case ManualMapError::BootstrapFailure: return "BootstrapFailure";
        case ManualMapError::Timeout: return "Timeout";
    }
    return "?";
}

struct WaitResult final {
    DWORD process_id{};
    bool compatible{};
    DWORD inspection_error{};
};

// Waits for a process whose executable name matches, so `--no-launch` can be
// used when the game is started manually (the documented policy is that the
// user launches the game, not this tool).
//
// A "compatible" candidate (same user, x64, inspectable) is preferred, because
// it also means the target is far enough along to be worth mapping.  But it is
// deliberately NOT required: AttachableProcess::Compatible() needs
// OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION), and ACE refuses exactly that
// call on the real target.  Insisting on it would time out on a perfectly
// healthy game, so after a grace period the name match is handed over anyway and
// ManualMapRuntimeCore reports the authoritative error.
WaitResult WaitForProcess(const std::wstring& name, std::chrono::seconds limit) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + limit;
    const auto grace = start + std::chrono::seconds(20);
    WaitResult fallback;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = cabbird::launcher::EnumerateAttachableProcesses(name);
        for (const auto& candidate : found) {
            if (candidate.process_id == 0) {
                continue;
            }
            if (candidate.Compatible()) {
                return WaitResult{candidate.process_id, true, candidate.inspection_error};
            }
            if (fallback.process_id == 0) {
                fallback = WaitResult{candidate.process_id, false, candidate.inspection_error};
            }
        }
        if (fallback.process_id != 0 && std::chrono::steady_clock::now() >= grace) {
            return fallback;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return fallback;
}

void ListProcesses(const std::wstring& target) {
    const auto found = cabbird::launcher::EnumerateAttachableProcesses(target);
    Print("%zu match(es) for %ls\n", found.size(), target.c_str());
    for (const auto& candidate : found) {
        Print("  pid=%-7lu x64=%d same_user=%d compatible=%d win32=%-5lu %ls\n",
              candidate.process_id, candidate.x64 ? 1 : 0,
              candidate.owned_by_current_user ? 1 : 0, candidate.Compatible() ? 1 : 0,
              candidate.inspection_error, candidate.executable_path.c_str());
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const auto exe_dir = ExecutableDirectory();
    // Cabbird.Core.dll is the framework entry point, and it exports all five core_api.h
    // entry points.
    std::filesystem::path dll_path = exe_dir / L"Cabbird.Core.dll";
    std::filesystem::path log_dir = exe_dir / L"logs";
    std::filesystem::path launcher_path = kDefaultLauncher;
    std::wstring target = kDefaultTarget;
    DWORD attach_pid = 0;
    bool launch = true;
    bool list_only = false;
    unsigned level = cabbird::launcher::CABBIRD_LEVEL_FROM_INI;
    int wait_seconds = 120;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool has_next = (i + 1) < argc;
        if (arg == L"--dll" && has_next) {
            dll_path = argv[++i];
        } else if (arg == L"--launcher" && has_next) {
            launcher_path = argv[++i];
        } else if (arg == L"--target" && has_next) {
            target = argv[++i];
        } else if (arg == L"--log-dir" && has_next) {
            log_dir = argv[++i];
        } else if (arg == L"--pid" && has_next) {
            attach_pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
            launch = false;
        } else if (arg == L"--level" && has_next) {
            level = static_cast<unsigned>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--wait" && has_next) {
            wait_seconds = static_cast<int>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--no-launch") {
            launch = false;
        } else if (arg == L"--list") {
            list_only = true;
        } else if (arg == L"--help" || arg == L"-h") {
            Print("usage: cabbird_inject [--dll P] [--launcher P] [--target NAME]\n"
                  "                  [--log-dir P] [--level N] [--pid N]\n"
                  "                  [--no-launch] [--wait SECONDS] [--list]\n"
                  "\n"
                  "  --list   print every process matching --target with its\n"
                  "           compatibility status, then exit (diagnostic)\n");
            return 0;
        } else {
            Print("unknown argument: %ls\n", arg.c_str());
            return 2;
        }
    }

    std::error_code ec;
    dll_path = std::filesystem::absolute(dll_path, ec);
    log_dir = std::filesystem::absolute(log_dir, ec);
    if (!std::filesystem::exists(dll_path)) {
        Print("FATAL: overlay image not found: %ls\n", dll_path.c_str());
        return 2;
    }
    std::filesystem::create_directories(log_dir, ec);

    // Which overlay config is on disk?
    //
    // This block used to claim that because the answer came from
    // ResolveOverlayIniPath() -- "the same function the mapping layer uses" -- the tool "cannot
    // print one path and deliver another".  That was false in the worst way: it printed the path
    // and delivered NOTHING.  ManualMapOptions::ini_path was never assigned by any front end,
    // the field it fed no longer exists in the ABI at all (upstream's CabbirdStartInfo carries
    // no ini_path), and the injector does not set start_info->ini_path.  The mapped runtime
    // reads <runtime_root>\cabbird.ini instead -- see BuildStartContext in core_main.cpp.
    //
    // The resolution is kept because --list is still the diagnostic mode and knowing which
    // cabbird_overlay.ini a run would pick up is useful when reading that file by hand, but it
    // is a report of the filesystem, not a promise that the file is handed to the image.
    //
    const std::filesystem::path overlay_ini =
        cabbird::launcher::ResolveOverlayIniPath({}, log_dir);

    if (list_only) {
        Print("  overlay ini: %ls\n",
              overlay_ini.empty() ? L"(none found - compiled-in defaults will apply)"
                                  : overlay_ini.c_str());
        ListProcesses(target);
        return 0;
    }

    Print("cabbird_inject\n");
    Print("  image      : %ls\n", dll_path.c_str());
    Print("  log dir    : %ls\n", log_dir.c_str());
    Print("  level      : %s (INERT - see ManualMapOptions::level; it no longer crosses the ABI)\n",
          level == cabbird::launcher::CABBIRD_LEVEL_FROM_INI
              ? "from ini" : std::to_string(level).c_str());
    Print("  target     : %ls\n", target.c_str());
    Print("  overlay ini: %ls\n",
          overlay_ini.empty() ? L"(none found - compiled-in defaults will apply)"
                              : overlay_ini.c_str());

    cabbird::launcher::ManualMapOptions map_options;
    map_options.core_path = dll_path;
    map_options.runtime_root = exe_dir;
    map_options.log_directory = log_dir;
    map_options.level = level;

    if (launch) {
        Print("  launcher   : %ls\n", launcher_path.c_str());
        cabbird::launcher::ManualMapLaunchOptions options;
        options.launcher_path = launcher_path;
        options.working_directory = launcher_path.parent_path();
        options.target_executable_name = target;
        options.target_timeout = std::chrono::seconds(wait_seconds);
        options.loader_timeout = std::chrono::seconds(wait_seconds);
        options.manual_map = map_options;

        const auto result = cabbird::launcher::LaunchAndManualMapRuntimeCore(options);
        Print("\nmapped pid=%lu image=0x%llx error=%s win32=%lu runtime=%lu\n",
              result.process_id, static_cast<unsigned long long>(result.mapping.remote_image),
              ErrorName(result.mapping.error), result.mapping.win32_error,
              result.mapping.runtime_start_error);
        if (!result.mapping.message.empty()) {
            Print("message: %s\n", result.mapping.message.c_str());
        }
        return result.Ok() ? 0 : 1;
    }

    if (attach_pid == 0) {
        Print("  waiting for %ls (up to %ds) -- start the game yourself now\n",
              target.c_str(), wait_seconds);
        const WaitResult wait = WaitForProcess(target, std::chrono::seconds(wait_seconds));
        if (wait.process_id == 0) {
            Print("FATAL: %ls did not appear within %ds -- nothing with that name was found\n",
                  target.c_str(), wait_seconds);
            return 3;
        }
        attach_pid = wait.process_id;
        if (!wait.compatible) {
            Print("  note       : process is not fully inspectable (win32=%lu) -- attaching\n"
                  "               anyway; ManualMapRuntimeCore reports the real error\n",
                  wait.inspection_error);
        }
    }
    map_options.process_id = attach_pid;
    Print("  attaching  : pid=%lu\n", attach_pid);

    const auto mapped = cabbird::launcher::ManualMapRuntimeCore(map_options);
    Print("\nmapped pid=%lu image=0x%llx error=%s win32=%lu runtime=%lu\n", attach_pid,
          static_cast<unsigned long long>(mapped.remote_image), ErrorName(mapped.error),
          mapped.win32_error, mapped.runtime_start_error);
    if (!mapped.message.empty()) {
        Print("message: %s\n", mapped.message.c_str());
    }
    return mapped.Ok() ? 0 : 1;
}
