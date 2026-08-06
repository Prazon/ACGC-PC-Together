#include "acnet/encounter.hpp"
#include "acnet/crypto.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace acnet {
namespace {

Revision next_revision(Revision revision) {
    return revision == std::numeric_limits<Revision>::max() ? 1 : revision + 1;
}

} // namespace

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
        if (empty == kInventorySlots) {
            result.code = ResultCode::Capacity;
        } else {
            const std::uint32_t hour = static_cast<std::uint32_t>((town_unix_seconds / 3600) % 24);
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
                static constexpr std::array<std::uint16_t, 6> fish{{0x2300, 0x2301, 0x2302, 0x2303, 0x2304, 0x2305}};
                static constexpr std::array<std::uint16_t, 6> insects{{0x2D00, 0x2D01, 0x2D02, 0x2D03, 0x2D04, 0x2D05}};
                const auto& table = request.kind == EncounterKind::Fish ? fish : insects;
                if (secure_random_) {
                    if (!secure_random(reinterpret_cast<std::uint8_t*>(&random_value), sizeof(random_value))) {
                        result.code = ResultCode::InternalError;
                        result.caught = false;
                        idempotency_[key] = result;
                        return result;
                    }
                } else random_value = random_();
                const std::size_t index = static_cast<std::size_t>((random_value + hour + player->zone) % table.size());
                result.item = table[index];
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
