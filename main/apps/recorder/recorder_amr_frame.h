#ifndef RECORDER_AMR_FRAME_H_
#define RECORDER_AMR_FRAME_H_
#include <cstddef>
#include <cstdint>

inline size_t RecorderAmrFrameSize(uint8_t toc) {
    constexpr uint8_t sizes[] = {13,14,16,18,20,21,27,32,6,0,0,0,0,0,0,1};
    if (toc & 0x83) return 0; // Stored AMR frames have no continuation or pad bits.
    return sizes[(toc >> 3) & 15];
}
#endif
