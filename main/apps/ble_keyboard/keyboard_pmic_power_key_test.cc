#include "keyboard_pmic_power_key.h"

#include <cstdio>
#include <cstdlib>

static void ExpectBool(const char* name, bool actual, bool expected) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "%s: expected %s, got %s\n",
                     name,
                     expected ? "true" : "false",
                     actual ? "true" : "false");
        std::exit(1);
    }
}

int main() {
    ExpectBool("short press bit is detected",
               IsAxp2101PowerKeyShortPressIrq(0x08),
               true);
    ExpectBool("short press bit is detected with other flags",
               IsAxp2101PowerKeyShortPressIrq(0x8F),
               true);
    ExpectBool("positive edge alone is ignored",
               IsAxp2101PowerKeyShortPressIrq(0x01),
               false);
    ExpectBool("negative edge alone is ignored",
               IsAxp2101PowerKeyShortPressIrq(0x02),
               false);
    ExpectBool("long press alone is ignored",
               IsAxp2101PowerKeyShortPressIrq(0x04),
               false);
    ExpectBool("no IRQ is ignored",
               IsAxp2101PowerKeyShortPressIrq(0x00),
               false);

    if (Axp2101PowerKeyShortPressMask() != 0x08) {
        std::fprintf(stderr,
                     "short press mask: expected 0x08, got 0x%02x\n",
                     Axp2101PowerKeyShortPressMask());
        return 1;
    }

    return 0;
}
