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

struct TownBootstrapTile {
    std::uint16_t item = 0;
    bool buried = false;
};

struct TownBootstrap {
    std::uint32_t town_seed = 0;
    std::uint16_t land_id = 0;
    std::array<std::uint8_t, 8> town_name{};
    PlayerAppearance appearance;
    std::vector<TownBootstrapTile> tiles;
};

struct TownBootstrapResult {
    ResultCode code = ResultCode::Ok;
    Revision revision = 0;
    bool initialized = false;
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
};

bool encode(const WorldOperation& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, WorldOperation& value);
bool encode(const WorldResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, WorldResult& value);
bool encode(const TownBootstrap& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, TownBootstrap& value);
bool encode(const TownBootstrapResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, TownBootstrapResult& value);
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
bool encode(const EncounterRequest& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, EncounterRequest& value);
bool encode(const EncounterResult& value, std::vector<std::uint8_t>& output);
bool decode(const std::vector<std::uint8_t>& input, EncounterResult& value);
bool encode_deltas(const std::vector<ReplicationDelta>& value, std::vector<std::uint8_t>& output);
bool decode_deltas(const std::vector<std::uint8_t>& input, std::vector<ReplicationDelta>& value);

} // namespace acnet
