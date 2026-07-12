#include "recorder_turn_clock.h"

#include <cstdio>
#include <ctime>
#include <limits>

namespace {

constexpr uint64_t kMinimumServerTimeMs = 1577836800000ULL;
constexpr uint64_t kMaximumServerTimeMs = 4102444800000ULL;
constexpr int32_t kMinimumTimezoneOffsetMinutes = -720;
constexpr int32_t kMaximumTimezoneOffsetMinutes = 840;

std::string FormatDate(uint64_t epoch_ms, int32_t offset_minutes) {
    const int64_t local_ms = static_cast<int64_t>(epoch_ms) +
        static_cast<int64_t>(offset_minutes) * 60LL * 1000LL;
    if (local_ms < 0) {
        return "unsynced";
    }
    const time_t seconds = static_cast<time_t>(local_ms / 1000LL);
    struct tm local = {};
    if (gmtime_r(&seconds, &local) == nullptr) {
        return "unsynced";
    }
    char date[9];
    if (std::strftime(date, sizeof(date), "%Y%m%d", &local) != 8) {
        return "unsynced";
    }
    return date;
}

std::string FormatTurnId(bool synced,
                         uint64_t created_at_ms,
                         uint32_t random) {
    char turn_id[40];
    const int length = std::snprintf(
        turn_id,
        sizeof(turn_id),
        "turn-%c-%08lx%08lx-%08lx",
        synced ? 's' : 'u',
        static_cast<unsigned long>(created_at_ms >> 32),
        static_cast<unsigned long>(created_at_ms & 0xffffffffULL),
        static_cast<unsigned long>(random));
    return length > 0 && static_cast<size_t>(length) < sizeof(turn_id)
        ? std::string(turn_id)
        : std::string();
}

}  // namespace

void RecorderTurnClock::Sync(uint64_t server_time_ms,
                             int32_t timezone_offset_minutes,
                             uint64_t monotonic_ms) {
    if (server_time_ms < kMinimumServerTimeMs ||
        server_time_ms > kMaximumServerTimeMs ||
        timezone_offset_minutes < kMinimumTimezoneOffsetMinutes ||
        timezone_offset_minutes > kMaximumTimezoneOffsetMinutes) {
        synced_ = false;
        return;
    }
    synced_ = true;
    server_time_ms_ = server_time_ms;
    monotonic_base_ms_ = monotonic_ms;
    timezone_offset_minutes_ = timezone_offset_minutes;
}

RecorderTurnStamp RecorderTurnClock::MakeStamp(uint64_t monotonic_ms,
                                                uint32_t random) const {
    RecorderTurnStamp stamp;
    stamp.synced = synced_;
    if (synced_) {
        const uint64_t elapsed_ms = monotonic_ms >= monotonic_base_ms_
            ? monotonic_ms - monotonic_base_ms_
            : 0;
        stamp.created_at_ms =
            elapsed_ms > std::numeric_limits<uint64_t>::max() - server_time_ms_
                ? std::numeric_limits<uint64_t>::max()
                : server_time_ms_ + elapsed_ms;
        stamp.date = FormatDate(
            stamp.created_at_ms, timezone_offset_minutes_);
    } else {
        stamp.created_at_ms = monotonic_ms;
        stamp.date = "unsynced";
    }
    stamp.turn_id = FormatTurnId(stamp.synced, stamp.created_at_ms, random);
    return stamp;
}
