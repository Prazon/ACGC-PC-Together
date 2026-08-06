#pragma once

#include "acnet/types.hpp"

#include <cstddef>
#include <deque>
#include <optional>

namespace acnet {

struct TimedTransform {
    Tick tick = 0;
    Transform transform;
};

class TransformHistory {
public:
    explicit TransformHistory(std::size_t capacity = 32);

    bool push(Tick tick, const Transform& transform);
    std::optional<Transform> sample(double render_tick,
                                    double max_extrapolation_ticks = 2.0,
                                    double tick_seconds = 1.0) const;
    void clear();
    std::size_t size() const { return history_.size(); }

private:
    std::size_t capacity_;
    std::deque<TimedTransform> history_;
};

} // namespace acnet
