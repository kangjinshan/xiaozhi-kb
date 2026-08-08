#ifndef MIC_CONTROLS_H_
#define MIC_CONTROLS_H_

enum class MicControlInput {
    kTouchRecording,
    kPhysicalKey,
    kPhysicalBoot,
    kPhysicalPower,
    kTouchMenu,
};

enum class MicControlAction {
    kToggleRecording,
    kGainUp,
    kGainDown,
    kToggleScreen,
    kExit,
};

// Keep all user-facing Mic controls in one pure mapping so host tests exercise
// the same actions that the hardware and LVGL callbacks publish at runtime.
MicControlAction MicActionForInput(MicControlInput input);

// Apply the fixed 10% KEY/BOOT step and clamp it to the codec UI range.
int MicGainAfterAction(int current_percent, MicControlAction action);

#endif  // MIC_CONTROLS_H_
