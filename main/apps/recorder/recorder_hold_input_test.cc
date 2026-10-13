#include "recorder_hold_input.h"
#include <cassert>

int main() {
    using A = RecorderHoldAction;
    using S = RecorderHoldSource;
    RecorderHoldInput input;
    assert(input.Set(S::kKey, true) == A::kStart);
    assert(input.Set(S::kTouch, true) == A::kNone);
    assert(input.Set(S::kTouch, false) == A::kNone); // PWR cancels only touch.
    assert(input.Set(S::kKey, true) == A::kNone);   // Repeated down is harmless.
    assert(input.Set(S::kKey, false) == A::kStop);
    assert(input.Set(S::kTouch, false) == A::kNone);
    assert(input.Set(S::kTouch, true) == A::kStart);
    assert(input.Set(S::kTouch, false) == A::kStop);
    RecorderDarkTouch dark;
    dark.Reset(true);
    assert(dark.Poll(true, 10) == A::kNone);
    assert(dark.Poll(true, 1000) == A::kNone); // Ignore a pre-existing hold.
    assert(dark.Poll(false, 1010) == A::kNone);
    assert(dark.Poll(true, 1100) == A::kNone);
    assert(dark.Poll(false, 1200) == A::kNone); // Short dark tap does nothing.
    assert(dark.Poll(true, 2000) == A::kNone);
    assert(dark.Poll(true, 2299) == A::kNone);
    assert(dark.Poll(true, 2300) == A::kStart);
    assert(dark.Poll(true, 3000) == A::kNone);
    assert(dark.Poll(false, 3100) == A::kStop);
    dark.Reset(false);
    dark.Poll(true, 0xfffffff0u);
    assert(dark.Poll(true, 0x11cu) == A::kStart); // Tick wrap.
    assert(dark.Reset(true) == A::kStop);        // Light transition ends dark hold.
    assert(dark.Poll(false, 500) == A::kNone);
    dark.AdoptHold();
    assert(dark.Poll(true, 700) == A::kNone); // PWR hands off an existing touch.
    assert(dark.Poll(false, 800) == A::kStop);
}
