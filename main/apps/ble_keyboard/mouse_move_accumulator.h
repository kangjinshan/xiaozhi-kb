#ifndef MOUSE_MOVE_ACCUMULATOR_H_
#define MOUSE_MOVE_ACCUMULATOR_H_

#include <cstdint>

struct MouseMoveChunk {
    int8_t x = 0;
    int8_t y = 0;
    int8_t wheel = 0;

    bool HasMovement() const {
        return x != 0 || y != 0 || wheel != 0;
    }
};

// Keeps relative mouse movement bounded while the BLE notification path is
// temporarily busy. Reports are drained in HID-sized chunks instead of
// dropping the samples collected during transport backoff.
class MouseMoveAccumulator {
public:
    static constexpr int32_t kMaximumPending = 1024;

    // Returns true when at least one axis had to be clamped to the bounded
    // pending range.
    bool Add(int32_t dx, int32_t dy, int32_t wheel = 0);
    MouseMoveChunk PeekChunk() const;
    void Commit(const MouseMoveChunk& chunk);
    void Reset();

    bool HasPending() const;
    int32_t pending_x() const { return pending_x_; }
    int32_t pending_y() const { return pending_y_; }
    int32_t pending_wheel() const { return pending_wheel_; }

private:
    static int32_t AddBounded(int32_t current,
                              int32_t delta,
                              bool* clamped);
    static int8_t ToHidChunk(int32_t pending);

    int32_t pending_x_ = 0;
    int32_t pending_y_ = 0;
    int32_t pending_wheel_ = 0;
};

#endif  // MOUSE_MOVE_ACCUMULATOR_H_
