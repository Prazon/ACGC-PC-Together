#include "acnet/entity_registry.hpp"

#include <limits>
#include <stdexcept>

namespace acnet {

EntityRegistry::EntityRegistry(std::uint32_t process_generation)
    : process_generation_(process_generation == 0 ? 1 : process_generation) {}

EntityId EntityRegistry::add(std::uintptr_t local_key,
                             std::uint16_t profile,
                             ZoneId zone,
                             std::uint32_t replication_flags) {
    if (local_key == 0 || zone == 0 || by_key_.find(local_key) != by_key_.end()) return 0;
    if (next_index_ == 0 || next_index_ == std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("network entity ID space exhausted");
    }
    const EntityId id = (static_cast<EntityId>(process_generation_) << 32) | next_index_++;
    EntityRecord record{id, local_key, profile, zone, process_generation_, replication_flags};
    by_id_.emplace(id, record);
    by_key_.emplace(local_key, id);
    return id;
}

bool EntityRegistry::restore(const EntityRecord& record) {
    const std::uint32_t index = static_cast<std::uint32_t>(record.id);
    const std::uint32_t generation = static_cast<std::uint32_t>(record.id >> 32);
    if (record.id == 0 || record.local_key == 0 || record.zone == 0 || index == 0 ||
        generation != process_generation_ || record.generation != generation ||
        by_id_.find(record.id) != by_id_.end() || by_key_.find(record.local_key) != by_key_.end()) return false;
    by_id_[record.id] = record;
    by_key_[record.local_key] = record.id;
    if (index >= next_index_) next_index_ = index + 1;
    return next_index_ != 0;
}

bool EntityRegistry::remove_by_id(EntityId id) {
    const auto found = by_id_.find(id);
    if (found == by_id_.end()) return false;
    by_key_.erase(found->second.local_key);
    by_id_.erase(found);
    return true;
}

bool EntityRegistry::remove_by_key(std::uintptr_t local_key) {
    const auto found = by_key_.find(local_key);
    return found != by_key_.end() && remove_by_id(found->second);
}

bool EntityRegistry::change_zone(EntityId id, ZoneId zone) {
    if (zone == 0) return false;
    const auto found = by_id_.find(id);
    if (found == by_id_.end()) return false;
    found->second.zone = zone;
    return true;
}

const EntityRecord* EntityRegistry::by_id(EntityId id) const {
    const auto found = by_id_.find(id);
    return found == by_id_.end() ? nullptr : &found->second;
}

const EntityRecord* EntityRegistry::by_key(std::uintptr_t local_key) const {
    const auto found = by_key_.find(local_key);
    return found == by_key_.end() ? nullptr : by_id(found->second);
}

} // namespace acnet
