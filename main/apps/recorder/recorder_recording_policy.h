#ifndef RECORDER_RECORDING_POLICY_H_
#define RECORDER_RECORDING_POLICY_H_

#include <cstdint>

// Count captured PCM frames, before DSP/AMR padding. Exactly one second is valid.
constexpr bool RecorderRecordingLongEnough(uint64_t captured_frames, int sample_rate) {
    return sample_rate > 0 && captured_frames >= static_cast<uint64_t>(sample_rate);
}

#endif
