#include "acnet/zone.hpp"
#include "acnet/crypto.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace acnet {

std::size_t TransferTokenHash::operator()(const TransferToken& token) const {
    const std::size_t high = std::hash<std::uint64_t>{}(token.high);
    const std::size_t low = std::hash<std::uint64_t>{}(token.low);
    return high ^ (low + 0x9E3779B9U + (high << 6) + (high >> 2));
}

ZoneCoordinator::ZoneCoordinator(PlayerDirectory* players, ZoneConfig config, std::uint64_t random_seed)
    : players_(players), config_(config) {
    if (config_.transfer_timeout_ticks == 0) config_.transfer_timeout_ticks = 600;
    if (config_.sleep_after_ticks == 0) config_.sleep_after_ticks = 300;
    secure_random_ = random_seed == 0;
    if (!secure_random_) random_.seed(random_seed);
}

bool ZoneCoordinator::add_zone(const ZoneState& zone_value) {
    if (zone_value.id == 0 || zone_value.capacity == 0 || zone_value.baseline_revision == 0) return false;
    return zones_.emplace(zone_value.id, zone_value).second;
}

bool ZoneCoordinator::add_door(const DoorDefinition& door) {
    if (door.id == 0 || door.source_zone == 0 || door.destination_zone == 0 ||
        zones_.find(door.source_zone) == zones_.end() || zones_.find(door.destination_zone) == zones_.end() ||
        door.interaction_radius <= 0.0F || !finite(door.source_position) || !finite(door.destination_position)) return false;
    return doors_.emplace(door.id, door).second;
}

bool ZoneCoordinator::set_door(const DoorDefinition& door) {
    if (door.id == 0 || door.source_zone == 0 || door.destination_zone == 0 ||
        zones_.find(door.source_zone) == zones_.end() || zones_.find(door.destination_zone) == zones_.end() ||
        door.interaction_radius <= 0.0F || !finite(door.source_position) || !finite(door.destination_position)) return false;
    doors_[door.id] = door;
    return true;
}

bool ZoneCoordinator::join(AccountId account, ZoneId zone_id, const Vec3& position, Tick tick) {
    if (players_ == nullptr || !finite(position)) return false;
    PlayerView* player = players_->by_account(account);
    ZoneState* destination = zone(zone_id);
    if (player == nullptr || destination == nullptr || destination->occupants.size() + reserved_for(zone_id) >= destination->capacity)
        return false;
    ZoneState* old = zone(player->zone);
    if (old != nullptr) old->occupants.erase(account);
    player->zone = zone_id;
    player->transform.position = position;
    player->transform.velocity = {};
    destination->occupants.insert(account);
    destination->runtime = ZoneRuntimeState::Active;
    destination->last_active_tick = tick;
    return true;
}

bool ZoneCoordinator::leave(AccountId account, Tick tick) {
    if (players_ == nullptr) return false;
    PlayerView* player = players_->by_account(account);
    if (player == nullptr) return false;
    ZoneState* current = zone(player->zone);
    if (current != nullptr) {
        current->occupants.erase(account);
        current->last_active_tick = tick;
    }
    cancel_transfer(account);
    return true;
}

TransferToken ZoneCoordinator::random_token() {
    TransferToken token;
    do {
        if (secure_random_) {
            if (!secure_random(reinterpret_cast<std::uint8_t*>(&token), sizeof(token)))
                throw std::runtime_error("operating-system random source failed");
        } else {
            token = {random_(), random_()};
        }
    } while (!token.valid() || issued_tokens_.find(token) != issued_tokens_.end());
    issued_tokens_.insert(token);
    return token;
}

bool ZoneCoordinator::door_in_range(const PlayerView& player, const DoorDefinition& door) const {
    if (player.zone != door.source_zone) return false;
    const float dx = player.transform.position.x - door.source_position.x;
    const float dy = player.transform.position.y - door.source_position.y;
    const float dz = player.transform.position.z - door.source_position.z;
    return dx * dx + dy * dy + dz * dz <= door.interaction_radius * door.interaction_radius;
}

TransferOffer ZoneCoordinator::request_transfer(AccountId account, std::uint32_t door_id, Tick tick) {
    TransferOffer result;
    result.account = account;
    if (players_ == nullptr || reservations_.find(account) != reservations_.end()) {
        result.code = ResultCode::Conflict;
        return result;
    }
    PlayerView* player = players_->by_account(account);
    const auto door_it = doors_.find(door_id);
    if (player == nullptr || door_it == doors_.end()) {
        result.code = ResultCode::NotFound;
        return result;
    }
    const DoorDefinition& door = door_it->second;
    if (!door_in_range(*player, door)) {
        result.code = ResultCode::OutOfRange;
        return result;
    }
    ZoneState* destination = zone(door.destination_zone);
    if (destination == nullptr) {
        result.code = ResultCode::NotFound;
        return result;
    }
    if (destination->occupants.size() + reserved_for(destination->id) >= destination->capacity) {
        result.code = ResultCode::Capacity;
        return result;
    }
    result.code = ResultCode::Ok;
    result.source_zone = door.source_zone;
    result.destination_zone = door.destination_zone;
    result.destination_position = door.destination_position;
    result.token = random_token();
    result.expires_tick = tick + config_.transfer_timeout_ticks;
    result.baseline_revision = destination->baseline_revision;
    reservations_[account] = TransferReservation{result, door_id};
    destination->runtime = ZoneRuntimeState::Active;
    destination->last_active_tick = tick;
    return result;
}

ResultCode ZoneCoordinator::acknowledge_ready(AccountId account, const TransferToken& token, Tick tick) {
    const auto found = reservations_.find(account);
    if (found == reservations_.end()) return ResultCode::NotFound;
    const TransferOffer offer = found->second.offer;
    if (!(offer.token == token)) return ResultCode::Unauthorized;
    if (tick > offer.expires_tick) {
        issued_tokens_.erase(offer.token);
        reservations_.erase(found);
        return ResultCode::InvalidState;
    }
    PlayerView* player = players_ == nullptr ? nullptr : players_->by_account(account);
    ZoneState* source = zone(offer.source_zone);
    ZoneState* destination = zone(offer.destination_zone);
    if (player == nullptr || source == nullptr || destination == nullptr || player->zone != source->id) {
        return ResultCode::InvalidState;
    }
    if (destination->occupants.size() >= destination->capacity) return ResultCode::Capacity;
    source->occupants.erase(account);
    source->last_active_tick = tick;
    destination->occupants.insert(account);
    destination->runtime = ZoneRuntimeState::Active;
    destination->last_active_tick = tick;
    player->zone = destination->id;
    player->transform.position = offer.destination_position;
    player->transform.velocity = {};
    issued_tokens_.erase(offer.token);
    reservations_.erase(found);
    return ResultCode::Ok;
}

bool ZoneCoordinator::cancel_transfer(AccountId account) {
    const auto found = reservations_.find(account);
    if (found == reservations_.end()) return false;
    issued_tokens_.erase(found->second.offer.token);
    reservations_.erase(found);
    return true;
}

std::size_t ZoneCoordinator::expire(Tick tick) {
    std::vector<AccountId> expired;
    for (const auto& item : reservations_) {
        if (tick > item.second.offer.expires_tick) expired.push_back(item.first);
    }
    for (AccountId account : expired) cancel_transfer(account);
    return expired.size();
}

void ZoneCoordinator::update_sleep_states(Tick tick) {
    for (auto& item : zones_) {
        ZoneState& value = item.second;
        if (!value.occupants.empty() || reserved_for(value.id) != 0) {
            value.runtime = ZoneRuntimeState::Active;
            value.last_active_tick = tick;
        } else if (tick >= value.last_active_tick && tick - value.last_active_tick >= config_.sleep_after_ticks) {
            value.runtime = ZoneRuntimeState::Sleeping;
        }
    }
}

ZoneState* ZoneCoordinator::zone(ZoneId id) {
    const auto found = zones_.find(id);
    return found == zones_.end() ? nullptr : &found->second;
}

const ZoneState* ZoneCoordinator::zone(ZoneId id) const {
    const auto found = zones_.find(id);
    return found == zones_.end() ? nullptr : &found->second;
}

const TransferReservation* ZoneCoordinator::reservation(AccountId account) const {
    const auto found = reservations_.find(account);
    return found == reservations_.end() ? nullptr : &found->second;
}

std::size_t ZoneCoordinator::reserved_for(ZoneId zone_id) const {
    std::size_t result = 0;
    for (const auto& item : reservations_) {
        if (item.second.offer.destination_zone == zone_id) ++result;
    }
    return result;
}

} // namespace acnet
