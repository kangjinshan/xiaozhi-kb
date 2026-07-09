#include "keyboard_zone_display_state.h"

#include <cstdio>

int main() {
    if (!NextKeyboardZoneGuideVisible(false)) {
        std::fprintf(stderr, "hidden guide should become visible\n");
        return 1;
    }
    if (NextKeyboardZoneGuideVisible(true)) {
        std::fprintf(stderr, "visible guide should become hidden\n");
        return 1;
    }
    return 0;
}
