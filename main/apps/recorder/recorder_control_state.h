#ifndef RECORDER_CONTROL_STATE_H_
#define RECORDER_CONTROL_STATE_H_

#include <cstdint>

enum class RecorderControlMode {
    kIdle,
    kRecording,
    kPlaying,
    kPaused,
};

enum class RecorderControlEvent {
    kTouchRecord,
    kTouchStop,
    kTouchPlay,
    kTouchPauseResume,
    kPhysicalLeft,
    kPhysicalRight,
    kPlaybackSelected,
    kPlaybackFinished,
    kExitRequested,
};

enum class RecorderControlAction {
    kNone,
    kStartRecording,
    kStopRecording,
    kOpenPlaybackMenu,
    kPausePlayback,
    kResumePlayback,
    kVolumeChanged,
    kStartPlayback,
    kExit,
};

struct RecorderControlState {
    RecorderControlMode mode = RecorderControlMode::kIdle;
    int volume = 70;
};

RecorderControlAction RecorderControlReduce(RecorderControlState* state,
                                             RecorderControlEvent event);
bool RecorderMenuHoldReached(uint32_t pressed_tick, uint32_t now_tick);

#endif  // RECORDER_CONTROL_STATE_H_
