#pragma once

#include <Windows.h>

#include "cabbird/core_api.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cabbird::launcher {

struct AttachableProcess final {
    DWORD process_id{};
    std::wstring executable_name;
    std::filesystem::path executable_path;
    bool owned_by_current_user{};
    bool x64{};
    DWORD inspection_error{};

    [[nodiscard]] bool Compatible() const noexcept {
        return process_id != 0 && owned_by_current_user && x64 && inspection_error == ERROR_SUCCESS;
    }
};

enum class ManualMapError : std::uint8_t {
    None,
    ProcessLaunchFailure,
    ProcessControlFailure,
    ProcessUnavailable,
    AccessDenied,
    DifferentUser,
    IncompatibleArchitecture,
    ImageUnavailable,
    ImageInvalid,
    AlreadyAttached,
    DependencyFailure,
    AllocationFailure,
    WriteFailure,
    ProtectionFailure,
    BootstrapFailure,
    Timeout,
};

/* Sentinel for ManualMapOptions::level, meaning "read the level from the ini".
 *
 * This used to live in include/cabbird/core_api.h as CABBIRD_LEVEL_FROM_INI, which was the
 * wrong side of the boundary: it is a launcher option value, not part of the ABI between the
 * injector and the mapped image.  core_api.h is now a faithful port of Anomaly's, and upstream
 * carries no level field at all, so the sentinel belongs in this header. */
inline constexpr std::uint32_t CABBIRD_LEVEL_FROM_INI = 0xFFFFFFFFu;

struct ManualMapOptions final {
    DWORD process_id{};
    std::filesystem::path core_path;
    std::filesystem::path runtime_root;
    std::filesystem::path log_directory;
    /* INERT.
     *
     * Neither field below reaches the image any more.  Upstream's CabbirdStartInfo has no
     * ini_path and no level, and the mapped runtime reads its own configuration from
     * <runtime_root>\cabbird.ini (src/runtime/core_main.cpp, BuildStartContext).  Both used to
     * be written into ABI fields that NO consumer read -- there is no start_info->ini_path and
     * no start_info->level anywhere in the runtime -- so nothing regresses, but a caller that
     * sets them is setting a value that goes nowhere.  `ini_path` in particular was never set
     * by any front end, so the CLI comment claiming it "cannot print one path and deliver
     * another" was describing a delivery that never happened.
     *
     * Left in place so this change stays confined to the ABI.  Deleting them is a follow-up. */
    std::filesystem::path ini_path;
    std::uint32_t level{CABBIRD_LEVEL_FROM_INI};
    std::chrono::milliseconds timeout{std::chrono::seconds(30)};
};

/* Decide which cabbird_overlay.ini gets delivered to the mapped image.
 *
 * The mapped image has no on-disk module path, so it cannot find its own config:
 * with `ini_path` empty it falls back to `<log_directory>\cabbird_overlay.ini`, a
 * directory the CALLER chose that usually contains no ini -- and the run then
 * proceeds silently on compiled-in defaults.  That actually happened: an ini
 * asking for `il2cpp_dump_methods=1` produced `present=0` and `methods=0`, and the
 * only symptom was a dump quietly missing half its content.
 *
 * Search order, first hit wins:
 *   1. explicit_path                                    -- caller's choice
 *   2. <injector exe dir>\cabbird_overlay.ini           -- where the build ships it
 *   3. <log_directory>\cabbird_overlay.ini              -- the image's own fallback
 *
 * Returns an empty path when none exists, which means the image will use its
 * compiled-in defaults.  This is a free function, and the mapping code calls it,
 * specifically so that every front end gets the same answer: an earlier version
 * of this fix lived in the CLI front end only, tested green, and did nothing for
 * the GUI the tool is actually driven from.
 */
std::filesystem::path ResolveOverlayIniPath(const std::filesystem::path& explicit_path,
                                            const std::filesystem::path& log_directory);

struct ManualMapResult final {
    ManualMapError error{ManualMapError::None};
    DWORD win32_error{ERROR_SUCCESS};
    DWORD runtime_start_error{ERROR_SUCCESS};
    std::uintptr_t remote_image{};
    std::string message;

    [[nodiscard]] bool Ok() const noexcept { return error == ManualMapError::None; }
};

struct ManualMapLaunchOptions final {
    std::filesystem::path launcher_path;
    std::wstring launcher_arguments;
    std::filesystem::path working_directory;
    std::wstring target_executable_name{L"AzurPromilia.exe"};
    DWORD creation_flags{};
    std::chrono::milliseconds target_timeout{std::chrono::minutes(2)};
    std::chrono::milliseconds loader_timeout{std::chrono::minutes(2)};
    ManualMapOptions manual_map;
};

struct ManualMapLaunchResult final {
    ManualMapResult mapping;
    DWORD process_id{};

    [[nodiscard]] bool Ok() const noexcept {
        return process_id != 0 && mapping.Ok();
    }
};

[[nodiscard]] AttachableProcess InspectAttachableProcess(DWORD process_id) noexcept;
[[nodiscard]] std::vector<AttachableProcess> EnumerateAttachableProcesses(
    std::wstring_view executable_name = L"AzurPromilia.exe") noexcept;
[[nodiscard]] ManualMapResult ManualMapRuntimeCore(const ManualMapOptions& options) noexcept;
[[nodiscard]] ManualMapLaunchResult LaunchAndManualMapRuntimeCore(
    const ManualMapLaunchOptions& options) noexcept;

}  // namespace cabbird::launcher
