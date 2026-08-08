#include "mic_stereo_processor.h"

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

void Offset(void* context, int16_t* input, int16_t* output) {
    const int offset = *static_cast<int*>(context);
    for (int index = 0; index < 160; ++index) {
        output[index] = static_cast<int16_t>(input[index] + offset);
    }
}

void Half(void*, int16_t* input, int16_t* output) {
    for (int index = 0; index < 160; ++index) {
        output[index] = static_cast<int16_t>(input[index] / 2);
    }
}

}  // namespace

int main() {
    int mid_offset = 10;
    MicStereoProcessor processor(16000, Offset, &mid_offset);
    Check(processor.valid(), "processor valid");
    Check(processor.noise_reduction_enabled(), "shared-mid NS enabled");

    std::vector<int16_t> input;
    for (int frame = 0; frame < 200; ++frame) {
        input.push_back(static_cast<int16_t>(frame));
        input.push_back(static_cast<int16_t>(1000 + frame));
    }
    std::vector<int16_t> output;
    Check(processor.Process(input, &output), "stereo process succeeds");
    Check(processor.Flush(&output), "stereo flush succeeds");
    Check(output.size() == input.size(), "sample count preserved at 16 kHz");
    for (size_t index = 0; index < output.size(); index += 2) {
        Check(output[index] == input[index] + 10, "left channel stays left");
        Check(output[index + 1] == input[index + 1] + 10,
              "right channel stays right");
        Check(output[index + 1] - output[index] ==
                  input[index + 1] - input[index],
              "shared NS preserves stereo side signal");
    }

    Check(processor.Reset(), "reset succeeds");
    std::vector<int16_t> odd = {1, 2, 3};
    output.clear();
    Check(!processor.Process(odd, &output), "odd interleaved input rejected");

    MicStereoProcessor realtime_processor(16000, Half, nullptr, true);
    std::vector<int16_t> realtime_input;
    for (int frame = 0; frame < 320; ++frame) {
        realtime_input.push_back(100);
        realtime_input.push_back(300);
    }
    output.clear();
    Check(realtime_processor.Process(realtime_input, &output),
          "C6 realtime NS process succeeds");
    Check(output.size() == realtime_input.size(),
          "C6 realtime mode emits both 10 ms halves");
    for (size_t index = 0; index < output.size(); index += 2) {
        Check(output[index] == 0 && output[index + 1] == 200,
              "held ESP-SR gain covers alternate 10 ms half");
        Check(output[index + 1] - output[index] == 200,
              "C6 realtime mode preserves stereo side signal");
    }

    MicStereoProcessor resampling_processor(24000);
    std::vector<int16_t> resampling_input;
    for (int frame = 0; frame < 960; ++frame) {
        resampling_input.push_back(123);
        resampling_input.push_back(2345);
    }
    output.clear();
    Check(resampling_processor.Process(resampling_input, &output),
          "24 kHz stereo process succeeds");
    Check(resampling_processor.Flush(&output), "24 kHz stereo flush succeeds");
    Check(output.size() == 640 * 2, "24 kHz converts to 640 stereo frames");
    for (size_t index = 0; index < output.size(); index += 2) {
        Check(output[index] == 123, "resampling keeps MIC1 on left");
        Check(output[index + 1] == 2345, "resampling keeps MIC2 on right");
    }
    return 0;
}
