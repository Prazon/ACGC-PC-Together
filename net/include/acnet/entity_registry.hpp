#pragma once

#include "acnet/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace acnet {

struct EntityRecord {
    EntityId id = 0;
    std::uintptr_t local_key = 0;
    std::uint16_t profile = 0;
    ZoneId zone = 0;
    std::uint32_t generation = 0;
    std::uint32_t replication_flags = 0;
};

class EntityRegistry {
public:
    explicit EntityRegistry(std::uint32_t process_generation = 1);

    EntityId add(std::uintptr_t local_key,
                 std::uint16_t profile,
                 ZoneId zone,
                 std::uint32_t replication_flags);
    bool restore(const EntityRecord& record);
    bool remove_by_id(EntityId id);
    bool remove_by_key(std::uintptr_t local_key);
    bool change_zone(EntityId id, ZoneId zone);

    const EntityRecord* by_id(EntityId id) const;
    const EntityRecord* by_key(std::uintptr_t local_key) const;
    std::size_t size() const { return by_id_.size(); }
    std::uint32_t process_generation() const { return process_generation_; }

private:
    std::uint32_t process_generation_;
    std::uint32_t next_index_ = 1;
    std::unordered_map<EntityId, EntityRecord> by_id_;
    std::unordered_map<std::uintptr_t, EntityId> by_key_;
};

} // namespace acnet
