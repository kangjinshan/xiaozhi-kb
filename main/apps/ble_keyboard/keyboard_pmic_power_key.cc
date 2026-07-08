#include "keyboard_pmic_power_key.h"

namespace {

constexpr uint8_t kAxp2101PowerKeyShortPressMask = 0x08;

}  // namespace

uint8_t Axp2101PowerKeyShortPressMask() {
    return kAxp2101PowerKeyShortPressMask;
}

bool IsAxp2101PowerKeyShortPressIrq(uint8_t irq2_status) {
    return (irq2_status & kAxp2101PowerKeyShortPressMask) != 0;
}
