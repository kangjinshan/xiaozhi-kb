#include "recorder_rate_converter.h"

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

std::vector<int16_t> ConvertInChunks(size_t input_samples,
                                     int source_rate,
                                     int destination_rate,
                                     size_t chunk_size) {
    RecorderRateConverter converter(source_rate, destination_rate);
    Check(converter.valid(), "converter initializes");
    std::vector<int16_t> input(input_samples);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<int16_t>(static_cast<int>(i % 2000) - 1000);
    }

    std::vector<int16_t> output;
    for (size_t offset = 0; offset < input.size(); offset += chunk_size) {
        const size_t count = std::min(chunk_size, input.size() - offset);
        std::vector<int16_t> block(input.begin() + offset, input.begin() + offset + count);
        Check(converter.Process(block, &output), "chunk converts");
    }
    Check(converter.Flush(&output), "converter flushes");
    return output;
}

}  // namespace

int main() {
    const auto down_single = ConvertInChunks(2400, 24000, 16000, 2400);
    const auto down_chunked = ConvertInChunks(2400, 24000, 16000, 317);
    Check(down_single.size() == 1600, "24k to 16k count");
    Check(down_chunked.size() == 1600, "chunked downsample count");
    Check(down_single == down_chunked, "downsample is chunk invariant");

    const auto up = ConvertInChunks(1600, 16000, 24000, 173);
    Check(up.size() == 2400, "16k to 24k count");

    RecorderRateConverter passthrough(24000, 24000);
    std::vector<int16_t> input = {1, -2, 3, -4};
    std::vector<int16_t> output;
    Check(passthrough.Process(input, &output), "passthrough converts");
    Check(passthrough.Flush(&output), "passthrough flushes");
    Check(output == input, "same-rate samples are unchanged");

    RecorderRateConverter invalid(0, 16000);
    Check(!invalid.valid(), "invalid rate is rejected");
    std::puts("recorder_rate_converter_test passed");
    return 0;
}
