#pragma once

#include "cabbird/unitymem_compat.hpp"
#include "cabbird/pattern_service.hpp"
#include "cabbird/hook_manager.hpp"
#include "cabbird/i18n.hpp"
#include "cabbird/platform_ui_model.hpp"
#include "cabbird/platform_settings.hpp"
#include "cabbird/repository_coordinator.hpp"
#include <cabbird/sdk/version.h>
#include "cabbird/service_graph.hpp"
#include "cabbird/config.hpp"

#include <filesystem>
#include <functional>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>
#include <memory>
#include <stop_token>

namespace cabbird {
class UnityAdapter;
class StructuredLogger;
}

namespace cabbird {

class PluginManager;

enum class PlatformUiPerformanceStage : std::uint8_t {
    SubmissionLockWait,
    OperationLockWait,
    WindowState,
    RefreshCatalog,
    RuntimePlugins,
    BuildSnapshot,
    RepositorySnapshot,
    ServiceGraphSnapshot,
    AdapterServicesSnapshot,
    UnityCompatibilitySnapshot,
    ModelPublish,
    SettingsRefresh,
    RefreshTotal,
    FrameSetup,
    ManagementShell,
    Popups,
    WindowPersist,
    Count,
};

struct PlatformDiagnostics {
    std::shared_ptr<const cabbird::Translator> translator;
    std::string runtime_version{CABBIRD_SDK_VERSION_STRING};
    std::filesystem::path runtime_root;
    std::filesystem::path log_file;
    std::function<cabbird::ServiceGraphSnapshot()> service_graph;
    std::function<std::string()> profile_json;
    std::function<cabbird::UnityCompatibilitySnapshot()> unity_compatibility;
    std::function<std::vector<cabbird::HookRecordView>()> hooks;
    std::function<cabbird::RepositoryCoordinatorSnapshot()> repository_snapshot;
    std::function<cabbird::RepositoryOperationSubmission()> repository_refresh;
    std::function<cabbird::RepositoryOperationSubmission(
        std::string_view plugin_id, std::string_view version)> repository_install;
    std::function<cabbird::RepositoryOperationSubmission(
        std::string_view plugin_id)> repository_uninstall;
    std::function<cabbird::PluginRepositoryConfig()> repository_config;
    std::function<cabbird::RepositoryOperationSubmission(
        const cabbird::PluginRepositoryConfig&)> repository_configure;
    std::function<cabbird::PlatformSettingsSnapshot()> settings_snapshot;
    std::function<cabbird::PlatformSettingsApplyResult(
        const cabbird::PlatformSettingsApplyRequest&)> settings_apply;
    std::function<bool(std::string_view route)> settings_record_route;
    // Phase 10 production evidence. The capture provider gates the GPU
    // readback work; the remaining callbacks are cheap no-op observations
    // when the evidence session is idle.
    // Zero means idle; a new non-zero token identifies every capture window.
    std::function<std::uint64_t()> capture_generation;
    std::function<void(
        std::uint64_t capture_generation,
        std::uint32_t thread_id)> render;
    std::function<void(
        std::uint64_t capture_generation,
        std::uint32_t thread_id,
        std::chrono::nanoseconds latency,
        bool success)> resize;
    std::function<void(
        std::uint64_t capture_generation,
        bool non_empty,
        bool success)> pixel_probe;
    // Called by the validated game tick anchor before plugin Update callbacks.
    // This binds and drains the RuntimeDispatchers Game domain on that thread.
    std::function<std::size_t()> game_pump;
    // Mutation work is routed to the lifecycle domain once the production
    // composition root publishes an invoker. Standalone fixtures may leave
    // this empty and use the local fallback executor.
    std::function<std::uint32_t(std::function<void()>)> lifecycle_invoke;
    // Render callbacks use a fire-and-drain submission path so they never
    // synchronously wait for plugin load/reload or lifecycle I/O.
    std::function<std::uint32_t(std::function<void()>)> lifecycle_post;
    // Called during UI teardown after new actions are gated. It waits for
    // lifecycle invocations that may have outlived their bounded caller.
    std::function<bool(std::chrono::milliseconds)> lifecycle_drain;
    std::function<void(
        PlatformUiPerformanceStage,
        std::chrono::steady_clock::duration)> performance_probe;
    std::function<bool()> performance_probe_enabled;
    std::shared_ptr<cabbird::StructuredLogger> logger;
};

void RunPlatform(
    const std::filesystem::path& root,
    const AnalyzerConfig& config,
    std::stop_token stop_token = {},
    cabbird::CoreMemoryServices memory_services = {},
    std::shared_ptr<cabbird::UnityAdapter> adapter = {},
    PlatformDiagnostics diagnostics = {},
    std::shared_ptr<PluginManager> plugins = {});
void RunEmbeddedPlatform(
    const std::filesystem::path& root,
    const AnalyzerConfig& config,
    std::stop_token stop_token = {},
    cabbird::CoreMemoryServices memory_services = {},
    std::shared_ptr<cabbird::UnityAdapter> adapter = {},
    PlatformDiagnostics diagnostics = {},
    std::shared_ptr<PluginManager> plugins = {});
[[nodiscard]] bool InitializePlatformUi(
    PluginManager& plugins,
    PlatformDiagnostics diagnostics,
    std::shared_ptr<PluginManager> plugin_owner);
// Returns true only when the host-owned management shell changed to open.
[[nodiscard]] bool RevealPlatformUi() noexcept;
// Consumes a game-thread request on the UI thread, restores a closed
// management shell, and expands it. Returns true when a request was consumed.
[[nodiscard]] bool ApplyHostUiManagementExpansionRequest() noexcept;
// Uploads management-shell resources after the plugin scopes are prepared and
// before ImGui begins the next frame.
void PreparePlatformUiResources() noexcept;
void DrawPlatformUi();
// The renderer consumes the active menu key while this is true, but must not
// collapse the surface before the settings recorder accepts that key.
[[nodiscard]] bool PlatformUiCapturingHotkey() noexcept;
// Flushes intents and deferred memory work after the render lock is released.
void FlushPlatformUiActions();
// Returns true only when the active UI owner has drained all callbacks and
// its ImGui context can be destroyed by the caller. A quarantined owner (or a
// shutdown handoff still in progress) returns false; the caller must retain
// the host generation instead of tearing down the context.
[[nodiscard]] bool ShutdownPlatformUi();
// Closes and retires the active owner without waiting for callbacks. The
// owner remains reachable in quarantine while late callbacks release captures.
// A true result means the handoff to quarantine was recorded; it does not
// authorize destruction of the ImGui context or host generation.
[[nodiscard]] bool QuarantinePlatformUi(
    std::chrono::milliseconds wait_timeout = std::chrono::milliseconds(100)) noexcept;
[[nodiscard]] bool PlatformUiQuarantined(const PluginManager* owner = nullptr) noexcept;
[[nodiscard]] bool StandaloneHostQuarantined(const PluginManager* owner = nullptr) noexcept;
[[nodiscard]] bool PlatformHostQuarantined(const PluginManager* owner = nullptr) noexcept;

}  // namespace cabbird
