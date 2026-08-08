#include "mic_controls.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

}  // namespace

int main() {
    Check(MicActionForInput(MicControlInput::kPhysicalKey) ==
              MicControlAction::kGainUp,
          "KEY maps to microphone gain up");
    Check(MicActionForInput(MicControlInput::kPhysicalBoot) ==
              MicControlAction::kGainDown,
          "BOOT maps to microphone gain down");
    Check(MicActionForInput(MicControlInput::kPhysicalPower) ==
              MicControlAction::kToggleScreen,
          "PWR maps to screen toggle");
    Check(MicActionForInput(MicControlInput::kTouchRecording) ==
              MicControlAction::kToggleRecording,
          "record button maps to optional WAV toggle");
    Check(MicActionForInput(MicControlInput::kTouchMenu) ==
              MicControlAction::kExit,
          "MENU maps to selector exit");

    Check(MicGainAfterAction(70, MicControlAction::kGainUp) == 80,
          "KEY raises both-mic gain by ten percent");
    Check(MicGainAfterAction(70, MicControlAction::kGainDown) == 60,
          "BOOT lowers both-mic gain by ten percent");
    Check(MicGainAfterAction(100, MicControlAction::kGainUp) == 100,
          "gain up clamps at one hundred percent");
    Check(MicGainAfterAction(0, MicControlAction::kGainDown) == 0,
          "gain down clamps at zero percent");
    Check(MicGainAfterAction(70, MicControlAction::kToggleScreen) == 70,
          "non-gain actions preserve microphone gain");
    return 0;
}
