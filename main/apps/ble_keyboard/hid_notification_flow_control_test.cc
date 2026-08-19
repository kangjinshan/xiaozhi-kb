#include "hid_notification_flow_control.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Expect(const char* name, bool actual, bool expected) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "%s: expected %s, got %s\n",
                     name,
                     expected ? "true" : "false",
                     actual ? "true" : "false");
        std::exit(1);
    }
}

}  // namespace

int main() {
    // The target currently reports 84 free buffers across every registered
    // pool. MSYS_1 has 24 blocks; keeping eight in reserve permits a total
    // decline of 16, so 68 is the last accepted value.
    Expect("healthy pool",
           HidNotificationHasBufferHeadroom(84, 84, 24, 8),
           true);
    Expect("reserve boundary",
           HidNotificationHasBufferHeadroom(68, 84, 24, 8),
           true);
    Expect("below reserve",
           HidNotificationHasBufferHeadroom(67, 84, 24, 8),
           false);
    Expect("other pools cannot hide exhaustion",
           HidNotificationHasBufferHeadroom(60, 84, 24, 8),
           false);
    Expect("invalid sample",
           HidNotificationHasBufferHeadroom(-1, 84, 24, 8),
           false);
    Expect("invalid reserve",
           HidNotificationHasBufferHeadroom(84, 84, 4, 8),
           false);
    return 0;
}
