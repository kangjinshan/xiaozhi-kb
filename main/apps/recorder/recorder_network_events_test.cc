#include "recorder_network.h"

#include <cassert>

int main() {
    RecorderReconnectPolicy policy;
    const uint32_t first = policy.NextDelayMs();
    const uint32_t second = policy.NextDelayMs();
    assert(first >= 1000 && first < 1200);
    assert(second >= 2000 && second < 2400);

    for (int index = 0; index < 10; ++index) {
        policy.NextDelayMs();
    }
    assert(policy.PeekBaseDelayMs() == 30000);

    policy.Reset();
    assert(policy.PeekBaseDelayMs() == 1000);
    assert(policy.NextDelayMs() >= 1000);
    return 0;
}
