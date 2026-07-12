#include "recorder_history_layout.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void TestConversationRowsExpandAndWrap() {
    const RecorderHistoryRowLayout layout =
        RecorderBuildHistoryRowLayout(true);
    Check(layout.content_height,
          "conversation row height follows its rendered content");
    Check(layout.minimum_height >= 82,
          "short conversation rows retain a useful touch target");
    Check(layout.wrap_detail,
          "long conversation text wraps instead of being clipped");
}

void TestLegacyRowsStayCompact() {
    const RecorderHistoryRowLayout layout =
        RecorderBuildHistoryRowLayout(false);
    Check(!layout.content_height,
          "legacy byte-size rows retain their compact fixed height");
    Check(layout.minimum_height == 82,
          "legacy rows retain the established height");
    Check(!layout.wrap_detail,
          "legacy byte-size detail remains a single line");
}

void TestHistoryRefreshStartsAtNewestRows() {
    Check(RecorderHistoryInitialScrollY() == 0,
          "opening refreshed history resets to its newest rows");
}

}  // namespace

int main() {
    TestConversationRowsExpandAndWrap();
    TestLegacyRowsStayCompact();
    TestHistoryRefreshStartsAtNewestRows();
    std::puts("recorder_history_layout_test passed");
    return 0;
}
