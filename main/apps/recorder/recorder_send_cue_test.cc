#include "recorder_send_cue.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

static std::vector<int16_t> Drain(RecorderSendCue& cue) {
    std::vector<int16_t> all, block;
    while (cue.Read(false, &block)) {
        assert(!block.empty() && block.size() <= 480);
        all.insert(all.end(), block.begin(), block.end());
    }
    return all;
}

int main() {
    RecorderSendCue baseline(24000);
    std::vector<int16_t> block;
    assert(!baseline.Read(false, &block));  // Nothing before an accepted upload.
    assert(!baseline.OnAccepted("old", true));
    assert(!baseline.pending());
    assert(baseline.OnAccepted("new", false));
    assert(!baseline.OnAccepted("new", false));
    const auto reference = Drain(baseline);
    assert(reference.size() == 5760);
    assert(reference.front() == 0 && reference.back() == 0);
    assert(std::all_of(reference.end() - 1920, reference.end(), [](auto x) { return x == 0; }));
    assert(std::any_of(reference.begin(), reference.end(), [](auto x) { return std::abs(x) > 5000; }));
    assert(std::all_of(reference.begin(), reference.end(), [](auto x) { return std::abs(x) <= 11000; }));

    RecorderSendCue interrupted(24000);
    assert(interrupted.OnAccepted("one", false));
    for (int i = 0; i < 20; ++i) assert(!interrupted.Read(true, &block) && block.empty());
    assert(interrupted.at_start());
    assert(interrupted.Read(false, &block));
    auto actual = block;
    assert(!interrupted.Read(true, &block) && block.empty());
    const auto tail = Drain(interrupted);
    actual.insert(actual.end(), tail.begin(), tail.end());
    assert(actual == reference);  // Recording suspends without advancing or losing audio.
    assert(!interrupted.OnAccepted("one", true));

    assert(interrupted.OnAccepted("two", false));
    assert(interrupted.OnAccepted("three", false));
    const auto queued = Drain(interrupted);
    assert(queued.size() == 2 * reference.size());
    assert(std::equal(reference.begin(), reference.end(), queued.begin()));
    assert(std::equal(reference.begin(), reference.end(), queued.begin() + reference.size()));

    RecorderSendCue at16k(16000);
    assert(at16k.OnAccepted("different-rate", false));
    assert(Drain(at16k).size() == 3840);
    RecorderSendCue invalid(0);
    assert(!invalid.OnAccepted("invalid", false));
}
