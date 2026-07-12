#ifndef RECORDER_TURN_CLOCK_H_
#define RECORDER_TURN_CLOCK_H_

#include <cstdint>
#include <string>

struct RecorderTurnStamp {
    std::string date;
    std::string turn_id;
    uint64_t created_at_ms = 0;
    bool synced = false;
};

class RecorderTurnClock {
public:
    void Sync(uint64_t server_time_ms,
              int32_t timezone_offset_minutes,
              uint64_t monotonic_ms);
    RecorderTurnStamp MakeStamp(uint64_t monotonic_ms,
                                uint32_t random) const;

private:
    bool synced_ = false;
    uint64_t server_time_ms_ = 0;
    uint64_t monotonic_base_ms_ = 0;
    int32_t timezone_offset_minutes_ = 0;
};

#endif  // RECORDER_TURN_CLOCK_H_
