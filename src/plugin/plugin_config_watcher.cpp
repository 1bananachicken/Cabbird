#include "cabbird/plugin_config_watcher.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace cabbird {
namespace {

std::uint64_t FileSignature(const std::filesystem::path& file) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) return 0;
    const auto size = std::filesystem::file_size(file, error);
    if (error) return 0;
    const auto write_time = std::filesystem::last_write_time(file, error);
    if (error) return 0;
    return (static_cast<std::uint64_t>(size & 0xFFFFFFFFULL) << 32) ^
        static_cast<std::uint64_t>(write_time.time_since_epoch().count());
}

}  // namespace

PluginConfigFileWatcher::PluginConfigFileWatcher(
    std::filesystem::path directory,
    std::filesystem::path file_name,
    PluginConfigWatcherOptions options)
    : directory_(std::move(directory)),
      file_name_(std::move(file_name)),
      options_(options) {}

PluginConfigFileWatcher::~PluginConfigFileWatcher() { Stop(); }

std::filesystem::path PluginConfigFileWatcher::Target() const {
    return directory_ / file_name_;
}

std::uint64_t PluginConfigFileWatcher::Signature() const {
    return FileSignature(Target());
}

bool PluginConfigFileWatcher::Start(Callback callback) {
    std::scoped_lock lock(mutex_);
    if (worker_.joinable() || !callback) return false;
    const HANDLE change = FindFirstChangeNotificationW(
        directory_.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION);
    if (change == INVALID_HANDLE_VALUE) return false;
    try {
        pending_ = false;
        last_signature_ = Signature();
        callback_ = std::move(callback);
        worker_ = std::jthread([this, change](std::stop_token token) {
            Run(token, change);
        });
    } catch (...) {
        static_cast<void>(FindCloseChangeNotification(change));
        callback_ = {};
        return false;
    }
    return true;
}

void PluginConfigFileWatcher::Stop() noexcept {
    std::jthread worker;
    {
        std::scoped_lock lock(mutex_);
        if (!worker_.joinable()) return;
        worker_.request_stop();
        worker = std::move(worker_);
    }
    worker.join();
    std::scoped_lock lock(mutex_);
    callback_ = {};
}

bool PluginConfigFileWatcher::Running() const noexcept {
    std::scoped_lock lock(mutex_);
    return worker_.joinable();
}

void PluginConfigFileWatcher::ResetBaseline() noexcept {
    std::scoped_lock lock(mutex_);
    pending_ = false;
    last_signature_ = Signature();
}

void PluginConfigFileWatcher::Run(std::stop_token stop_token, void* change_handle) {
    const HANDLE change = static_cast<HANDLE>(change_handle);
    const HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
        static_cast<void>(FindCloseChangeNotification(change));
        return;
    }
    std::stop_callback stop_callback(stop_token, [stop_event] {
        static_cast<void>(SetEvent(stop_event));
    });
    const HANDLE events[]{stop_event, change};
    const auto wait_timeout = [](const Clock::duration remaining) {
        if (remaining <= Clock::duration::zero()) return DWORD{};
        const auto rounded =
            std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        return static_cast<DWORD>((std::min)(
            rounded, static_cast<std::int64_t>(INFINITE - 1)));
    };
    const auto mark_changed = [this] {
        std::scoped_lock lock(mutex_);
        pending_ = true;
    };
    const auto publish_if_settled = [this] {
        Callback callback;
        {
            std::scoped_lock lock(mutex_);
            pending_ = false;
            const std::uint64_t signature = Signature();
            if (signature == last_signature_) return;
            last_signature_ = signature;
            callback = callback_;
        }
        if (callback) callback();
    };

    bool running = true;
    while (running) {
        const DWORD signaled = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (signaled == WAIT_OBJECT_0) break;
        if (signaled != WAIT_OBJECT_0 + 1) break;
        mark_changed();
        if (FindNextChangeNotification(change) == FALSE) break;

        auto deadline = Clock::now() +
            (std::max)(options_.debounce, std::chrono::milliseconds::zero());
        while (running) {
            const DWORD settled = WaitForMultipleObjects(
                2, events, FALSE, wait_timeout(deadline - Clock::now()));
            if (settled == WAIT_OBJECT_0) {
                running = false;
                break;
            }
            if (settled == WAIT_OBJECT_0 + 1) {
                mark_changed();
                if (FindNextChangeNotification(change) == FALSE) {
                    running = false;
                    break;
                }
                deadline = Clock::now() +
                    (std::max)(options_.debounce, std::chrono::milliseconds::zero());
                continue;
            }
            if (settled == WAIT_TIMEOUT) publish_if_settled();
            break;
        }
    }
    static_cast<void>(FindCloseChangeNotification(change));
    CloseHandle(stop_event);
}

}  // namespace cabbird
