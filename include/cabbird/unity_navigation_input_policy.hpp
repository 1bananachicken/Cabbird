#pragma once

#include "cabbird/hook_manager.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace cabbird {

// Host-owned input policy used by the UNITY navigation bridge. It is active only
// while MoveToPointByTransform is being dispatched on the Game thread.
class UnityNavigationInputPolicy final {
public:
    UnityNavigationInputPolicy(
        std::unique_ptr<HookBackend> backend,
        std::uint32_t controller_get_player_character_vtable_offset,
        std::uint32_t character_set_custom_ignore_move_input_vtable_offset,
        std::uint32_t character_set_custom_limit_input_vtable_offset);
    ~UnityNavigationInputPolicy();

    UnityNavigationInputPolicy(const UnityNavigationInputPolicy&) = delete;
    UnityNavigationInputPolicy& operator=(const UnityNavigationInputPolicy&) = delete;

    [[nodiscard]] bool Start(void* target);
    bool Stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    [[nodiscard]] bool Started() const noexcept;

    // Returns the prior thread-local scope and marks this policy active.
    [[nodiscard]] void* Enter() noexcept;
    void Leave(void* previous) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabbird
