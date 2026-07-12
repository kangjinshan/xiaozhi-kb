#ifndef RECORDER_CONTROL_STATE_H_
#define RECORDER_CONTROL_STATE_H_

#include "agent_voice_state.h"

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
    kAgentReplyReady,
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
    kStartAgentReplyPlayback,
    kExit,
};

struct RecorderControlState {
    RecorderControlMode mode = RecorderControlMode::kIdle;
    int volume = 70;
    AgentVoicePhase voice_phase = AgentVoicePhase::kOffline;
    bool voice_turn_pending = false;
};

RecorderControlAction RecorderControlReduce(RecorderControlState* state,
                                             RecorderControlEvent event);
bool RecorderMenuHoldReached(uint32_t pressed_tick, uint32_t now_tick);

#endif  // RECORDER_CONTROL_STATE_H_
