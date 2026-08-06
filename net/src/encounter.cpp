#include "acnet/encounter.hpp"
#include "acnet/crypto.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace acnet {

#include "encounter_tables.inc"

namespace {

Revision next_revision(Revision revision) {
    return revision == std::numeric_limits<Revision>::max() ? 1 : revision + 1;
}

/* Species the original game spawns outside the month tables. */
constexpr std::uint16_t kCoelacanthItem = kFishItemBase + 31;
constexpr std::uint16_t kBeeItem = kInsectItemBase + 8;
constexpr std::uint16_t kAntItem = kInsectItemBase + 38;

/* Fish time slots, include/ac_set_ovl_gyoei.h:
 *   0 = 9pm-3:59am, 1 = 4am-8:59am, 2 = 9am-3:59pm, 3 = 4pm-8:59pm */
int fish_time_slot(int hour) {
    if (hour >= 21 || hour <= 3) return 0;
    if (hour <= 8) return 1;
    if (hour <= 15) return 2;
    return 3;
}

/* Insect terms, include/ac_set_ovl_insect.h:
 *   0 = 11pm-3:59am, 1 = 4am-7:59am, 2 = 8am-3:59pm,
 *   3 = 4pm-4:59pm,  4 = 5pm-6:59pm, 5 = 7pm-10:59pm */
int insect_term(int hour) {
    if (hour >= 23 || hour <= 3) return 0;
    if (hour <= 7) return 1;
    if (hour <= 15) return 2;
    if (hour == 16) return 3;
    if (hour <= 18) return 4;
    return 5;
}

bool fish_available(std::size_t species, const TownDate& date) {
    if (species >= kFishSpeciesCount) return false;
    const std::uint8_t months = kFishAvailability[species][date.month - 1];
    const int slot = fish_time_slot(date.hour);
    /* The original game blends the outgoing half-month's fish in over a
     * randomised transition of up to five days, so a catch legal in either
     * half of the current month is accepted rather than rejected as forged. */
    return (months & (1U << slot)) != 0 || (months & (1U << (4 + slot))) != 0;
}

bool insect_available(std::size_t species, const TownDate& date) {
    if (species >= kInsectSpeciesCount) return false;
    return (kInsectAvailability[species][date.month - 1] & (1U << insect_term(date.hour))) != 0;
}

} // namespace

TownDate town_date_from_seconds(std::int64_t town_unix_seconds) {
    /* Days-to-civil from Howard Hinnant's chrono algorithms, kept local so the
     * portable core stays free of timezone-dependent C library calls. */
    std::int64_t seconds = town_unix_seconds;
    std::int64_t days = seconds / 86400;
    std::int64_t rem = seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    TownDate date;
    date.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    date.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    date.year = static_cast<int>(yoe + era * 400 + (date.month <= 2 ? 1 : 0));
    date.hour = static_cast<int>(rem / 3600);
    return date;
}

bool encounter_species_is_available(EncounterKind kind,
                                    std::uint16_t item,
                                    const TownDate& date,
                                    std::uint8_t weather) {
    if (date.month < 1 || date.month > 12 || date.hour < 0 || date.hour > 23) return false;
    if (kind == EncounterKind::Fish) {
        if (item < kFishItemBase || item >= kFishItemBase + kEncounterSpeciesPerKind) return false;
        if (item == kCoelacanthItem) return weather != 0;
        return fish_available(static_cast<std::size_t>(item - kFishItemBase), date);
    }
    if (item < kInsectItemBase || item >= kInsectItemBase + kEncounterSpeciesPerKind) return false;
    if (item == kBeeItem || item == kAntItem) return true;
    return insect_available(static_cast<std::size_t>(item - kInsectItemBase), date);
}

EncounterAuthority::EncounterAuthority(PlayerDirectory* players, WorldAuthority* world, std::uint64_t seed)
    : players_(players), world_(world), random_(seed) {
    secure_random_ = seed == 0;
}

std::size_t EncounterAuthority::KeyHash::operator()(const Key& key) const {
    std::size_t value = std::hash<AccountId>{}(key.account);
    value ^= std::hash<std::uint64_t>{}(key.idempotency.high) + 0x9E3779B9U + (value << 6) + (value >> 2);
    value ^= std::hash<std::uint64_t>{}(key.idempotency.low) + 0x9E3779B9U + (value << 6) + (value >> 2);
    return value;
}

EncounterResult EncounterAuthority::resolve(const EncounterRequest& request,
                                             Tick tick,
                                             std::int64_t town_unix_seconds,
                                             std::uint8_t weather) {
    EncounterResult result;
    result.idempotency = request.idempotency;
    const Key key{request.account, request.idempotency};
    const auto replay = idempotency_.find(key);
    if (replay != idempotency_.end()) {
        result = replay->second;
        result.replayed = true;
        return result;
    }
    const PlayerView* player = players_ == nullptr ? nullptr : players_->by_account(request.account);
    const InventoryState* current = world_ == nullptr ? nullptr : world_->inventory(request.account);
    if (request.account == 0 || !request.idempotency.valid() || player == nullptr || current == nullptr ||
        request.tool_slot >= kInventorySlots ||
        static_cast<std::uint8_t>(request.kind) > static_cast<std::uint8_t>(EncounterKind::Insect)) {
        result.code = ResultCode::Malformed;
    } else if (request.expected_inventory_revision != current->revision) {
        result.code = ResultCode::StaleRevision;
        result.inventory_revision = current->revision;
    } else if ((request.kind == EncounterKind::Fish &&
                current->slots[request.tool_slot].item != 0x2203 &&
                current->slots[request.tool_slot].item != 0x223C) ||
               (request.kind == EncounterKind::Insect &&
                current->slots[request.tool_slot].item != 0x2200 &&
                current->slots[request.tool_slot].item != 0x2239)) {
        result.code = ResultCode::InvalidState;
    } else if (cooldowns_[request.account] > tick) {
        result.code = ResultCode::RateLimited;
        result.next_allowed_tick = cooldowns_[request.account];
    } else {
        InventoryState inventory = *current;
        std::size_t empty = kInventorySlots;
        for (std::size_t i = 0; i < inventory.slots.size(); ++i) {
            if (inventory.slots[i].item == 0) {
                empty = i;
                break;
            }
        }
        const TownDate date = town_date_from_seconds(town_unix_seconds);
        if (empty == kInventorySlots) {
            result.code = ResultCode::Capacity;
        } else if (request.species != 0 &&
                   !encounter_species_is_available(request.kind, request.species, date, weather)) {
            /* The claimed species cannot appear at this date, hour, or weather,
             * so the request is a forgery or a stale client. Nothing is
             * committed and no cooldown is spent. */
            result.code = ResultCode::InvalidState;
        } else {
            std::uint64_t random_value = 0;
            if (secure_random_) {
                if (!secure_random(reinterpret_cast<std::uint8_t*>(&random_value), sizeof(random_value))) {
                    result.code = ResultCode::InternalError;
                    idempotency_[key] = result;
                    return result;
                }
            } else random_value = random_();
            const std::uint64_t roll = random_value % 100;
            const std::uint64_t threshold = weather == 0 ? 72 : 78;
            result.caught = roll < threshold;
            result.code = ResultCode::Ok;
            result.next_allowed_tick = tick + 90;
            cooldowns_[request.account] = result.next_allowed_tick;
            if (result.caught) {
                if (request.species != 0) {
                    result.item = request.species;
                } else {
                    /* No claim: choose uniformly from everything that can
                     * legally appear now, rather than from a fixed prefix of
                     * the item range. */
                    const std::uint16_t base =
                        request.kind == EncounterKind::Fish ? kFishItemBase : kInsectItemBase;
                    std::array<std::uint16_t, kEncounterSpeciesPerKind> legal{};
                    std::size_t legal_count = 0;
                    for (std::uint16_t offset = 0; offset < kEncounterSpeciesPerKind; ++offset) {
                        const std::uint16_t candidate = static_cast<std::uint16_t>(base + offset);
                        if (encounter_species_is_available(request.kind, candidate, date, weather))
                            legal[legal_count++] = candidate;
                    }
                    if (legal_count == 0) {
                        result.caught = false;
                        result.inventory_revision = inventory.revision;
                        idempotency_[key] = result;
                        return result;
                    }
                    if (secure_random_) {
                        if (!secure_random(reinterpret_cast<std::uint8_t*>(&random_value), sizeof(random_value))) {
                            result.code = ResultCode::InternalError;
                            result.caught = false;
                            idempotency_[key] = result;
                            return result;
                        }
                    } else random_value = random_();
                    result.item = legal[static_cast<std::size_t>(random_value % legal_count)];
                }
                result.inventory_slot = static_cast<std::uint8_t>(empty);
                inventory.slots[empty].item = result.item;
                inventory.revision = next_revision(inventory.revision);
                if (!world_->set_inventory(request.account, inventory)) {
                    result.code = ResultCode::InternalError;
                    result.caught = false;
                    result.item = 0;
                }
            }
            result.inventory_revision = inventory.revision;
        }
    }
    idempotency_[key] = result;
    return result;
}

} // namespace acnet
