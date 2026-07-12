#include "recorder_control_state.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void TestTouchRecordAndStop() {
    RecorderControlState state;
    Check(RecorderControlReduce(&state, RecorderControlEvent::kTouchRecord) ==
              RecorderControlAction::kStartRecording,
          "REC starts recording");
    Check(state.mode == RecorderControlMode::kRecording,
          "recording state entered");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kTouchStop) ==
              RecorderControlAction::kStopRecording,
          "STOP stops recording");
    Check(state.mode == RecorderControlMode::kIdle, "idle state restored");
}

void TestAgentVoicePhaseGatesRecordingAndStartsReplyPlayback() {
    RecorderControlState offline;
    offline.voice_phase = AgentVoicePhase::kOffline;
    Check(RecorderControlReduce(&offline, RecorderControlEvent::kTouchRecord) ==
              RecorderControlAction::kStartRecording,
          "recording remains available offline");

    for (AgentVoicePhase phase : {
             AgentVoicePhase::kSending,
             AgentVoicePhase::kThinking,
             AgentVoicePhase::kReceiving,
         }) {
        RecorderControlState busy;
        busy.voice_phase = phase;
        Check(RecorderControlReduce(&busy, RecorderControlEvent::kTouchRecord) ==
                  RecorderControlAction::kNone,
              "recording is rejected while an Agent turn is active");
        Check(busy.mode == RecorderControlMode::kIdle,
              "rejected recording remains idle");
    }

    RecorderControlState queued;
    queued.voice_phase = AgentVoicePhase::kOffline;
    queued.voice_turn_pending = true;
    Check(RecorderControlReduce(&queued, RecorderControlEvent::kTouchRecord) ==
              RecorderControlAction::kNone,
          "offline queued turn blocks a second recording");
    Check(RecorderControlReduce(&queued, RecorderControlEvent::kTouchPlay) ==
              RecorderControlAction::kNone,
          "queued turn blocks manual playback from starving network events");

    RecorderControlState reply;
    reply.voice_phase = AgentVoicePhase::kReadyToPlay;
    Check(RecorderControlReduce(&reply, RecorderControlEvent::kAgentReplyReady) ==
              RecorderControlAction::kStartAgentReplyPlayback,
          "completed Agent reply starts playback");
    Check(reply.mode == RecorderControlMode::kPlaying,
          "Agent reply enters playing state");
}

void TestIdlePlayOpensMenuAndSelectionStartsPlayback() {
    RecorderControlState state;
    Check(RecorderControlReduce(&state, RecorderControlEvent::kTouchPlay) ==
              RecorderControlAction::kOpenPlaybackMenu,
          "PLAY opens the recordings menu");
    Check(state.mode == RecorderControlMode::kIdle,
          "opening the menu stays idle");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPlaybackSelected) ==
              RecorderControlAction::kStartPlayback,
          "selecting a file starts playback");
    Check(state.mode == RecorderControlMode::kPlaying,
          "playing state entered");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPlaybackFinished) ==
              RecorderControlAction::kNone,
          "playback completion needs no side effect");
    Check(state.mode == RecorderControlMode::kIdle,
          "playback completion restores idle");
}

void TestRecordingIgnoresPhysicalKeys() {
    RecorderControlState state{RecorderControlMode::kRecording, 70};
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalLeft) ==
              RecorderControlAction::kNone,
          "left key ignored while recording");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalRight) ==
              RecorderControlAction::kNone,
          "right key ignored while recording");
    Check(state.volume == 70, "ignored keys keep volume");
}

void TestIdleIgnoresPhysicalKeys() {
    RecorderControlState state{RecorderControlMode::kIdle, 70};
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalLeft) ==
              RecorderControlAction::kNone,
          "left key ignored while idle");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalRight) ==
              RecorderControlAction::kNone,
          "right key ignored while idle");
    Check(state.volume == 70, "idle keys keep volume");
}

void TestPowerKeyTogglesScreenWithoutChangingAssistantMode() {
    for (RecorderControlMode mode : {
             RecorderControlMode::kIdle,
             RecorderControlMode::kRecording,
             RecorderControlMode::kPlaying,
             RecorderControlMode::kPaused,
         }) {
        RecorderControlState state{mode, 70};
        state.voice_phase = AgentVoicePhase::kThinking;
        Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalPower) ==
                  RecorderControlAction::kScreenPowerChanged,
              "PWR turns the screen off in every mode");
        Check(!state.screen_on && state.mode == mode && state.volume == 70 &&
                  state.voice_phase == AgentVoicePhase::kThinking,
              "screen off preserves assistant state");
        Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalPower) ==
                  RecorderControlAction::kScreenPowerChanged,
              "second PWR press turns the screen on");
        Check(state.screen_on && state.mode == mode,
              "screen on restores only display state");
    }
}

void TestPauseResumeAndVolumeBounds() {
    RecorderControlState state{RecorderControlMode::kPlaying, 95};
    Check(RecorderControlReduce(&state, RecorderControlEvent::kTouchPauseResume) ==
              RecorderControlAction::kPausePlayback,
          "PAUSE pauses");
    Check(state.mode == RecorderControlMode::kPaused, "paused state entered");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalLeft) ==
              RecorderControlAction::kVolumeChanged,
          "left key raises volume while paused");
    Check(state.volume == 100, "volume clamps at 100");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalLeft) ==
              RecorderControlAction::kNone,
          "volume at 100 does not report a change");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kTouchPauseResume) ==
              RecorderControlAction::kResumePlayback,
          "RESUME resumes");
    Check(state.mode == RecorderControlMode::kPlaying,
          "playing state restored");
    for (int i = 0; i < 10; ++i) {
        RecorderControlReduce(&state, RecorderControlEvent::kPhysicalRight);
    }
    Check(state.volume == 0, "volume clamps at zero");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalRight) ==
              RecorderControlAction::kNone,
          "volume at zero does not report a change");
}

void TestMenuExitIsAvailableInEveryState() {
    for (RecorderControlMode mode : {
             RecorderControlMode::kIdle,
             RecorderControlMode::kRecording,
             RecorderControlMode::kPlaying,
             RecorderControlMode::kPaused,
         }) {
        RecorderControlState state{mode, 70};
        Check(RecorderControlReduce(&state, RecorderControlEvent::kExitRequested) ==
                  RecorderControlAction::kExit,
              "MENU exit accepted in every state");
    }
}

void TestMenuHoldTiming() {
    Check(!RecorderMenuHoldReached(1000, 2999), "1999 ms does not exit");
    Check(RecorderMenuHoldReached(1000, 3000), "2000 ms exits");
    Check(RecorderMenuHoldReached(0xFFFFFF00u, 0x000006D0u),
          "hold timing survives tick wrap");
}

}  // namespace

int main() {
    TestTouchRecordAndStop();
    TestAgentVoicePhaseGatesRecordingAndStartsReplyPlayback();
    TestIdlePlayOpensMenuAndSelectionStartsPlayback();
    TestRecordingIgnoresPhysicalKeys();
    TestIdleIgnoresPhysicalKeys();
    TestPowerKeyTogglesScreenWithoutChangingAssistantMode();
    TestPauseResumeAndVolumeBounds();
    TestMenuExitIsAvailableInEveryState();
    TestMenuHoldTiming();
    std::puts("recorder_control_state_test passed");
    return 0;
}
