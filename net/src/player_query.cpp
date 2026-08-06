#include "acnet/player_query.hpp"

#include <algorithm>
#include <cmath>

namespace acnet {
namespace {

float distance_squared(const Vec3& a, const Vec3& b) {
    const float x = a.x - b.x;
    const float y = a.y - b.y;
    const float z = a.z - b.z;
    return x * x + y * y + z * z;
}

} // namespace

bool PlayerDirectory::upsert(const PlayerView& player) {
    if (player.account == 0 || player.entity == 0 || player.zone == 0 ||
        !finite(player.transform.position) || !finite(player.transform.velocity)) return false;
    const auto entity_owner = entities_.find(player.entity);
    if (entity_owner != entities_.end() && entity_owner->second != player.account) return false;
    const auto existing = players_.find(player.account);
    if (existing != players_.end() && existing->second.entity != player.entity) {
        entities_.erase(existing->second.entity);
    }
    players_[player.account] = player;
    entities_[player.entity] = player.account;
    if (player.local) return set_local(player.account);
    return true;
}

bool PlayerDirectory::remove(AccountId account) {
    const auto found = players_.find(account);
    if (found == players_.end()) return false;
    entities_.erase(found->second.entity);
    players_.erase(found);
    if (local_account_ == account) local_account_ = 0;
    return true;
}

bool PlayerDirectory::set_local(AccountId account) {
    const auto found = players_.find(account);
    if (found == players_.end()) return false;
    if (local_account_ != 0) {
        const auto old = players_.find(local_account_);
        if (old != players_.end()) old->second.local = false;
    }
    local_account_ = account;
    found->second.local = true;
    return true;
}

PlayerView* PlayerDirectory::local() { return by_account(local_account_); }
const PlayerView* PlayerDirectory::local() const { return by_account(local_account_); }

PlayerView* PlayerDirectory::by_account(AccountId account) {
    const auto found = players_.find(account);
    return found == players_.end() ? nullptr : &found->second;
}

const PlayerView* PlayerDirectory::by_account(AccountId account) const {
    const auto found = players_.find(account);
    return found == players_.end() ? nullptr : &found->second;
}

PlayerView* PlayerDirectory::by_entity(EntityId entity) {
    const auto found = entities_.find(entity);
    return found == entities_.end() ? nullptr : by_account(found->second);
}

const PlayerView* PlayerDirectory::by_entity(EntityId entity) const {
    const auto found = entities_.find(entity);
    return found == entities_.end() ? nullptr : by_account(found->second);
}

const PlayerView* PlayerDirectory::nearest(const Vec3& position, ZoneId zone, float radius) const {
    if (!finite(position) || zone == 0 || radius < 0.0F) return nullptr;
    const float maximum = radius * radius;
    const PlayerView* result = nullptr;
    float best = maximum;
    for (const auto& item : players_) {
        const PlayerView& player = item.second;
        if (player.zone != zone || !player.interaction_eligible) continue;
        const float distance = distance_squared(position, player.transform.position);
        if (distance <= best) {
            best = distance;
            result = &player;
        }
    }
    return result;
}

std::vector<const PlayerView*> PlayerDirectory::query_radius(const Vec3& position,
                                                             ZoneId zone,
                                                             float radius,
                                                             std::size_t capacity) const {
    std::vector<std::pair<float, const PlayerView*>> matches;
    if (!finite(position) || zone == 0 || radius < 0.0F || capacity == 0) return {};
    const float maximum = radius * radius;
    for (const auto& item : players_) {
        const PlayerView& player = item.second;
        if (player.zone != zone || !player.interaction_eligible) continue;
        const float distance = distance_squared(position, player.transform.position);
        if (distance <= maximum) matches.emplace_back(distance, &player);
    }
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second->account < b.second->account;
    });
    if (matches.size() > capacity) matches.resize(capacity);
    std::vector<const PlayerView*> result;
    result.reserve(matches.size());
    for (const auto& match : matches) result.push_back(match.second);
    return result;
}

std::vector<const PlayerView*> PlayerDirectory::query_zone(ZoneId zone, std::size_t capacity) const {
    std::vector<const PlayerView*> result;
    if (zone == 0 || capacity == 0) return result;
    for (const auto& item : players_) {
        if (item.second.zone == zone) result.push_back(&item.second);
    }
    std::sort(result.begin(), result.end(), [](const PlayerView* a, const PlayerView* b) {
        return a->account < b->account;
    });
    if (result.size() > capacity) result.resize(capacity);
    return result;
}

} // namespace acnet

