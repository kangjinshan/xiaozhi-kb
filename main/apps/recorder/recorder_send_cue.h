#ifndef RECORDER_SEND_CUE_H_
#define RECORDER_SEND_CUE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Main-task-only, bounded 20 ms output blocks. It never touches SD or the codec.
class RecorderSendCue {
public:
    explicit RecorderSendCue(int output_rate) : output_rate_(output_rate) {}
    bool OnAccepted(const std::string& turn_id, bool replay);
    bool Read(bool microphone_active, std::vector<int16_t>* output);
    bool pending() const { return pending_count_ != 0; }
    bool at_start() const { return pending() && offset_ == 0; }

private:
    int output_rate_;
    uint32_t pending_count_ = 0;
    size_t offset_ = 0;
    std::string last_accepted_;
};

#endif
