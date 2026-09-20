#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cabbird {

// This describes a recorder boundary, not a packet inferred from a UDP length.
// Only a verified post-handler boundary can supply the exact valid bit count.
enum class UnityPacketCaptureStage : std::uint8_t {
    Unknown,
    PrePacketHandler,
    PostPacketHandler,
};

enum class UnityPacketDirection : std::uint8_t {
    Unknown,
    Inbound,
    Outbound,
};

enum class UnityPacketProtection : std::uint8_t {
    Clear,
    Compressed,
    Encrypted,
    Unknown,
};

enum class UnityAckMode : std::uint8_t {
    None,
    Always,
    PresenceBit,
};

enum class UnityPartialIdMode : std::uint8_t {
    // The wire layout contains no grouping identifier. A partial bunch is
    // therefore rejected until a validated profile supplies a grouping codec.
    None,
    Explicit,
    // Reserved for a separately verified codec. A reliable sequence by itself
    // cannot distinguish all fragments in a multi-fragment bunch.
    ReliableSequence,
};

enum class UnitySchemaFamily : std::uint8_t {
    LegacyRepLayout,
    Iris,
};

// The format of the field stream at the verified capture boundary. This is
// deliberately separate from the UE schema family: raw RepLayout and Iris
// streams require distinct, evidence-backed codecs and are never inferred.
enum class UnityFieldStreamEncoding : std::uint8_t {
    Unknown,
    VerifiedTaggedFieldsV1,
};

enum class UnityFieldEncoding : std::uint8_t {
    Boolean,
    Unsigned,
    Signed,
    Float32,
    Utf8,
    Bytes,
    Vector3Float32,
};

enum class UnityPacketDeserializeStatus : std::uint8_t {
    Decoded,
    Empty,
    Duplicate,
    ReassemblyPending,
    UnsupportedProfile,
    Encrypted,
    Compressed,
    Truncated,
    Malformed,
    UnknownSchema,
    LimitExceeded,
    ResourceExhausted,
};

struct UnityPacketLimits final {
    std::size_t maximum_packet_bits{65535U * 8U};
    std::size_t maximum_connections{64};
    std::size_t maximum_packet_history{128};
    std::size_t maximum_bunches_per_packet{1024};
    std::size_t maximum_open_partial_bunches{64};
    std::size_t maximum_partial_fragments{64};
    std::size_t maximum_partial_packet_age{256};
    std::size_t maximum_partial_bunch_bits{65535U * 8U};
    std::size_t maximum_partial_state_bits{65535U * 8U * 16U};
    std::size_t maximum_fields_per_bunch{256};
    std::size_t maximum_field_bytes{4096};
};

struct UnityPacketHeaderLayout final {
    std::uint8_t sequence_bits{};
    UnityAckMode ack_mode{UnityAckMode::None};
    std::uint8_t ack_sequence_bits{};
    std::uint8_t ack_history_bits{};
    // Zero consumes bunches until the exact packet bit count. A nonzero value
    // serializes the number of bunches as an unsigned fixed-width integer.
    std::uint8_t bunch_count_bits{};
};

struct UnityBunchLayout final {
    std::uint32_t maximum_channels{};
    std::uint32_t maximum_channel_types{};
    std::uint32_t maximum_reliable_sequence{};
    std::uint32_t maximum_partial_id{};
    std::uint32_t maximum_payload_bits{};
    bool close_has_dormancy_bit{};
    bool has_package_map_export_flags{};
    UnityPartialIdMode partial_id_mode{UnityPartialIdMode::None};
};

struct UnityFieldDefinition final {
    std::uint32_t handle{};
    std::string name;
    UnityFieldEncoding encoding{UnityFieldEncoding::Boolean};
    // Used by Unsigned and Signed encodings. Float and vector fields always
    // consume their IEEE-754 width.
    std::uint8_t bit_width{};
    // Used by Utf8 and Bytes. The limit is inclusive.
    std::uint32_t maximum_length{};
};

struct UnityChannelSchema final {
    std::uint32_t channel_type{};
    std::string id;
    UnitySchemaFamily family{UnitySchemaFamily::LegacyRepLayout};
    UnityFieldStreamEncoding encoding{UnityFieldStreamEncoding::Unknown};
    // VerifiedTaggedFieldsV1 consists of a SerializeInt field count followed
    // by handle/value pairs. It may only be selected when the exact capture
    // boundary is proven to produce this normalized format.
    std::vector<UnityFieldDefinition> fields;
};

// A profile is supplied only after the recorder ABI, packet handler stage,
// packet layout, and schema have all been independently verified. No bundled
// profile enables this API today.
struct UnityPacketProtocolProfile final {
    std::string profile_hash;
    bool post_handler_boundary_validated{};
    UnityPacketHeaderLayout header;
    UnityBunchLayout bunch;
    UnityPacketLimits limits;
    std::vector<UnityChannelSchema> schemas;
};

struct UnityPacketInput final {
    std::uint64_t connection_id{};
    UnityPacketDirection direction{UnityPacketDirection::Unknown};
    // Strictly increasing per connection and direction. The recorder assigns
    // this before a packet enters the Worker-domain deserializer.
    std::uint64_t capture_sequence{};
    std::string_view profile_hash;
    UnityPacketCaptureStage stage{UnityPacketCaptureStage::Unknown};
    UnityPacketProtection protection{UnityPacketProtection::Unknown};
    std::span<const std::uint8_t> bytes;
    std::size_t bit_count{};
};

struct UnityPacketHeader final {
    std::uint64_t sequence{};
    bool has_ack{};
    std::uint64_t ack_sequence{};
    std::uint64_t ack_history{};
};

struct UnityBunch final {
    std::uint32_t channel_index{};
    std::uint32_t channel_type{};
    bool open{};
    bool close{};
    bool dormant{};
    bool reliable{};
    std::uint32_t reliable_sequence{};
    bool partial{};
    bool partial_initial{};
    bool partial_final{};
    std::uint32_t partial_id{};
    bool has_package_map_exports{};
    bool has_must_be_mapped_guids{};
    std::size_t payload_bit_count{};
    std::vector<std::uint8_t> payload;
};

struct UnityVector3f final {
    float x{};
    float y{};
    float z{};
};

using UnityFieldValue = std::variant<
    bool,
    std::uint64_t,
    std::int64_t,
    float,
    std::string,
    std::vector<std::uint8_t>,
    UnityVector3f>;

struct UnityDecodedField final {
    std::uint32_t channel_index{};
    std::uint32_t channel_type{};
    std::string schema_id;
    std::uint32_t handle{};
    std::string name;
    UnityFieldValue value{false};
};

struct UnityPacketDeserializeResult final {
    UnityPacketDeserializeStatus status{UnityPacketDeserializeStatus::UnsupportedProfile};
    UnityPacketHeader header;
    std::vector<UnityBunch> bunches;
    std::vector<UnityDecodedField> fields;
    std::string diagnostic;

    [[nodiscard]] bool Decoded() const noexcept {
        return status == UnityPacketDeserializeStatus::Decoded;
    }
};

// Validates profile-defined limits and codec declarations without consuming a
// packet. Callers use this before exposing a profile to a Worker decoder.
[[nodiscard]] std::optional<std::string> ValidateUnityPacketProtocolProfile(
    const UnityPacketProtocolProfile& profile);

class UnityNetworkDeserializer final {
public:
    explicit UnityNetworkDeserializer(UnityPacketProtocolProfile profile);
    ~UnityNetworkDeserializer();

    UnityNetworkDeserializer(const UnityNetworkDeserializer&) = delete;
    UnityNetworkDeserializer& operator=(const UnityNetworkDeserializer&) = delete;
    UnityNetworkDeserializer(UnityNetworkDeserializer&&) = delete;
    UnityNetworkDeserializer& operator=(UnityNetworkDeserializer&&) = delete;

    // Instances have one Worker-domain owner. Callers must serialize access so
    // reliable and partial-bunch state is observed in capture order.
    [[nodiscard]] UnityPacketDeserializeResult Deserialize(const UnityPacketInput& input);
    void Reset() noexcept;
    void ResetConnection(std::uint64_t connection_id) noexcept;

    [[nodiscard]] const UnityPacketProtocolProfile& Profile() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabbird
