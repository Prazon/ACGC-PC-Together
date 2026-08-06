#include "acnet/world.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace acnet {
namespace {

std::size_t mix(std::size_t seed, std::uint64_t value) {
    seed ^= static_cast<std::size_t>(value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2));
    return seed;
}

} // namespace

std::size_t TileAddressHash::operator()(const TileAddress& value) const {
    std::size_t result = 0;
    result = mix(result, value.zone);
    result = mix(result, static_cast<std::uint16_t>(value.x));
    return mix(result, static_cast<std::uint16_t>(value.z));
}

std::size_t WorldAuthority::OperationKeyHash::operator()(const OperationKey& value) const {
    std::size_t result = 0;
    result = mix(result, value.account);
    result = mix(result, value.key.high);
    return mix(result, value.key.low);
}

WorldAuthority::WorldAuthority(PlayerDirectory* players, WorldConfig config)
    : players_(players), config_(config) {
    if (config_.tile_size <= 0.0F) config_.tile_size = 40.0F;
    if (config_.maximum_interaction_distance <= 0.0F) config_.maximum_interaction_distance = 90.0F;
    if (config_.maximum_idempotency_records == 0) config_.maximum_idempotency_records = 1;
}

namespace {

bool portable_item(std::uint16_t item) {
    if (item == 0 || item <= 0x0082 || (item >= 0x0800 && item <= 0x0920) ||
        (item >= 0x5800 && item <= 0x5852) || (item >= 0xF0F3 && item <= 0xF0FA) ||
        item == 0xF0FF || item == 0xF101 || item == 0xF120 || item == 0xFFFF) return false;
    return true;
}

bool shovel_item(std::uint16_t item) {
    return item == 0x2202 || item == 0x223B;
}

bool axe_item(std::uint16_t item) {
    return item == 0x2201 || item == 0x223A || (item >= 0x223D && item <= 0x2243);
}

bool furniture_item(std::uint16_t item) {
    const std::uint16_t type = item & 0xF000U;
    return type == 0x1000U || type == 0x3000U;
}

std::uint16_t planted_world_item(std::uint16_t inventory_item) {
    switch (inventory_item) {
        case 0x2800: return 0x0805; // apple
        case 0x2801: return 0x0825; // cherry
        case 0x2802: return 0x081D; // pear
        case 0x2803: return 0x0815; // peach
        case 0x2804: return 0x080D; // orange
        case 0x2807: return 0x0854; // coconut
        case 0x2900: return 0x0800; // sapling
        case 0x2901: return 0x085D; // cedar sapling
        default:
            if (inventory_item >= 0x2902 && inventory_item <= 0x290A)
                return static_cast<std::uint16_t>(0x083C + inventory_item - 0x2902);
            return 0;
    }
}

} // namespace

bool WorldAuthority::register_inventory(AccountId account, const InventoryState& inventory_value) {
    if (account == 0 || inventory_value.revision == 0) return false;
    return inventories_.emplace(account, inventory_value).second;
}

bool WorldAuthority::set_inventory(AccountId account, const InventoryState& inventory_value) {
    if (account == 0 || inventory_value.revision == 0) return false;
    inventories_[account] = inventory_value;
    return true;
}

bool WorldAuthority::set_tile(const TileAddress& address, const TileState& tile_value) {
    if (address.zone == 0 || tile_value.revision == 0) return false;
    tiles_[address] = tile_value;
    return true;
}

const InventoryState* WorldAuthority::inventory(AccountId account) const {
    const auto found = inventories_.find(account);
    return found == inventories_.end() ? nullptr : &found->second;
}

const TileState* WorldAuthority::tile(const TileAddress& address) const {
    const auto found = tiles_.find(address);
    return found == tiles_.end() ? nullptr : &found->second;
}

std::vector<std::pair<TileAddress, TileState>> WorldAuthority::tiles_in_zone(ZoneId zone) const {
    std::vector<std::pair<TileAddress, TileState>> result;
    for (const auto& item : tiles_) {
        if (item.first.zone == zone) result.push_back(item);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.first.z != b.first.z) return a.first.z < b.first.z;
        return a.first.x < b.first.x;
    });
    return result;
}

Revision WorldAuthority::next_revision(Revision current) {
    return current == std::numeric_limits<Revision>::max() ? 1 : current + 1;
}

std::optional<std::uint8_t> WorldAuthority::first_empty_slot(const InventoryState& inventory_value) {
    for (std::size_t i = 0; i < inventory_value.slots.size(); ++i) {
        if (inventory_value.slots[i].item == 0) return static_cast<std::uint8_t>(i);
    }
    return std::nullopt;
}

bool WorldAuthority::in_range(const WorldOperation& operation) const {
    if (players_ == nullptr) return false;
    const PlayerView* player = players_->by_account(operation.account);
    if (player == nullptr || player->zone != operation.tile.zone) return false;
    const float center_x = (static_cast<float>(operation.tile.x) + 0.5F) * config_.tile_size;
    const float center_z = (static_cast<float>(operation.tile.z) + 0.5F) * config_.tile_size;
    const float dx = player->transform.position.x - center_x;
    const float dz = player->transform.position.z - center_z;
    const float maximum = config_.maximum_interaction_distance;
    return dx * dx + dz * dz <= maximum * maximum;
}

WorldResult WorldAuthority::reject(const WorldOperation& operation,
                                   ResultCode code,
                                   const TileState* tile_value,
                                   const InventoryState* inventory_value) const {
    WorldResult result;
    result.code = code;
    result.idempotency = operation.idempotency;
    result.tile = operation.tile;
    if (tile_value != nullptr) result.tile_revision = tile_value->revision;
    if (inventory_value != nullptr) result.inventory_revision = inventory_value->revision;
    result.inventory_slot = operation.inventory_slot;
    return result;
}

void WorldAuthority::remember(const OperationKey& key, const WorldResult& result) {
    idempotency_[key] = result;
    idempotency_order_.push_back(key);
    while (idempotency_order_.size() > config_.maximum_idempotency_records) {
        idempotency_.erase(idempotency_order_.front());
        idempotency_order_.erase(idempotency_order_.begin());
    }
}

WorldResult WorldAuthority::apply(const WorldOperation& operation, Tick tick) {
    const OperationKey operation_key{operation.account, operation.idempotency};
    if (!operation.idempotency.valid()) return reject(operation, ResultCode::Malformed, nullptr, nullptr);
    const auto prior = idempotency_.find(operation_key);
    if (prior != idempotency_.end()) {
        WorldResult replay = prior->second;
        replay.replayed = true;
        return replay;
    }
    const auto cooldown = next_operation_tick_.find(operation.account);
    if (tick != 0 && cooldown != next_operation_tick_.end() && tick < cooldown->second) {
        const WorldResult result = reject(operation, ResultCode::RateLimited, nullptr, inventory(operation.account));
        remember(operation_key, result);
        return result;
    }

    auto inventory_it = inventories_.find(operation.account);
    auto tile_it = tiles_.find(operation.tile);
    if (inventory_it == inventories_.end() || tile_it == tiles_.end()) {
        const WorldResult result = reject(operation,
                                          ResultCode::NotFound,
                                          tile_it == tiles_.end() ? nullptr : &tile_it->second,
                                          inventory_it == inventories_.end() ? nullptr : &inventory_it->second);
        remember(operation_key, result);
        return result;
    }
    InventoryState& inventory_value = inventory_it->second;
    TileState& tile_value = tile_it->second;
    if (!in_range(operation)) {
        const WorldResult result = reject(operation, ResultCode::OutOfRange, &tile_value, &inventory_value);
        remember(operation_key, result);
        return result;
    }
    if (operation.expected_tile_revision != tile_value.revision ||
        operation.expected_inventory_revision != inventory_value.revision) {
        const WorldResult result = reject(operation, ResultCode::StaleRevision, &tile_value, &inventory_value);
        remember(operation_key, result);
        return result;
    }

    TileState candidate_tile = tile_value;
    InventoryState candidate_inventory = inventory_value;
    WorldResult result;
    result.code = ResultCode::Ok;
    result.idempotency = operation.idempotency;
    result.tile = operation.tile;
    result.inventory_slot = operation.inventory_slot;
    bool inventory_changed = false;
    /* What is actually in the hand. The client used to name the slot it wanted
     * checked, which made "own a shovel" and "be holding a shovel" the same
     * thing; holding is now a committed transaction, so the server can ask the
     * question the game asks. */
    const auto tool_item = [&]() -> std::uint16_t { return candidate_inventory.equipped.item; };

    const auto take_inventory_item = [&]() -> bool {
        if (operation.inventory_slot >= candidate_inventory.slots.size()) return false;
        ItemSlot& slot = candidate_inventory.slots[operation.inventory_slot];
        if (slot.item == 0 || (operation.expected_item != 0 && slot.item != operation.expected_item)) return false;
        result.transferred_item = slot.item;
        candidate_tile.item = slot.item;
        candidate_tile.condition = slot.condition;
        slot = {};
        inventory_changed = true;
        return true;
    };
    const auto give_tile_item = [&]() -> bool {
        if (candidate_tile.item == 0 ||
            (operation.expected_item != 0 && candidate_tile.item != operation.expected_item)) return false;
        const auto slot = first_empty_slot(candidate_inventory);
        if (!slot.has_value()) return false;
        result.inventory_slot = *slot;
        result.transferred_item = candidate_tile.item;
        candidate_inventory.slots[*slot] = {candidate_tile.item, candidate_tile.condition};
        candidate_tile.item = 0;
        candidate_tile.condition = 0;
        inventory_changed = true;
        return true;
    };

    bool valid = false;
    switch (operation.type) {
        case WorldOpType::DropItem:
            valid = candidate_tile.item == 0 && !candidate_tile.buried && !candidate_tile.placed_furniture &&
                    operation.inventory_slot < candidate_inventory.slots.size() &&
                    portable_item(candidate_inventory.slots[operation.inventory_slot].item) && take_inventory_item();
            break;
        case WorldOpType::PickupItem:
            valid = !candidate_tile.buried && !candidate_tile.placed_furniture &&
                    portable_item(candidate_tile.item) && give_tile_item();
            break;
        case WorldOpType::Dig:
            valid = shovel_item(tool_item()) &&
                    candidate_tile.item == 0 && candidate_tile.terrain == TerrainState::Normal &&
                    !candidate_tile.placed_furniture;
            if (valid) {
                candidate_tile.terrain = TerrainState::Hole;
                candidate_tile.item = 0x0011;
            }
            break;
        case WorldOpType::Bury:
            valid = shovel_item(tool_item()) && candidate_tile.terrain == TerrainState::Hole &&
                    (candidate_tile.item == 0 || (candidate_tile.item >= 0x0011 && candidate_tile.item <= 0x0029)) &&
                    take_inventory_item();
            if (valid) candidate_tile.buried = true;
            break;
        case WorldOpType::Plant:
            if (operation.inventory_slot < candidate_inventory.slots.size()) {
                const std::uint16_t source_item = candidate_inventory.slots[operation.inventory_slot].item;
                const std::uint16_t planted = planted_world_item(source_item);
                const bool fruit = (source_item >= 0x2800 && source_item <= 0x2804) || source_item == 0x2807;
                valid = planted != 0 && candidate_tile.item == 0 &&
                        ((fruit && shovel_item(tool_item()) && candidate_tile.terrain == TerrainState::Hole) ||
                         (!fruit && candidate_tile.terrain == TerrainState::Normal)) && take_inventory_item();
                if (valid) {
                    candidate_tile.item = planted;
                    candidate_tile.condition = 0;
                    candidate_tile.terrain = TerrainState::Planted;
                    candidate_tile.buried = false;
                }
            }
            break;
        case WorldOpType::ChopTree:
            valid = axe_item(tool_item()) &&
                    candidate_tile.terrain == TerrainState::Tree;
            if (valid) {
                candidate_tile.terrain = TerrainState::Stump;
                candidate_tile.item = 0x0001;
            }
            break;
        case WorldOpType::PlaceFurniture:
            valid = candidate_tile.item == 0 && candidate_tile.terrain == TerrainState::Normal &&
                    operation.inventory_slot < candidate_inventory.slots.size() &&
                    furniture_item(candidate_inventory.slots[operation.inventory_slot].item) && take_inventory_item();
            if (valid) candidate_tile.placed_furniture = true;
            break;
        case WorldOpType::RemoveFurniture:
            valid = candidate_tile.placed_furniture && give_tile_item();
            if (valid) candidate_tile.placed_furniture = false;
            break;
        case WorldOpType::FillHole:
            valid = shovel_item(tool_item()) && candidate_tile.terrain == TerrainState::Hole &&
                    !candidate_tile.buried &&
                    (candidate_tile.item == 0 || (candidate_tile.item >= 0x0011 && candidate_tile.item <= 0x0029));
            if (valid) {
                candidate_tile.item = 0;
                candidate_tile.condition = 0;
                candidate_tile.terrain = TerrainState::Normal;
            }
            break;
    }
    if (!valid) {
        const WorldResult rejected = reject(operation, ResultCode::InvalidState, &tile_value, &inventory_value);
        remember(operation_key, rejected);
        return rejected;
    }

    candidate_tile.revision = next_revision(candidate_tile.revision);
    if (inventory_changed) candidate_inventory.revision = next_revision(candidate_inventory.revision);
    result.tile_revision = candidate_tile.revision;
    result.inventory_revision = candidate_inventory.revision;
    if (commit_hook_ && !commit_hook_(operation, result, candidate_tile, candidate_inventory)) {
        return reject(operation, ResultCode::InternalError, &tile_value, &inventory_value);
    }
    tile_value = candidate_tile;
    inventory_value = candidate_inventory;
    if (tick != 0) {
        const bool terrain_action = operation.type == WorldOpType::Dig || operation.type == WorldOpType::Bury ||
                                    operation.type == WorldOpType::Plant || operation.type == WorldOpType::ChopTree ||
                                    operation.type == WorldOpType::FillHole;
        next_operation_tick_[operation.account] = tick + (terrain_action ? config_.terrain_action_cooldown_ticks
                                                                         : config_.operation_cooldown_ticks);
    }
    remember(operation_key, result);
    return result;
}

std::uint64_t WorldAuthority::total_item_units() const {
    std::uint64_t result = 0;
    for (const auto& item : inventories_) {
        for (const ItemSlot& slot : item.second.slots) {
            if (slot.item != 0) ++result;
        }
        /* The hand holds a real item. Leaving it out would read as a leak the
         * moment anyone equipped a tool. */
        if (item.second.equipped.item != 0) ++result;
    }
    for (const auto& tile_item : tiles_) {
        if (tile_item.second.item != 0) ++result;
    }
    return result;
}

} // namespace acnet
