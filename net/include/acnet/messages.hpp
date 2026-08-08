#pragma once

#include "acnet/economy.hpp"
#include "acnet/encounter.hpp"
#include "acnet/housing.hpp"
#include "acnet/npc.hpp"
#include "acnet/replication.hpp"
#include "acnet/world.hpp"
#include "acnet/zone.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace acnet {

constexpr std::size_t kTownBootstrapTileCount = 5U * 6U * 16U * 16U;
/* Two 16x16 island acres; see kIslandTileCount in zone.hpp. */
constexpr std::size_t kIslandBootstrapTileCount = 2U * 16U * 16U;
/* Field grid width; an island acre index must fall inside it. */
constexpr std::uint8_t kFieldBlockXCount = 7;

struct TownBootstrapTile {
    std::uint16_t item = 0;
    bool buried = false;
};

struct TownBootstrap {
    std::uint32_t town_seed = 0;
    std::uint16_t land_id = 0;
    /* The fruit this town grows (Save_Get(fruit)), decided during town
     * generation. The server needs it to price fruit, which is worth a
     * quarter at home of what it fetches anywhere else. Zero means the client
     * could not report one, and every fruit then prices as foreign. */
    std::uint16_t native_fruit = 0;
    std::array<std::uint8_t, 8> town_name{};
    PlayerAppearance appearance;
    CustomPattern pattern;
    std::vector<TownBootstrapTile> tiles;
    /* Acre x indices of the island's two blocks within the 7-wide field grid,
     * which the client discovers from the acre kinds rather than a constant.
     * The island tile list is empty when the client could not read the field
     * layout yet; the server then keeps waiting rather than installing a wrong
     * island, and a later login supplies it. */
    std::array<std::uint8_t, 2> island_block_x{};
    std::vector<TownBootstrapTile> island_tiles;
};

struct TownBootstrapResult {
    ResultCode code = ResultCode::Ok;
    Revision revision = 0;
    bool initialized = false;
};

struct AppearanceUpdate {
    PlayerAppearance appearance;
    CustomPattern pattern;
};

struct AppearanceResult {
    ResultCode code = ResultCode::Ok;
    Revision revision = 0;
};

enum class TradeAction : std::uint8_t { Create, UpdateOffer, Confirm, Cancel };
struct TradeRequest {
    TradeAction action = TradeAction::Create;
    std::uint64_t trade_id = 0;
    AccountId account = 0;
    AccountId other_account = 0;
    Revision expected_trade_revision = 0;
    std::vector<std::uint8_t> slots;
};

enum class ConversationAction : std::uint8_t { Begin, Advance, End };
struct ConversationRequest {
    ConversationAction action = ConversationAction::Begin;
    AccountId account = 0;
    EntityId npc = 0;
    std::uint32_t lease_id = 0;
    std::uint16_t choice = 0;
};

struct ZoneTransferRequest {
    AccountId account = 0;
    std::uint32_t door_id = 0;
};

struct ZoneReadyRequest {
    AccountId account = 0;
    TransferToken token;
    Transform destination_transform;
};

bool encode(const WorldOperation& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, WorldOperation& value);
bool encode(const WorldResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, WorldResult& value);
bool encode(const TownBootstrap& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, TownBootstrap& value);
bool encode(const TownBootstrapResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, TownBootstrapResult& value);
bool encode(const AppearanceUpdate& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, AppearanceUpdate& value);
bool encode(const AppearanceResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, AppearanceResult& value);
bool encode(const EconomyRequest& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, EconomyRequest& value);
bool encode(const EconomyResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, EconomyResult& value);
bool encode(const TradeRequest& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, TradeRequest& value);
bool encode(const TradeResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, TradeResult& value);
bool encode(const ConversationRequest& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, ConversationRequest& value);
bool encode(const ConversationResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, ConversationResult& value);
bool encode(const ZoneTransferRequest& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, ZoneTransferRequest& value);
bool encode(const TransferOffer& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, TransferOffer& value);
bool encode(const ZoneReadyRequest& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, ZoneReadyRequest& value);
bool encode(const FurnitureOperation& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, FurnitureOperation& value);
bool encode(const FurnitureResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, FurnitureResult& value);
bool encode(const HouseUpdate& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, HouseUpdate& value);
bool encode(const HouseUpdateResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, HouseUpdateResult& value);
bool encode(const GyroidOperation& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, GyroidOperation& value);
bool encode(const GyroidResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, GyroidResult& value);
bool encode(const TownTuneUpdate& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, TownTuneUpdate& value);
bool encode(const TownTuneResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, TownTuneResult& value);
bool encode(const EncounterRequest& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, EncounterRequest& value);
bool encode(const EncounterResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, EncounterResult& value);
bool encode_deltas(const std::vector<ReplicationDelta>& value, std::vector<std::uint8_t>& output);
bool decode_deltas(const std::vector<std::uint8_t>& input, std::vector<ReplicationDelta>& value);

} // namespace acnet
