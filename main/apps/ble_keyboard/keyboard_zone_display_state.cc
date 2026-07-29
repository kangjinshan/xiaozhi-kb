#include "keyboard_zone_display_state.h"

bool NextKeyboardZoneGuideVisible(bool current_visible) {
    return !current_visible;
}

bool IsKeyboardBatteryLevelValid(int level) {
    return level >= 0 && level <= 100;
}
