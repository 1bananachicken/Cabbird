#include "cabbird/unitymem_compat.hpp"
#include "../render/dx11/embedded_host_internal.hpp"

#include "cabbird/host_ui_service.hpp"

namespace cabbird::embedded {

const CabbirdUiServiceV1* EmbeddedUiServiceTable() noexcept {
    return cabbird::HostUiServiceTable();
}

}  // namespace cabbird::embedded
