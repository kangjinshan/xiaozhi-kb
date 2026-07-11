#include "recorder_noise_reducer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct FakeNs {
    int calls = 0;

    static void Process(void* context, int16_t* input, int16_t* output) {
        auto* self = static_cast<FakeNs*>(context);
        ++self->calls;
        for (size_t i = 0; i < 160; ++i) {
            output[i] = static_cast<int16_t>(input[i] / 2);
        }
    }
};

void Passthrough(void*, int16_t* input, int16_t* output) {
    std::copy(input, input + 160, output);
}

void TestEveryFrameUsesNoiseSuppressor() {
    FakeNs fake;
    RecorderNoiseReducer reducer(24000, FakeNs::Process, &fake);
    std::vector<int16_t> input(2400, 100);
    std::vector<int16_t> output;
    Check(reducer.Process(input, &output), "process succeeds");
    Check(reducer.Flush(&output), "flush succeeds");
    Check(output.size() == 1600, "24k input becomes 16k output");
    Check(fake.calls == 10, "all ten 10ms frames use NS");
    Check(std::all_of(output.begin(), output.end(),
                      [](int16_t sample) { return sample == 300; }),
          "NS output receives fixed post gain");
}

void TestChunkedTailKeepsExactDuration() {
    FakeNs fake;
    RecorderNoiseReducer reducer(24000, FakeNs::Process, &fake);
    std::vector<int16_t> output;
    Check(reducer.Process(std::vector<int16_t>(123, 100), &output),
          "first uneven block processes");
    Check(reducer.Process(std::vector<int16_t>(456, 100), &output),
          "second uneven block processes");
    Check(reducer.Flush(&output), "uneven stream flushes");
    Check(output.size() == 579 * 16000 / 24000,
          "tail preserves converted duration");
    Check(fake.calls == 3, "partial tail is zero-padded through NS");
}

void TestLimiterCapsPostGain() {
    RecorderNoiseReducer reducer(24000, Passthrough, nullptr);
    std::vector<int16_t> output;
    Check(reducer.Process(std::vector<int16_t>(240, 6000), &output),
          "loud frame processes");
    Check(reducer.Flush(&output), "loud frame flushes");
    Check(output.size() == 160, "one output frame produced");
    Check(std::all_of(output.begin(), output.end(),
                      [](int16_t sample) { return sample == 30000; }),
          "post gain is limited to 30000");
}

void TestNoNsFallbackDoesNotUseDeletedGate() {
    RecorderNoiseReducer reducer(24000);
    Check(!reducer.noise_reduction_enabled(), "host default has no NS backend");
    std::vector<int16_t> output;
    Check(reducer.Process(std::vector<int16_t>(240, 20), &output),
          "fallback frame processes");
    Check(reducer.Flush(&output), "fallback frame flushes");
    Check(std::all_of(output.begin(), output.end(),
                      [](int16_t sample) { return sample == 120; }),
          "small samples are gained, not hard-gated to zero");
}

void TestResetStartsASecondStream() {
    FakeNs fake;
    RecorderNoiseReducer reducer(24000, FakeNs::Process, &fake);
    std::vector<int16_t> first;
    Check(reducer.Process(std::vector<int16_t>(240, 100), &first),
          "first stream processes");
    Check(reducer.Flush(&first), "first stream flushes");
    Check(reducer.Reset(), "reducer resets");

    std::vector<int16_t> second;
    Check(reducer.Process(std::vector<int16_t>(240, 200), &second),
          "second stream processes");
    Check(reducer.Flush(&second), "second stream flushes");
    Check(first.size() == 160 && second.size() == 160,
          "both streams keep independent durations");
    Check(first.front() == 300 && second.front() == 600,
          "second stream does not retain first stream samples");
}

}  // namespace

int main() {
    TestEveryFrameUsesNoiseSuppressor();
    TestChunkedTailKeepsExactDuration();
    TestLimiterCapsPostGain();
    TestNoNsFallbackDoesNotUseDeletedGate();
    TestResetStartsASecondStream();
    std::puts("recorder_noise_reducer_test passed");
    return 0;
}
