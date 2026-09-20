#pragma once

#include "cabbird/pattern_service.hpp"
#include "cabbird/plugin_scope.hpp"
#include "cabbird/sdk/services/interop.h"
#include "cabbird/sdk/services/platform.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace cabbird {

class HookBackend;

struct ScopedPluginServiceOwner final {
    std::shared_ptr<PluginScope> scope;
    std::filesystem::path state_directory;
    std::filesystem::path configuration_directory;
};

struct ScopedPlatformResourceCounts final {
    std::size_t configs{};
    std::size_t self_tests{};
    std::size_t tasks{};
    std::size_t commands{};
    std::size_t notifications{};
    std::size_t hooks{};
    std::size_t patches{};
};

struct ScopedPlatformDiagnosticsView final {
    std::size_t ledger_resources{};
    ScopedPlatformResourceCounts resources;
    std::size_t queued_tasks{};
    std::uint64_t callback_calls{};
    std::uint64_t callback_faults{};
    std::uint64_t slow_callbacks{};
};

enum class ScopedPlatformRevokePhase : std::uint8_t {
    PreStop,
    Final,
};

// Host-owned service implementation for one PluginManager.  The SDK tables are
// per-plugin views, but their records live here so a resource can be revoked by
// the PluginScope ledger before an old DLL generation is released.
class ScopedPlatformServices final {
public:
    class Impl;

    ScopedPlatformServices(
        CoreMemoryServices memory_services,
        std::shared_ptr<ResourceLedger> ledger);
    ScopedPlatformServices(
        CoreMemoryServices memory_services,
        std::shared_ptr<ResourceLedger> ledger,
        std::unique_ptr<HookBackend> hook_backend);
    ~ScopedPlatformServices();

    ScopedPlatformServices(const ScopedPlatformServices&) = delete;
    ScopedPlatformServices& operator=(const ScopedPlatformServices&) = delete;

    [[nodiscard]] CabbirdStatusV1 RegisterConfigSchema(
        const ScopedPluginServiceOwner& owner,
        std::string_view schema_id,
        std::uint32_t schema_version,
        CabbirdByteSpanV1 schema_json,
        CabbirdGenerationHandleV1* handle) noexcept;
    [[nodiscard]] CabbirdStatusV1 UnregisterConfigSchema(
        const ScopedPluginServiceOwner& owner, CabbirdGenerationHandleV1 handle) noexcept;
    [[nodiscard]] CabbirdStatusV1 ReadConfig(
        const ScopedPluginServiceOwner& owner,
        std::string_view schema_id,
        std::uint32_t* schema_version,
        CabbirdMutableByteSpanV1 destination,
        std::size_t* inout_size) noexcept;
    [[nodiscard]] CabbirdStatusV1 WriteConfig(
        const ScopedPluginServiceOwner& owner,
        std::string_view schema_id,
        std::uint32_t schema_version,
        CabbirdByteSpanV1 document) noexcept;
    [[nodiscard]] CabbirdStatusV1 MigrateConfig(
        const ScopedPluginServiceOwner& owner,
        std::string_view schema_id,
        CabbirdConfigMigrationV1 migration,
        void* migration_user) noexcept;

    [[nodiscard]] CabbirdStatusV1 ReadStorage(
        const ScopedPluginServiceOwner& owner,
        std::string_view relative_path,
        CabbirdMutableByteSpanV1 destination,
        std::size_t* inout_size) noexcept;
    [[nodiscard]] CabbirdStatusV1 WriteStorage(
        const ScopedPluginServiceOwner& owner,
        std::string_view relative_path,
        CabbirdByteSpanV1 source) noexcept;
    [[nodiscard]] CabbirdStatusV1 RemoveStorage(
        const ScopedPluginServiceOwner& owner, std::string_view relative_path) noexcept;

    [[nodiscard]] CabbirdStatusV1 RuntimeInfo(
        const ScopedPluginServiceOwner& owner, CabbirdRuntimeInfoV1* snapshot) noexcept;
    [[nodiscard]] CabbirdStatusV1 RuntimeVersion(
        char* destination, std::size_t* inout_size) noexcept;

    [[nodiscard]] CabbirdStatusV1 RegisterSelfTest(
        const ScopedPluginServiceOwner& owner,
        std::string_view id,
        CabbirdDiagnosticSelfTestV1 callback,
        void* callback_user,
        CabbirdGenerationHandleV1* handle) noexcept;
    [[nodiscard]] CabbirdStatusV1 UnregisterSelfTest(
        const ScopedPluginServiceOwner& owner, CabbirdGenerationHandleV1 handle) noexcept;
    [[nodiscard]] CabbirdStatusV1 RunSelfTest(
        const ScopedPluginServiceOwner& owner,
        std::string_view id,
        CabbirdMutableByteSpanV1 destination,
        std::size_t* inout_size) noexcept;
    [[nodiscard]] CabbirdStatusV1 DiagnosticsSnapshot(
        const ScopedPluginServiceOwner& owner,
        CabbirdMutableByteSpanV1 destination,
        std::size_t* inout_size) noexcept;
    // Host-only typed view used by the manager, pipe, and UI. It never calls
    // plugin code and is safe to collect while worker callbacks are queued.
    [[nodiscard]] ScopedPlatformDiagnosticsView Snapshot(
        const ScopedPluginServiceOwner& owner) const noexcept;

    [[nodiscard]] CabbirdStatusV1 Schedule(
        const ScopedPluginServiceOwner& owner,
        std::uint32_t delay_milliseconds,
        CabbirdTaskCallbackV1 callback,
        void* callback_user,
        CabbirdGenerationHandleV1* handle) noexcept;
    [[nodiscard]] CabbirdStatusV1 CancelTask(
        const ScopedPluginServiceOwner& owner, CabbirdGenerationHandleV1 handle) noexcept;

    [[nodiscard]] CabbirdStatusV1 RegisterCommand(
        const ScopedPluginServiceOwner& owner,
        std::string_view name,
        std::string_view description,
        CabbirdCommandCallbackV1 callback,
        void* callback_user,
        CabbirdGenerationHandleV1* handle) noexcept;
    [[nodiscard]] CabbirdStatusV1 UnregisterCommand(
        const ScopedPluginServiceOwner& owner, CabbirdGenerationHandleV1 handle) noexcept;
    [[nodiscard]] CabbirdStatusV1 InvokeCommand(
        const ScopedPluginServiceOwner& owner,
        std::string_view name,
        std::string_view arguments,
        CabbirdMutableByteSpanV1 destination,
        std::size_t* inout_size) noexcept;

    [[nodiscard]] CabbirdStatusV1 PostNotification(
        const ScopedPluginServiceOwner& owner,
        CabbirdNotificationSeverityV1 severity,
        std::string_view title,
        std::string_view body,
        std::uint32_t timeout_milliseconds,
        CabbirdGenerationHandleV1* handle) noexcept;
    [[nodiscard]] CabbirdStatusV1 DismissNotification(
        const ScopedPluginServiceOwner& owner, CabbirdGenerationHandleV1 handle) noexcept;

    [[nodiscard]] CabbirdStatusV1 ResolveSignature(
        const ScopedPluginServiceOwner& owner,
        std::string_view module_name,
        std::string_view section_name,
        std::string_view pattern,
        std::uintptr_t* address) noexcept;
    [[nodiscard]] CabbirdStatusV1 CreateHook(
        const ScopedPluginServiceOwner& owner,
        const CabbirdHookRequestV1* request,
        std::uintptr_t* original,
        CabbirdGenerationHandleV1* handle) noexcept;
    [[nodiscard]] CabbirdStatusV1 ReleaseHook(
        const ScopedPluginServiceOwner& owner, CabbirdGenerationHandleV1 handle) noexcept;
    [[nodiscard]] CabbirdStatusV1 BeginHookCallback(
        const ScopedPluginServiceOwner& owner,
        CabbirdGenerationHandleV1 hook,
        CabbirdGenerationHandleV1* callback_lease) noexcept;
    [[nodiscard]] CabbirdStatusV1 EndHookCallback(
        const ScopedPluginServiceOwner& owner,
        CabbirdGenerationHandleV1 callback_lease) noexcept;
    [[nodiscard]] CabbirdStatusV1 ApplyPatch(
        const ScopedPluginServiceOwner& owner,
        std::uintptr_t address,
        CabbirdByteSpanV1 replacement,
        std::string_view label,
        CabbirdGenerationHandleV1* handle) noexcept;
    [[nodiscard]] CabbirdStatusV1 ReleasePatch(
        const ScopedPluginServiceOwner& owner, CabbirdGenerationHandleV1 handle) noexcept;

    // Stops tracked hooks before releasing their outer scope records. PreStop
    // retains Config schemas so an on_stop lifecycle callback can persist its
    // final settings; Final revokes every remaining scoped resource before
    // on_unload. A failed hook drain/removal leaves its record intact so the
    // caller can quarantine the generation instead of unmapping its DLL.
    [[nodiscard]] bool RevokeScope(
        const ScopedPluginServiceOwner& owner,
        std::chrono::steady_clock::time_point deadline,
        ScopedPlatformRevokePhase phase = ScopedPlatformRevokePhase::Final) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabbird
