#include "cabbird/plugin_capability_policy.hpp"

#include <algorithm>
#include <array>

namespace cabbird {
namespace {

struct ServiceCapabilityMapping {
    std::string_view service;
    std::string_view capability;
};

/* `std::to_array` RATHER THAN A HAND-WRITTEN COUNT, AND THAT IS THE FIX RATHER THAN A TIDY-UP.
 *
 * This table was declared `std::array<std::string_view, 33>` while holding 32 names, and
 * `kServiceCapabilities` was declared `..., 34>` while holding 33 entries.  `std::array` value-
 * initialises the slots an initialiser list does not fill, so both tables ended with a trailing
 * EMPTY string_view -- and `IsKnownCapability("")` then answered true.  The manifest schema's
 * capability pattern (`^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$`) rejects the empty string before it can
 * reach this code, so nothing was exploitable; the real cost was that "how many capabilities are
 * there" had two answers, and the wrong one was the one written down.  Deriving the size from the
 * list is what stops it drifting again. */
constexpr auto kKnownCapabilities = std::to_array<std::string_view>({
    "unity-il2cpp",
    "commands",
    "configuration",
    "diagnostics",
    "ipc",
    "game-events",
    "interop-hook",
    "interop-patch",
    "interop-signature",
    "memory-read",
    "memory-write",
    "notifications",
    "runtime-info",
    "scheduler",
    "storage",
    "input",
    "ui-font",
    "ui-texture",
    "ui-window",
    "unity-overlay",
    "unity-entities",
    /* Reading the local player's world position.  IT NO LONGER INCLUDES MOVING IT: the write
     * half is `unity-player-teleport` below, and this comment used to argue the opposite ("the
     * two halves are one feature ... a split would create a third state that no manifest asks
     * for").  Both halves of that argument turned out wrong -- the sibling project's
     * `NtePosition` and `NteTeleport` manifests ask for exactly the two states, and this tree's
     * own `player_coords` plugin is a manifest that wants to look without wanting to move.
     *
     * NAMED FOR THE READ, like the sibling's `nte-player-snapshot`, and that is not cosmetic:
     * `unity-player` said nothing about which half it was, which is how the write entries below
     * stayed reachable through this capability's service for as long as they did.  The pair now
     * reads as the sibling's does -- `...-snapshot` / `...-teleport` -- so the name alone answers
     * "may I look, or may I move?" */
    "unity-player-snapshot",
    /* The WRITE half of the player bridge, split from the snapshot above so a coordinate display
     * does not need the ability to move anything.
     *
     * The split is the sibling project's, and its manifests are the evidence: its `NtePosition`
     * ("Coordinate Display") declares `anomaly.nte.player` alone -- a read -- while `NteTeleport`
     * declares that AND `anomaly.nte.player-teleport`.  Two visibly different grants there, and
     * a single table forced them to be one here.
     *
     * NOT folded into `unity-transform` either, although a teleport ends in a transform write:
     * `unity-player-teleport` names a POLICY ("move the local player, and tell me whether it
     * stuck" -- the host owns the identity decision and the post-check), while
     * `unity-transform` names a MECHANISM ("move this object").  A teleporter wants the first; a
     * tool that repositions arbitrary objects wants the second. */
    "unity-player-teleport",
    
    "unity-transform",
    /* Reading the live IL2CPP type system out of the process.  Worth its own capability
     * rather than reusing a neighbouring one: it is the only entry in this list that can
     * make the host walk every class in the runtime, and the crash it can cause (a lazy
     * metadata initialisation on a thread the runtime does not know) is not something a
     * plugin should be able to reach by accident. */
    "unity-dump",
    "ui",
    "json",
    "websocket",
});  // kKnownCapabilities -- size derived from the list, so the count cannot drift

constexpr auto kServiceCapabilities = std::to_array<ServiceCapabilityMapping>({
    {"cabbird.unity.il2cpp", "unity-il2cpp"},
    {"cabbird.plugin-state", "configuration"},
    {"cabbird.config", "configuration"},
    {"cabbird.storage", "storage"},
    {"cabbird.runtime-info", "runtime-info"},
    {"cabbird.diagnostics", "diagnostics"},
    {"cabbird.scheduler", "scheduler"},
    {"cabbird.ipc", "ipc"},
    {"cabbird.websocket", "websocket"},
    {"cabbird.commands", "commands"},
    {"cabbird.notifications", "notifications"},
    {"cabbird.interop.signature", "interop-signature"},
    {"cabbird.interop.hook", "interop-hook"},
    {"cabbird.interop.patch", "interop-patch"},
    {"cabbird.ui", "ui"},
    {"cabbird.localization", "ui"},
    {"cabbird.window", "ui-window"},
    {"cabbird.font", "ui-font"},
    {"cabbird.texture", "ui-texture"},
    {"cabbird.input", "input"},
    {"cabbird.json", "json"},
    {"cabbird.unity.overlay", "unity-overlay"},
    
    /* The entity set.  Its own capability for the same reason the table is its own service: a
     * consumer that wants the object model should not have to be granted the overlay. */
    {"cabbird.unity.entities", "unity-entities"},
    /* The type-system dump.  Same reasoning as the capability itself: a plugin that asks
     * for it can make the host walk every class, so it is a separate grant. */
    {"cabbird.unity.dump", "unity-dump"},
    /* The local player SNAPSHOT.  The write half moved to the entry below; the two legacy write
     * entries the v1 table still carries are authorized per call through
     * `PluginCapabilityGrant::AuthorizePlayerWrite`, the same shape as the memory pair. */
    {"cabbird.unity.player", "unity-player-snapshot"},
    /* The teleport.  Its own entry because it is the mutation: a manifest that asks to LOOK at
     * the player must not gain the ability to MOVE it, which is precisely what one shared
     * grant (`unity-player`, before the rename above) could not express. */
    {"cabbird.unity.player-teleport", "unity-player-teleport"},
    /* The engine-side transform capability.  Mapped to its OWN capability and not to
     * `unity-player-teleport`, so a manifest that only wants the player teleport does not
     * silently gain "move any object", and one that wants the mechanism does not have to claim
     * to want the player. */
    {"cabbird.unity.transform", "unity-transform"},
});  // kServiceCapabilities -- size derived from the list

/* `cabbird.unity.player` maps to `unity-player-snapshot` (a read), but its v1 table still carries
 * the two write entries `write_position` / `write_state` -- append-only ABI means they cannot be
 * removed, and `include/cabbird/unity_services.hpp` keeps them working on purpose ("the
 * compatibility path and the documented path are the SAME code").
 *
 * SO THIS TABLE, ALONE WITH `cabbird.core`, MIXES TWO CHARACTERS -- and the framework already has
 * an answer for that shape, which is the one the sibling project uses for the same problem:
 * `cabbird.core` publishes memory reads and writes in one table, and they are authorized per call
 * by the NAMED PAIR `memory-read` / `memory-write` (`PluginCapabilityGrant::AuthorizeRawMemory`),
 * not by the service grant alone.
 *
 * `AuthorizePlayerWrite` below is that same mechanism for the player write, and it lives in THIS
 * file rather than in the query path because that is where the whole policy is legible: the
 * mapping table says what a SERVICE costs, this says what one mixed-character ENTRY costs.  A
 * plugin that declares only `unity-player-snapshot` therefore cannot move the player, and the
 * reason is visible without reading the adapter. */

/* The one capability `AuthorizePlayerWrite` names.  The read half needs no constant here: it is
 * the mapping table's business (`cabbird.unity.player` -> `unity-player-snapshot`). */
constexpr std::string_view kPlayerTeleportCapability = "unity-player-teleport";

[[nodiscard]] bool IsKnownCapability(const std::string_view capability) noexcept {
    return std::ranges::find(kKnownCapabilities, capability) != kKnownCapabilities.end();
}

[[nodiscard]] const ServiceCapabilityMapping* FindServiceCapability(
    const std::string_view service) noexcept {
    const auto found = std::ranges::find(
        kServiceCapabilities, service, &ServiceCapabilityMapping::service);
    return found == kServiceCapabilities.end() ? nullptr : &*found;
}

}  // namespace

bool PluginCapabilityGrant::HasCapability(const std::string_view capability) const noexcept {
    return std::ranges::find(capabilities_, capability) != capabilities_.end();
}

PluginServiceAuthorization PluginCapabilityGrant::AuthorizeService(
    const std::string_view service_id) const noexcept {
    if (service_id == "cabbird.core") return {true, {}};
    const ServiceCapabilityMapping* mapping = FindServiceCapability(service_id);
    if (mapping == nullptr) return {false, {}};
    return {HasCapability(mapping->capability), mapping->capability};
}

PluginServiceAuthorization PluginCapabilityGrant::AuthorizeRawMemory(
    const std::string_view capability) const noexcept {
    constexpr std::string_view kMemoryReadCapability = "memory-read";
    constexpr std::string_view kMemoryWriteCapability = "memory-write";
    const std::string_view required_capability = capability == kMemoryReadCapability
        ? kMemoryReadCapability
        : capability == kMemoryWriteCapability ? kMemoryWriteCapability : std::string_view{};
    if (required_capability.empty()) return {false, {}};
    return {HasCapability(required_capability), required_capability};
}

PluginServiceAuthorization PluginCapabilityGrant::AuthorizePlayerWrite() const noexcept {
    return {HasCapability(kPlayerTeleportCapability), kPlayerTeleportCapability};
}

PluginCapabilityGrant ResolvePluginCapabilityGrant(const PluginManifest* manifest) {
    PluginCapabilityGrant grant;
    if (manifest == nullptr ||
        manifest->schema_version != kLatestPluginManifestSchemaVersion) {
        grant.enforceable_ = false;
        return grant;
    }

    const auto add_capability = [&grant](const std::string_view capability) {
        if (!grant.HasCapability(capability)) grant.capabilities_.emplace_back(capability);
    };

    for (const std::string& capability : manifest->capabilities) {
        if (IsKnownCapability(capability)) {
            add_capability(capability);
        } else {
            grant.audits_.push_back({
                PluginCapabilityAuditCode::UnknownCapability,
                capability,
                {},
            });
        }
    }

    for (const PluginServiceRequirement& service : manifest->services) {
        if (service.optional || service.id == "cabbird.core") continue;
        const ServiceCapabilityMapping* mapping = FindServiceCapability(service.id);
        if (mapping == nullptr) {
            grant.enforceable_ = false;
            grant.audits_.push_back({
                PluginCapabilityAuditCode::RequiredServiceMissingMapping,
                {}, service.id,
            });
        } else if (!grant.HasCapability(mapping->capability)) {
            grant.enforceable_ = false;
            grant.audits_.push_back({
                PluginCapabilityAuditCode::RequiredServiceMissingCapability,
                std::string(mapping->capability), service.id,
            });
        }
    }
    if (std::ranges::any_of(grant.audits_, [](const PluginCapabilityAudit& audit) {
            return audit.code == PluginCapabilityAuditCode::UnknownCapability;
        })) {
        grant.enforceable_ = false;
    }
    return grant;
}

}  // namespace cabbird
