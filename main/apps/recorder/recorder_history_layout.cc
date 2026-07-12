#include "recorder_history_layout.h"

RecorderHistoryRowLayout RecorderBuildHistoryRowLayout(
        bool conversation_detail) {
    return {
        .content_height = conversation_detail,
        .minimum_height = 82,
        .wrap_detail = conversation_detail,
    };
}

int RecorderHistoryInitialScrollY() {
    return 0;
}
