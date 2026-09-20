#pragma once

#include "cabbird/plugin_scope.hpp"
#include "cabbird/sdk/services/ipc.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cabbird {

enum class IpcCallingDomain : std::uint8_t {
    Unknown,
    Lifecycle,
    Worker,
    Game,
    Render,
};

struct IpcPluginOwner final {
    std::shared_ptr<PluginScope> scope;
    std::vector<std::string> dependencies;
};

struct IpcEndpointDiagnostics final {
    std::string id;
    std::string provider;
    std::vector<std::string> consumers;
    std::string request_schema_hash;
    std::string response_schema_hash;
    std::string event_schema_hash;
    std::uint64_t generation{};
    std::uint32_t major_version{};
    std::uint32_t minor_version{};
    std::uint32_t modes{};
    std::uint32_t affinity{};
    std::uint64_t calls{};
    std::uint64_t failures{};
    std::uint64_t timeouts{};
    std::uint64_t events{};
    std::size_t subscriptions{};
    std::size_t pending_calls{};
    double p95_milliseconds{};
};

struct IpcDiagnostics final {
    std::vector<IpcEndpointDiagnostics> endpoints;
};

using IpcPost = std::function<bool(
    std::uint32_t affinity,
    std::string owner,
    std::uint64_t generation,
    std::function<void()> callback)>;

class IpcRegistry final {
public:
    explicit IpcRegistry(IpcPost post = {});
    ~IpcRegistry();

    IpcRegistry(const IpcRegistry&) = delete;
    IpcRegistry& operator=(const IpcRegistry&) = delete;

    [[nodiscard]] CabbirdStatusV1 RegisterEndpoint(
        const IpcPluginOwner& owner,
        const CabbirdIpcEndpointDescriptorV1* descriptor,
        CabbirdIpcRequestHandlerV1 request_handler,
        void* callback_user,
        CabbirdGenerationHandleV1* endpoint) noexcept;
    [[nodiscard]] CabbirdStatusV1 UnregisterEndpoint(
        const IpcPluginOwner& owner, CabbirdGenerationHandleV1 endpoint) noexcept;
    [[nodiscard]] CabbirdStatusV1 Invoke(
        const IpcPluginOwner& owner,
        IpcCallingDomain calling_domain,
        const CabbirdIpcEndpointSelectorV1* selector,
        CabbirdByteSpanV1 request,
        CabbirdMutableByteSpanV1 response,
        std::size_t* response_size) noexcept;
    [[nodiscard]] CabbirdStatusV1 InvokeAsync(
        const IpcPluginOwner& owner,
        const CabbirdIpcEndpointSelectorV1* selector,
        CabbirdByteSpanV1 request,
        CabbirdIpcCompletionCallbackV1 completion,
        void* completion_user,
        CabbirdGenerationHandleV1* pending_call) noexcept;
    [[nodiscard]] CabbirdStatusV1 Cancel(
        const IpcPluginOwner& owner, CabbirdGenerationHandleV1 pending_call) noexcept;
    [[nodiscard]] CabbirdStatusV1 Subscribe(
        const IpcPluginOwner& owner,
        const CabbirdIpcEndpointSelectorV1* selector,
        CabbirdIpcEventCallbackV1 callback,
        void* callback_user,
        CabbirdGenerationHandleV1* subscription) noexcept;
    [[nodiscard]] CabbirdStatusV1 Unsubscribe(
        const IpcPluginOwner& owner, CabbirdGenerationHandleV1 subscription) noexcept;
    [[nodiscard]] CabbirdStatusV1 Publish(
        const IpcPluginOwner& owner,
        CabbirdGenerationHandleV1 endpoint,
        CabbirdByteSpanV1 event) noexcept;
    [[nodiscard]] IpcDiagnostics Snapshot() const noexcept;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace cabbird
