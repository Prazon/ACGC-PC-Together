#include "acnet/npc.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace acnet {

NpcAuthority::NpcAuthority(PlayerDirectory* players, NpcConfig config)
    : players_(players), config_(config) {
    if (config_.interaction_radius <= 0.0F) config_.interaction_radius = 100.0F;
    if (config_.lease_duration_ticks == 0) config_.lease_duration_ticks = 600;
}

Revision NpcAuthority::next_revision(Revision revision) {
    return revision == std::numeric_limits<Revision>::max() ? 1 : revision + 1;
}

bool NpcAuthority::add_npc(const NpcState& npc_value) {
    if (npc_value.entity == 0 || npc_value.zone == 0 || npc_value.revision == 0 ||
        !finite(npc_value.transform.position) || !finite(npc_value.transform.velocity)) return false;
    return npcs_.emplace(npc_value.entity, npc_value).second;
}

bool NpcAuthority::remove_npc(EntityId entity) {
    conversations_.erase(entity);
    return npcs_.erase(entity) != 0;
}

NpcState* NpcAuthority::npc(EntityId entity) {
    const auto found = npcs_.find(entity);
    return found == npcs_.end() ? nullptr : &found->second;
}

const NpcState* NpcAuthority::npc(EntityId entity) const {
    const auto found = npcs_.find(entity);
    return found == npcs_.end() ? nullptr : &found->second;
}

const ConversationState* NpcAuthority::conversation(EntityId entity) const {
    const auto found = conversations_.find(entity);
    return found == conversations_.end() ? nullptr : &found->second;
}

std::vector<NpcState> NpcAuthority::zone_snapshot(ZoneId zone) const {
    std::vector<NpcState> result;
    for (const auto& item : npcs_) {
        if (item.second.zone == zone) result.push_back(item.second);
    }
    std::sort(result.begin(), result.end(), [](const NpcState& a, const NpcState& b) {
        return a.entity < b.entity;
    });
    return result;
}

bool NpcAuthority::player_in_range(AccountId account, const NpcState& npc_value) const {
    if (players_ == nullptr) return false;
    const PlayerView* player = players_->by_account(account);
    if (player == nullptr || player->zone != npc_value.zone || !player->interaction_eligible) return false;
    const float dx = player->transform.position.x - npc_value.transform.position.x;
    const float dy = player->transform.position.y - npc_value.transform.position.y;
    const float dz = player->transform.position.z - npc_value.transform.position.z;
    const float radius = config_.interaction_radius;
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

std::uint32_t NpcAuthority::next_lease_id() {
    ++lease_counter_;
    if (lease_counter_ == 0) ++lease_counter_;
    return lease_counter_;
}

ConversationResult NpcAuthority::request_conversation(AccountId account, EntityId npc_id, Tick tick) {
    ConversationResult result;
    result.npc = npc_id;
    NpcState* npc_value = npc(npc_id);
    if (account == 0 || npc_value == nullptr) {
        result.code = ResultCode::NotFound;
        return result;
    }
    if (!player_in_range(account, *npc_value)) {
        result.code = ResultCode::OutOfRange;
        return result;
    }
    ConversationState& state = conversations_[npc_id];
    if (state.active && tick <= state.lease.expires_tick) {
        if (state.lease.owner != account) {
            result.code = ResultCode::Conflict;
            result.lease_id = state.lease.lease_id;
            result.expires_tick = state.lease.expires_tick;
            return result;
        }
        state.lease.expires_tick = tick + config_.lease_duration_ticks;
    } else {
        state.active = true;
        state.dialogue_node = 1;
        state.revision = next_revision(state.revision);
        state.lease = {account, npc_id, next_lease_id(), tick + config_.lease_duration_ticks};
        npc_value->revision = next_revision(npc_value->revision);
        npc_value->animation = 1;
    }
    result.code = ResultCode::Ok;
    result.lease_id = state.lease.lease_id;
    result.dialogue_node = state.dialogue_node;
    result.revision = state.revision;
    result.expires_tick = state.lease.expires_tick;
    return result;
}

ConversationResult NpcAuthority::advance_conversation(AccountId account,
                                                       EntityId npc_id,
                                                       std::uint32_t lease_id,
                                                       std::uint16_t choice,
                                                       Tick tick) {
    ConversationResult result;
    result.npc = npc_id;
    const auto found = conversations_.find(npc_id);
    if (found == conversations_.end() || !found->second.active) {
        result.code = ResultCode::NotFound;
        return result;
    }
    ConversationState& state = found->second;
    result.lease_id = state.lease.lease_id;
    result.dialogue_node = state.dialogue_node;
    result.revision = state.revision;
    result.expires_tick = state.lease.expires_tick;
    if (state.lease.owner != account || state.lease.lease_id != lease_id) {
        result.code = ResultCode::Unauthorized;
        return result;
    }
    if (tick > state.lease.expires_tick) {
        state.active = false;
        result.code = ResultCode::InvalidState;
        return result;
    }
    const NpcState* npc_value = npc(npc_id);
    if (npc_value == nullptr || !player_in_range(account, *npc_value)) {
        result.code = ResultCode::OutOfRange;
        return result;
    }
    std::optional<std::uint32_t> next;
    if (resolver_) {
        next = resolver_(npc_id, account, state.dialogue_node, choice);
    } else {
        next = choice == std::numeric_limits<std::uint16_t>::max() ? 0U : state.dialogue_node + 1;
    }
    if (!next.has_value()) {
        result.code = ResultCode::InvalidState;
        return result;
    }
    state.dialogue_node = *next;
    state.revision = next_revision(state.revision);
    state.lease.expires_tick = tick + config_.lease_duration_ticks;
    if (*next == 0) {
        state.active = false;
        NpcState* mutable_npc = npc(npc_id);
        if (mutable_npc != nullptr) {
            mutable_npc->animation = 0;
            mutable_npc->revision = next_revision(mutable_npc->revision);
        }
        result.completed = true;
    }
    result.code = ResultCode::Ok;
    result.dialogue_node = state.dialogue_node;
    result.revision = state.revision;
    result.expires_tick = state.lease.expires_tick;
    return result;
}

bool NpcAuthority::release_conversation(AccountId account, EntityId npc_id, std::uint32_t lease_id) {
    const auto found = conversations_.find(npc_id);
    if (found == conversations_.end() || !found->second.active || found->second.lease.owner != account ||
        found->second.lease.lease_id != lease_id) return false;
    found->second.active = false;
    NpcState* npc_value = npc(npc_id);
    if (npc_value != nullptr) {
        npc_value->animation = 0;
        npc_value->revision = next_revision(npc_value->revision);
    }
    return true;
}

std::size_t NpcAuthority::release_player(AccountId account) {
    std::size_t released = 0;
    for (auto& item : conversations_) {
        if (item.second.active && item.second.lease.owner == account) {
            item.second.active = false;
            NpcState* npc_value = npc(item.first);
            if (npc_value != nullptr) npc_value->animation = 0;
            ++released;
        }
    }
    if (event_.has_value() && event_->owner == account) event_.reset();
    return released;
}

std::size_t NpcAuthority::expire(Tick tick) {
    std::size_t expired = 0;
    for (auto& item : conversations_) {
        if (item.second.active && tick > item.second.lease.expires_tick) {
            item.second.active = false;
            NpcState* npc_value = npc(item.first);
            if (npc_value != nullptr) npc_value->animation = 0;
            ++expired;
        }
    }
    if (event_.has_value() && tick > event_->expires_tick) event_.reset();
    return expired;
}

bool NpcAuthority::acquire_event(std::uint32_t event_id, AccountId account, Tick tick, Tick duration) {
    if (event_id == 0 || account == 0 || duration == 0) return false;
    if (event_.has_value() && tick <= event_->expires_tick &&
        (event_->event_id != event_id || event_->owner != account)) return false;
    event_ = EventLease{event_id, account, tick + duration};
    return true;
}

bool NpcAuthority::release_event(std::uint32_t event_id, AccountId account) {
    if (!event_.has_value() || event_->event_id != event_id || event_->owner != account) return false;
    event_.reset();
    return true;
}

const PlayerView* NpcAuthority::nearest_player(EntityId npc_id, float radius) const {
    const NpcState* npc_value = npc(npc_id);
    return npc_value == nullptr || players_ == nullptr
               ? nullptr
               : players_->nearest(npc_value->transform.position, npc_value->zone, radius);
}

} // namespace acnet

