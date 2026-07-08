#ifndef KEYBOARD_PMIC_POWER_KEY_H_
#define KEYBOARD_PMIC_POWER_KEY_H_

#include <cstdint>

uint8_t Axp2101PowerKeyShortPressMask();
bool IsAxp2101PowerKeyShortPressIrq(uint8_t irq2_status);

#endif  // KEYBOARD_PMIC_POWER_KEY_H_
