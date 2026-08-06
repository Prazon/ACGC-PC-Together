#pragma once

#include "acnet/types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>

namespace acnet {

struct MovementConfig {
    float tick_seconds = 1.0F / 60.0F;
    float maximum_speed = 420.0F;
    float acceleration = 1800.0F;
    float friction = 2200.0F;
    float maximum_world_coordinate = 100000.0F;
    std::size_t maximum_queued_inputs = 120;
};

using CollisionValidator = std::function<bool(ZoneId zone, const Vec3& from, const Vec3& to)>;

struct MovementPlayer {
    AccountId account = 0;
    EntityId entity = 0;
    ZoneId zone = 1;
    Transform transform;
    PlayerAnimation animation;
    std::uint32_t last_received_sequence = 0;
    std::uint32_t last_processed_sequence = 0;
    std::deque<InputCommand> queued_inputs;
};

class MovementSimulator {
public:
    explicit MovementSimulator(MovementConfig config = {}, CollisionValidator collision = {});

    bool add_player(AccountId account, EntityId entity, ZoneId zone, const Transform& transform);
    bool remove_player(AccountId account);
    ResultCode submit(AccountId account, const InputCommand& command);
    bool teleport(AccountId account, ZoneId zone, const Transform& transform);
    void set_collision_validator(CollisionValidator collision) { collision_ = std::move(collision); }
    void tick();

    MovementPlayer* player(AccountId account);
    const MovementPlayer* player(AccountId account) const;
    PlayerSnapshot snapshot(AccountId account) const;
    Tick current_tick() const { return tick_; }

    static void simulate_step(Transform& transform, const InputCommand& input, const MovementConfig& config);

private:
    bool valid_transform(const Transform& transform) const;
    MovementConfig config_;
    CollisionValidator collision_;
    Tick tick_ = 0;
    std::unordered_map<AccountId, MovementPlayer> players_;
};

class ClientPredictor {
public:
    explicit ClientPredictor(MovementConfig config = {});

    void reset(const Transform& authoritative, std::uint32_t acknowledged_sequence = 0);
    InputCommand predict(std::int16_t stick_x,
                         std::int16_t stick_y,
                         std::uint16_t buttons,
                         std::uint16_t action,
                         const PlayerAnimation& animation,
                         Tick estimated_server_tick);
    bool reconcile(const Transform& authoritative, std::uint32_t acknowledged_sequence);

    const Transform& transform() const { return predicted_; }
    std::size_t pending_count() const { return pending_.size(); }
    std::uint32_t next_sequence() const { return next_sequence_; }

private:
    MovementConfig config_;
    Transform predicted_;
    std::deque<InputCommand> pending_;
    std::uint32_t next_sequence_ = 1;
};

} // namespace acnet
