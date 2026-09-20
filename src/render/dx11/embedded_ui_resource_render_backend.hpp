#pragma once

#include "cabbird/unitymem_compat.hpp"
#include <memory>

namespace cabbird {
class UiResourceRenderBackend;
}

namespace cabbird::embedded {

struct EmbeddedState;

// The factory is local to the D3D12 host. Its result crosses into
// PluginManager only through the backend-neutral internal interface.
[[nodiscard]] std::shared_ptr<cabbird::UiResourceRenderBackend>
CreateEmbeddedUiResourceRenderBackend(EmbeddedState& state) noexcept;

}  // namespace cabbird::embedded
