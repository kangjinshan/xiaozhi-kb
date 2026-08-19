#include "hid_notification_flow_control.h"

#include <cstdint>
#include <limits>

bool HidNotificationHasBufferHeadroom(int total_free_buffers,
                                      int high_watermark_free_buffers,
                                      int small_pool_capacity,
                                      int small_pool_reserve) {
    if (total_free_buffers < 0 || high_watermark_free_buffers < 0 ||
        small_pool_capacity < 0 || small_pool_reserve < 0 ||
        small_pool_reserve > small_pool_capacity) {
        return false;
    }

    const int64_t expendable =
        static_cast<int64_t>(small_pool_capacity) - small_pool_reserve;
    const int64_t minimum_total =
        static_cast<int64_t>(high_watermark_free_buffers) - expendable;
    return minimum_total <= std::numeric_limits<int>::max() &&
           total_free_buffers >= minimum_total;
}
