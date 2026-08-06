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

/* Item identifier bases from include/m_name_table.h. A caught species is
 * always base + the original game's species index. */
constexpr std::uint16_t kFishItemBase = 0x2300;
constexpr std::uint16_t kInsectItemBase = 0x2D00;
constexpr std::uint16_t kEncounterSpeciesPerKind = 40;

struct EncounterRequest {
    AccountId account = 0;
    IdempotencyKey idempotency;
    EncounterKind kind = EncounterKind::Fish;
    Revision expected_inventory_revision = 0;
    std::uint8_t tool_slot = 0;
    /* The item the client observed itself hooking or swinging at, so the
     * encyclopedia entry and the pocket agree. Spawns are still simulated on
     * the client, so this is a claim, not an outcome: the server accepts it
     * only if that species can legally appear right now, and it alone decides
     * whether the catch succeeds and commits the inventory. Zero means "no
     * claim" and lets the server choose from the legal set. */
    std::uint16_t species = 0;
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

/* Civil date and hour of the authoritative town clock, derived from the town's
 * local seconds. Exposed so tests and the availability query agree. */
struct TownDate {
    int year = 1970;
    int month = 1; // 1-12
    int day = 1;   // 1-31
    int hour = 0;  // 0-23
};

TownDate town_date_from_seconds(std::int64_t town_unix_seconds);

/* Whether `item` can legally be caught at this date under this weather.
 *
 * Availability comes from net/src/encounter_tables.inc, generated from the
 * original spawn overlays by tools/gen_encounter_tables.py. Three species are
 * never in those month tables because the original game spawns them through
 * dedicated paths, and they are allowed here on the same terms: the coelacanth
 * only while it is raining or snowing, and the bee and ant unconditionally
 * because they come from shaking a tree and from candy or trash. */
bool encounter_species_is_available(EncounterKind kind,
                                    std::uint16_t item,
                                    const TownDate& date,
                                    std::uint8_t weather);

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
