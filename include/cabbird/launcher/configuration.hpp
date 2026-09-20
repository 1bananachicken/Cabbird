#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cabbird::launcher {

enum class AzurPromiliaClient : std::uint8_t {
    MainlandChina,
    Global,
};

struct LauncherClientConfiguration final {
    std::filesystem::path game_directory;
    std::filesystem::path launcher_executable;
};

struct LauncherConfiguration final {
    AzurPromiliaClient selected_client{AzurPromiliaClient::MainlandChina};
    LauncherClientConfiguration mainland_china;
    LauncherClientConfiguration global;

    [[nodiscard]] LauncherClientConfiguration& Selected() noexcept {
        return selected_client == AzurPromiliaClient::Global ? global : mainland_china;
    }

    [[nodiscard]] const LauncherClientConfiguration& Selected() const noexcept {
        return selected_client == AzurPromiliaClient::Global ? global : mainland_china;
    }
};

struct LauncherConfigurationLoadResult final {
    LauncherConfiguration configuration;
    bool loaded{};
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;
};

struct LauncherConfigurationSaveResult final {
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;

    [[nodiscard]] bool Ok() const noexcept { return win32_error == ERROR_SUCCESS; }
};

struct LauncherDiscoveryOptions final {
    AzurPromiliaClient client{AzurPromiliaClient::MainlandChina};
    std::filesystem::path payload_root;
    std::vector<std::filesystem::path> running_game_executables;
    std::vector<std::filesystem::path> running_launcher_executables;
    std::vector<std::filesystem::path> search_roots;
    bool include_system_locations{true};
    bool allow_unpaired_game_discovery{};
};

[[nodiscard]] std::filesystem::path LauncherConfigurationPath(
    const std::filesystem::path& payload_root) noexcept;
[[nodiscard]] LauncherConfigurationLoadResult LoadLauncherConfiguration(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] LauncherConfigurationSaveResult SaveLauncherConfiguration(
    const std::filesystem::path& path,
    const LauncherConfiguration& configuration) noexcept;

[[nodiscard]] bool IsAzurPromiliaGameDirectory(
    const std::filesystem::path& directory) noexcept;
[[nodiscard]] bool IsAzurPromiliaLauncherExecutable(
    const std::filesystem::path& executable, AzurPromiliaClient client) noexcept;
[[nodiscard]] LauncherClientConfiguration DiscoverLauncherConfiguration(
    const LauncherClientConfiguration& preferred,
    const LauncherDiscoveryOptions& options);

}  // namespace cabbird::launcher
