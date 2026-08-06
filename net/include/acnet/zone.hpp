#pragma once

#include "acnet/player_query.hpp"
#include "acnet/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace acnet {

enum class ZoneKind : std::uint8_t {
    Exterior,
    PublicInterior,
    ResidentHouse,
    NpcHouse,
    Island,
};

enum class ZoneRuntimeState : std::uint8_t {
    Active,
    Sleeping,
};

struct ZoneState {
    ZoneId id = 0;
    ZoneKind kind = ZoneKind::Exterior;
    std::size_t capacity = 8;
    ZoneRuntimeState runtime = ZoneRuntimeState::Sleeping;
    Revision baseline_revision = 1;
    Tick last_active_tick = 0;
    std::unordered_set<AccountId> occupants;
};

struct DoorDefinition {
    std::uint32_t id = 0;
    ZoneId source_zone = 0;
    ZoneId destination_zone = 0;
    Vec3 source_position;
    Vec3 destination_position;
    float interaction_radius = 80.0F;
};

struct TransferToken {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    bool valid() const { return high != 0 || low != 0; }
    bool operator==(const TransferToken& other) const { return high == other.high && low == other.low; }
};

struct TransferOffer {
    ResultCode code = ResultCode::InternalError;
    AccountId account = 0;
    ZoneId source_zone = 0;
    ZoneId destination_zone = 0;
    Vec3 destination_position;
    TransferToken token;
    Tick expires_tick = 0;
    Revision baseline_revision = 0;
};

struct TransferReservation {
    TransferOffer offer;
    std::uint32_t door_id = 0;
};

struct TransferTokenHash {
    std::size_t operator()(const TransferToken& token) const;
};

struct ZoneConfig {
    Tick transfer_timeout_ticks = 600;
    Tick sleep_after_ticks = 300;
};

class ZoneCoordinator {
public:
    explicit ZoneCoordinator(PlayerDirectory* players, ZoneConfig config = {}, std::uint64_t random_seed = 0);

    bool add_zone(const ZoneState& zone);
    bool add_door(const DoorDefinition& door);
    bool set_door(const DoorDefinition& door);
    bool join(AccountId account, ZoneId zone, const Vec3& position, Tick tick);
    bool leave(AccountId account, Tick tick);

    TransferOffer request_transfer(AccountId account, std::uint32_t door_id, Tick tick);
    ResultCode acknowledge_ready(AccountId account, const TransferToken& token, Tick tick);
    bool cancel_transfer(AccountId account);
    std::size_t expire(Tick tick);
    void update_sleep_states(Tick tick);

    ZoneState* zone(ZoneId id);
    const ZoneState* zone(ZoneId id) const;
    const TransferReservation* reservation(AccountId account) const;
    std::size_t reserved_for(ZoneId zone) const;

private:
    TransferToken random_token();

    PlayerDirectory* players_;
    ZoneConfig config_;
    std::mt19937_64 random_;
    bool secure_random_ = true;
    std::unordered_map<ZoneId, ZoneState> zones_;
    std::unordered_map<std::uint32_t, DoorDefinition> doors_;
    std::unordered_map<AccountId, TransferReservation> reservations_;
    std::unordered_set<TransferToken, TransferTokenHash> issued_tokens_;
};

} // namespace acnet
