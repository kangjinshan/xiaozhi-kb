#include "recorder_turn_clock.h"

#include <cstdlib>
#include <iostream>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

}  // namespace

int main() {
    RecorderTurnClock clock;
    const RecorderTurnStamp offline = clock.MakeStamp(1234, 0x12abcdef);
    Check(!offline.synced, "offline stamp reports unsynced");
    Check(offline.date == "unsynced", "offline date is explicit");
    Check(offline.created_at_ms == 1234, "offline time uses monotonic clock");
    Check(offline.turn_id == "turn-u-00000000000004d2-12abcdef",
          "offline id uses fixed-width halves");

    clock.Sync(1783857600123ULL, 480, 5000);
    const RecorderTurnStamp online = clock.MakeStamp(6000, 0x89abcdef);
    Check(online.synced, "online stamp reports synced");
    Check(online.date == "20260712", "server offset produces local date");
    Check(online.created_at_ms == 1783857601123ULL,
          "server time advances with monotonic elapsed time");
    Check(online.turn_id == "turn-s-0000019f5632da63-89abcdef",
          "online id uses exact fixed-width epoch");

    const RecorderTurnStamp before_base = clock.MakeStamp(4000, 1);
    Check(before_base.created_at_ms == 1783857600123ULL,
          "backward monotonic input cannot move server time backward");

    clock.Sync(1783871999000ULL, 480, 100);
    Check(clock.MakeStamp(2100, 2).date == "20260713",
          "local date crosses midnight using elapsed monotonic time");

    RecorderTurnClock invalid;
    invalid.Sync(0, 900, 10);
    Check(!invalid.MakeStamp(20, 3).synced,
          "invalid server clock leaves offline fallback active");
    invalid.Sync(4102444800001ULL, 0, 10);
    Check(!invalid.MakeStamp(20, 4).synced,
          "server clock beyond year 2100 remains unsynced");

    std::cout << "recorder_turn_clock_test passed" << std::endl;
    return 0;
}
