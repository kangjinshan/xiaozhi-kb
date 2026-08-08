#include "mic_controls.h"

#include "mic_recording_utils.h"

namespace {

constexpr int kGainStepPercent = 10;

}  // namespace

MicControlAction MicActionForInput(MicControlInput input) {
    switch (input) {
        case MicControlInput::kPhysicalKey:
            return MicControlAction::kGainUp;
        case MicControlInput::kPhysicalBoot:
            return MicControlAction::kGainDown;
        case MicControlInput::kPhysicalPower:
            return MicControlAction::kToggleScreen;
        case MicControlInput::kTouchMenu:
            return MicControlAction::kExit;
        case MicControlInput::kTouchRecording:
        default:
            return MicControlAction::kToggleRecording;
    }
}

int MicGainAfterAction(int current_percent, MicControlAction action) {
    if (action == MicControlAction::kGainUp) {
        current_percent += kGainStepPercent;
    } else if (action == MicControlAction::kGainDown) {
        current_percent -= kGainStepPercent;
    }
    return MicClampGainPercent(current_percent);
}
