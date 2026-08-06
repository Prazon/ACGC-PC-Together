#include "acnet/interpolation.hpp"

#include <algorithm>
#include <cmath>

namespace acnet {
namespace {

float lerp(float a, float b, double alpha) {
    return static_cast<float>(a + (b - a) * alpha);
}

Transform blend(const Transform& a, const Transform& b, double alpha) {
    Transform result;
    result.position = {lerp(a.position.x, b.position.x, alpha),
                       lerp(a.position.y, b.position.y, alpha),
                       lerp(a.position.z, b.position.z, alpha)};
    result.velocity = {lerp(a.velocity.x, b.velocity.x, alpha),
                       lerp(a.velocity.y, b.velocity.y, alpha),
                       lerp(a.velocity.z, b.velocity.z, alpha)};
    const std::int32_t raw_delta = static_cast<std::int16_t>(b.yaw - a.yaw);
    result.yaw = static_cast<std::int16_t>(a.yaw + static_cast<std::int32_t>(raw_delta * alpha));
    result.action = alpha < 0.5 ? a.action : b.action;
    return result;
}

} // namespace

TransformHistory::TransformHistory(std::size_t capacity) : capacity_(std::max<std::size_t>(2, capacity)) {}

bool TransformHistory::push(Tick tick, const Transform& transform) {
    if (!finite(transform.position) || !finite(transform.velocity)) return false;
    if (!history_.empty() && tick <= history_.back().tick) return false;
    history_.push_back({tick, transform});
    while (history_.size() > capacity_) history_.pop_front();
    return true;
}

std::optional<Transform> TransformHistory::sample(double render_tick,
                                                  double max_extrapolation_ticks,
                                                  double tick_seconds) const {
    if (history_.empty() || !std::isfinite(render_tick) || !std::isfinite(tick_seconds) || tick_seconds < 0.0)
        return std::nullopt;
    if (render_tick <= history_.front().tick) return history_.front().transform;
    for (std::size_t i = 1; i < history_.size(); ++i) {
        if (render_tick <= history_[i].tick) {
            const double span = static_cast<double>(history_[i].tick - history_[i - 1].tick);
            const double alpha = span <= 0.0 ? 0.0 : (render_tick - history_[i - 1].tick) / span;
            return blend(history_[i - 1].transform, history_[i].transform, std::clamp(alpha, 0.0, 1.0));
        }
    }
    const TimedTransform& latest = history_.back();
    const double ticks = std::clamp(render_tick - latest.tick, 0.0, max_extrapolation_ticks);
    Transform result = latest.transform;
    result.position.x += static_cast<float>(result.velocity.x * ticks * tick_seconds);
    result.position.y += static_cast<float>(result.velocity.y * ticks * tick_seconds);
    result.position.z += static_cast<float>(result.velocity.z * ticks * tick_seconds);
    return result;
}

void TransformHistory::clear() {
    history_.clear();
}

} // namespace acnet
