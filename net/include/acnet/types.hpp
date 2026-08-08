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
constexpr std::uint16_t kProtocolVersion = 20;
constexpr std::size_t kMaxPacketBytes = 1200;
constexpr std::size_t kMaxPayloadBytes = 1152;
constexpr std::size_t kEncryptionTagBytes = 16;
constexpr std::size_t kMaxPlaintextPayloadBytes = kMaxPayloadBytes - kEncryptionTagBytes;
constexpr std::size_t kMaxPlayersPerZone = 16;
constexpr std::size_t kReconnectTokenBytes = 32;
constexpr std::size_t kCustomPatternTextureBytes = 32U * 32U / 2U;

/* Bounds of the original player state machines, mirrored here so the portable
 * core can reject a presentation field without including a game header:
 * mPlayer_INDEX_NUM, mPlayer_ANIM_NUM, mPlayer_ITEM_MAIN_NUM and
 * mPlayer_PART_TABLE_NUM in include/m_player.h. These are indices into fixed
 * tables the receiving client dereferences, so an out-of-range value is not a
 * cosmetic lie -- it reads past an animation or part table. Validate on the
 * wire, never on arrival. Raise them only together with the game enum. */
constexpr std::uint16_t kPlayerActionCount = 121;
constexpr std::uint16_t kPlayerAnimationCount = 157;
constexpr std::uint8_t kPlayerItemStateCount = 24;
constexpr std::uint8_t kPlayerPartTableCount = 5;

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
    AppearanceUpdate = 8,
    AppearanceResult = 9,
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
    GyroidRequest = 34,
    GyroidResult = 35,
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

/* Who a player *is*. Deliberately no equipment: what is in the hand belongs to
 * the inventory the server commits (InventoryState::equipped), not to the
 * presentation the client authors, and mixing the two made every tool swap a
 * reliable appearance broadcast. */
struct PlayerAppearance {
    std::array<std::uint8_t, 8> name{};
    std::uint8_t gender = 0;
    std::uint8_t face = 0;
    std::uint16_t clothing = 0;
    std::uint16_t clothing_index = 0;
    Revision revision = 0;
};

/* What a player's skeleton is doing this instant, in the original game's own
 * terms: two animation indices combined through a part table, exactly as
 * Player_actor_InitAnimation_Base* drives keyframe0/keyframe1. It is authored
 * by the acting client -- only that client runs the state machine that decides
 * a swing has begun -- so every field is bounds-checked on arrival before any
 * viewer indexes a table with it. */
struct PlayerAnimation {
    std::uint8_t body = 0;       /* mPlayer_ANIM_*, keyframe0 */
    std::uint8_t overlay = 0;    /* mPlayer_ANIM_*, keyframe1 */
    std::uint8_t part_table = 0; /* mPlayer_PART_TABLE_* */
    std::uint8_t item_state = 0; /* mPlayer_ITEM_MAIN_*, which tool is in use */
    bool looping = true;         /* cKF_FRAMECONTROL_REPEAT, else _STOP */
    bool reversed = false;

    bool operator==(const PlayerAnimation& other) const {
        return body == other.body && overlay == other.overlay && part_table == other.part_table &&
               item_state == other.item_state && looping == other.looping && reversed == other.reversed;
    }
    bool operator!=(const PlayerAnimation& other) const { return !(*this == other); }
};

/* Presentation bits that change the *resources* a viewer draws a player with,
 * rather than the pose. Each one is something the original reads from the
 * acting client's own state and no viewer can derive.
 *
 * These are conceptually appearance, but they are carried on the presentation
 * delta rather than in AppearanceUpdate deliberately: appearance is rate-capped
 * at one per second and journalled, and a bee sting is transient enough that it
 * would either be delayed behind the bucket or burn it. Presentation is
 * change-triggered and not journalled, which is the right shape for a face that
 * swells and subsides. */
struct PlayerAppearanceBits {
    /* Common_Get(player_bee_swell_flag): a stung player's face resource. */
    bool bee_swell = false;
    /* Common_Get(player_decoy_flag): the Halloween decoy face. */
    bool decoy = false;
    /* PLAYER_ACTOR::change_color_flag, a fog override the original uses for the
     * golden-tool sparkle and the sting flash. */
    bool change_color = false;
    /* Now_Private->sunburn.rank, 0-8: selects a darker face palette. */
    std::uint8_t sunburn = 0;
    /* PLAYER_ACTOR::umbrella_state (aTOL_ACTION_*), which drives the umbrella's
     * open and close animation. Without it every remote umbrella is born open
     * and never animates. */
    std::uint8_t umbrella_state = 0;
    /* main_data.pickup.item / get_scoop.item -- what is in the hand *during* a
     * pickup or scoop, which is not the equipped tool and is not yet in the
     * pockets, so neither of the other two fields carries it. 0 when neither
     * animation is running. */
    std::uint16_t carried_item = 0;

    bool operator==(const PlayerAppearanceBits& other) const {
        return bee_swell == other.bee_swell && decoy == other.decoy && change_color == other.change_color &&
               sunburn == other.sunburn && umbrella_state == other.umbrella_state &&
               carried_item == other.carried_item;
    }
};

constexpr std::uint8_t kMaximumSunburnRank = 8;  // mPr_sunburn_c::rank is 0-8
constexpr std::uint8_t kUmbrellaStateCount = 6;  // aTOL_ACTION_NUM

/* Everything a viewer needs to draw another player beyond their transform and
 * appearance. Sent as a reliable Player delta on change rather than in the
 * 15 Hz snapshot: a full 16-player snapshot already sits near the unfragmented
 * MTU, and a dropped animation transition is a stuck pose, not a stale one. */
struct PlayerPresentation {
    PlayerAnimation animation;
    /* Server-owned, mirrored from InventoryState::equipped. */
    std::uint16_t equipped_item = 0;
    PlayerAppearanceBits appearance_bits;

    bool operator==(const PlayerPresentation& other) const {
        return animation == other.animation && equipped_item == other.equipped_item &&
               appearance_bits == other.appearance_bits;
    }
    bool operator!=(const PlayerPresentation& other) const { return !(*this == other); }
};

inline bool valid(const PlayerAppearanceBits& bits) {
    return bits.sunburn <= kMaximumSunburnRank && bits.umbrella_state < kUmbrellaStateCount;
}

inline bool valid(const PlayerAnimation& animation) {
    return animation.body < kPlayerAnimationCount && animation.overlay < kPlayerAnimationCount &&
           animation.part_table < kPlayerPartTableCount && animation.item_state < kPlayerItemStateCount;
}

/* A custom shirt is process-independent presentation data. It is kept out of
 * the high-rate transform snapshot and sent only in reliable appearance and
 * baseline messages. */
struct CustomPattern {
    bool present = false;
    std::uint8_t palette = 0;
    std::array<std::uint8_t, kCustomPatternTextureBytes> texture{};
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
    /* Baseline-only, like appearance: the unreliable snapshot carries neither.
     * Player deltas keep it live between baselines. */
    PlayerPresentation presentation;
    CustomPattern pattern;
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
    /* Same reasoning as the transform: only the originating client runs the
     * state machine that knows a swing has started, so it reports what its
     * skeleton is doing. The server bounds-checks it and forwards it -- it is
     * presentation, and nothing gameplay-authoritative reads it. */
    PlayerAnimation animation;
    /* Same reasoning again: which face resource, umbrella state and mid-pickup
     * item to draw this player with are all read from the acting client's own
     * state and are pure presentation. */
    PlayerAppearanceBits appearance_bits;
};

struct TransformSnapshot {
    Tick server_tick = 0;
    Revision baseline_revision = 0;
    std::uint8_t house_light_mask = 0;
    std::vector<PlayerSnapshot> players;
};

Delivery delivery_for(Channel channel);
bool finite(const Vec3& value);

} // namespace acnet
