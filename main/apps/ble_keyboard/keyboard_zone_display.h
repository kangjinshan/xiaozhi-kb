#ifndef KEYBOARD_ZONE_DISPLAY_H_
#define KEYBOARD_ZONE_DISPLAY_H_

#include <driver/i2c_master.h>
#include <esp_err.h>

esp_err_t KeyboardZoneDisplayToggle(i2c_master_dev_handle_t pmic);

#endif  // KEYBOARD_ZONE_DISPLAY_H_
