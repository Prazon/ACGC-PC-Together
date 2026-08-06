#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace acnet {

using EntityId = std::uint64_t;
using AccountId = std::uint64_t;
using TownId = std::uint64_t;
using SessionId = std::uint64_t;
using ZoneId = std::uint32_t;
using Revision = std::uint32_t;
using Tick = std::uint32_t;

constexpr std::uint32_t kWireMagic = 0x41434E54U; // ACNT
constexpr std::uint16_t kProtocolVersion = 7;
constexpr std::size_t kMaxPacketBytes = 1200;
constexpr std::size_t kMaxPayloadBytes = 1152;
constexpr std::size_t kEncryptionTagBytes = 16;
constexpr std::size_t kMaxPlaintextPayloadBytes = kMaxPayloadBytes - kEncryptionTagBytes;
constexpr std::size_t kMaxPlayersPerZone = 16;
constexpr std::size_t kReconnectTokenBytes = 32;

enum class Channel : std::uint8_t {
    Control = 0,
    Transactions = 1,
    Chat = 2,
    Snapshots = 3,
    Events = 4,
    Bulk = 5,
    Count = 6,
};

enum class Delivery : std::uint8_t {
    ReliableOrdered,
    ReliableUnordered,
    UnreliableSequenced,
};

enum class MessageType : std::uint16_t {
    ClientHello = 1,
    ServerHello = 2,
    Disconnect = 3,
    Ping = 4,
    Pong = 5,
    TownBootstrap = 6,
    TownBootstrapResult = 7,
    InputCommand = 10,
    TransformSnapshot = 11,
    Baseline = 12,
    ReplicationDeltas = 13,
    WorldRequest = 20,
    WorldResult = 21,
    InventoryRequest = 22,
    InventoryResult = 23,
    TradeRequest = 24,
    TradeResult = 25,
    FurnitureRequest = 26,
    FurnitureResult = 27,
    HouseUpdate = 28,
    HouseUpdateResult = 29,
    ConversationRequest = 30,
    ConversationResult = 31,
    EncounterRequest = 32,
    EncounterResult = 33,
    ZoneTransferRequest = 40,
    ZoneTransferOffer = 41,
    ZoneReady = 42,
    AdminCommand = 50,
    AdminResult = 51,
};

enum PacketFlags : std::uint8_t {
    PacketReliable = 1U << 0,
    PacketFragment = 1U << 1,
    PacketEncrypted = 1U << 2,
    PacketAckOnly = 1U << 3,
};

enum class ResultCode : std::uint16_t {
    Ok = 0,
    Malformed = 1,
    UnsupportedVersion = 2,
    Unauthorized = 3,
    Capacity = 4,
    StaleRevision = 5,
    Conflict = 6,
    InvalidState = 7,
    OutOfRange = 8,
    RateLimited = 9,
    NotFound = 10,
    InternalError = 11,
};

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct Transform {
    Vec3 position;
    Vec3 velocity;
    std::int16_t yaw = 0;
    std::uint16_t action = 0;
};

struct PlayerAppearance {
    std::array<std::uint8_t, 8> name{};
    std::uint8_t gender = 0;
    std::uint8_t face = 0;
    std::uint16_t clothing = 0;
    std::uint16_t equipped_item = 0;
};

enum class DoorTransitionPhase : std::uint8_t {
    None = 0,
    Leaving = 1,
    Arriving = 2,
};

struct PlayerSnapshot {
    EntityId entity = 0;
    AccountId account = 0;
    ZoneId zone = 0;
    std::uint32_t acknowledged_input = 0;
    Transform transform;
    PlayerAppearance appearance;
    DoorTransitionPhase transition_phase = DoorTransitionPhase::None;
    std::uint32_t transition_door = 0;
    Tick transition_expires_tick = 0;
};

struct PacketHeader {
    std::uint16_t protocol_version = kProtocolVersion;
    MessageType message_type = MessageType::Ping;
    Channel channel = Channel::Control;
    std::uint8_t flags = 0;
    SessionId session = 0;
    std::uint32_t sequence = 0;
    std::uint32_t acknowledged_sequence = 0;
    std::uint32_t acknowledged_bits = 0;
    std::uint16_t payload_size = 0;
};

struct ClientHello {
    std::uint16_t minimum_version = kProtocolVersion;
    std::uint16_t maximum_version = kProtocolVersion;
    std::uint64_t build_id = 0;
    std::uint64_t feature_flags = 0;
    TownId town = 0;
    AccountId account = 0;
    std::uint64_t client_nonce = 0;
    std::array<std::uint8_t, kReconnectTokenBytes> reconnect_token{};
    std::uint8_t reconnect_token_size = 0;
    std::array<std::uint8_t, 32> invite_proof{};
};

struct ServerHello {
    ResultCode result = ResultCode::Ok;
    std::uint16_t negotiated_version = kProtocolVersion;
    SessionId session = 0;
    EntityId player_entity = 0;
    Tick server_tick = 0;
    std::uint64_t server_nonce = 0;
    std::uint32_t town_seed = 1;
    std::uint16_t town_land_id = 0x3001;
    std::uint8_t resident_slot = 0xFF;
    bool town_initialized = false;
    std::array<std::uint8_t, 8> town_name{};
    std::array<std::uint8_t, kReconnectTokenBytes> reconnect_token{};
    std::uint8_t reconnect_token_size = 0;
    std::array<std::uint8_t, 32> server_proof{};
};

struct InputCommand {
    std::uint32_t sequence = 0;
    Tick estimated_server_tick = 0;
    std::int16_t stick_x = 0;
    std::int16_t stick_y = 0;
    std::uint16_t buttons = 0;
    std::uint16_t action = 0;
    /* Online towns deliberately trust the originating game client for player
     * movement.  The original game's camera-relative movement, terrain and
     * collision are substantially richer than the dedicated server's world
     * representation, so re-simulating raw stick axes on the server produces
     * visible pulling and mirrored motion. */
    Transform client_transform;
};

struct TransformSnapshot {
    Tick server_tick = 0;
    Revision baseline_revision = 0;
    std::uint8_t occupied_house_mask = 0;
    std::vector<PlayerSnapshot> players;
};

Delivery delivery_for(Channel channel);
bool finite(const Vec3& value);

} // namespace acnet
