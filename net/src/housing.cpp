#include "acnet/housing.hpp"

#include <limits>

namespace acnet {

std::size_t FurnitureAddressHash::operator()(const FurnitureAddress& address) const {
    return (static_cast<std::size_t>(address.x) << 16) |
           (static_cast<std::size_t>(address.z) << 8) | address.layer;
}

std::size_t HousingAuthority::OperationKeyHash::operator()(const OperationKey& value) const {
    std::size_t result = std::hash<std::uint64_t>{}(value.account);
    result ^= std::hash<std::uint64_t>{}(value.key.high) + 0x9E3779B9U + (result << 6) + (result >> 2);
    result ^= std::hash<std::uint64_t>{}(value.key.low) + 0x9E3779B9U + (result << 6) + (result >> 2);
    return result;
}

HousingAuthority::HousingAuthority(WorldAuthority* world, PlayerDirectory* players)
    : world_(world), players_(players) {}

Revision HousingAuthority::next_revision(Revision revision) {
    return revision == std::numeric_limits<Revision>::max() ? 1 : revision + 1;
}

std::uint64_t HousingAuthority::house_id_for_slot(std::uint8_t slot) {
    return 10000ULL + slot;
}

std::optional<std::uint8_t> HousingAuthority::empty_slot(const InventoryState& inventory) {
    for (std::size_t i = 0; i < inventory.slots.size(); ++i) {
        if (inventory.slots[i].item == 0) return static_cast<std::uint8_t>(i);
    }
    return std::nullopt;
}

bool HousingAuthority::register_resident(std::uint8_t original_slot, AccountId owner, ZoneId zone) {
    if (original_slot >= residents_.size() || owner == 0 || zone == 0 || residents_[original_slot] != 0 ||
        owner_houses_.find(owner) != owner_houses_.end()) return false;
    const std::uint64_t id = house_id_for_slot(original_slot);
    HouseState house_value;
    house_value.house_id = id;
    house_value.owner = owner;
    house_value.original_slot = original_slot;
    house_value.zone = zone;
    houses_[id] = house_value;
    residents_[original_slot] = owner;
    owner_houses_[owner] = id;
    return true;
}

const HouseState* HousingAuthority::house(std::uint64_t house_id) const {
    const auto found = houses_.find(house_id);
    return found == houses_.end() ? nullptr : &found->second;
}

const HouseState* HousingAuthority::house_for(AccountId owner) const {
    const auto found = owner_houses_.find(owner);
    return found == owner_houses_.end() ? nullptr : house(found->second);
}

FurnitureResult HousingAuthority::apply(const FurnitureOperation& operation) {
    FurnitureResult result;
    result.idempotency = operation.idempotency;
    result.house_id = operation.house_id;
    result.inventory_slot = operation.inventory_slot;
    if (!operation.idempotency.valid() || world_ == nullptr) {
        result.code = ResultCode::Malformed;
        return result;
    }
    const OperationKey key{operation.account, operation.idempotency};
    const auto prior = idempotency_.find(key);
    if (prior != idempotency_.end()) {
        result = prior->second;
        result.replayed = true;
        return result;
    }
    const auto house_it = houses_.find(operation.house_id);
    const InventoryState* current_inventory = world_->inventory(operation.account);
    if (house_it == houses_.end() || current_inventory == nullptr) {
        result.code = ResultCode::NotFound;
        idempotency_[key] = result;
        return result;
    }
    const HouseState& current_house = house_it->second;
    result.house_revision = current_house.revision;
    result.inventory_revision = current_inventory->revision;
    if (current_house.owner != operation.account) {
        result.code = ResultCode::Unauthorized;
        idempotency_[key] = result;
        return result;
    }
    if (players_ != nullptr) {
        const PlayerView* player = players_->by_account(operation.account);
        if (player == nullptr || !player->interaction_eligible || player->zone != current_house.zone) {
            result.code = ResultCode::OutOfRange;
            idempotency_[key] = result;
            return result;
        }
    }
    if (current_house.revision != operation.expected_house_revision ||
        current_inventory->revision != operation.expected_inventory_revision) {
        result.code = ResultCode::StaleRevision;
        idempotency_[key] = result;
        return result;
    }
    HouseState candidate_house = current_house;
    InventoryState candidate_inventory = *current_inventory;
    if (operation.type == FurnitureOpType::Place) {
        if (candidate_house.furniture.find(operation.address) != candidate_house.furniture.end() ||
            operation.inventory_slot >= candidate_inventory.slots.size()) {
            result.code = ResultCode::InvalidState;
            idempotency_[key] = result;
            return result;
        }
        ItemSlot& slot = candidate_inventory.slots[operation.inventory_slot];
        if (slot.item == 0 || (operation.expected_item != 0 && slot.item != operation.expected_item)) {
            result.code = ResultCode::InvalidState;
            idempotency_[key] = result;
            return result;
        }
        result.item = slot.item;
        candidate_house.furniture[operation.address] = slot;
        slot = {};
    } else {
        const auto furniture = candidate_house.furniture.find(operation.address);
        const auto slot = empty_slot(candidate_inventory);
        if (furniture == candidate_house.furniture.end() || !slot.has_value() ||
            (operation.expected_item != 0 && furniture->second.item != operation.expected_item)) {
            result.code = ResultCode::InvalidState;
            idempotency_[key] = result;
            return result;
        }
        result.item = furniture->second.item;
        result.inventory_slot = *slot;
        candidate_inventory.slots[*slot] = furniture->second;
        candidate_house.furniture.erase(furniture);
    }
    candidate_house.revision = next_revision(candidate_house.revision);
    candidate_inventory.revision = next_revision(candidate_inventory.revision);
    if (!world_->set_inventory(operation.account, candidate_inventory)) {
        result.code = ResultCode::InternalError;
        return result;
    }
    house_it->second = std::move(candidate_house);
    result.code = ResultCode::Ok;
    result.house_revision = house_it->second.revision;
    result.inventory_revision = candidate_inventory.revision;
    idempotency_[key] = result;
    return result;
}

std::size_t HousingAuthority::resident_count() const {
    std::size_t count = 0;
    for (AccountId account : residents_) {
        if (account != 0) ++count;
    }
    return count;
}

std::uint64_t HousingAuthority::total_furniture_units() const {
    std::uint64_t result = 0;
    for (const auto& item : houses_) result += item.second.furniture.size();
    return result;
}

bool HousingAuthority::restore_house(const HouseState& house) {
    if (house.house_id == 0 || house.owner == 0 || house.original_slot >= kOriginalResidentSlots ||
        house.zone == 0 || house.revision == 0 ||
        (residents_[house.original_slot] != 0 && residents_[house.original_slot] != house.owner)) return false;
    residents_[house.original_slot] = house.owner;
    houses_[house.house_id] = house;
    owner_houses_[house.owner] = house.house_id;
    return true;
}

} // namespace acnet
