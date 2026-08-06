#pragma once

#include "acnet/player_query.hpp"
#include "acnet/world.hpp"

#include <cstdint>
#include <random>
#include <unordered_map>

namespace acnet {

enum class EncounterKind : std::uint8_t {
    Fish = 0,
    Insect = 1,
};

struct EncounterRequest {
    AccountId account = 0;
    IdempotencyKey idempotency;
    EncounterKind kind = EncounterKind::Fish;
    Revision expected_inventory_revision = 0;
    std::uint8_t tool_slot = 0;
};

struct EncounterResult {
    ResultCode code = ResultCode::InternalError;
    IdempotencyKey idempotency;
    Revision inventory_revision = 0;
    std::uint16_t item = 0;
    std::uint8_t inventory_slot = 0;
    Tick next_allowed_tick = 0;
    bool caught = false;
    bool replayed = false;
};

class EncounterAuthority {
public:
    EncounterAuthority(PlayerDirectory* players, WorldAuthority* world, std::uint64_t seed = 0);
    EncounterResult resolve(const EncounterRequest& request,
                            Tick tick,
                            std::int64_t town_unix_seconds,
                            std::uint8_t weather);

private:
    struct Key {
        AccountId account = 0;
        IdempotencyKey idempotency;
        bool operator==(const Key& other) const {
            return account == other.account && idempotency == other.idempotency;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const;
    };

    PlayerDirectory* players_;
    WorldAuthority* world_;
    std::mt19937_64 random_;
    bool secure_random_ = true;
    std::unordered_map<AccountId, Tick> cooldowns_;
    std::unordered_map<Key, EncounterResult, KeyHash> idempotency_;
};

} // namespace acnet
