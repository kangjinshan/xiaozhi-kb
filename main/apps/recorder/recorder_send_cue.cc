#include "recorder_send_cue.h"

#include <algorithm>
#include <iterator>
#include <limits>

namespace {
constexpr int kSourceRate = 24000;
constexpr int16_t kPcm[] = {
#include "recorder_send_cue_pcm.inc"
};
}

bool RecorderSendCue::OnAccepted(const std::string& turn_id, bool replay) {
    if (output_rate_ <= 0 || turn_id.empty() || turn_id == last_accepted_) return false;
    last_accepted_ = turn_id;
    // Cached replies and duplicate acknowledgements are not new sends.
    if (replay || pending_count_ == std::numeric_limits<uint32_t>::max()) return false;
    ++pending_count_;
    return true;
}

bool RecorderSendCue::Read(bool microphone_active, std::vector<int16_t>* output) {
    if (output == nullptr) return false;
    output->clear();
    if (microphone_active || !pending()) return false;
    const size_t total = (std::size(kPcm) * static_cast<uint64_t>(output_rate_) +
                          kSourceRate - 1) / kSourceRate;
    const size_t count = std::min<size_t>(std::max(1, output_rate_ / 50), total - offset_);
    output->reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const uint64_t position = (offset_ + index) * static_cast<uint64_t>(kSourceRate);
        const size_t base = position / output_rate_;
        const int64_t fraction = position % output_rate_;
        const int32_t a = kPcm[base];
        const int32_t b = kPcm[std::min(base + 1, std::size(kPcm) - 1)];
        output->push_back(static_cast<int16_t>(a + (b - a) * fraction / output_rate_));
    }
    offset_ += count;
    if (offset_ == total) {
        offset_ = 0;
        --pending_count_;
    }
    return true;
}
