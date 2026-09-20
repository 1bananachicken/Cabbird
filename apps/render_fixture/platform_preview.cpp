/* Cabbird platform preview -- the host UI in an ordinary window, with no game and no injection.
 *
 * PORTED FROM Anomaly's apps/render_fixture/platform_preview.cpp (77 lines).  That file is the
 * whole tool: it loads the ini, resolves the locale, builds a PluginManager, publishes the
 * settings store, and calls RunPlatform().  There is no UI code in it at all -- the main UI
 * comes from platform_host.cpp, which is the point.  A preview that re-draws its own panels
 * would be previewing itself.
 *
 * WHY THIS EXISTS: the real host UI can only otherwise be seen by launching the game and
 * injecting, which turns every styling change into a round trip through a running game.
 *
 * WHAT IT PROVES: the whole startup path -- ini, locale, i18n catalog, PluginManager, plugin
 * discovery, settings store, theme, shell, navigation, every route -- runs and draws without a
 * game, a process target or an injected thread.  It does NOT prove the pixels match Anomaly,
 * which is a question about the theme sources rather than about a screenshot.
 *
 * DELIBERATE DEPARTURE FROM UPSTREAM, and the only one: `--frames=N` renders N frames and exits,
 * so the regression suite can run this unattended.  It is implemented through the documented
 * PlatformDiagnostics::performance_probe hook, which the host already calls once per frame, so
 * no host code is modified and no behaviour changes when the flag is absent.
 */

#include "cabbird/i18n.hpp"

#include "cabbird/config.hpp"
#include "cabbird/platform_host.hpp"
#include "cabbird/plugin_manager.hpp"
#include "cabbird/cabbird_ui_theme.hpp"
#include "cabbird/platform_settings.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace {

std::filesystem::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

/* Upstream reads --palette=<name> straight off the raw command line and lowercases the ASCII
 * letters, which sidesteps the fact that a WIN32-subsystem process has no argv without pulling in
 * the CRT's parser.  Kept as-is: the parse is part of the tool's behaviour, and a mismatch here
 * would silently select the wrong theme rather than fail. */
std::string PreviewPaletteArgument() {
    const std::wstring command_line = GetCommandLineW();
    constexpr std::wstring_view prefix = L"--palette=";
    const std::size_t start = command_line.find(prefix);
    if (start == std::wstring::npos) return "cabbirdhub";
    const std::size_t value_start = start + prefix.size();
    const std::size_t value_end = command_line.find_first_of(L" \t\r\n", value_start);
    const std::wstring value = command_line.substr(
        value_start, value_end == std::wstring::npos ? std::wstring::npos : value_end - value_start);
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character >= L'A' && character <= L'Z') {
            result.push_back(static_cast<char>(character - L'A' + L'a'));
        } else if (character >= L'a' && character <= L'z') {
            result.push_back(static_cast<char>(character));
        }
    }
    return result.empty() ? "cabbirdhub" : result;
}

/* Cabbird addition; see the header comment.  Returns 0 when --frames is absent, meaning "run
 * until the window is closed". */
int PreviewFrameLimit() {
    const std::wstring command_line = GetCommandLineW();
    constexpr std::wstring_view prefix = L"--frames=";
    const std::size_t start = command_line.find(prefix);
    if (start == std::wstring::npos) return 0;
    const std::size_t value_start = start + prefix.size();
    const std::size_t value_end = command_line.find_first_of(L" \t\r\n", value_start);
    const std::wstring value = command_line.substr(
        value_start, value_end == std::wstring::npos ? std::wstring::npos : value_end - value_start);
    int frames = 0;
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') break;
        frames = frames * 10 + static_cast<int>(character - L'0');
        if (frames > 1000000) break;
    }
    return frames;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto palette = cabbird::ParseCabbirdUiPalette(PreviewPaletteArgument());
    cabbird::SetCabbirdUiPalette(palette);
    const auto root = ExecutableDirectory() / L"Cabbird";
    auto config = cabbird::AnalyzerConfig::Load(root / L"cabbird.ini");
    const auto locale = cabbird::ResolveUserLocale(config.platform_language);
    cabbird::PlatformDiagnostics diagnostics;
    diagnostics.runtime_root = root;
    diagnostics.translator = cabbird::LoadHostCatalog(
        locale.locale, root / L"locales" / L"host").translator;
    config.platform_enabled = true;
    config.platform_visible = true;
    config.platform_embedded = false;
    config.platform_attach_to_process_window = false;

    // Cabbird addition: bounded run for the unattended regression.  Armed before the host starts
    // so it cannot be missed by a frame that begins during construction.
    const int frame_limit = PreviewFrameLimit();
    auto frames_drawn = std::make_shared<std::atomic<int>>(0);
    auto exit_posted = std::make_shared<std::atomic<bool>>(false);
    if (frame_limit > 0) {
        diagnostics.performance_probe = [frame_limit, frames_drawn, exit_posted](
                                            cabbird::PlatformUiPerformanceStage stage,
                                            std::chrono::steady_clock::duration) {
            // RefreshTotal is emitted exactly once per frame, after every other stage.
            if (stage != cabbird::PlatformUiPerformanceStage::RefreshTotal) return;
            const int drawn = frames_drawn->fetch_add(1) + 1;
            bool expected = false;
            if (drawn >= frame_limit && exit_posted->compare_exchange_strong(expected, true)) {
                std::printf("rendered %d frame(s)\n", drawn);
                std::fflush(stdout);
                PostQuitMessage(0);
            }
        };
        diagnostics.performance_probe_enabled = [] { return true; };
    }

    auto plugins = std::make_shared<cabbird::PluginManager>(root, config.plugin_directory);
    plugins->SetTranslator(diagnostics.translator);
    plugins->LoadAll();
    std::filesystem::create_directories(root / L"config");
    auto settings = std::make_shared<cabbird::PlatformSettingsStore>(root);
    static_cast<void>(settings->Start());
    diagnostics.settings_snapshot = [settings] { return settings->Snapshot(); };
    diagnostics.settings_apply = [settings](const cabbird::PlatformSettingsApplyRequest& request) {
        return settings->Apply(request);
    };
    diagnostics.settings_record_route = [settings](const std::string_view route) {
        return settings->RecordLastRoute(route);
    };
    cabbird::RunPlatform(root, config, {}, {}, {}, std::move(diagnostics), plugins);

    // Cabbird addition #2, in the same spirit as --frames: a preview that cannot say what it
    // loaded is not much of a diagnostic.  An empty enablement map and an empty pluginWindows
    // array are both consistent with "loaded" and with "not loaded", so neither can answer the
    // question.  Printing the catalog
    // the manager actually holds makes the answer a measurement instead of an argument.
    const std::vector<cabbird::PluginView> loaded = plugins->Plugins();
    // The scan directory is printed, not assumed: the first thing
    // the measurement should settle is which directory was actually scanned.
    const std::filesystem::path scan_directory = plugins->Directory();
    std::error_code scan_error;
    const bool scan_exists = std::filesystem::exists(scan_directory, scan_error);
    std::printf("plugin scan directory: %s (exists=%d)\n",
        scan_directory.string().c_str(), scan_exists ? 1 : 0);
    std::printf("plugins discovered: %zu\n", loaded.size());
    // PluginManager already records every Log() line in a ring the public Events() accessor
    // exposes, so a catalog rejection does not need a logger to be wired up -- it only needs
    // someone to ask.  Without this, "discovered: 0" is indistinguishable from "the directory is
    // wrong", "the manifest is malformed" and "the entry file is missing", because an entry that
    // fails before its manifest parses never reaches the plugin list at all.
    const std::vector<std::string> events = plugins->Events();
    for (const std::string& event : events) {
        std::printf("  log: %s\n", event.c_str());
    }
    for (const cabbird::PluginView& view : loaded) {
        std::printf("  %s [%s] state=%s enabled=%d visible=%d source=%s\n",
            view.id.c_str(), view.name.c_str(), view.state.c_str(),
            view.enabled ? 1 : 0, view.visible ? 1 : 0,
            view.source.filename().string().c_str());
        if (!view.status_reason.empty()) {
            std::printf("    reason: %s\n", view.status_reason.c_str());
        }
    }
    std::fflush(stdout);

    static_cast<void>(plugins->StopForRuntime());
    return 0;
}
