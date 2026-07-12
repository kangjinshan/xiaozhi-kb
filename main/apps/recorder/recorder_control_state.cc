#include "recorder_control_state.h"

#include <algorithm>

namespace {

RecorderControlAction ChangeVolume(RecorderControlState* state, int delta) {
    const int previous = state->volume;
    state->volume = std::clamp(previous + delta, 0, 100);
    return state->volume == previous
        ? RecorderControlAction::kNone
        : RecorderControlAction::kVolumeChanged;
}

bool AgentTurnBlocksRecording(const RecorderControlState& state) {
    if (state.voice_turn_pending) {
        return true;
    }
    return state.voice_phase == AgentVoicePhase::kSending ||
           state.voice_phase == AgentVoicePhase::kThinking ||
           state.voice_phase == AgentVoicePhase::kReceiving ||
           state.voice_phase == AgentVoicePhase::kReadyToPlay ||
           state.voice_phase == AgentVoicePhase::kPlaying;
}

}  // namespace

RecorderControlAction RecorderControlReduce(RecorderControlState* state,
                                             RecorderControlEvent event) {
    if (state == nullptr) {
        return RecorderControlAction::kNone;
    }
    if (event == RecorderControlEvent::kPhysicalPower) {
        state->screen_on = !state->screen_on;
        return RecorderControlAction::kScreenPowerChanged;
    }
    if (event == RecorderControlEvent::kExitRequested) {
        return RecorderControlAction::kExit;
    }
    if (event == RecorderControlEvent::kPlaybackFinished) {
        if (state->mode == RecorderControlMode::kPlaying ||
            state->mode == RecorderControlMode::kPaused) {
            state->mode = RecorderControlMode::kIdle;
        }
        return RecorderControlAction::kNone;
    }
    if (event == RecorderControlEvent::kAgentReplyReady &&
        state->mode == RecorderControlMode::kIdle) {
        state->mode = RecorderControlMode::kPlaying;
        return RecorderControlAction::kStartAgentReplyPlayback;
    }

    switch (state->mode) {
        case RecorderControlMode::kIdle:
            if (event == RecorderControlEvent::kTouchRecord &&
                !AgentTurnBlocksRecording(*state)) {
                state->mode = RecorderControlMode::kRecording;
                return RecorderControlAction::kStartRecording;
            }
            if (event == RecorderControlEvent::kTouchPlay &&
                !state->voice_turn_pending) {
                return RecorderControlAction::kOpenPlaybackMenu;
            }
            if (event == RecorderControlEvent::kPlaybackSelected) {
                state->mode = RecorderControlMode::kPlaying;
                return RecorderControlAction::kStartPlayback;
            }
            break;

        case RecorderControlMode::kRecording:
            if (event == RecorderControlEvent::kTouchStop) {
                state->mode = RecorderControlMode::kIdle;
                return RecorderControlAction::kStopRecording;
            }
            break;

        case RecorderControlMode::kPlaying:
            if (event == RecorderControlEvent::kTouchPauseResume) {
                state->mode = RecorderControlMode::kPaused;
                return RecorderControlAction::kPausePlayback;
            }
            if (event == RecorderControlEvent::kPhysicalLeft) {
                return ChangeVolume(state, 10);
            }
            if (event == RecorderControlEvent::kPhysicalRight) {
                return ChangeVolume(state, -10);
            }
            break;

        case RecorderControlMode::kPaused:
            if (event == RecorderControlEvent::kTouchPauseResume) {
                state->mode = RecorderControlMode::kPlaying;
                return RecorderControlAction::kResumePlayback;
            }
            if (event == RecorderControlEvent::kPhysicalLeft) {
                return ChangeVolume(state, 10);
            }
            if (event == RecorderControlEvent::kPhysicalRight) {
                return ChangeVolume(state, -10);
            }
            break;
    }
    return RecorderControlAction::kNone;
}

bool RecorderMenuHoldReached(uint32_t pressed_tick, uint32_t now_tick) {
    return static_cast<uint32_t>(now_tick - pressed_tick) >= 2000;
}
