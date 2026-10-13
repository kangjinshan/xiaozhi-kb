#include "recorder_display_guard.h"
#include <cassert>
int main() {
    int depth=1; // PWR turned the panel off.
    assert(!RecorderDisplayCanResume(false,depth)); // The old recording-start bug.
    ++depth; // Acquire SD pause.
    assert(RecorderDisplayCanResume(false,depth));--depth;
    assert(depth==1 && !RecorderDisplayCanResume(false,depth));
    assert(RecorderDisplayCanResume(true,depth));--depth; // Only PWR wakes it.
    assert(depth==0 && !RecorderDisplayCanResume(true,depth));
}
