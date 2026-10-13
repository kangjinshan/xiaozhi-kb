#ifndef RECORDER_DISPLAY_GUARD_H_
#define RECORDER_DISPLAY_GUARD_H_
// A dark panel owns one persistent pause. Only an explicit screen-on action may
// release that last pause; SD preparation/completion must never wake LVGL.
inline bool RecorderDisplayCanResume(bool screen_on, int pause_depth) {
    return pause_depth > 0 && (screen_on || pause_depth > 1);
}
#endif
