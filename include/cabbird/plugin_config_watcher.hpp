#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace cabbird {

struct PluginConfigWatcherOptions {
    std::chrono::milliseconds debounce{750};
};

// Watches one file inside a flat directory using a Windows directory change
// notification. The target file is re-signed after the configured debounce, so
// temp-file then rename publish sequences settle before callers observe the
// new content.
class PluginConfigFileWatcher final {
public:
    using Callback = std::function<void()>;
    using Clock = std::chrono::steady_clock;

    PluginConfigFileWatcher(
        std::filesystem::path directory,
        std::filesystem::path file_name,
        PluginConfigWatcherOptions options = {});
    ~PluginConfigFileWatcher();

    PluginConfigFileWatcher(const PluginConfigFileWatcher&) = delete;
    PluginConfigFileWatcher& operator=(const PluginConfigFileWatcher&) = delete;

    [[nodiscard]] bool Start(Callback callback);
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    void ResetBaseline() noexcept;

private:
    [[nodiscard]] std::filesystem::path Target() const;
    [[nodiscard]] std::uint64_t Signature() const;
    void Run(std::stop_token stop_token, void* change_handle);

    std::filesystem::path directory_;
    std::filesystem::path file_name_;
    PluginConfigWatcherOptions options_;
    mutable std::mutex mutex_;
    bool pending_{};
    std::uint64_t last_signature_{};
    Callback callback_;
    std::jthread worker_;
};

}  // namespace cabbird
