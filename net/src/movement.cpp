#include "acnet/movement.hpp"

#include <algorithm>
#include <cmath>

namespace acnet {
namespace {

float approach(float current, float target, float maximum_change) {
    if (current < target) return std::min(current + maximum_change, target);
    return std::max(current - maximum_change, target);
}

float clamp_axis(std::int16_t value) {
    constexpr float maximum = 32767.0F;
    return std::clamp(static_cast<float>(value) / maximum, -1.0F, 1.0F);
}

} // namespace

MovementSimulator::MovementSimulator(MovementConfig config, CollisionValidator collision)
    : config_(config), collision_(std::move(collision)) {
    if (config_.tick_seconds <= 0.0F) config_.tick_seconds = 1.0F / 60.0F;
    if (config_.maximum_speed < 0.0F) config_.maximum_speed = 0.0F;
    if (config_.acceleration < 0.0F) config_.acceleration = 0.0F;
    if (config_.friction < 0.0F) config_.friction = 0.0F;
    if (config_.maximum_queued_inputs == 0) config_.maximum_queued_inputs = 1;
}

bool MovementSimulator::valid_transform(const Transform& transform) const {
    if (!finite(transform.position) || !finite(transform.velocity)) return false;
    const float limit = config_.maximum_world_coordinate;
    return std::fabs(transform.position.x) <= limit && std::fabs(transform.position.y) <= limit &&
           std::fabs(transform.position.z) <= limit;
}

bool MovementSimulator::add_player(AccountId account,
                                   EntityId entity,
                                   ZoneId zone,
                                   const Transform& transform) {
    if (account == 0 || entity == 0 || zone == 0 || !valid_transform(transform) ||
        players_.find(account) != players_.end()) return false;
    MovementPlayer player;
    player.account = account;
    player.entity = entity;
    player.zone = zone;
    player.transform = transform;
    players_.emplace(account, player);
    return true;
}

bool MovementSimulator::remove_player(AccountId account) {
    return players_.erase(account) != 0;
}

bool MovementSimulator::teleport(AccountId account, ZoneId zone, const Transform& transform) {
    MovementPlayer* value = player(account);
    if (value == nullptr || zone == 0 || !valid_transform(transform)) return false;
    value->zone = zone;
    value->transform = transform;
    value->queued_inputs.clear();
    return true;
}

ResultCode MovementSimulator::submit(AccountId account, const InputCommand& command) {
    MovementPlayer* target = player(account);
    if (target == nullptr) return ResultCode::NotFound;
    if (command.sequence == 0 || command.sequence <= target->last_received_sequence) return ResultCode::Conflict;
    if (target->queued_inputs.size() >= config_.maximum_queued_inputs) return ResultCode::RateLimited;
    if (std::abs(static_cast<int>(command.stick_x)) > 32767 ||
        std::abs(static_cast<int>(command.stick_y)) > 32767) return ResultCode::Malformed;
    if (!valid_transform(command.client_transform)) return ResultCode::OutOfRange;
    target->last_received_sequence = command.sequence;
    target->queued_inputs.push_back(command);
    return ResultCode::Ok;
}

void MovementSimulator::simulate_step(Transform& transform,
                                      const InputCommand& input,
                                      const MovementConfig& config) {
    float x = clamp_axis(input.stick_x);
    float z = clamp_axis(input.stick_y);
    const float magnitude = std::sqrt(x * x + z * z);
    if (magnitude > 1.0F) {
        x /= magnitude;
        z /= magnitude;
    }
    const float target_x = x * config.maximum_speed;
    const float target_z = z * config.maximum_speed;
    const float change = (magnitude > 0.01F ? config.acceleration : config.friction) * config.tick_seconds;
    transform.velocity.x = approach(transform.velocity.x, target_x, change);
    transform.velocity.z = approach(transform.velocity.z, target_z, change);
    transform.velocity.y = 0.0F;
    transform.position.x += transform.velocity.x * config.tick_seconds;
    transform.position.z += transform.velocity.z * config.tick_seconds;
    transform.action = input.action;
    if (magnitude > 0.01F) {
        constexpr float angle_scale = 32768.0F / 3.14159265358979323846F;
        transform.yaw = static_cast<std::int16_t>(std::atan2(x, z) * angle_scale);
    }
}

void MovementSimulator::tick() {
    ++tick_;
    for (auto& item : players_) {
        MovementPlayer& target = item.second;
        if (target.queued_inputs.empty()) continue;

        /* Position packets are state updates, not instructions for a second
         * movement simulation. Consume every queued update and retain the
         * newest transform so delayed packets cannot make a client appear to
         * run in slow motion. Sequence checks in submit() reject reordering. */
        while (!target.queued_inputs.empty()) {
            const InputCommand& input = target.queued_inputs.front();
            target.transform = input.client_transform;
            target.last_processed_sequence = input.sequence;
            target.queued_inputs.pop_front();
        }
    }
}

MovementPlayer* MovementSimulator::player(AccountId account) {
    const auto found = players_.find(account);
    return found == players_.end() ? nullptr : &found->second;
}

const MovementPlayer* MovementSimulator::player(AccountId account) const {
    const auto found = players_.find(account);
    return found == players_.end() ? nullptr : &found->second;
}

PlayerSnapshot MovementSimulator::snapshot(AccountId account) const {
    PlayerSnapshot result;
    const MovementPlayer* source = player(account);
    if (source == nullptr) return result;
    result.entity = source->entity;
    result.account = source->account;
    result.zone = source->zone;
    result.acknowledged_input = source->last_processed_sequence;
    result.transform = source->transform;
    return result;
}

ClientPredictor::ClientPredictor(MovementConfig config) : config_(config) {}

void ClientPredictor::reset(const Transform& authoritative, std::uint32_t acknowledged_sequence) {
    predicted_ = authoritative;
    pending_.clear();
    next_sequence_ = acknowledged_sequence + 1;
    if (next_sequence_ == 0) next_sequence_ = 1;
}

InputCommand ClientPredictor::predict(std::int16_t stick_x,
                                      std::int16_t stick_y,
                                      std::uint16_t buttons,
                                      std::uint16_t action,
                                      Tick estimated_server_tick) {
    InputCommand command;
    command.sequence = next_sequence_++;
    command.estimated_server_tick = estimated_server_tick;
    command.stick_x = stick_x;
    command.stick_y = stick_y;
    command.buttons = buttons;
    command.action = action;
    MovementSimulator::simulate_step(predicted_, command, config_);
    command.client_transform = predicted_;
    pending_.push_back(command);
    return command;
}

bool ClientPredictor::reconcile(const Transform& authoritative, std::uint32_t acknowledged_sequence) {
    if (!finite(authoritative.position) || !finite(authoritative.velocity)) return false;
    while (!pending_.empty() && pending_.front().sequence <= acknowledged_sequence) pending_.pop_front();
    predicted_ = authoritative;
    for (const InputCommand& command : pending_) {
        MovementSimulator::simulate_step(predicted_, command, config_);
    }
    return true;
}

} // namespace acnet
