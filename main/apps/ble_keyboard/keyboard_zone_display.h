#ifndef KEYBOARD_ZONE_DISPLAY_H_
#define KEYBOARD_ZONE_DISPLAY_H_

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

esp_err_t KeyboardZoneDisplayPrepareSharedSpiBus();
esp_err_t KeyboardZoneDisplayToggle(i2c_master_dev_handle_t pmic);
esp_err_t KeyboardAirMouseDisplayToggle(i2c_master_dev_handle_t pmic,
                                        SemaphoreHandle_t i2c_mutex = nullptr);
bool KeyboardZoneDisplayInitialized();
bool KeyboardAirMouseDisplayIsOn();
esp_err_t KeyboardZoneDisplayLastError();

#endif  // KEYBOARD_ZONE_DISPLAY_H_
