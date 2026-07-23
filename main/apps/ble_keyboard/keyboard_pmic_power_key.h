#ifndef KEYBOARD_PMIC_POWER_KEY_H_
#define KEYBOARD_PMIC_POWER_KEY_H_

#include "keyboard_touch_action.h"

#include <cstdint>

uint8_t Axp2101PowerKeyShortPressMask();
uint8_t Axp2101PowerKeyIrqEnableMask();
uint8_t Axp2101PowerKeyToggleEventMask();
bool IsAxp2101PowerKeyShortPressIrq(uint8_t irq2_status);
bool IsAxp2101PowerKeyToggleEvent(uint8_t irq2_status);
bool PowerKeyShortPressTogglesDisplay(KeyboardProfile profile);
uint8_t PowerKeyShortPressHidKey(KeyboardProfile profile);

#endif  // KEYBOARD_PMIC_POWER_KEY_H_
