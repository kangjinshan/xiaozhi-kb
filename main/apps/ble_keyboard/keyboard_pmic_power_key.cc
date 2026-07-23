#include "keyboard_pmic_power_key.h"

#include "ble_hid_keyboard.h"

namespace {

constexpr uint8_t kAxp2101PowerKeyShortPressMask = 0x08;
constexpr uint8_t kAxp2101PowerKeyPositiveEdgeMask = 0x01;
constexpr uint8_t kAxp2101PowerKeyNegativeEdgeMask = 0x02;

}  // namespace

uint8_t Axp2101PowerKeyShortPressMask() {
    return kAxp2101PowerKeyShortPressMask;
}

uint8_t Axp2101PowerKeyIrqEnableMask() {
    return kAxp2101PowerKeyShortPressMask |
           kAxp2101PowerKeyPositiveEdgeMask |
           kAxp2101PowerKeyNegativeEdgeMask;
}

uint8_t Axp2101PowerKeyToggleEventMask() {
    return kAxp2101PowerKeyShortPressMask |
           kAxp2101PowerKeyPositiveEdgeMask;
}

bool IsAxp2101PowerKeyShortPressIrq(uint8_t irq2_status) {
    return (irq2_status & kAxp2101PowerKeyShortPressMask) != 0;
}

bool IsAxp2101PowerKeyToggleEvent(uint8_t irq2_status) {
    return (irq2_status & Axp2101PowerKeyToggleEventMask()) != 0;
}

bool PowerKeyShortPressTogglesDisplay(KeyboardProfile profile) {
    return profile == KeyboardProfile::kProfile2;
}

uint8_t PowerKeyShortPressHidKey(KeyboardProfile profile) {
    if (PowerKeyShortPressTogglesDisplay(profile)) {
        return 0;
    }
    return HID_KEY_BACKSPACE;
}
