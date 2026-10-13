#include "recorder_control_state.h"
#include "recorder_recording_policy.h"

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

void TestRecordingContinuesDuringAgentTurns() {
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
                  RecorderControlAction::kStartRecording,
              "a second recording starts while the previous turn is active");
        Check(busy.mode == RecorderControlMode::kRecording,
              "busy network does not block the microphone");
    }

    RecorderControlState queued;
    queued.voice_phase = AgentVoicePhase::kOffline;
    queued.voice_turn_pending = true;
    Check(RecorderControlReduce(&queued, RecorderControlEvent::kTouchRecord) ==
              RecorderControlAction::kStartRecording,
          "offline recordings can queue");
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

void TestHoldInterruptsPlaybackAndReplyWaitsForRelease() {
    for (auto mode : {RecorderControlMode::kPlaying, RecorderControlMode::kPaused}) {
        RecorderControlState state{mode, 70};
        Check(RecorderControlReduce(&state, RecorderControlEvent::kTouchRecord) ==
                  RecorderControlAction::kStartRecording, "hold interrupts playback");
        Check(RecorderControlReduce(&state, RecorderControlEvent::kAgentReplyReady) ==
                  RecorderControlAction::kNone, "reply must not interrupt recording");
        Check(state.mode == RecorderControlMode::kRecording, "microphone keeps priority");
        RecorderControlReduce(&state, RecorderControlEvent::kTouchStop);
        Check(RecorderControlReduce(&state, RecorderControlEvent::kAgentReplyReady) ==
                  RecorderControlAction::kStartAgentReplyPlayback,
              "queued reply automatically plays after release");
    }
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

void TestVolumeKeysPreserveEveryModeAndScreenState() {
    for (auto mode : {RecorderControlMode::kIdle, RecorderControlMode::kRecording,
                      RecorderControlMode::kPlaying, RecorderControlMode::kPaused}) {
        for (bool screen_on : {false, true}) {
            RecorderControlState state{mode, 70};
            state.screen_on = screen_on;
            state.voice_phase = AgentVoicePhase::kThinking;
            state.voice_turn_pending = true;
            Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalRight) ==
                      RecorderControlAction::kVolumeChanged,
                  "BOOT lowers volume in every mode");
            Check(state.volume == 60, "BOOT decreases volume by ten");
            Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalLeft) ==
                      RecorderControlAction::kVolumeChanged,
                  "KEY raises volume in every mode");
            Check(state.volume == 70 && state.mode == mode &&
                      state.screen_on == screen_on && state.voice_turn_pending &&
                      state.voice_phase == AgentVoicePhase::kThinking,
                  "volume keys preserve microphone, panel and active voice turn");
        }
    }
}

void TestShortRecordingThresholdUsesUnpaddedCapture() {
    for (int sample_rate : {8000, 16000, 24000}) {
        Check(!RecorderRecordingLongEnough(0, sample_rate), "empty gesture is discarded");
        Check(!RecorderRecordingLongEnough(sample_rate / 2, sample_rate),
              "half-second gesture is discarded");
        Check(!RecorderRecordingLongEnough(sample_rate - 1, sample_rate),
              "last sample below one second is discarded despite AMR tail padding");
        Check(RecorderRecordingLongEnough(sample_rate, sample_rate),
              "exactly one second is sent");
        Check(RecorderRecordingLongEnough(sample_rate + 1, sample_rate),
              "longer recording is sent");
    }
    Check(!RecorderRecordingLongEnough(24000, 0), "unknown capture rate cannot send");
    Check(!RecorderRecordingLongEnough(24000, -1), "invalid capture rate cannot send");
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
    TestHoldInterruptsPlaybackAndReplyWaitsForRelease();
    TestRecordingContinuesDuringAgentTurns();
    TestIdlePlayOpensMenuAndSelectionStartsPlayback();
    TestVolumeKeysPreserveEveryModeAndScreenState();
    TestShortRecordingThresholdUsesUnpaddedCapture();
    TestPowerKeyTogglesScreenWithoutChangingAssistantMode();
    TestPauseResumeAndVolumeBounds();
    TestMenuExitIsAvailableInEveryState();
    TestMenuHoldTiming();
    std::puts("recorder_control_state_test passed");
    return 0;
}
