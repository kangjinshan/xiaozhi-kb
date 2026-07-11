#ifndef RECORDER_DISPLAY_AREA_H_
#define RECORDER_DISPLAY_AREA_H_

#include <cstdint>

struct RecorderDisplayArea {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
};

// SH8601 QSPI transfers require even start coordinates and odd end coordinates.
void RecorderRoundDisplayArea(RecorderDisplayArea* area);

#endif  // RECORDER_DISPLAY_AREA_H_
