#include "cabbird/unitymem_compat.hpp"
#include "cabbird/launcher/manual_map.hpp"
#include "cabbird/launcher/configuration.hpp"
#include "cabbird/i18n.hpp"
#include "cabbird/cabbird_ui_theme.hpp"
#include "cabbird/runtime_launch.hpp"
#include "cabbird/runtime_recovery.hpp"
#include "cabbird/ui_resource_decoder.hpp"
#include "cabbird/config.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kLogoResourceId = 101;
constexpr int kIconResourceId = 201;
constexpr float kHeaderHeight = 56.0f;
constexpr float kModeHeight = 48.0f;
constexpr float kFooterHeight = 42.0f;
constexpr float kLauncherFontScale = 15.0f / 13.0f;
constexpr float kDefaultDpi = 96.0f;

ImVec4 ThemeColor(const unitymem::CabbirdUiColor& color) noexcept {
    return {color.red, color.green, color.blue, color.alpha};
}

ImVec4 ThemeColorWithAlpha(
    const unitymem::CabbirdUiColor& color, const float alpha) noexcept {
    return {color.red, color.green, color.blue, alpha};
}

enum class LauncherMode : std::uint8_t { Attach };
enum class MessageKind : std::uint8_t { Neutral, Success, Error };

struct LauncherMessage final {
    cabbird::MessageId id{cabbird::MessageId::LauncherStateReady};
    std::vector<std::string> arguments;
    std::string detail;
};

struct LauncherSnapshot final {
    cabbird::launcher::AzurPromiliaClient selected_client{
        cabbird::launcher::AzurPromiliaClient::MainlandChina};
    std::filesystem::path game_directory;
    std::filesystem::path launcher_executable;
    std::optional<cabbird::RuntimeRecoveryState> recovery;
    std::string recovery_message;
    std::vector<cabbird::launcher::AttachableProcess> processes;
    DWORD attached_process{};
    bool core_available{};
    std::string runtime_version;
    std::string runtime_message;
    std::uint32_t toggle_key{VK_INSERT};
    bool busy{};
    LauncherMessage message;
    MessageKind message_kind{MessageKind::Neutral};
};

std::filesystem::path ExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path);
}

std::filesystem::path ExecutableDirectory() {
    return ExecutablePath().parent_path();
}

struct AdministratorLaunchResult final {
    bool run_current_process{};
    int exit_code{};
};

AdministratorLaunchResult EnsureAdministrator(PWSTR command_line) noexcept {
    HANDLE token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return {false, static_cast<int>(GetLastError())};
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned{};
    const BOOL queried = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned);
    const DWORD query_error = queried == FALSE ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(token);
    if (queried == FALSE) return {false, static_cast<int>(query_error)};
    if (elevation.TokenIsElevated != 0) return {true, ERROR_SUCCESS};

    const auto executable = ExecutablePath();
    if (executable.empty()) return {false, ERROR_FILE_NOT_FOUND};
    const std::wstring working_directory = executable.parent_path().wstring();
    SHELLEXECUTEINFOW launch{
        .cbSize = sizeof(launch),
        .fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
        .lpVerb = L"runas",
        .lpFile = executable.c_str(),
        .lpParameters = command_line != nullptr && command_line[0] != L'\0'
            ? command_line : nullptr,
        .lpDirectory = working_directory.c_str(),
        .nShow = SW_SHOWNORMAL,
    };
    if (ShellExecuteExW(&launch) == FALSE) {
        return {false, static_cast<int>(GetLastError())};
    }
    if (launch.hProcess != nullptr) CloseHandle(launch.hProcess);
    return {false, ERROR_SUCCESS};
}

HMODULE LoadSystemDwmapi() {
    std::wstring directory(32768, L'\0');
    const UINT length = GetSystemDirectoryW(
        directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) return nullptr;
    directory.resize(length);
    return LoadLibraryExW(
        (std::filesystem::path(directory) / L"dwmapi.dll").c_str(),
        nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

std::string WideUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

std::wstring Utf8Wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size) != size) {
        return {};
    }
    return result;
}

std::string PathUtf8(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    return WideUtf8(value);
}

std::string VirtualKeyName(const std::uint32_t key) {
    const std::string fallback = "Key " + std::to_string(key);
    const UINT scan_code = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    wchar_t buffer[64]{};
    const LONG parameter = static_cast<LONG>(scan_code << 16U);
    if (GetKeyNameTextW(parameter, buffer, static_cast<int>(std::size(buffer))) <= 0) {
        return fallback;
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return fallback;
    std::string result(static_cast<std::size_t>(size), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8, 0, buffer, -1, result.data(), size, nullptr, nullptr));
    result.pop_back();
    return result;
}

bool PathsEqual(
    const std::filesystem::path& left, const std::filesystem::path& right) noexcept {
    try {
        const std::wstring left_value = left.lexically_normal().wstring();
        const std::wstring right_value = right.lexically_normal().wstring();
        return !left_value.empty() && !right_value.empty() &&
            CompareStringOrdinal(
                left_value.c_str(), -1, right_value.c_str(), -1, TRUE) == CSTR_EQUAL;
    } catch (...) {
        return false;
    }
}

std::string EncodeUtf8(char32_t codepoint) {
    std::string result;
    if (codepoint <= 0x7fU) {
        result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        result.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        result.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    return result;
}

std::string Ellipsize(std::string_view value, float width) {
    if (ImGui::CalcTextSize(value.data(), value.data() + value.size()).x <= width) {
        return std::string(value);
    }
    constexpr std::string_view suffix{"..."};
    if (ImGui::CalcTextSize(suffix.data(), suffix.data() + suffix.size()).x > width) {
        return {};
    }
    std::size_t end = value.size();
    while (end > 0) {
        --end;
        while (end > 0 &&
               (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) {
            --end;
        }
        const auto candidate = std::string(value.substr(0, end)) + std::string(suffix);
        if (ImGui::CalcTextSize(candidate.c_str()).x <= width) return candidate;
    }
    return std::string(suffix);
}

const char* Glyph(char32_t codepoint) {
    struct Entry final { char32_t codepoint; std::string text; };
    static const std::array entries{
        Entry{0xe838, EncodeUtf8(0xe838)},
        Entry{0xe72c, EncodeUtf8(0xe72c)},
        Entry{0xe768, EncodeUtf8(0xe768)},
        Entry{0xe73e, EncodeUtf8(0xe73e)},
        Entry{0xe7ba, EncodeUtf8(0xe7ba)},
        Entry{0xe711, EncodeUtf8(0xe711)},
        Entry{0xe8b7, EncodeUtf8(0xe8b7)},
    };
    const auto found = std::find_if(entries.begin(), entries.end(), [codepoint](const auto& entry) {
        return entry.codepoint == codepoint;
    });
    return found == entries.end() ? "?" : found->text.c_str();
}

LauncherMessage MakeLauncherMessage(
    cabbird::MessageId id,
    std::initializer_list<std::string_view> arguments = {},
    std::string detail = {}) {
    LauncherMessage message;
    message.id = id;
    message.arguments.reserve(arguments.size());
    for (const auto argument : arguments) message.arguments.emplace_back(argument);
    message.detail = std::move(detail);
    return message;
}

class LauncherController final {
public:
    explicit LauncherController(std::filesystem::path payload_root)
        : payload_root_(std::move(payload_root)),
          runtime_directory_(payload_root_ / L"Cabbird"),
          configuration_path_(cabbird::launcher::LauncherConfigurationPath(payload_root_)),
          worker_([this](std::stop_token stop) { WorkerMain(stop); }) {
        Queue(cabbird::MessageId::LauncherStatusScanningLocal, [this] {
            InitializePathsImpl();
        });
    }

    ~LauncherController() {
        worker_.request_stop();
        queue_changed_.notify_all();
    }

    LauncherController(const LauncherController&) = delete;
    LauncherController& operator=(const LauncherController&) = delete;

    [[nodiscard]] LauncherSnapshot Snapshot() const {
        std::scoped_lock lock(state_mutex_);
        return state_;
    }

    void SelectGameDirectory(std::filesystem::path directory) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return;
            state_.game_directory = std::move(directory);
            configuration_.Selected().game_directory = state_.game_directory;
        }
        Queue(cabbird::MessageId::LauncherStatusInspectingCore, [this] {
            ReconcileRelatedPathsImpl();
            const auto saved = PersistConfigurationImpl();
            RefreshHotkeyImpl();
            RefreshRecoveryImpl();
            RefreshProcessesImpl(false);
            if (!saved.Ok()) {
                PublishMessage(cabbird::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            }
        });
    }

    void RestoreRecovery(cabbird::RuntimeRecoveryAxis axis) {
        Queue(cabbird::MessageId::LauncherStatusRestoringRecovery, [this, axis] {
            const auto runtime_root = GameDirectory() / L"Cabbird";
            cabbird::RuntimeRecoveryStore store(runtime_root);
            PublishRecovery(store.Restore(axis), true);
        });
    }

    void RefreshProcesses() {
        Queue(cabbird::MessageId::LauncherStatusScanningProcesses,
            [this] { RefreshProcessesImpl(); });
    }

    void SelectLauncherExecutable(std::filesystem::path executable) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return;
            state_.launcher_executable = std::move(executable);
            configuration_.Selected().launcher_executable = state_.launcher_executable;
        }
        Queue(cabbird::MessageId::LauncherStatusScanningLocal, [this] {
            ReconcileRelatedPathsImpl();
            const auto saved = PersistConfigurationImpl();
            RefreshProcessesImpl();
            if (!saved.Ok()) {
                PublishMessage(cabbird::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            }
        });
    }

    void LaunchAndAttach() {
        Queue(cabbird::MessageId::LauncherStatusLaunchingAttach, [this] {
            const auto launcher = LauncherExecutable();
            const auto selected = ResolveAttachRuntime();
            if (!selected.Ok()) {
                PublishRuntimeFailure(selected);
                return;
            }
            cabbird::launcher::ManualMapLaunchOptions options;
            options.launcher_path = launcher;
            options.working_directory = launcher.parent_path();
            options.manual_map.core_path = selected.core_path;
            options.manual_map.runtime_root = selected.runtime_root;
            options.manual_map.log_directory = options.manual_map.runtime_root / L"logs";
            const auto result =
                cabbird::launcher::LaunchAndManualMapRuntimeCore(options);
            if (result.Ok()) RefreshProcessesImpl();

            std::scoped_lock lock(state_mutex_);
            if (result.Ok()) {
                state_.attached_process = result.process_id;
                const std::string process_id = std::to_string(result.process_id);
                state_.message = MakeLauncherMessage(
                    cabbird::MessageId::LauncherStatusLaunchAttached, {process_id});
                state_.message_kind = MessageKind::Success;
            } else {
                const std::string error = std::to_string(result.mapping.win32_error);
                state_.message = MakeLauncherMessage(
                    cabbird::MessageId::LauncherStatusLaunchAttachFailed, {error},
                    result.mapping.message);
                state_.message_kind = MessageKind::Error;
            }
        });
    }

    void SetToggleKey(const std::uint32_t key) {
        Queue(cabbird::MessageId::LauncherStatusSavingSettings, [this, key] {
            if (!SaveToggleKeyImpl(key)) {
                PublishMessage(cabbird::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, "menu toggle preference could not be written");
                return;
            }
            std::scoped_lock lock(state_mutex_);
            state_.toggle_key = key;
            state_.message = MakeLauncherMessage(
                cabbird::MessageId::LauncherStatusSettingsSaved);
            state_.message_kind = MessageKind::Success;
        });
    }

private:
    using Work = std::function<void()>;

    [[nodiscard]] cabbird::RuntimeLaunchResult ResolveAttachRuntime() const {
        auto bundled = cabbird::ResolveRuntimeLaunch({runtime_directory_});
        if (bundled.Ok()) return bundled;
        auto installed = cabbird::ResolveRuntimeLaunch({GameDirectory() / L"Cabbird"});
        return installed.Ok() ? installed : bundled;
    }

    void PublishRuntimeFailure(const cabbird::RuntimeLaunchResult& selected) {
        std::scoped_lock lock(state_mutex_);
        state_.runtime_version.clear();
        state_.runtime_message = selected.message;
        state_.core_available = false;
        state_.message = MakeLauncherMessage(
            cabbird::MessageId::LauncherStatusCoreUnavailable, {}, selected.message);
        state_.message_kind = MessageKind::Error;
    }

    void PublishMessage(cabbird::MessageId id, MessageKind kind, std::string detail = {}) {
        std::scoped_lock lock(state_mutex_);
        state_.message = MakeLauncherMessage(id, {}, std::move(detail));
        state_.message_kind = kind;
    }

    [[nodiscard]] std::filesystem::path GameDirectory() const {
        std::scoped_lock lock(state_mutex_);
        return state_.game_directory;
    }

    [[nodiscard]] std::filesystem::path LauncherExecutable() const {
        std::scoped_lock lock(state_mutex_);
        return state_.launcher_executable;
    }

    [[nodiscard]] std::filesystem::path RuntimeSettingsRoot() const {
        const auto game_directory = GameDirectory();
        if (!game_directory.empty()) {
            const auto installed = game_directory / L"Cabbird";
            std::error_code error;
            if (std::filesystem::is_regular_file(
                    installed / L"Cabbird.Core.dll", error) && !error) {
                return installed;
            }
        }
        return runtime_directory_;
    }

    void RefreshHotkeyImpl() {
        const auto root = RuntimeSettingsRoot();
        const auto config = unitymem::AnalyzerConfig::Load(root / L"cabbird.ini");
        std::scoped_lock lock(state_mutex_);
        state_.toggle_key = config.platform_toggle_key;
    }

    [[nodiscard]] bool SaveToggleKeyImpl(const std::uint32_t key) const {
        const std::wstring value = std::to_wstring(key);
        return WritePrivateProfileStringW(
            L"Platform", L"ToggleKey", value.c_str(),
            (RuntimeSettingsRoot() / L"cabbird.ini").c_str()) != FALSE;
    }

    bool Queue(cabbird::MessageId activity, Work work) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return false;
            state_.busy = true;
            state_.message = MakeLauncherMessage(activity);
            state_.message_kind = MessageKind::Neutral;
        }
        {
            std::scoped_lock lock(queue_mutex_);
            queue_.push_back(std::move(work));
        }
        queue_changed_.notify_one();
        return true;
    }

    void WorkerMain(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Work work;
            {
                std::unique_lock lock(queue_mutex_);
                queue_changed_.wait(lock, stop, [this] { return !queue_.empty(); });
                if (stop.stop_requested()) break;
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                work();
            } catch (...) {
                std::scoped_lock lock(state_mutex_);
                state_.message = MakeLauncherMessage(
                    cabbird::MessageId::LauncherStatusUnexpectedFailure);
                state_.message_kind = MessageKind::Error;
            }
            std::scoped_lock lock(state_mutex_);
            state_.busy = false;
        }
    }

    void RefreshRecoveryImpl() {
        const auto runtime_root = GameDirectory() / L"Cabbird";
        std::error_code error;
        if (!std::filesystem::is_directory(runtime_root, error) || error) {
            std::scoped_lock lock(state_mutex_);
            state_.recovery.reset();
            state_.recovery_message.clear();
            return;
        }
        cabbird::RuntimeRecoveryStore store(runtime_root);
        PublishRecovery(store.Load(), false);
    }

    void PublishRecovery(cabbird::RuntimeRecoveryResult result, bool announce) {
        std::scoped_lock lock(state_mutex_);
        if (result.Ok()) {
            state_.recovery = std::move(result.state);
            state_.recovery_message.clear();
            if (announce) {
                state_.message = MakeLauncherMessage(
                    cabbird::MessageId::LauncherStatusRecoveryRestored);
                state_.message_kind = MessageKind::Success;
            }
            return;
        }
        state_.recovery.reset();
        if (result.error == cabbird::RuntimeRecoveryError::StateUnavailable) {
            state_.recovery_message.clear();
            return;
        }
        state_.recovery_message = std::move(result.message);
        if (announce) {
            state_.message = MakeLauncherMessage(
                cabbird::MessageId::LauncherStatusRecoveryRestoreFailed, {},
                state_.recovery_message);
            state_.message_kind = MessageKind::Error;
        }
    }

    void InitializePathsImpl() {
        const auto loaded = cabbird::launcher::LoadLauncherConfiguration(configuration_path_);
        const auto game_processes = cabbird::launcher::EnumerateAttachableProcesses();
        const auto mainland_launcher_processes =
            cabbird::launcher::EnumerateAttachableProcesses(L"launcher.exe");
        const auto global_launcher_processes =
            cabbird::launcher::EnumerateAttachableProcesses(L"launcher.exe");
        // Only 国服 is offered: the CN/Global buttons were removed at the operator's request.
        // Pinning here (rather than trusting the JSON) is what makes the removal real -- a
        // `selectedClient: global` already on disk would otherwise keep selecting the other
        // client's paths with no UI left to change it.  PersistConfigurationImpl writes this
        // back on the next save, so the file converges on mainlandChina.
        auto configuration = loaded.configuration;
        configuration.selected_client = cabbird::launcher::AzurPromiliaClient::MainlandChina;
        const auto discover = [this, &game_processes, &configuration](
                                  const cabbird::launcher::AzurPromiliaClient client,
                                  const auto& launcher_processes) {
            cabbird::launcher::LauncherDiscoveryOptions options;
            options.client = client;
            options.payload_root = payload_root_;
            options.allow_unpaired_game_discovery =
                configuration.selected_client == client;
            if (configuration.selected_client == client) {
                for (const auto& process : game_processes) {
                    if (!process.executable_path.empty()) {
                        options.running_game_executables.push_back(process.executable_path);
                    }
                }
            }
            for (const auto& process : launcher_processes) {
                if (!process.executable_path.empty()) {
                    options.running_launcher_executables.push_back(process.executable_path);
                }
            }
            return cabbird::launcher::DiscoverLauncherConfiguration(
                client == cabbird::launcher::AzurPromiliaClient::Global
                    ? configuration.global : configuration.mainland_china,
                options);
        };
        configuration.mainland_china = discover(
            cabbird::launcher::AzurPromiliaClient::MainlandChina, mainland_launcher_processes);
        configuration.global = discover(
            cabbird::launcher::AzurPromiliaClient::Global, global_launcher_processes);
        {
            std::scoped_lock lock(state_mutex_);
            configuration_ = std::move(configuration);
            const auto& selected = configuration_.Selected();
            state_.selected_client = configuration_.selected_client;
            state_.game_directory = selected.game_directory;
            state_.launcher_executable = selected.launcher_executable;
        }
        const auto saved = PersistConfigurationImpl();
        RefreshHotkeyImpl();
        RefreshRecoveryImpl();
        RefreshProcessesImpl();
        if (!saved.Ok()) {
            PublishMessage(cabbird::MessageId::LauncherStatusUnexpectedFailure,
                MessageKind::Error, saved.message);
        }
    }

    void ReconcileRelatedPathsImpl() {
        cabbird::launcher::LauncherClientConfiguration preferred;
        cabbird::launcher::AzurPromiliaClient client{};
        {
            std::scoped_lock lock(state_mutex_);
            preferred.game_directory = state_.game_directory;
            preferred.launcher_executable = state_.launcher_executable;
            client = configuration_.selected_client;
        }
        cabbird::launcher::LauncherDiscoveryOptions options;
        options.client = client;
        options.payload_root = payload_root_;
        options.allow_unpaired_game_discovery = true;
        const auto discovered = cabbird::launcher::DiscoverLauncherConfiguration(
            preferred, options);
        std::scoped_lock lock(state_mutex_);
        configuration_.Selected() = discovered;
        state_.game_directory = discovered.game_directory;
        state_.launcher_executable = discovered.launcher_executable;
    }

    [[nodiscard]] cabbird::launcher::LauncherConfigurationSaveResult
    PersistConfigurationImpl() const {
        cabbird::launcher::LauncherConfiguration configuration;
        {
            std::scoped_lock lock(state_mutex_);
            configuration = configuration_;
        }
        return cabbird::launcher::SaveLauncherConfiguration(
            configuration_path_, configuration);
    }

    void RefreshProcessesImpl(bool announce = true) {
        auto processes = cabbird::launcher::EnumerateAttachableProcesses();
        const auto game_directory = GameDirectory();
        if (!game_directory.empty()) {
            std::erase_if(processes, [&game_directory](const auto& process) {
                return process.executable_path.empty() ||
                    !PathsEqual(process.executable_path.parent_path(), game_directory);
            });
        }
        const auto runtime = ResolveAttachRuntime();
        const bool core_available = runtime.Ok();
        std::scoped_lock lock(state_mutex_);
        state_.processes = std::move(processes);
        state_.core_available = core_available;
        state_.runtime_version = runtime.version;
        state_.runtime_message = runtime.message;
        if (std::none_of(
                state_.processes.begin(), state_.processes.end(),
                [this](const auto& process) {
                    return process.process_id == state_.attached_process;
                })) {
            state_.attached_process = 0;
        }
        if (!announce) {
            return;
        }
        if (!core_available) {
            state_.message = runtime.message.empty()
                ? MakeLauncherMessage(cabbird::MessageId::LauncherStatusCoreUnavailable)
                : MakeLauncherMessage(
                    cabbird::MessageId::LauncherStatusCoreUnavailable, {},
                    runtime.message);
            state_.message_kind = MessageKind::Error;
        } else if (state_.processes.empty()) {
            state_.message = MakeLauncherMessage(
                cabbird::MessageId::LauncherStatusNoProcesses);
            state_.message_kind = MessageKind::Neutral;
        } else {
            state_.message = MakeLauncherMessage(
                cabbird::MessageId::LauncherStatusProcessesRefreshed);
            state_.message_kind = MessageKind::Neutral;
        }
    }

    std::filesystem::path payload_root_;
    std::filesystem::path runtime_directory_;
    std::filesystem::path configuration_path_;
    mutable std::mutex state_mutex_;
    cabbird::launcher::LauncherConfiguration configuration_;
    LauncherSnapshot state_;
    std::mutex queue_mutex_;
    std::condition_variable_any queue_changed_;
    std::deque<Work> queue_;
    std::jthread worker_;
};

struct Graphics final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> render_target;
    ComPtr<ID3D11ShaderResourceView> logo;
};

Graphics* g_graphics{};
float g_launcher_dpi_scale{1.0f};
bool g_launcher_dpi_changed{};
bool g_launcher_hotkey_capture{};
std::array<bool, 256> g_launcher_hotkey_down{};

bool IsLauncherHotkeyModifier(const std::uint32_t key) noexcept {
    return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU ||
        key == VK_LCONTROL ||
        key == VK_RCONTROL || key == VK_LMENU || key == VK_RMENU;
}

void BeginLauncherHotkeyCapture() {
    g_launcher_hotkey_capture = true;
    for (std::uint32_t key = 0; key <= 0xff; ++key) {
        g_launcher_hotkey_down[key] =
            (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    }
}

std::optional<std::uint32_t> CaptureLauncherHotkey() {
    if (!g_launcher_hotkey_capture) return std::nullopt;
    for (std::uint32_t key = 8; key <= 0xff; ++key) {
        if (key >= VK_LBUTTON && key <= VK_XBUTTON2) continue;
        // The generic aliases report both physical Shift keys. Skip them so
        // the left/right virtual key, especially VK_RSHIFT, can be captured.
        if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) continue;
        const bool down =
            (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        const bool pressed = down && !g_launcher_hotkey_down[key];
        g_launcher_hotkey_down[key] = down;
        if (!pressed) continue;
        if (key == VK_ESCAPE) {
            g_launcher_hotkey_capture = false;
            return std::nullopt;
        }
        if (IsLauncherHotkeyModifier(key)) continue;
        g_launcher_hotkey_capture = false;
        return key;
    }
    return std::nullopt;
}

float DpiScale(UINT dpi) noexcept {
    return dpi == 0 ? 1.0f : static_cast<float>(dpi) / kDefaultDpi;
}

float Scale(float value) noexcept {
    return std::round(value * g_launcher_dpi_scale);
}

ImVec2 Scale(float x, float y) noexcept {
    return ImVec2(Scale(x), Scale(y));
}

float ButtonHeight(float logical_height) noexcept {
    return (std::max)(Scale(logical_height), ImGui::GetFrameHeight());
}

void ApplyLauncherDpiScale() noexcept {
    unitymem::ApplyCabbirdUiStyle();
    ImGui::GetStyle().ScaleAllSizes(g_launcher_dpi_scale);
    static_cast<void>(unitymem::ApplyCabbirdUiFontScale(
        g_launcher_dpi_scale * kLauncherFontScale));
    g_launcher_dpi_changed = false;
}

bool CreateRenderTarget(Graphics& graphics) {
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(graphics.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    return SUCCEEDED(graphics.device->CreateRenderTargetView(
        back_buffer.Get(), nullptr, &graphics.render_target));
}

bool CreateGraphics(HWND window, Graphics& graphics) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION, &description, &graphics.swap_chain, &graphics.device,
        &selected, &graphics.context);
    return SUCCEEDED(result) && CreateRenderTarget(graphics);
}

void LoadLogo(Graphics& graphics) {
    const HRSRC resource = FindResourceW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kLogoResourceId), RT_RCDATA);
    if (resource == nullptr) return;
    const HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), resource);
    const DWORD size = SizeofResource(GetModuleHandleW(nullptr), resource);
    const void* data = loaded == nullptr ? nullptr : LockResource(loaded);
    if (data == nullptr || size == 0) return;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const auto decoded = cabbird::DecodeUiImageRgba8(std::span(bytes, size));
    if (!decoded) return;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = decoded.image.width;
    description.Height = decoded.image.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = decoded.image.pixels.data();
    initial.SysMemPitch = decoded.image.width * 4U;
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(graphics.device->CreateTexture2D(&description, &initial, &texture))) {
        static_cast<void>(graphics.device->CreateShaderResourceView(
            texture.Get(), nullptr, &graphics.logo));
    }
}

LRESULT WINAPI WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    switch (message) {
    case WM_SIZE:
        if (g_graphics != nullptr && g_graphics->swap_chain != nullptr &&
            wparam != SIZE_MINIMIZED) {
            g_graphics->render_target.Reset();
            if (SUCCEEDED(g_graphics->swap_chain->ResizeBuffers(
                    0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0))) {
                static_cast<void>(CreateRenderTarget(*g_graphics));
            }
        }
        return 0;
    case WM_DPICHANGED: {
        g_launcher_dpi_scale = DpiScale(HIWORD(wparam));
        g_launcher_dpi_changed = true;
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
            static_cast<void>(SetWindowPos(
                window, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER));
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize = {
            static_cast<LONG>(Scale(720.0f)), static_cast<LONG>(Scale(500.0f))};
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0U) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::optional<std::filesystem::path> ChooseGameDirectory(
    HWND owner, const std::filesystem::path& current,
    const cabbird::Translator& translator) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    const std::wstring title = Utf8Wide(
        translator.Text(cabbird::MessageId::LauncherDialogGameDirectory));
    dialog->SetTitle(title.c_str());
    if (!current.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::optional<std::filesystem::path> ChooseLauncherExecutable(
    HWND owner, const std::filesystem::path& current,
    const cabbird::launcher::AzurPromiliaClient client, const cabbird::Translator& translator) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
            FOS_PATHMUSTEXIST);
    }
    const std::wstring executable_filter = Utf8Wide(
        translator.Text(cabbird::MessageId::LauncherDialogExecutableFilter));
    const wchar_t* launcher_name = client == cabbird::launcher::AzurPromiliaClient::Global
        ? L"launcher.exe" : L"launcher.exe";
    const wchar_t* launcher_label = client == cabbird::launcher::AzurPromiliaClient::Global
        ? L"UNITY Global Launcher" : L"UNITY Launcher";
    const COMDLG_FILTERSPEC filters[]{
        {launcher_label, launcher_name},
        {executable_filter.c_str(), L"*.exe"},
    };
    static_cast<void>(dialog->SetFileTypes(
        static_cast<UINT>(std::size(filters)), filters));
    const std::wstring title = Utf8Wide(
        translator.Text(cabbird::MessageId::LauncherDialogUnityLauncher));
    dialog->SetTitle(title.c_str());
    if (!current.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

void Tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", text);
    }
}

bool IconButton(const char* id, char32_t glyph, const char* tooltip, bool enabled = true) {
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    const float extent = ButtonHeight(30.0f);
    const bool pressed = ImGui::Button(Glyph(glyph), ImVec2(extent, extent));
    ImGui::EndDisabled();
    Tooltip(tooltip);
    ImGui::PopID();
    return pressed && enabled;
}

bool CommandButton(
    const char* id, char32_t glyph, const char* label,
    bool primary, bool enabled, ImVec2 size = {}) {
    std::string text = std::string(Glyph(glyph)) + "  " + label;
    ImVec2 scaled_size(
        size.x > 0.0f ? Scale(size.x) : 0.0f,
        ButtonHeight(size.y > 0.0f ? size.y : 30.0f));
    if (scaled_size.x > 0.0f) {
        text = Ellipsize(text,
            (std::max)(0.0f, scaled_size.x - ImGui::GetStyle().FramePadding.x * 2.0f));
    }
    const auto& theme = unitymem::CabbirdUiTheme();
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button,
        ThemeColor(primary ? theme.accent : theme.button));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        ThemeColor(primary ? theme.accent_hovered : theme.button_hovered));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        ThemeColor(primary ? theme.accent_active : theme.button_active));
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        ThemeColor(primary ? theme.inverse_text : theme.text_muted));
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(text.c_str(), scaled_size);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    return pressed && enabled;
}

bool ModeButton(const char* id, const char* label, bool selected, float width) {
    const auto& theme = unitymem::CabbirdUiTheme();
    const ImVec2 size(Scale(width), ButtonHeight(30.0f));
    const std::string text = Ellipsize(label,
        (std::max)(0.0f, size.x - ImGui::GetStyle().FramePadding.x * 2.0f));
    ImGui::PushID(id);
    ImGui::PushStyleColor(
        ImGuiCol_Button,
        selected ? ThemeColorWithAlpha(theme.accent, 0.16f) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        selected ? ThemeColorWithAlpha(theme.accent, 0.26f)
                 : ThemeColor(theme.button_hovered));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ThemeColor(selected ? theme.accent : theme.text_muted));
    const bool pressed = ImGui::Button(text.c_str(), size);
    ImGui::PopStyleColor(3);
    ImGui::PopID();
    return pressed;
}

const char* Text(const cabbird::Translator& translator, cabbird::MessageId id) noexcept {
    return translator.Text(id).data();
}

std::string StableLabel(
    const cabbird::Translator& translator, cabbird::MessageId id,
    std::string_view stable_id) {
    return cabbird::StableDisplayLabel(translator.Text(id), stable_id);
}

std::string Format(
    const cabbird::Translator& translator, cabbird::MessageId id,
    std::initializer_list<std::string_view> arguments) {
    return translator.Format(id, std::span<const std::string_view>(
        arguments.begin(), arguments.size()));
}

std::string RenderMessage(
    const cabbird::Translator& translator, const LauncherMessage& message) {
    std::vector<std::string_view> arguments;
    arguments.reserve(message.arguments.size());
    for (const auto& argument : message.arguments) arguments.push_back(argument);
    std::string result = message.arguments.empty()
        ? std::string(translator.Text(message.id))
        : translator.Format(message.id, arguments);
    if (!message.detail.empty()) {
        if (!result.empty()) result.append(": ");
        result.append(message.detail);
    }
    return result;
}

void DrawHeader(
    Graphics& graphics, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    const auto& theme = unitymem::CabbirdUiTheme();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColor(theme.header_background));
    ImGui::BeginChild(
        "LauncherHeader", ImVec2(0.0f, Scale(kHeaderHeight)), ImGuiChildFlags_None);
    ImGui::SetCursorPos(Scale(16.0f, 13.0f));
    if (graphics.logo != nullptr) {
        ImGui::Image(
            static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(graphics.logo.Get())),
            Scale(30.0f, 30.0f));
        ImGui::SameLine(0.0f, Scale(10.0f));
    }
    ImGui::SetCursorPosY(Scale(18.0f));
    ImGui::TextUnformatted("CabbirdLauncher");
    const char* state = Text(translator, snapshot.busy
        ? cabbird::MessageId::LauncherStateWorking
        : cabbird::MessageId::LauncherStateReady);
    const ImVec2 state_size = ImGui::CalcTextSize(state);
    ImGui::SetCursorPos(ImVec2(
        ImGui::GetWindowWidth() - state_size.x - Scale(18.0f), Scale(19.0f)));
    ImGui::TextColored(snapshot.busy ? ThemeColor(theme.warning) : ThemeColor(theme.success),
        "%s", state);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawModes(
    const LauncherSnapshot& snapshot,
    LauncherMode& mode, const cabbird::Translator& translator) {
    const auto& theme = unitymem::CabbirdUiTheme();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(16.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColor(theme.toolbar_background));
    // NoScrollbar, because this bar showed one on its right edge.  The cause was arithmetic: the
    // child is exactly Scale(kModeHeight) (48 design units) and AlwaysUseWindowPadding removes 2x9 of
    // it, leaving 30 -- exactly ButtonHeight(30.0f), with nothing to spare.  The table this bar used
    // to sit in then added CellPadding.y on top of the row, so the content overflowed by roughly that
    // padding and ImGui offered a scrollbar for a bar with one row and nothing to scroll.  Removing
    // the table (the server selector is gone) fixes it; the flag stops a style change re-creating it.
    ImGui::BeginChild(
        "LauncherModes", ImVec2(0.0f, Scale(kModeHeight)),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::BeginDisabled(snapshot.busy);
    if (ModeButton("live-attach", Text(translator,
            cabbird::MessageId::LauncherModeLiveAttach),
            mode == LauncherMode::Attach, 126.0f)) {
        mode = LauncherMode::Attach;
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void DrawReadOnlyPath(
    const char* id, const std::filesystem::path& path, float trailing_width,
    const cabbird::Translator& translator) {
    std::string value = path.empty()
        ? std::string(translator.Text(cabbird::MessageId::LauncherNoDirectorySelected))
        : PathUtf8(path);
    ImGui::SetNextItemWidth((std::max)(
        Scale(120.0f), ImGui::GetContentRegionAvail().x - Scale(trailing_width)));
    ImGui::InputText(
        id, value.data(), value.size() + 1,
        ImGuiInputTextFlags_ReadOnly);
}

void DrawRecoveryAxis(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator, const char* stable_id,
    cabbird::MessageId label, cabbird::MessageId action,
    cabbird::RuntimeRecoveryAxis axis) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, Scale(28.0f));
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, label));
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginDisabled(snapshot.busy);
    ImGui::PushID(stable_id);
    if (ImGui::SmallButton(Text(translator, action))) controller.RestoreRecovery(axis);
    ImGui::PopID();
    ImGui::EndDisabled();
}

void DrawRecoveryState(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    const auto& theme = unitymem::CabbirdUiTheme();
    const bool active = snapshot.recovery && snapshot.recovery->safe_mode.Active();
    if (!active && snapshot.recovery_message.empty()) return;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, cabbird::MessageId::LauncherSectionRecovery));
    if (!snapshot.recovery_message.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ThemeColor(theme.danger), "%s", snapshot.recovery_message.c_str());
        ImGui::PopTextWrapPos();
        return;
    }

    const auto& safe_mode = snapshot.recovery->safe_mode;
    ImGui::TextColored(ThemeColor(theme.warning), "%s",
        Text(translator, cabbird::MessageId::LauncherRecoverySafeModeActive));
    if (!safe_mode.reason.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ThemeColor(theme.text_muted), "%s", safe_mode.reason.c_str());
    }
    if (ImGui::BeginTable(
            "RecoveryAxes", 2, ImGuiTableFlags_SizingStretchProp,
            ImVec2(0.0f, 0.0f))) {
        const std::string axis_column = StableLabel(translator,
            cabbird::MessageId::LauncherRecoveryAxis, "recovery-axis");
        const std::string action_column = StableLabel(translator,
            cabbird::MessageId::LauncherRecoveryAction, "recovery-action");
        ImGui::TableSetupColumn(axis_column.c_str(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            action_column.c_str(), ImGuiTableColumnFlags_WidthFixed, Scale(72.0f));
        if (safe_mode.minimal_core) {
            DrawRecoveryAxis(
                controller, snapshot, translator, "minimal-core",
                cabbird::MessageId::LauncherRecoveryMinimalCore,
                cabbird::MessageId::LauncherRecoveryRestore,
                cabbird::RuntimeRecoveryAxis::MinimalCore);
        }
        if (safe_mode.third_party_plugins_suspended) {
            DrawRecoveryAxis(
                controller, snapshot, translator, "third-party-plugins",
                cabbird::MessageId::LauncherRecoveryThirdPartyPlugins,
                cabbird::MessageId::LauncherRecoveryRestore,
                cabbird::RuntimeRecoveryAxis::ThirdPartyPlugins);
        }
        if (safe_mode.profile_overrides_suspended) {
            DrawRecoveryAxis(
                controller, snapshot, translator, "profile-overrides",
                cabbird::MessageId::LauncherRecoveryProfileOverrides,
                cabbird::MessageId::LauncherRecoveryRestore,
                cabbird::RuntimeRecoveryAxis::ProfileOverrides);
        }
        ImGui::EndTable();
    }
}

void DrawStartupSettings(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, cabbird::MessageId::LauncherSectionSettings));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, cabbird::MessageId::LauncherSettingMenuToggle));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Scale(190.0f));
    const auto captured = CaptureLauncherHotkey();
    if (captured) controller.SetToggleKey(*captured);
    const std::string label = g_launcher_hotkey_capture
        ? std::string(Text(translator, cabbird::MessageId::LauncherSettingPressKey))
        : VirtualKeyName(snapshot.toggle_key);
    ImGui::BeginDisabled(snapshot.busy);
    if (ImGui::Button(label.c_str(), ImVec2(Scale(180.0f), ButtonHeight(30.0f)))) {
        BeginLauncherHotkeyCapture();
    }
    ImGui::EndDisabled();
    if (g_launcher_hotkey_capture) {
        ImGui::TextDisabled("%s", Text(translator, cabbird::MessageId::LauncherSettingEscapeHint));
    }
}

void DrawAttachMode(
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    const auto& theme = unitymem::CabbirdUiTheme();
    ImGui::TextDisabled("%s",
        Text(translator, cabbird::MessageId::LauncherSectionUnityLauncher));
    DrawReadOnlyPath(
        "##UnityLauncherExecutable", snapshot.launcher_executable, 38.0f, translator);
    ImGui::SameLine();
    if (IconButton(
            "choose-unity-launcher", 0xe838,
            Text(translator, cabbird::MessageId::LauncherChooseUnityLauncher),
            !snapshot.busy)) {
        if (const auto selected = ChooseLauncherExecutable(
                window, snapshot.launcher_executable, snapshot.selected_client, translator)) {
            controller.SelectLauncherExecutable(*selected);
        }
    }

    ImGui::Spacing();
    if (snapshot.runtime_version.empty()) {
        ImGui::TextColored(ThemeColor(theme.text_muted), "%s", snapshot.runtime_message.c_str());
    } else {
        const std::string runtime = Format(translator,
            cabbird::MessageId::LauncherRuntimeVersion, {snapshot.runtime_version});
        ImGui::TextColored(ThemeColor(theme.text_muted), "%s", runtime.c_str());
    }
    DrawStartupSettings(controller, snapshot, translator);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Text(translator, cabbird::MessageId::LauncherSectionProcesses));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Scale(30.0f));
    if (IconButton("refresh-processes", 0xe72c,
            Text(translator, cabbird::MessageId::LauncherProcessRefresh), !snapshot.busy)) {
        controller.RefreshProcesses();
    }

    const float table_height = (std::max)(
        Scale(140.0f), ImGui::GetContentRegionAvail().y - Scale(62.0f));
    if (ImGui::BeginTable(
            "AttachProcesses", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0.0f, table_height))) {
        const std::string process_column = StableLabel(translator,
            cabbird::MessageId::LauncherProcessColumnProcess, "process-column");
        const std::string pid_column = StableLabel(translator,
            cabbird::MessageId::LauncherProcessColumnPid, "pid-column");
        const std::string path_column = StableLabel(translator,
            cabbird::MessageId::LauncherProcessColumnPath, "path-column");
        const std::string state_column = StableLabel(translator,
            cabbird::MessageId::LauncherProcessColumnState, "state-column");
        ImGui::TableSetupColumn(
            process_column.c_str(), ImGuiTableColumnFlags_WidthFixed, Scale(120.0f));
        ImGui::TableSetupColumn(
            pid_column.c_str(), ImGuiTableColumnFlags_WidthFixed, Scale(74.0f));
        ImGui::TableSetupColumn(path_column.c_str(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            state_column.c_str(), ImGuiTableColumnFlags_WidthFixed, Scale(110.0f));
        ImGui::TableHeadersRow();
        for (const auto& process : snapshot.processes) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, Scale(30.0f));
            ImGui::TableSetColumnIndex(0);
            const std::string process_label = WideUtf8(process.executable_name);
            ImGui::TextUnformatted(process_label.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%lu", process.process_id);
            ImGui::TableSetColumnIndex(2);
            const std::string path = PathUtf8(process.executable_path);
            ImGui::TextUnformatted(path.c_str());
            ImGui::TableSetColumnIndex(3);
            const bool attached = process.process_id == snapshot.attached_process;
            const cabbird::MessageId state = attached
                ? cabbird::MessageId::LauncherProcessStateAttached
                : process.Compatible() ? cabbird::MessageId::LauncherProcessStateDetected
                : process.inspection_error == ERROR_ACCESS_DENIED
                    ? cabbird::MessageId::LauncherProcessStateDenied
                : !process.owned_by_current_user
                    ? cabbird::MessageId::LauncherProcessStateOtherUser
                : !process.x64 ? cabbird::MessageId::LauncherProcessStateNotX64
                               : cabbird::MessageId::LauncherProcessStateUnavailable;
            ImGui::TextColored(
                attached ? ThemeColor(theme.success)
                         : process.Compatible() ? ThemeColor(theme.text_muted)
                                                 : ThemeColor(theme.danger),
                "%s", Text(translator, state));
        }
        ImGui::EndTable();
    }

    const bool can_launch = !snapshot.busy && snapshot.core_available &&
        !snapshot.launcher_executable.empty() && snapshot.processes.empty();
    if (CommandButton(
            "launch-attach-core", 0xe768,
            Text(translator, cabbird::MessageId::LauncherLaunchAttach), true, can_launch,
            ImVec2(174.0f, 32.0f))) {
        controller.LaunchAndAttach();
    }
}

void DrawFooter(
    const LauncherSnapshot& snapshot, const cabbird::Translator& translator) {
    const auto& theme = unitymem::CabbirdUiTheme();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(16.0f, 11.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColor(theme.navigation_background));
    ImGui::BeginChild(
        "LauncherFooter", ImVec2(0.0f, Scale(kFooterHeight)),
        ImGuiChildFlags_AlwaysUseWindowPadding);
    const ImVec4 color = snapshot.message_kind == MessageKind::Success
        ? ThemeColor(theme.success)
        : snapshot.message_kind == MessageKind::Error
            ? ThemeColor(theme.danger)
            : ThemeColor(theme.text_muted);
    const char32_t icon = snapshot.message_kind == MessageKind::Success ? 0xe73e
        : snapshot.message_kind == MessageKind::Error ? 0xe7ba : 0xe72c;
    ImGui::TextColored(color, "%s", Glyph(icon));
    ImGui::SameLine();
    const std::string rendered = RenderMessage(translator, snapshot.message);
    const std::string message = Ellipsize(
        rendered, (std::max)(32.0f, ImGui::GetContentRegionAvail().x));
    ImGui::TextColored(color, "%s", message.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void DrawLauncher(
    HWND window, Graphics& graphics, LauncherController& controller, LauncherMode& mode,
    const cabbird::Translator& translator) {
    const auto& theme = unitymem::CabbirdUiTheme();
    const LauncherSnapshot snapshot = controller.Snapshot();
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin(
        "CabbirdLauncherRoot", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
    DrawHeader(graphics, snapshot, translator);
    DrawModes(snapshot, mode, translator);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(16.0f, 16.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColor(theme.window_background));
    ImGui::BeginChild(
        "LauncherBody", ImVec2(0.0f, -Scale(kFooterHeight)),
        ImGuiChildFlags_AlwaysUseWindowPadding);
    DrawAttachMode(window, controller, snapshot, translator);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    DrawFooter(snapshot, translator);
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    static_cast<void>(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    const auto administrator = EnsureAdministrator(command_line);
    if (!administrator.run_current_process) return administrator.exit_code;

    // Pin the system dwmapi before ImGui's delay imports are resolved.  The pin costs one
    // LoadLibrary and keeps this program independent of whatever appears next to it later.
    const HMODULE system_dwmapi = LoadSystemDwmapi();
    if (system_dwmapi == nullptr) return static_cast<int>(GetLastError());

    const std::filesystem::path runtime_root = ExecutableDirectory() / L"Cabbird";
    const unitymem::AnalyzerConfig config =
        unitymem::AnalyzerConfig::Load(runtime_root / L"cabbird.ini");
    const auto locale = cabbird::ResolveUserLocale(config.platform_language);
    const auto translator_result = cabbird::LoadHostCatalog(
        locale.locale, runtime_root / L"locales" / L"host");
    if (translator_result.translator == nullptr) return ERROR_RESOURCE_DATA_NOT_FOUND;
    const std::shared_ptr<const cabbird::Translator> translator =
        translator_result.translator;

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    WNDCLASSEXW window_class{
        .cbSize = sizeof(window_class),
        .style = CS_CLASSDC,
        .lpfnWndProc = WindowProc,
        .hInstance = instance,
        .hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kIconResourceId)),
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .lpszClassName = L"CabbirdLauncherWindow",
        .hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(kIconResourceId)),
    };
    if (RegisterClassExW(&window_class) == 0) return 1;
    const std::wstring window_title = Utf8Wide(
        translator->Text(cabbird::MessageId::LauncherWindowTitle));
    g_launcher_dpi_scale = DpiScale(GetDpiForSystem());
    const HWND window = CreateWindowExW(
        0, window_class.lpszClassName, window_title.c_str(),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        static_cast<int>(Scale(860.0f)), static_cast<int>(Scale(600.0f)),
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        UnregisterClassW(window_class.lpszClassName, instance);
        return 2;
    }

    Graphics graphics;
    g_graphics = &graphics;
    if (!CreateGraphics(window, graphics)) {
        DestroyWindow(window);
        UnregisterClassW(window_class.lpszClassName, instance);
        g_graphics = nullptr;
        return 3;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    static_cast<void>(unitymem::ConfigureCabbirdUiFontAtlas(runtime_root));
    g_launcher_dpi_scale = DpiScale(GetDpiForWindow(window));
    ApplyLauncherDpiScale();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAuto = false;
    io.ConfigNavEscapeClearFocusWindow = true;
    io.IniFilename = nullptr;
    if (!ImGui_ImplWin32_Init(window) ||
        !ImGui_ImplDX11_Init(graphics.device.Get(), graphics.context.Get())) {
        ImGui::DestroyContext();
        graphics = {};
        DestroyWindow(window);
        UnregisterClassW(window_class.lpszClassName, instance);
        g_graphics = nullptr;
        return 4;
    }
    LoadLogo(graphics);
    LauncherController controller(ExecutableDirectory());
    LauncherMode mode = LauncherMode::Attach;
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    bool running = true;
    while (running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (g_launcher_dpi_changed) ApplyLauncherDpiScale();
        if (IsIconic(window)) {
            Sleep(16);
            continue;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawLauncher(window, graphics, controller, mode, *translator);
        ImGui::Render();
        const ImVec4 clear = ThemeColor(unitymem::CabbirdUiTheme().window_background);
        graphics.context->OMSetRenderTargets(1, graphics.render_target.GetAddressOf(), nullptr);
        graphics.context->ClearRenderTargetView(graphics.render_target.Get(),
            &clear.x);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        graphics.swap_chain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_graphics = nullptr;
    graphics = {};
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
