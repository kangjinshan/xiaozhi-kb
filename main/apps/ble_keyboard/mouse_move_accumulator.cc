#include "mouse_move_accumulator.h"

#include <algorithm>
#include <cstdint>

namespace {

constexpr int32_t kHidRelativeMinimum = -127;
constexpr int32_t kHidRelativeMaximum = 127;

}  // namespace

bool MouseMoveAccumulator::Add(int32_t dx, int32_t dy, int32_t wheel) {
    bool clamped = false;
    pending_x_ = AddBounded(pending_x_, dx, &clamped);
    pending_y_ = AddBounded(pending_y_, dy, &clamped);
    pending_wheel_ = AddBounded(pending_wheel_, wheel, &clamped);
    return clamped;
}

MouseMoveChunk MouseMoveAccumulator::PeekChunk() const {
    return {
        .x = ToHidChunk(pending_x_),
        .y = ToHidChunk(pending_y_),
        .wheel = ToHidChunk(pending_wheel_),
    };
}

void MouseMoveAccumulator::Commit(const MouseMoveChunk& chunk) {
    pending_x_ -= chunk.x;
    pending_y_ -= chunk.y;
    pending_wheel_ -= chunk.wheel;
}

void MouseMoveAccumulator::Reset() {
    pending_x_ = 0;
    pending_y_ = 0;
    pending_wheel_ = 0;
}

bool MouseMoveAccumulator::HasPending() const {
    return pending_x_ != 0 || pending_y_ != 0 || pending_wheel_ != 0;
}

int32_t MouseMoveAccumulator::AddBounded(int32_t current,
                                         int32_t delta,
                                         bool* clamped) {
    const int64_t sum = static_cast<int64_t>(current) + delta;
    if (sum > kMaximumPending) {
        *clamped = true;
        return kMaximumPending;
    }
    if (sum < -kMaximumPending) {
        *clamped = true;
        return -kMaximumPending;
    }
    return static_cast<int32_t>(sum);
}

int8_t MouseMoveAccumulator::ToHidChunk(int32_t pending) {
    return static_cast<int8_t>(
        std::clamp(pending, kHidRelativeMinimum, kHidRelativeMaximum));
}
