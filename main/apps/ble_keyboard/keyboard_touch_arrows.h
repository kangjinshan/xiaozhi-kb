#ifndef KEYBOARD_TOUCH_ARROWS_H_
#define KEYBOARD_TOUCH_ARROWS_H_

#include "keyboard_touch_action.h"

class BleHidKeyboard;

void StartKeyboardTouchArrows(BleHidKeyboard& keyboard, KeyboardProfile profile);
void ShowKeyboardTouchZoneGuide();

#endif  // KEYBOARD_TOUCH_ARROWS_H_
