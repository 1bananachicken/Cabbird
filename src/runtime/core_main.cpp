#include "cabbird/unitymem_compat.hpp"
#include "cabbird/analyzer.hpp"
#include "cabbird/json.hpp"
#include "cabbird/core_api.hpp"
#include "cabbird/adapter_service_registry.hpp"
#include "cabbird/crash_reporter.hpp"
#include "cabbird/diagnostic_pipe_service.hpp"
#include "cabbird/i18n.hpp"
#include "cabbird/unity_profile_runtime.hpp"
#include "cabbird/platform_settings.hpp"
#include "cabbird/repository_coordinator.hpp"
#include "cabbird/runtime_crash_coordinator.hpp"
#include "cabbird/runtime_recovery.hpp"
#include "cabbird/runtime_session.hpp"
#include "cabbird/sdk/version.h"
#include "cabbird/service_graph.hpp"
#include "cabbird/service_graph_diagnostics.hpp"
#include "cabbird/structured_logger.hpp"
#include "cabbird/config.hpp"
#include "cabbird/pipe_server.hpp"
#include "cabbird/platform_host.hpp"
#include "cabbird/plugin_manager.hpp"
#include "cabbird/unity_services.hpp"
#include "cabbird/host_ui_service.hpp"
#include "build_provenance.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

struct CoreContext {
    std::filesystem::path runtime_root;
    std::filesystem::path log_directory;
    unitymem::AnalyzerConfig config;
    std::shared_ptr<const cabbird::Translator> translator;
    cabbird::CoreMemoryServices memory_services;
    std::unique_ptr<cabbird::CrashReporter> crash_reporter;
    std::shared_ptr<cabbird::DiagnosticPipeService> diagnostic_pipe;
    std::shared_ptr<cabbird::StructuredLogger> logger;
    std::shared_ptr<cabbird::PlatformSettingsStore> settings;
    HMODULE game_module{};
    std::shared_ptr<cabbird::UnityProfileRuntime> profile_runtime;
    std::shared_ptr<cabbird::RepositoryCoordinator> repository;
    std::string repository_diagnostics{
        "{\"schemaVersion\":1,\"state\":\"unavailable\",\"reason\":\"not-started\"}"};
    cabbird::RuntimeSafeModeState safe_mode;
    bool recovery_state_conservative{};
    mutable std::mutex recovery_mutex;
    std::unique_ptr<cabbird::RuntimeCrashCoordinatorClient> crash_coordinator;
    std::string crash_coordinator_state{"unavailable"};
    std::string recovery_diagnostics{
        "{\"schemaVersion\":1,\"state\":\"normal\",\"minimalCore\":false,"
        "\"thirdPartyPluginsSuspended\":false,\"profileOverridesSuspended\":false,"
        "\"reason\":\"\"}"};
    mutable std::mutex plugins_mutex;
    std::shared_ptr<unitymem::PluginManager> plugins;
    std::weak_ptr<cabbird::RuntimeSession> session;
    std::string plugin_stop_diagnostics{"[]"};
    std::string profile_diagnostics{"null"};
    std::weak_ptr<cabbird::ServiceGraph> services;
};

std::string EscapeJson(std::string_view value);

std::string RecoveryDiagnosticsJson(const CoreContext& context) {
    const auto& safe_mode = context.safe_mode;
    const char* state = context.recovery_state_conservative
        ? "conservative" : safe_mode.Active() ? "safe-mode" : "normal";
    return "{\"schemaVersion\":1,\"state\":\"" + std::string(state) +
        "\",\"minimalCore\":" + (safe_mode.minimal_core ? "true" : "false") +
        ",\"thirdPartyPluginsSuspended\":" +
        (safe_mode.third_party_plugins_suspended ? "true" : "false") +
        ",\"profileOverridesSuspended\":" +
        (safe_mode.profile_overrides_suspended ? "true" : "false") +
        ",\"crashCoordinator\":\"" + EscapeJson(context.crash_coordinator_state) + "\"" +
        ",\"reason\":\"" + EscapeJson(safe_mode.reason) + "\"}";
}

std::string RecoveryDiagnosticsSnapshot(const CoreContext& context) {
    std::scoped_lock lock(context.recovery_mutex);
    return context.recovery_diagnostics;
}

struct RuntimeControl {
    std::mutex mutex;
    std::shared_ptr<cabbird::RuntimeSession> session;
    cabbird::RuntimeSessionSnapshot last_snapshot;
};

HMODULE g_core_module{};
std::unique_ptr<RuntimeControl> g_runtime_control = std::make_unique<RuntimeControl>();

DWORD CurrentExceptionError() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (const std::filesystem::filesystem_error&) {
        return ERROR_PATH_NOT_FOUND;
    } catch (const std::system_error&) {
        return ERROR_GEN_FAILURE;
    } catch (...) {
        return ERROR_UNHANDLED_EXCEPTION;
    }
}

std::filesystem::path ModulePath(HMODULE module) {
    if (module == nullptr) return {};
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
}

std::filesystem::path ModuleDirectory(HMODULE module) {
    return ModulePath(module).parent_path();
}

void RuntimeLog(
    const std::shared_ptr<CoreContext>& context,
    cabbird::LogLevel level,
    std::string event_id,
    std::string message,
    cabbird::LogThreadDomain thread_domain = cabbird::LogThreadDomain::Lifecycle) {
    std::ofstream(context->log_directory / L"cabbird-runtime.log", std::ios::app)
        << "pid=" << GetCurrentProcessId() << ' ' << message << '\n';
    if (context->logger == nullptr) return;
    cabbird::LogDetails details;
    details.thread_domain = thread_domain;
    details.event_id = std::move(event_id);
    static_cast<void>(context->logger->Log(
        level, "runtime", std::move(message), std::move(details)));
}

std::shared_ptr<unitymem::PluginManager> PluginHostSnapshot(
    const std::shared_ptr<CoreContext>& context) {
    std::scoped_lock lock(context->plugins_mutex);
    return context->plugins;
}

void PublishPluginHost(
    const std::shared_ptr<CoreContext>& context,
    std::shared_ptr<unitymem::PluginManager> plugins) {
    std::scoped_lock lock(context->plugins_mutex);
    context->plugins = std::move(plugins);
}

std::shared_ptr<unitymem::PluginManager> TakePluginHost(
    const std::shared_ptr<CoreContext>& context) {
    std::scoped_lock lock(context->plugins_mutex);
    return std::exchange(context->plugins, {});
}

bool WriteDiagnosticsSummary(
    const std::shared_ptr<CoreContext>& context,
    std::string_view runtime_state) noexcept {
    try {
        const std::filesystem::path state_directory = context->runtime_root / L"state";
        const std::filesystem::path destination = state_directory / L"diagnostics-summary.json";
        const std::filesystem::path temporary = state_directory /
            (L"diagnostics-summary.json.tmp-" + std::to_wstring(GetCurrentProcessId()));

        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        const std::string repository = context->repository == nullptr
            ? context->repository_diagnostics
            : cabbird::SerializeRepositoryCoordinatorSnapshotJson(
                  context->repository->Snapshot());
        output << "{\"schemaVersion\":1,\"runtimeVersion\":\""
               << CABBIRD_SDK_VERSION_STRING << "\",\"runtimeState\":\""
               << runtime_state << "\",\"profile\":"
               << context->profile_diagnostics << ",\"repository\":" << repository
               << ",\"recovery\":" << RecoveryDiagnosticsSnapshot(*context)
               << ",\"plugins\":"
               << context->plugin_stop_diagnostics << "}\n";
        output.flush();
        if (!output) {
            output.close();
            static_cast<void>(DeleteFileW(temporary.c_str()));
            return false;
        }
        output.close();
        if (!output || MoveFileExW(
                temporary.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
            static_cast<void>(DeleteFileW(temporary.c_str()));
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void LogDiagnosticsSummaryFailure(
    const std::shared_ptr<CoreContext>& context,
    cabbird::LogThreadDomain thread_domain = cabbird::LogThreadDomain::Lifecycle) {
    RuntimeLog(
        context, cabbird::LogLevel::Warning,
        "diagnostics.summary.write_failed", "diagnostics_summary=unavailable",
        thread_domain);
}

DWORD InitializeCore(const std::shared_ptr<CoreContext>& context, std::stop_token stop_token) {
    if (stop_token.stop_requested()) return ERROR_CANCELLED;

    std::error_code error;
    std::filesystem::create_directories(context->runtime_root / L"plugins", error);
    if (error) return ERROR_CANNOT_MAKE;
    std::filesystem::create_directories(context->log_directory, error);
    if (error) return ERROR_CANNOT_MAKE;
    std::filesystem::create_directories(context->runtime_root / L"state", error);
    if (error) return ERROR_CANNOT_MAKE;
    std::filesystem::create_directories(context->runtime_root / L"config", error);
    if (error) return ERROR_CANNOT_MAKE;

    context->settings = std::make_shared<cabbird::PlatformSettingsStore>(context->runtime_root);
    static_cast<void>(context->settings->Start());
    const auto platform_settings = context->settings->Snapshot();

    std::string logger_failure{"startup exception"};
    try {
        cabbird::StructuredLoggerOptions logger_options;
        if (platform_settings.ready) {
            logger_options.ring_capacity = platform_settings.values.diagnostics_ring_capacity;
            logger_options.minimum_level = static_cast<cabbird::LogLevel>(
                platform_settings.values.diagnostics_log_level);
        }
        logger_options.max_file_size_bytes = 16U * 1024U * 1024U;
        logger_options.retained_archive_count = 4;
        auto logger = std::make_shared<cabbird::StructuredLogger>(std::move(logger_options));
        if (logger->Start(context->log_directory / L"cabbird-runtime.jsonl")) {
            context->logger = std::move(logger);
        } else if (const auto failure = logger->LastError()) {
            logger_failure = "operation=" +
                std::to_string(static_cast<unsigned>(failure->operation)) +
                " code=" + std::to_string(failure->code.value()) +
                " message=" + failure->message;
        }
    } catch (const std::exception& exception) {
        logger_failure = exception.what();
    } catch (...) {
    }
    if (context->logger == nullptr) {
        std::ofstream(context->log_directory / L"cabbird-runtime.log", std::ios::app)
            << "pid=" << GetCurrentProcessId() << " structured_logger=disabled error="
            << logger_failure << '\n';
    }

    auto crash_reporter = std::make_unique<cabbird::CrashReporter>(
        cabbird::CrashReporterOptions{
            context->runtime_root / L"crashes", CABBIRD_SDK_VERSION_STRING});
    std::string crash_error;
    if (!crash_reporter->Install(&crash_error)) {
        RuntimeLog(
            context, cabbird::LogLevel::Warning, "crash_reporter.disabled",
            "crash_reporter=disabled error=" + crash_error);
    } else {
        context->crash_reporter = std::move(crash_reporter);
    }

    context->config = unitymem::AnalyzerConfig::Load(context->runtime_root / L"cabbird.ini");
    for (const auto& diagnostic : context->config.diagnostics) {
        RuntimeLog(
            context, cabbird::LogLevel::Warning, "config.invalid_value",
            "config_key=" + diagnostic.key + " reason=" + diagnostic.message);
    }
    const auto locale = cabbird::ResolveUserLocale(context->config.platform_language);
    if (locale.system_query_failed) {
        RuntimeLog(
            context, cabbird::LogLevel::Warning, "i18n.system_locale_unavailable",
            "requested_locale=auto fallback_locale=en-US");
    }
    auto translator = cabbird::LoadHostCatalog(
        locale.locale, context->runtime_root / L"locales" / L"host");
    for (const auto& diagnostic : translator.diagnostics) {
        RuntimeLog(
            context, cabbird::LogLevel::Warning, "i18n.host_catalog_invalid",
            "catalog_path=" + diagnostic.path + " reason=" + diagnostic.message);
    }
    context->translator = std::move(translator.translator);
    cabbird::RuntimeRecoveryStore recovery(context->runtime_root);
    const auto recovery_state = recovery.Load();
    if (recovery_state.Ok()) {
        context->safe_mode = recovery_state.state->safe_mode;
    } else if (recovery_state.error != cabbird::RuntimeRecoveryError::StateUnavailable) {
        context->safe_mode.minimal_core = true;
        context->safe_mode.reason = recovery_state.message.empty()
            ? "Runtime recovery state is unavailable"
            : "Runtime recovery state rejected: " + recovery_state.message;
        context->recovery_state_conservative = true;
    }

    const auto session = context->session.lock();
    const auto generation = session == nullptr ? 0 : session->Snapshot().generation;
    std::filesystem::path coordinator_executable =
        ModuleDirectory(g_core_module) / L"CabbirdCrashCoordinator.exe";
    if (!std::filesystem::is_regular_file(coordinator_executable)) {
        coordinator_executable = context->runtime_root / L"CabbirdCrashCoordinator.exe";
    }
    auto crash_coordinator = std::make_unique<cabbird::RuntimeCrashCoordinatorClient>(
        cabbird::RuntimeCrashCoordinatorOptions{
            context->runtime_root,
            std::move(coordinator_executable),
            GetCurrentProcessId(),
            generation,
            CABBIRD_SDK_VERSION_STRING});
    const auto coordinator_started = crash_coordinator->Start();
    if (coordinator_started.Ok()) {
        context->crash_coordinator_state = "monitoring";
        context->crash_coordinator = std::move(crash_coordinator);
    } else {
        context->crash_coordinator_state =
            std::string(cabbird::RuntimeCrashCoordinatorErrorName(
                coordinator_started.error));
        RuntimeLog(
            context, cabbird::LogLevel::Warning,
            "runtime.crash_coordinator_unavailable",
            "crash_coordinator=unavailable reason=" + coordinator_started.message);
    }
    {
        std::scoped_lock lock(context->recovery_mutex);
        context->recovery_diagnostics = RecoveryDiagnosticsJson(*context);
    }
    context->profile_diagnostics = "null";
    if (!WriteDiagnosticsSummary(context, "starting")) {
        LogDiagnosticsSummaryFailure(context);
    }
    RuntimeLog(
        context, cabbird::LogLevel::Info, "runtime.start",
        "runtime=start version=" CABBIRD_SDK_VERSION_STRING);
    if (context->safe_mode.Active()) {
        RuntimeLog(
            context, cabbird::LogLevel::Warning, "runtime.safe_mode",
            "runtime_safe_mode=" + RecoveryDiagnosticsSnapshot(*context));
    }
    return stop_token.stop_requested() ? ERROR_CANCELLED : ERROR_SUCCESS;
}

DWORD PrepareRepository(
    const std::shared_ptr<CoreContext>& context, std::stop_token stop_token) {
    if (stop_token.stop_requested()) return ERROR_CANCELLED;
    cabbird::RepositoryCoordinatorOptions options;
    options.runtime_root = context->runtime_root;
    options.plugin_directory = context->config.plugin_directory;
    options.game = context->config.game_id;
    options.api_major = CABBIRD_PLUGIN_API_V1_MAJOR;
    if (context->settings != nullptr) {
        const auto settings = context->settings->Snapshot();
        if (settings.ready) options.automatic_refresh = settings.values.updates_automatic_check;
    }
    auto repository = std::make_shared<cabbird::RepositoryCoordinator>(std::move(options));
    const bool started = repository->Start();
    const auto snapshot = repository->Snapshot();
    context->repository = std::move(repository);
    context->repository_diagnostics =
        cabbird::SerializeRepositoryCoordinatorSnapshotJson(snapshot);
    RuntimeLog(
        context,
        started ? cabbird::LogLevel::Info : cabbird::LogLevel::Warning,
        "repository.coordinator",
        "repository=" + std::string(cabbird::RepositoryCoordinatorStateName(snapshot.state)) +
            " sources=" + std::to_string(snapshot.configured_sources) +
            " plugins=" + std::to_string(snapshot.plugins.size()) +
            " reason=" + snapshot.reason,
        cabbird::LogThreadDomain::Worker);
    if (!started) return ERROR_GEN_FAILURE;
    return stop_token.stop_requested() ? ERROR_CANCELLED : ERROR_SUCCESS;
}

void StopRepository(const std::shared_ptr<CoreContext>& context) noexcept {
    auto repository = std::exchange(context->repository, {});
    if (repository != nullptr) {
        context->repository_diagnostics =
            cabbird::SerializeRepositoryCoordinatorSnapshotJson(repository->Snapshot());
        repository->Stop();
    }
}

DWORD PrepareDiagnosticPipe(
    const std::shared_ptr<CoreContext>& context,
    const std::weak_ptr<cabbird::ServiceGraph>& services,
    std::stop_token stop_token) {
    if (stop_token.stop_requested()) return ERROR_CANCELLED;
    const auto pipe_name =
        unitymem::BuildPipeName(context->config.pipe_prefix, GetCurrentProcessId());
    auto analyzer = std::make_shared<const unitymem::Analyzer>(
        context->runtime_root,
        context->config,
        [context, services] {
            const auto graph = services.lock();
            std::string runtime = graph == nullptr
                ? std::string("null")
                : cabbird::SerializeServiceGraphSnapshotJson(graph->Snapshot());
            std::string plugin_diagnostics{"{\"schemaVersion\":1,\"plugins\":[]}"};
            try {
                if (const auto plugins = PluginHostSnapshot(context)) {
                    plugin_diagnostics = plugins->DiagnosticsJson();
                }
            } catch (...) {
                plugin_diagnostics = "{\"schemaVersion\":1,\"plugins\":[]}";
            }
            if (runtime.empty() || runtime.back() != '}') return runtime;
            runtime.pop_back();
            const std::string repository = context->repository == nullptr
                ? context->repository_diagnostics
                : cabbird::SerializeRepositoryCoordinatorSnapshotJson(
                      context->repository->Snapshot());
            runtime += ",\"repository\":" + repository +
                ",\"recovery\":" + RecoveryDiagnosticsSnapshot(*context) +
                // The dump service's own view of the walk.  Added because "the progress
                // shows all zeros" has two causes that the plugin CANNOT distinguish from
                // the outside: the walk is genuinely stalled before its first report, or the
                // plugin's polling is broken.  Reading the PRODUCER directly separates them
                // -- `elapsedMs` advancing while `classes` stays 0 localises the stall to
                // the walk, not to the reporting.
                ",\"unity_dump\":" + [context] {
                    try {
                        const cabbird::HostDumpStats stats =
                            cabbird::SnapshotHostUnityDump();
                        return std::string("{\"state\":") + std::to_string(stats.state) +
                            ",\"claimed\":" + (stats.claimed ? "true" : "false") +
                            ",\"classes\":" + std::to_string(stats.classes) +
                            ",\"fields\":" + std::to_string(stats.fields) +
                            ",\"images\":" + std::to_string(stats.images) +
                            ",\"elapsedMs\":" + std::to_string(stats.elapsed_ms) + "}";
                    } catch (...) {
                        return std::string("null");
                    }
                }() +
                // The entity overlay source's own view, for the same reason the dump service's is
                // here: "0 entities" has several causes that a plugin cannot tell apart from
                // the outside, and the producer's own words are the only way to separate
                // "not in a battle yet" from "the binding is wrong".
                //
                // The camera fields are here because the camera is the OTHER half of the ESP
                // and it is probed on the same tick; without them, "the boxes do not appear"
                // is again two indistinguishable failures.
                ",\"unity_entity_overlay\":" + [] {
                    try {
                        const cabbird::HostEntityStats stats =
                            cabbird::SnapshotHostEntities();
                        return std::string("{\"entities\":") +
                            std::to_string(stats.entities) +
                            ",\"generation\":" + std::to_string(stats.generation) +
                            ",\"classResolved\":" +
                            (stats.class_resolved ? "true" : "false") +
                            ",\"reason\":" + cabbird::json::Quote(
                                stats.unavailable_reason == nullptr
                                    ? std::string_view{}
                                    : std::string_view{stats.unavailable_reason}) +
                            ",\"cameraAnchorResolved\":" +
                            (stats.camera_anchor_resolved ? "true" : "false") +
                            ",\"cameraMatrixValid\":" +
                            (stats.camera_matrix_valid ? "true" : "false") +
                            // The self-check: a point the solve did NOT use, projected through
                            // the finished matrix versus the camera's own answer.  "Valid" and
                            // "correct" are different claims, and this is the one that
                            // separates them without a screenshot.
                            ",\"cameraCheckOk\":" +
                            (stats.camera_check_ok ? "true" : "false") +
                            ",\"cameraCheckErrorPixels\":" +
                            std::to_string(stats.camera_check_error_pixels) +
                            ",\"mainCamera\":\"" + [&stats] {
                                char buffer[32]{};
                                std::snprintf(buffer, sizeof(buffer), "0x%llX",
                                              static_cast<unsigned long long>(
                                                  stats.main_camera_object));
                                return std::string(buffer);
                            }() + "\",\"mainCameraNative\":\"" + [&stats] {
                                char buffer[32]{};
                                std::snprintf(buffer, sizeof(buffer), "0x%llX",
                                              static_cast<unsigned long long>(
                                                  stats.main_camera_native));
                                return std::string(buffer);
                            }() + "\",\"cameraReason\":" + cabbird::json::Quote(
                                stats.camera_reason == nullptr
                                    ? std::string_view{}
                                    : std::string_view{stats.camera_reason}) +
                            ",\"cameraRaw\":" + cabbird::json::Quote(
                                stats.camera_raw == nullptr
                                    ? std::string_view{}
                                    : std::string_view{stats.camera_raw}) +
                            ",\"cameraCrossCheck\":" + cabbird::json::Quote(
                                stats.camera_cross_check == nullptr
                                    ? std::string_view{}
                                    : std::string_view{stats.camera_cross_check}) +
                            ",\"projectedEntities\":" + std::to_string(stats.projected_entities) +
                            ",\"fullMaskEntities\":" + std::to_string(stats.full_mask_entities) +
                            ",\"totalEntities\":" + std::to_string(stats.total_entities) +
                            ",\"tickMicros\":" + std::to_string(stats.tick_micros) +
                            ",\"cameraMicros\":" + std::to_string(stats.camera_micros) +
                            ",\"walkMicros\":" + std::to_string(stats.walk_micros) +
                            ",\"labelsResolved\":" +
                            (stats.labels_resolved ? "true" : "false") + ",\"labelReason\":" +
                            cabbird::json::Quote(stats.label_reason == nullptr
                                                     ? std::string_view{}
                                                     : std::string_view{stats.label_reason}) +
                            ",\"campStage\":" + cabbird::json::Quote(stats.camp_stage == nullptr
                                                            ? std::string_view{}
                                                            : std::string_view{stats.camp_stage}) +
                            ",\"nameProbe\":" + cabbird::json::Quote(stats.name_probe == nullptr
                                                            ? std::string_view{}
                                                            : std::string_view{stats.name_probe}) +
                            ",\"positionReads\":" + std::to_string(stats.position_reads) +
                            ",\"positionFailures\":" + std::to_string(stats.position_failures) +
                            ",\"positionMicros\":" + std::to_string(stats.position_micros) +
                            ",\"directPosition\":" +
                            cabbird::json::Quote(stats.direct_position == nullptr
                                                     ? std::string_view{}
                                                     : std::string_view{stats.direct_position}) +
                            ",\"directPositionReads\":" +
                            std::to_string(stats.direct_position_reads) +
                            ",\"directPositionFaults\":" +
                            std::to_string(stats.direct_position_faults) +
                            ",\"directPositionMicros\":" +
                            std::to_string(stats.direct_position_micros) +
                            ",\"labelReads\":" + std::to_string(stats.label_reads) +
                            ",\"labelCacheHits\":" + std::to_string(stats.label_cache_hits) +
                            ",\"labelMicros\":" + std::to_string(stats.label_micros) +
                            ",\"positionCacheHits\":" + std::to_string(stats.position_cache_hits) +
                            ",\"positionCacheRejects\":" + std::to_string(stats.position_cache_rejects) +
                            ",\"labelCacheRejects\":" + std::to_string(stats.label_cache_rejects) +
                            ",\"kinds\":" + cabbird::json::Quote(stats.kind_histogram == nullptr
                                                            ? std::string_view{}
                                                            : std::string_view{stats.kind_histogram}) +
                            ",\"dataClasses\":" + cabbird::json::Quote(stats.data_class_histogram == nullptr
                                                            ? std::string_view{}
                                                            : std::string_view{stats.data_class_histogram}) +
                            ",\"labelStages\":" + cabbird::json::Quote(stats.label_stage_summary == nullptr
                                                            ? std::string_view{}
                                                            : std::string_view{stats.label_stage_summary}) +
                            ",\"labelSamples\":" + cabbird::json::Quote(stats.label_samples == nullptr
                                                            ? std::string_view{}
                                                            : std::string_view{stats.label_samples}) +
                            ",\"campsKnown\":" + (stats.camps_known ? "true" : "false") +
                            ",\"campMonster\":" + std::to_string(stats.camp_monster) +
                            ",\"campPlayer\":" + std::to_string(stats.camp_player) + "}";
                    } catch (...) {
                        return std::string("null");
                    }
                }() +
                ",\"unity_player\":" + [] {
                    try {
                        const cabbird::HostUnityPlayerStats stats =
                            cabbird::SnapshotHostUnityPlayer();
                        return std::string("{\"live\":") +
                            (stats.live ? "true" : "false") +
                            ",\"entityId\":" + std::to_string(stats.entity_id) +
                            ",\"dataClass\":" + cabbird::json::Quote(stats.data_class) +
                            ",\"identityFromProximity\":" +
                            (stats.identity_from_proximity ? "true" : "false") +
                            ",\"position\":[" + std::to_string(stats.position[0]) + "," +
                            std::to_string(stats.position[1]) + "," +
                            std::to_string(stats.position[2]) + "]" +
                            ",\"distanceToCamera\":" +
                            std::to_string(stats.distance_to_camera) +
                            ",\"reason\":" + cabbird::json::Quote(
                                stats.reason == nullptr
                                    ? std::string_view{}
                                    : std::string_view{stats.reason}) + "}";
                    } catch (...) {
                        return std::string("null");
                    }
                }() +
                ",\"plugin_diagnostics\":" + plugin_diagnostics + '}';
            return runtime;
        },
        context->memory_services,
        [context] {
            return context->profile_runtime == nullptr
                ? std::string("{\"ok\":true,\"state\":\"no-profile\",\"symbols\":[],\"features\":[]}")
                : context->profile_runtime->DiagnosticsJson();
        },
        [](std::string_view arguments) {
            static_cast<void>(arguments);
            /* UnityProfileRuntime::ExecuteReflectionQuery was an ADAPTER operation, and the
             * adapter is not ported at all, so every
             * reflection query reports unavailable.  Upstream's exact wording is kept so a pipe
             * client sees the same error string it would when no adapter is present. */
            return std::string{"{\"ok\":false,\"error\":\"UE reflection queries are unavailable\"}"};
        });
    auto pipe = std::make_shared<cabbird::DiagnosticPipeService>(
        cabbird::PipeServiceOptions{std::move(analyzer), pipe_name});
    const DWORD result = pipe->Prepare();
    if (result != ERROR_SUCCESS) {
        RuntimeLog(
            context, cabbird::LogLevel::Error, "diagnostics.pipe_prepare_failed",
            "pipe_prepare=failed code=" + std::to_string(result));
        return ERROR_SUCCESS;
    }
    if (stop_token.stop_requested()) {
        pipe->Stop();
        return ERROR_CANCELLED;
    }
    std::string pipe_message{"pipe="};
    for (const auto character : pipe_name) {
        pipe_message.push_back(static_cast<char>(character));
    }
    RuntimeLog(
        context, cabbird::LogLevel::Info, "diagnostics.pipe_ready",
        std::move(pipe_message));
    context->diagnostic_pipe = std::move(pipe);
    return ERROR_SUCCESS;
}

DWORD RunDiagnosticPipe(
    const std::shared_ptr<CoreContext>& context, std::stop_token stop_token) {
    const auto pipe = context->diagnostic_pipe;
    if (pipe == nullptr) return ERROR_SUCCESS;
    const DWORD result = pipe->Run(stop_token);
    if (result != ERROR_SUCCESS && !stop_token.stop_requested()) {
        RuntimeLog(
            context, cabbird::LogLevel::Error, "diagnostics.pipe_run_failed",
            "pipe_run=failed code=" + std::to_string(result),
            cabbird::LogThreadDomain::Worker);
        return ERROR_SUCCESS;
    }
    return result;
}

void StopDiagnosticPipe(const std::shared_ptr<CoreContext>& context) noexcept {
    if (context->diagnostic_pipe != nullptr) context->diagnostic_pipe->Stop();
}

DWORD PrepareUnityProfile(
    const std::shared_ptr<CoreContext>& context,
    std::stop_token stop_token) {
    if (stop_token.stop_requested()) return ERROR_CANCELLED;
    if (context->safe_mode.minimal_core) {
        context->profile_diagnostics =
            "{\"state\":\"suspended\",\"reason\":\"minimal-core recovery mode\"}";
        RuntimeLog(
            context, cabbird::LogLevel::Warning, "profile.suspended",
            "profile=suspended reason=minimal-core");
        if (!WriteDiagnosticsSummary(context, "running")) {
            LogDiagnosticsSummaryFailure(context);
        }
        return ERROR_SUCCESS;
    }
    cabbird::UnityProfileRuntimeOptions options;
    options.runtime_root = context->runtime_root;
    options.start_frame_clock = context->config.platform_enabled && context->config.platform_embedded;
    options.game_id = context->config.game_id;
    options.game_module = context->game_module;
    options.snapshot_sampling.player_tick_interval = static_cast<std::uint32_t>((std::min)(
        context->config.player_snapshot_tick_interval,
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
    options.snapshot_sampling.entity_tick_interval = static_cast<std::uint32_t>((std::min)(
        context->config.entity_snapshot_tick_interval,
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
    options.snapshot_sampling.actor_tick_interval = static_cast<std::uint32_t>((std::min)(
        context->config.actor_tick_interval,
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
    options.snapshot_sampling.entity_direct_position = context->config.entity_direct_position;
    auto runtime = std::make_shared<cabbird::UnityProfileRuntime>(std::move(options));
    if (!runtime->Start(stop_token)) {
        return stop_token.stop_requested() ? ERROR_CANCELLED : ERROR_GEN_FAILURE;
    }
    if (stop_token.stop_requested()) {
        runtime->Stop();
        return ERROR_CANCELLED;
    }
    context->profile_diagnostics = runtime->DiagnosticsJson();
    if (!WriteDiagnosticsSummary(context, "running")) {
        LogDiagnosticsSummaryFailure(context);
    }
    RuntimeLog(
        context, cabbird::LogLevel::Info, "profile.ready",
        "profile=" + context->profile_diagnostics);
    context->profile_runtime = std::move(runtime);
    if (context->crash_coordinator != nullptr) {
        static_cast<void>(context->crash_coordinator->SetFailureContext(
            cabbird::RuntimeFailureSource::RuntimeStartup));
    }
    return ERROR_SUCCESS;
}

void StopUnityProfile(const std::shared_ptr<CoreContext>& context) noexcept {
    bool stopped{};
    if (context->profile_runtime != nullptr) {
        const auto runtime = context->profile_runtime;
        stopped = runtime->Stop(std::chrono::seconds(5));
        context->profile_diagnostics = runtime->DiagnosticsJson();
        if (!stopped) {
            RuntimeLog(
                context, cabbird::LogLevel::Warning, "profile.stop_deferred",
                "profile=game_tick_generation_quarantined");
        }
    }
    context->profile_runtime.reset();
}

std::string EscapeJson(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20) result += '?';
            else result.push_back(static_cast<char>(character));
            break;
        }
    }
    return result;
}

DWORD PreparePluginHost(
    const std::shared_ptr<CoreContext>& context, std::stop_token stop_token) {
    if (stop_token.stop_requested()) return ERROR_CANCELLED;
    try {
        const bool minimal_core = context->safe_mode.minimal_core;
        const bool suspend_third_party =
            context->safe_mode.third_party_plugins_suspended;
        auto plugins = std::make_shared<unitymem::PluginManager>(
            context->runtime_root, context->config.plugin_directory,
            context->memory_services,
            unitymem::PluginCallbackBudgets{
                context->config.update_slow_milliseconds,
                context->config.draw_slow_milliseconds},
            context->logger,
            [session = context->session](
                std::string owner, std::uint64_t generation,
                std::function<void()> callback) -> bool {
                const auto runtime = session.lock();
                if (runtime == nullptr || !callback) return false;
                return static_cast<bool>(runtime->Dispatchers().Post(
                    cabbird::ExecutionDomain::Lifecycle,
                    std::move(owner), generation, std::move(callback)));
            },
            [session = context->session](
                std::string owner, std::uint64_t generation,
                std::function<void()> callback) -> bool {
                const auto runtime = session.lock();
                if (runtime == nullptr || !callback) return false;
                return static_cast<bool>(runtime->Dispatchers().Post(
                    cabbird::ExecutionDomain::Worker,
                    std::move(owner), generation, std::move(callback)));
            },
            [session = context->session](
                const std::uint32_t affinity,
                std::string owner, std::uint64_t generation,
                std::function<void()> callback) -> bool {
                const auto runtime = session.lock();
                if (runtime == nullptr || !callback) return false;
                cabbird::ExecutionDomain domain{};
                switch (affinity) {
                case CABBIRD_IPC_AFFINITY_V1_LIFECYCLE:
                    domain = cabbird::ExecutionDomain::Lifecycle;
                    break;
                case CABBIRD_IPC_AFFINITY_V1_GAME:
                    domain = cabbird::ExecutionDomain::Game;
                    break;
                case CABBIRD_IPC_AFFINITY_V1_RENDER:
                    domain = cabbird::ExecutionDomain::Render;
                    break;
                default:
                    domain = cabbird::ExecutionDomain::Worker;
                    break;
                }
                return static_cast<bool>(runtime->Dispatchers().Post(
                    domain, std::move(owner), generation, std::move(callback)));
            },
            [minimal_core, suspend_third_party](
                const cabbird::PluginManifest& manifest) {
                if (minimal_core) return false;
                return !suspend_third_party ||
                    manifest.id.starts_with("cabbird.builtin.");
            },
            [weak = std::weak_ptr<CoreContext>(context)](
                std::string_view plugin_id,
                std::uint64_t generation,
                bool entering) {
                const auto current = weak.lock();
                if (current == nullptr || current->crash_coordinator == nullptr) return;
                if (entering) {
                    static_cast<void>(current->crash_coordinator->SetFailureContext(
                        cabbird::RuntimeFailureSource::PluginGeneration,
                        {}, std::string(plugin_id), generation));
                } else {
                    static_cast<void>(current->crash_coordinator->SetFailureContext(
                        cabbird::RuntimeFailureSource::RuntimeStartup));
                }
            });
        plugins->SetTranslator(context->translator);
        const auto session = context->session;
        plugins->SetQueuedCallbackCanceller(
            [session](std::string_view owner, std::uint64_t generation) {
                if (const auto runtime = session.lock()) {
                    static_cast<void>(runtime->Dispatchers().CancelOwnerGeneration(
                        owner, generation));
                }
            });
        plugins->LoadAll();
        PublishPluginHost(context, std::move(plugins));
        const auto published = PluginHostSnapshot(context);
        RuntimeLog(
            context, cabbird::LogLevel::Info, "plugin.host.ready",
            "plugin_host=ready count=" +
                std::to_string(published == nullptr ? 0 : published->Plugins().size()));
        return ERROR_SUCCESS;
    } catch (...) {
        RuntimeLog(
            context, cabbird::LogLevel::Error, "plugin.host.start_failed",
            "plugin_host=start_failed");
        return CurrentExceptionError();
    }
}

DWORD StopPluginHost(
    const std::shared_ptr<CoreContext>& context,
    std::chrono::milliseconds timeout) noexcept {
    // A bounded UI teardown may leave a lifecycle callback owner in
    // quarantine. Do not start unloading the corresponding PluginManager
    // until that owner has either drained or been retired; the owner keeps
    // the module mapping alive for late callbacks.
    const auto current = PluginHostSnapshot(context);
    if (current != nullptr && unitymem::PlatformHostQuarantined(current.get())) {
        RuntimeLog(
            context, cabbird::LogLevel::Warning, "plugin.host.stop_deferred",
            "plugin_host=stop_deferred reason=platform_ui_quarantine");
        return ERROR_TIMEOUT;
    }
    const auto plugins = TakePluginHost(context);
    if (plugins == nullptr) {
        // RuntimeSession invokes this callback before ServiceGraph::StopAll;
        // the plugin-host service callback can therefore arrive a second time.
        // Keep the diagnostics captured by the first stop instead of erasing
        // them when the ownership slot is already empty.
        return ERROR_SUCCESS;
    }
    bool stopped{};
    try {
        stopped = plugins->StopForRuntime(timeout);
        const auto diagnostics = plugins->StopDiagnostics();
        std::ostringstream json;
        json << '[';
        for (std::size_t index = 0; index < diagnostics.size(); ++index) {
            if (index != 0) json << ',';
            const auto& diagnostic = diagnostics[index];
            json << "{\"id\":\"" << EscapeJson(diagnostic.id)
                 << "\",\"generation\":" << diagnostic.generation
                 << ",\"drained\":" << (diagnostic.drained ? "true" : "false")
                 << ",\"timedOut\":" << (diagnostic.timed_out ? "true" : "false")
                 << ",\"inFlight\":" << diagnostic.in_flight_callbacks
                 << ",\"resources\":" << diagnostic.resources
                 << ",\"reason\":\"" << EscapeJson(diagnostic.reason) << "\"}";
        }
        json << ']';
        context->plugin_stop_diagnostics = json.str();
        RuntimeLog(
            context, stopped ? cabbird::LogLevel::Info : cabbird::LogLevel::Warning,
            "plugin.host.stopped",
            "plugin_host=stopped drained=" + std::to_string(stopped ? 1 : 0) +
                " generations=" + std::to_string(diagnostics.size()));
    } catch (...) {
        context->plugin_stop_diagnostics = "[]";
        RuntimeLog(
            context, cabbird::LogLevel::Error, "plugin.host.stop_failed",
            "plugin_host=stop_exception");
        return CurrentExceptionError();
    }
    return stopped ? ERROR_SUCCESS : ERROR_TIMEOUT;
}

DWORD RunPlatform(const std::shared_ptr<CoreContext>& context, std::stop_token stop_token) {
    if (stop_token.stop_requested() || !context->config.platform_enabled) return ERROR_SUCCESS;
    if (context->crash_coordinator != nullptr) {
        static_cast<void>(context->crash_coordinator->SetFailureContext(
            cabbird::RuntimeFailureSource::RenderInitialization));
    }
    unitymem::PlatformDiagnostics diagnostics;
    diagnostics.translator = context->translator;
    diagnostics.runtime_root = context->runtime_root;
    diagnostics.log_file = context->runtime_root / L"cabbird-platform.log";
    diagnostics.service_graph = [weak = context->services] {
        const auto graph = weak.lock();
        return graph ? graph->Snapshot() : cabbird::ServiceGraphSnapshot{};
    };
    diagnostics.profile_json = [weak = std::weak_ptr<cabbird::UnityProfileRuntime>(context->profile_runtime)] {
        const auto runtime = weak.lock();
        return runtime ? runtime->DiagnosticsJson() : std::string{"{\"state\":\"unavailable\"}"};
    };
    diagnostics.unity_compatibility =
        [profile = std::weak_ptr<cabbird::UnityProfileRuntime>(context->profile_runtime),
         game_id = context->config.game_id] {
            const auto runtime = profile.lock();
            if (runtime == nullptr) {
                cabbird::UnityProfileDocumentStatus document;
                document.game = game_id;
                document.error = "the profile runtime is not running";
                return cabbird::BuildUnityCompatibilitySnapshot(document);
            }
            // One source of truth: the profile document the frame clock binds against.  It is
            // the only per-build profile data this tree ships, so the page reports THAT
            // document's identity, hash and per-method binding results -- and reports a missing
            // or broken document as exactly that, instead of asking a second profile format
            // that never had a document to read.
            const auto current = runtime->Evidence();
            cabbird::UnityProfileDocumentStatus document;
            document.loaded = current.unity_profile.has_value();
            document.game = game_id;
            document.source = current.unity_profile_path.string();
            document.hash = current.unity_profile_hash;
            document.error = current.unity_profile_error;
            if (current.unity_profile.has_value()) {
                document.build_id = current.unity_profile->BuildId();
                document.assembly = current.unity_profile->GameAssemblyName();
            }
            document.methods.reserve(current.unity_methods.size());
            for (const auto& method : current.unity_methods) {
                document.methods.push_back({method.key, method.bound, method.how});
            }
            return cabbird::BuildUnityCompatibilitySnapshot(document);
        };
    diagnostics.hooks = [weak = std::weak_ptr<cabbird::UnityProfileRuntime>(context->profile_runtime)] {
        const auto runtime = weak.lock();
        return runtime ? runtime->Hooks() : std::vector<cabbird::HookRecordView>{};
    };
    diagnostics.repository_snapshot =
        [weak = std::weak_ptr<cabbird::RepositoryCoordinator>(context->repository)] {
            const auto repository = weak.lock();
            return repository == nullptr
                ? cabbird::RepositoryCoordinatorSnapshot{}
                : repository->Snapshot();
        };
    diagnostics.repository_refresh =
        [weak = std::weak_ptr<cabbird::RepositoryCoordinator>(context->repository)] {
            const auto repository = weak.lock();
            return repository == nullptr
                ? cabbird::RepositoryOperationSubmission{
                      false, 0, "repository coordinator is unavailable"}
                : repository->Refresh();
        };
    diagnostics.repository_install =
        [weak = std::weak_ptr<cabbird::RepositoryCoordinator>(context->repository)](
            std::string_view plugin_id, std::string_view version) {
            const auto repository = weak.lock();
            return repository == nullptr
                ? cabbird::RepositoryOperationSubmission{
                      false, 0, "repository coordinator is unavailable"}
                : repository->InstallPlugin(plugin_id, version);
        };
    diagnostics.repository_uninstall =
        [weak = std::weak_ptr<cabbird::RepositoryCoordinator>(context->repository)](
            std::string_view plugin_id) {
            const auto repository = weak.lock();
            return repository == nullptr
                ? cabbird::RepositoryOperationSubmission{
                      false, 0, "repository coordinator is unavailable"}
                : repository->UninstallPlugin(plugin_id);
        };
    diagnostics.repository_config =
        [weak = std::weak_ptr<cabbird::RepositoryCoordinator>(context->repository)] {
            const auto repository = weak.lock();
            return repository == nullptr ? cabbird::PluginRepositoryConfig{}
                                         : repository->Configuration();
        };
    diagnostics.repository_configure =
        [weak = std::weak_ptr<cabbird::RepositoryCoordinator>(context->repository)](
            const cabbird::PluginRepositoryConfig& config) {
            const auto repository = weak.lock();
            return repository == nullptr
                ? cabbird::RepositoryOperationSubmission{
                      false, 0, "repository coordinator is unavailable"}
                : repository->Configure(config);
        };
    diagnostics.settings_snapshot =
        [weak = std::weak_ptr<cabbird::PlatformSettingsStore>(context->settings)] {
            const auto settings = weak.lock();
            return settings == nullptr
                ? cabbird::PlatformSettingsSnapshot{}
                : settings->Snapshot();
        };
    diagnostics.settings_apply =
        [weak = std::weak_ptr<cabbird::PlatformSettingsStore>(context->settings),
         logger = std::weak_ptr<cabbird::StructuredLogger>(context->logger),
         repository = std::weak_ptr<cabbird::RepositoryCoordinator>(context->repository)](
            const cabbird::PlatformSettingsApplyRequest& request) {
            const auto settings = weak.lock();
            if (settings == nullptr) return cabbird::PlatformSettingsApplyResult{};
            auto result = settings->Apply(request);
            if (!result.Applied()) return result;
            if (const auto active_logger = logger.lock()) {
                static_cast<void>(active_logger->Reconfigure(
                    static_cast<cabbird::LogLevel>(
                        result.snapshot.values.diagnostics_log_level),
                    result.snapshot.values.diagnostics_ring_capacity));
            }
            if (const auto active_repository = repository.lock()) {
                static_cast<void>(active_repository->Refresh());
            }
            return result;
        };
    diagnostics.settings_record_route =
        [weak = std::weak_ptr<cabbird::PlatformSettingsStore>(context->settings)](
            const std::string_view route) {
            const auto settings = weak.lock();
            return settings != nullptr && settings->RecordLastRoute(route);
        };
    diagnostics.game_pump = [weak = context->session]() -> std::size_t {
        const auto session = weak.lock();
        return session == nullptr ? 0 : session->Dispatchers().PumpGame();
    };
    diagnostics.lifecycle_invoke = [context](std::function<void()> operation) -> std::uint32_t {
        const auto session = context->session.lock();
        if (session == nullptr) return ERROR_NOT_READY;
        const auto state = session->Snapshot().state;
        if (state == CABBIRD_RUNTIME_STATE_STOP_REQUESTED ||
            state == CABBIRD_RUNTIME_STATE_STOPPING_PLUGINS ||
            state == CABBIRD_RUNTIME_STATE_STOPPING_SERVICES ||
            state == CABBIRD_RUNTIME_STATE_FAILED ||
            state == CABBIRD_RUNTIME_STATE_STOPPED) {
            return ERROR_CANCELLED;
        }
        // UI submission is bounded; the lifecycle callback itself owns the
        // plugin serialization lock and reports a typed failure on timeout.
        return session->Dispatchers().Invoke(
            cabbird::ExecutionDomain::Lifecycle, std::move(operation), std::chrono::seconds(5));
    };
    diagnostics.lifecycle_post = [context](std::function<void()> operation) -> std::uint32_t {
        const auto session = context->session.lock();
        if (session == nullptr) return ERROR_NOT_READY;
        const auto state = session->Snapshot().state;
        if (state == CABBIRD_RUNTIME_STATE_STOP_REQUESTED ||
            state == CABBIRD_RUNTIME_STATE_STOPPING_PLUGINS ||
            state == CABBIRD_RUNTIME_STATE_STOPPING_SERVICES ||
            state == CABBIRD_RUNTIME_STATE_FAILED ||
            state == CABBIRD_RUNTIME_STATE_STOPPED) {
            return ERROR_CANCELLED;
        }
        const auto task = session->Dispatchers().Post(
            cabbird::ExecutionDomain::Lifecycle,
            "cabbird.platform.ui", 1, std::move(operation));
        return task ? ERROR_SUCCESS : ERROR_NOT_READY;
    };
    diagnostics.lifecycle_drain = [context](std::chrono::milliseconds timeout) -> bool {
        const auto session = context->session.lock();
        if (session == nullptr) return true;
        const auto bounded = (std::max)(timeout, std::chrono::milliseconds::zero());
        const auto deadline = bounded == std::chrono::milliseconds::max()
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + bounded;
        const auto remaining = [&] {
            if (deadline == std::chrono::steady_clock::time_point::max()) {
                return std::chrono::milliseconds::max();
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::chrono::milliseconds::zero();
            return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        };
        // The lifecycle pump may already be leaving RUNNING when the render
        // worker begins its teardown. Cancel queued UI posts before draining;
        // otherwise a stopped pump would strand their owner captures until
        // dispatcher destruction.
        static_cast<void>(session->Dispatchers().CancelOwnerGeneration(
            "cabbird.platform.ui", 1));
        if (!session->Dispatchers().Drain("cabbird.platform.ui", 1, remaining())) {
            return false;
        }
        return session->Dispatchers().DrainInvocations(remaining());
    };
    diagnostics.logger = context->logger;
    if (context->config.platform_embedded) {
        unitymem::RunEmbeddedPlatform(
            context->runtime_root, context->config, stop_token, context->memory_services,
            context->profile_runtime == nullptr ? nullptr : context->profile_runtime->Adapter(),
            std::move(diagnostics), PluginHostSnapshot(context));
    } else {
        unitymem::RunPlatform(
            context->runtime_root, context->config, stop_token, context->memory_services,
            context->profile_runtime == nullptr ? nullptr : context->profile_runtime->Adapter(),
            std::move(diagnostics), PluginHostSnapshot(context));
    }
    const auto plugins = PluginHostSnapshot(context);
    const bool stopped = !unitymem::PlatformHostQuarantined(plugins.get());
    return stopped ? ERROR_SUCCESS : ERROR_TIMEOUT;
}

DWORD ConfirmRuntimeHealth(
    const std::shared_ptr<CoreContext>& context,
    std::stop_token stop_token) {
    constexpr auto stability_window = std::chrono::seconds(5);
    const auto deadline = std::chrono::steady_clock::now() + stability_window;
    while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        Sleep(50);
    }
    if (stop_token.stop_requested()) return ERROR_SUCCESS;
    const auto session = context->session.lock();
    if (session == nullptr ||
        session->Snapshot().state != CABBIRD_RUNTIME_STATE_RUNNING) {
        return ERROR_SUCCESS;
    }

    if (context->crash_coordinator != nullptr) {
        const auto marked = context->crash_coordinator->MarkHealthy();
        if (!marked.Ok()) {
            RuntimeLog(
                context, cabbird::LogLevel::Warning,
                "runtime.crash_coordinator_health_rejected",
                "crash_coordinator_health=rejected reason=" + marked.message,
                cabbird::LogThreadDomain::Worker);
        } else {
            std::scoped_lock lock(context->recovery_mutex);
            context->crash_coordinator_state = "healthy";
            context->recovery_diagnostics = RecoveryDiagnosticsJson(*context);
        }
    }

    cabbird::RuntimeRecoveryStore recovery(context->runtime_root);
    const auto recovery_state = recovery.Load();
    if (recovery_state.Ok()) {
        const auto healthy = recovery.MarkHealthy();
        if (!healthy.Ok()) {
            RuntimeLog(
                context, cabbird::LogLevel::Warning,
                "runtime.recovery_health_rejected",
                "runtime_recovery_health=rejected reason=" + healthy.message,
                cabbird::LogThreadDomain::Worker);
        }
    }
    RuntimeLog(
        context, cabbird::LogLevel::Info,
        "runtime.health_confirmed",
        "runtime_health=confirmed version=" CABBIRD_SDK_VERSION_STRING,
        cabbird::LogThreadDomain::Worker);
    return ERROR_SUCCESS;
}

void ShutdownCore(const std::shared_ptr<CoreContext>& context) noexcept {
    try {
        if (context->crash_coordinator != nullptr) {
            static_cast<void>(context->crash_coordinator->MarkStopping());
            static_cast<void>(context->crash_coordinator->WaitForMonitor(
                std::chrono::milliseconds(500)));
            std::scoped_lock lock(context->recovery_mutex);
            context->crash_coordinator_state = "stopped";
            context->recovery_diagnostics = RecoveryDiagnosticsJson(*context);
        }
    } catch (...) {
    }
    try {
        if (context->crash_reporter != nullptr) context->crash_reporter->Uninstall();
        context->crash_reporter.reset();
    } catch (...) {
    }
    try {
        if (!WriteDiagnosticsSummary(context, "stopped")) {
            LogDiagnosticsSummaryFailure(context);
        }
        RuntimeLog(
            context, cabbird::LogLevel::Info, "runtime.stop", "runtime=stopped");
    } catch (...) {
    }
    if (context->logger != nullptr) {
        try {
            const bool stopped = context->logger->Stop();
            const auto stats = context->logger->Stats();
            if (!stopped || stats.error_count != 0 || stats.dropped != 0) {
                std::ofstream output(
                    context->log_directory / L"cabbird-runtime.log", std::ios::app);
                output << "pid=" << GetCurrentProcessId()
                       << " structured_logger=degraded stopped=" << (stopped ? 1 : 0)
                       << " errors=" << stats.error_count
                       << " dropped=" << stats.dropped;
                if (const auto failure = context->logger->LastError()) {
                    output << " operation=" << static_cast<unsigned>(failure->operation)
                           << " code=" << failure->code.value();
                }
                output << '\n';
            }
        } catch (...) {
            std::ofstream(context->log_directory / L"cabbird-runtime.log", std::ios::app)
                << "pid=" << GetCurrentProcessId()
                << " structured_logger=stop_exception\n";
        }
        context->logger.reset();
    }
}

DWORD ValidateStartInfo(const CabbirdStartInfo* start_info) noexcept {
    if (start_info == nullptr) return ERROR_INVALID_PARAMETER;
    if (start_info->struct_size < CABBIRD_START_INFO_V1_SIZE) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (start_info->bootstrap_abi_version != CABBIRD_BOOTSTRAP_ABI_VERSION) {
        return ERROR_REVISION_MISMATCH;
    }
    if (start_info->flags != 0) return ERROR_INVALID_FLAGS;
    if (start_info->bootstrap_type > CABBIRD_BOOTSTRAP_TYPE_EXTERNAL) {
        return ERROR_INVALID_PARAMETER;
    }
    return ERROR_SUCCESS;
}

cabbird::RuntimeStartContext BuildStartContext(const CabbirdStartInfo& start_info) {
    cabbird::RuntimeStartContext result;
    result.bootstrap_abi_version = start_info.bootstrap_abi_version;
    result.bootstrap_type = start_info.bootstrap_type;
    result.bootstrap_module = start_info.bootstrap_module;
    result.game_module = start_info.game_module != nullptr
        ? start_info.game_module
        : GetModuleHandleW(nullptr);

    if (start_info.runtime_root != nullptr && start_info.runtime_root[0] != L'\0') {
        result.runtime_root = start_info.runtime_root;
    } else if (start_info.bootstrap_module != nullptr) {
        result.runtime_root = ModuleDirectory(start_info.bootstrap_module) / L"Cabbird";
    } else {
        result.runtime_root = ModuleDirectory(g_core_module);
    }
    if (result.runtime_root.empty()) {
        throw std::filesystem::filesystem_error(
            "runtime root is unavailable", std::error_code{});
    }
    result.runtime_root = std::filesystem::absolute(result.runtime_root);

    if (start_info.log_directory != nullptr && start_info.log_directory[0] != L'\0') {
        result.log_directory = std::filesystem::absolute(start_info.log_directory);
    } else {
        result.log_directory = result.runtime_root / L"logs";
    }
    result.external_stop_event = start_info.external_stop_event;
    return result;
}

cabbird::RuntimeSessionOptions BuildSessionOptions(
    const std::shared_ptr<CoreContext>& context) {
    cabbird::RuntimeSessionOptions options;
    auto services = std::make_shared<cabbird::ServiceGraph>();
    context->services = services;
    context->memory_services = cabbird::CreateCoreMemoryServices();

    cabbird::ServiceDescriptor runtime_info;
    runtime_info.id = "cabbird.runtime.info";
    runtime_info.lifetime = cabbird::ServiceLifetime::Provided;
    DWORD result = services->Register(std::move(runtime_info));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(), "register runtime info service");
    }

    cabbird::ServiceDescriptor config;
    config.id = "cabbird.config";
    config.startup = cabbird::ServiceStartup::Blocking;
    config.affinity = cabbird::ServiceAffinity::Lifecycle;
    config.required_dependencies.push_back({"cabbird.runtime.info", 1});
    config.start = [context](std::stop_token stop_token) {
        return InitializeCore(context, stop_token);
    };
    config.stop = [] {};
    result = services->Register(std::move(config));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(), "register config service");
    }

    cabbird::ServiceDescriptor module_memory;
    module_memory.id = "cabbird.internal.module-memory";
    module_memory.lifetime = cabbird::ServiceLifetime::Provided;
    module_memory.affinity = cabbird::ServiceAffinity::Any;
    result = services->Register(std::move(module_memory));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register module memory service");
    }

    cabbird::ServiceDescriptor pattern;
    pattern.id = "cabbird.internal.pattern";
    pattern.lifetime = cabbird::ServiceLifetime::Provided;
    pattern.affinity = cabbird::ServiceAffinity::Any;
    pattern.required_dependencies.push_back({"cabbird.internal.module-memory", 1});
    result = services->Register(std::move(pattern));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register pattern service");
    }

    cabbird::ServiceDescriptor pipe;
    pipe.id = "cabbird.internal.pipe";
    pipe.startup = cabbird::ServiceStartup::Blocking;
    pipe.affinity = cabbird::ServiceAffinity::Worker;
    pipe.required_dependencies.push_back({"cabbird.config", 1});
    pipe.required_dependencies.push_back({"cabbird.internal.module-memory", 1});
    pipe.required_dependencies.push_back({"cabbird.internal.pattern", 1});
    pipe.required_dependencies.push_back({"cabbird.internal.unity-profile", 1});
    const std::weak_ptr<cabbird::ServiceGraph> weak_services = services;
    pipe.start = [context, weak_services](std::stop_token stop_token) {
        return PrepareDiagnosticPipe(context, weak_services, stop_token);
    };
    pipe.stop = [context] { StopDiagnosticPipe(context); };
    result = services->Register(std::move(pipe));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register diagnostic pipe service");
    }

    cabbird::ServiceDescriptor profile;
    profile.id = "cabbird.internal.unity-profile";
    profile.startup = cabbird::ServiceStartup::Blocking;
    profile.affinity = cabbird::ServiceAffinity::Worker;
    profile.required_dependencies.push_back({"cabbird.config", 1});
    profile.required_dependencies.push_back({"cabbird.internal.module-memory", 1});
    profile.required_dependencies.push_back({"cabbird.internal.pattern", 1});
    profile.required_dependencies.push_back({"cabbird.repository.coordinator", 1});
    profile.start = [context](std::stop_token stop_token) {
        return PrepareUnityProfile(context, stop_token);
    };
    profile.stop = [context] { StopUnityProfile(context); };
    result = services->Register(std::move(profile));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register UNITY profile service");
    }

    cabbird::ServiceDescriptor repository;
    repository.id = "cabbird.repository.coordinator";
    repository.startup = cabbird::ServiceStartup::Blocking;
    repository.affinity = cabbird::ServiceAffinity::Worker;
    repository.required_dependencies.push_back({"cabbird.config", 1});
    repository.start = [context](std::stop_token stop_token) {
        return PrepareRepository(context, stop_token);
    };
    repository.stop = [context] { StopRepository(context); };
    result = services->Register(std::move(repository));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register repository coordinator service");
    }

    cabbird::ServiceDescriptor plugin_host;
    plugin_host.id = "cabbird.plugin.host";
    plugin_host.startup = cabbird::ServiceStartup::Blocking;
    plugin_host.affinity = cabbird::ServiceAffinity::Lifecycle;
    plugin_host.required_dependencies.push_back({"cabbird.config", 1});
    plugin_host.required_dependencies.push_back({"cabbird.internal.unity-profile", 1});
    plugin_host.required_dependencies.push_back({"cabbird.repository.coordinator", 1});
    plugin_host.start = [context](std::stop_token stop_token) {
        return PreparePluginHost(context, stop_token);
    };
    plugin_host.stop = [context] {
        static_cast<void>(StopPluginHost(context, std::chrono::seconds(1)));
    };
    result = services->Register(std::move(plugin_host));
    if (result != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(result), std::system_category(),
            "register plugin host service");
    }

    // These provided nodes make the production ownership boundaries explicit
    // in diagnostics; their concrete pumps are owned by RuntimeSession and
    // the platform worker below.
    const auto register_provided = [&](std::string id, cabbird::ServiceAffinity affinity,
                                       std::vector<cabbird::ServiceDependency> dependencies = {}) {
        cabbird::ServiceDescriptor descriptor;
        descriptor.id = std::move(id);
        descriptor.lifetime = cabbird::ServiceLifetime::Provided;
        descriptor.affinity = affinity;
        descriptor.required_dependencies = std::move(dependencies);
        const DWORD registration = services->Register(std::move(descriptor));
        if (registration != ERROR_SUCCESS) {
            throw std::system_error(
                static_cast<int>(registration), std::system_category(),
                "register production boundary service");
        }
    };
    register_provided("cabbird.dispatchers.lifecycle", cabbird::ServiceAffinity::Lifecycle);
    register_provided("cabbird.dispatchers.worker", cabbird::ServiceAffinity::Worker);
    register_provided("cabbird.dispatchers.game", cabbird::ServiceAffinity::Game);
    register_provided("cabbird.dispatchers.render", cabbird::ServiceAffinity::Render);
    register_provided(
        "cabbird.platform.host", cabbird::ServiceAffinity::Render,
        {{"cabbird.plugin.host", 1}});
    register_provided(
        "cabbird.platform.input", cabbird::ServiceAffinity::Render,
        {{"cabbird.platform.host", 1}});
    register_provided(
        "cabbird.platform.ui", cabbird::ServiceAffinity::Render,
        {{"cabbird.platform.host", 1}});
    options.services = std::move(services);
    options.stop_plugins = [context](std::chrono::milliseconds timeout) {
        return StopPluginHost(context, timeout);
    };
    options.shutdown = [context] { ShutdownCore(context); };
    options.workers.push_back({"pipe", [context](std::stop_token stop_token) {
        return RunDiagnosticPipe(context, stop_token);
    }});
    options.workers.push_back({"platform", [context](std::stop_token stop_token) {
        return RunPlatform(context, stop_token);
    }});
    options.workers.push_back({"runtime-health", [context](std::stop_token stop_token) {
        return ConfirmRuntimeHealth(context, stop_token);
    }});
    return options;
}

cabbird::RuntimeSessionSnapshot CurrentSnapshotLocked(const RuntimeControl& control) {
    return control.session != nullptr ? control.session->Snapshot() : control.last_snapshot;
}

void ArchiveStoppedSessionLocked(RuntimeControl& control) {
    if (control.session == nullptr ||
        control.session->Snapshot().state != CABBIRD_RUNTIME_STATE_STOPPED) {
        return;
    }
    control.session->Join();
    control.last_snapshot = control.session->Snapshot();
    control.session.reset();
}

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI CabbirdStart(
    const CabbirdStartInfo* start_info) {
    const DWORD validation = ValidateStartInfo(start_info);
    if (validation != ERROR_SUCCESS) return validation;
    if (g_runtime_control == nullptr) return ERROR_INVALID_STATE;

    try {
        const auto start_context = BuildStartContext(*start_info);
        auto core_context = std::make_shared<CoreContext>();
        core_context->runtime_root = start_context.runtime_root;
        core_context->log_directory = start_context.log_directory;
        core_context->game_module = start_context.game_module;
        auto session = std::make_shared<cabbird::RuntimeSession>(
            start_context, BuildSessionOptions(core_context));
        core_context->session = session;

        auto& control = *g_runtime_control;
        std::scoped_lock lock(control.mutex);
        ArchiveStoppedSessionLocked(control);
        if (control.session != nullptr) return ERROR_ALREADY_INITIALIZED;

        const DWORD result = session->Start();
        control.session = session;
        if (result != ERROR_SUCCESS) ArchiveStoppedSessionLocked(control);
        return result;
    } catch (...) {
        return CurrentExceptionError();
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI CabbirdRequestStop() {
    if (g_runtime_control == nullptr) return ERROR_INVALID_STATE;
    std::shared_ptr<cabbird::RuntimeSession> session;
    {
        auto& control = *g_runtime_control;
        std::scoped_lock lock(control.mutex);
        session = control.session;
        if (session == nullptr &&
            control.last_snapshot.state == CABBIRD_RUNTIME_STATE_STOPPED) {
            return ERROR_SUCCESS;
        }
    }
    if (session == nullptr) return ERROR_NOT_READY;
    session->RequestStop();
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD WINAPI CabbirdGetState(
    CabbirdRuntimeStateInfo* state_info) {
    if (state_info == nullptr) return ERROR_INVALID_PARAMETER;
    if (state_info->struct_size < CABBIRD_RUNTIME_STATE_INFO_V1_SIZE) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (state_info->state_info_version != CABBIRD_RUNTIME_STATE_INFO_VERSION) {
        return ERROR_REVISION_MISMATCH;
    }
    if (g_runtime_control == nullptr) return ERROR_INVALID_STATE;

    cabbird::RuntimeSessionSnapshot snapshot;
    {
        auto& control = *g_runtime_control;
        std::scoped_lock lock(control.mutex);
        snapshot = CurrentSnapshotLocked(control);
    }
    state_info->state = snapshot.state;
    state_info->last_error = snapshot.last_error;
    state_info->session_generation = snapshot.generation;
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD WINAPI CabbirdWaitForStop(DWORD timeout_ms) {
    if (g_runtime_control == nullptr) return ERROR_INVALID_STATE;
    std::shared_ptr<cabbird::RuntimeSession> session;
    {
        auto& control = *g_runtime_control;
        std::scoped_lock lock(control.mutex);
        session = control.session;
        if (session == nullptr) {
            return control.last_snapshot.state == CABBIRD_RUNTIME_STATE_STOPPED
                ? ERROR_SUCCESS
                : ERROR_NOT_READY;
        }
    }

    const auto timeout = timeout_ms == INFINITE
        ? std::chrono::milliseconds::max()
        : std::chrono::milliseconds(timeout_ms);
    if (!session->WaitForStop(timeout)) return ERROR_TIMEOUT;
    session->Join();

    auto& control = *g_runtime_control;
    std::scoped_lock lock(control.mutex);
    if (control.session == session) {
        control.last_snapshot = session->Snapshot();
        control.session.reset();
    }
    return ERROR_SUCCESS;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_core_module = module;
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_core_module = nullptr;
        static_cast<void>(g_runtime_control.release());
    }
    return TRUE;
}
