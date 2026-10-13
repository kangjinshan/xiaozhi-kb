#ifndef RECORDER_HOLD_INPUT_H_
#define RECORDER_HOLD_INPUT_H_

#include <cstdint>

enum class RecorderHoldAction { kNone, kStart, kStop };
enum class RecorderHoldSource : uint8_t { kTouch = 1, kKey = 2 };

// Touch and the physical key may overlap. Only releasing the last held input
// ends recording; a stale release from one input cannot stop the other.
class RecorderHoldInput {
public:
    RecorderHoldAction Set(RecorderHoldSource source, bool pressed) {
        const uint8_t before = held_;
        const auto bit = static_cast<uint8_t>(source);
        if (pressed) held_ |= bit;
        else held_ &= static_cast<uint8_t>(~bit);
        if (before == 0 && held_ != 0) return RecorderHoldAction::kStart;
        if (before != 0 && held_ == 0) return RecorderHoldAction::kStop;
        return RecorderHoldAction::kNone;
    }
private:
    uint8_t held_ = 0;
};

// The dark screen uses a deliberate 300 ms hold, not a tap-to-wake. A finger
// left on the screen during a power transition must first be released.
class RecorderDarkTouch {
public:
    void AdoptHold() { active_ = down_ = true; wait_for_release_ = false; }
    RecorderHoldAction Reset(bool wait_for_release) {
        const bool was_active = active_;
        active_ = down_ = false;
        wait_for_release_ = wait_for_release;
        return was_active ? RecorderHoldAction::kStop : RecorderHoldAction::kNone;
    }
    RecorderHoldAction Poll(bool touched, uint32_t now_ms) {
        if (wait_for_release_) {
            if (!touched) wait_for_release_ = false;
            return RecorderHoldAction::kNone;
        }
        if (!touched) {
            const bool was_active = active_;
            active_ = down_ = false;
            return was_active ? RecorderHoldAction::kStop : RecorderHoldAction::kNone;
        }
        if (!down_) { down_ = true; pressed_ms_ = now_ms; }
        if (!active_ && static_cast<uint32_t>(now_ms - pressed_ms_) >= 300) {
            active_ = true;
            return RecorderHoldAction::kStart;
        }
        return RecorderHoldAction::kNone;
    }
private:
    uint32_t pressed_ms_ = 0;
    bool down_ = false;
    bool active_ = false;
    bool wait_for_release_ = true;
};

#endif
