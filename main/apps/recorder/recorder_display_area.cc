#include "recorder_display_area.h"

void RecorderRoundDisplayArea(RecorderDisplayArea* area) {
    if (area == nullptr) {
        return;
    }
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}
