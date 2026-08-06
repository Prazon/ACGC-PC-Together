#pragma once

#include "acnet/player_query.hpp"
#include "acnet/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace acnet {

struct NpcState {
    EntityId entity = 0;
    ZoneId zone = 0;
    Transform transform;
    std::uint16_t schedule_state = 0;
    std::uint16_t animation = 0;
    std::uint16_t emotion = 0;
    EntityId destination = 0;
    Revision revision = 1;
};

struct NpcInteractionLease {
    AccountId owner = 0;
    EntityId npc = 0;
    std::uint32_t lease_id = 0;
    Tick expires_tick = 0;
};

struct ConversationState {
    NpcInteractionLease lease;
    std::uint32_t dialogue_node = 1;
    Revision revision = 1;
    bool active = false;
};

struct ConversationResult {
    ResultCode code = ResultCode::InternalError;
    EntityId npc = 0;
    std::uint32_t lease_id = 0;
    std::uint32_t dialogue_node = 0;
    Revision revision = 0;
    Tick expires_tick = 0;
    bool completed = false;
};

struct EventLease {
    std::uint32_t event_id = 0;
    AccountId owner = 0;
    Tick expires_tick = 0;
};

using DialogueResolver = std::function<std::optional<std::uint32_t>(EntityId npc,
                                                                   AccountId player,
                                                                   std::uint32_t node,
                                                                   std::uint16_t choice)>;

struct NpcConfig {
    float interaction_radius = 100.0F;
    Tick lease_duration_ticks = 600;
};

class NpcAuthority {
public:
    explicit NpcAuthority(PlayerDirectory* players, NpcConfig config = {});

    bool add_npc(const NpcState& npc);
    bool remove_npc(EntityId entity);
    NpcState* npc(EntityId entity);
    const NpcState* npc(EntityId entity) const;
    const ConversationState* conversation(EntityId entity) const;
    std::vector<NpcState> zone_snapshot(ZoneId zone) const;
    const std::unordered_map<EntityId, NpcState>& all_npcs() const { return npcs_; }

    ConversationResult request_conversation(AccountId account, EntityId npc, Tick tick);
    ConversationResult advance_conversation(AccountId account,
                                             EntityId npc,
                                             std::uint32_t lease_id,
                                             std::uint16_t choice,
                                             Tick tick);
    bool release_conversation(AccountId account, EntityId npc, std::uint32_t lease_id);
    std::size_t release_player(AccountId account);
    std::size_t expire(Tick tick);

    bool acquire_event(std::uint32_t event_id, AccountId account, Tick tick, Tick duration);
    bool release_event(std::uint32_t event_id, AccountId account);
    const EventLease* active_event() const { return event_.has_value() ? &*event_ : nullptr; }

    const PlayerView* nearest_player(EntityId npc, float radius) const;
    void set_dialogue_resolver(DialogueResolver resolver) { resolver_ = std::move(resolver); }

private:
    bool player_in_range(AccountId account, const NpcState& npc) const;
    std::uint32_t next_lease_id();
    static Revision next_revision(Revision revision);

    PlayerDirectory* players_;
    NpcConfig config_;
    DialogueResolver resolver_;
    std::unordered_map<EntityId, NpcState> npcs_;
    std::unordered_map<EntityId, ConversationState> conversations_;
    std::optional<EventLease> event_;
    std::uint32_t lease_counter_ = 0;
};

} // namespace acnet
