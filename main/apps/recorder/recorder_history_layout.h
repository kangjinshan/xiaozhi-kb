#ifndef RECORDER_HISTORY_LAYOUT_H_
#define RECORDER_HISTORY_LAYOUT_H_

struct RecorderHistoryRowLayout {
    bool content_height = false;
    int minimum_height = 82;
    bool wrap_detail = false;
};

RecorderHistoryRowLayout RecorderBuildHistoryRowLayout(
    bool conversation_detail);
int RecorderHistoryInitialScrollY();

#endif  // RECORDER_HISTORY_LAYOUT_H_
