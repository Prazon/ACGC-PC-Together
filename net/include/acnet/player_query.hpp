#pragma once

#include "acnet/types.hpp"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace acnet {

enum class PlayerKind : std::uint8_t {
    Resident,
    Visitor,
    ServerProxy,
    RemotePresentation,
};

struct PlayerView {
    AccountId account = 0;
    EntityId entity = 0;
    ZoneId zone = 0;
    PlayerKind kind = PlayerKind::Visitor;
    bool local = false;
    bool interaction_eligible = true;
    Transform transform;
    PlayerAppearance appearance;
};

class PlayerDirectory {
public:
    bool upsert(const PlayerView& player);
    bool remove(AccountId account);
    bool set_local(AccountId account);

    PlayerView* local();
    const PlayerView* local() const;
    PlayerView* by_account(AccountId account);
    const PlayerView* by_account(AccountId account) const;
    PlayerView* by_entity(EntityId entity);
    const PlayerView* by_entity(EntityId entity) const;

    const PlayerView* nearest(const Vec3& position, ZoneId zone, float radius) const;
    std::vector<const PlayerView*> query_radius(const Vec3& position,
                                                ZoneId zone,
                                                float radius,
                                                std::size_t capacity) const;
    std::vector<const PlayerView*> query_zone(ZoneId zone, std::size_t capacity) const;
    std::size_t size() const { return players_.size(); }

private:
    std::unordered_map<AccountId, PlayerView> players_;
    std::unordered_map<EntityId, AccountId> entities_;
    AccountId local_account_ = 0;
};

} // namespace acnet
