#ifndef HID_NOTIFICATION_FLOW_CONTROL_H_
#define HID_NOTIFICATION_FLOW_CONTROL_H_

// Small HID notifications use NimBLE's MSYS_1 pool, while
// os_msys_num_free() reports every registered pool together. Limit the total
// decline from the observed high-water mark to the expendable part of MSYS_1;
// this remains conservative when unrelated pools are temporarily in use.
bool HidNotificationHasBufferHeadroom(int total_free_buffers,
                                      int high_watermark_free_buffers,
                                      int small_pool_capacity,
                                      int small_pool_reserve);

#endif  // HID_NOTIFICATION_FLOW_CONTROL_H_
