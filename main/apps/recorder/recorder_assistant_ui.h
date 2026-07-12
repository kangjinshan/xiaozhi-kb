#ifndef RECORDER_ASSISTANT_UI_H_
#define RECORDER_ASSISTANT_UI_H_

#include "recorder_control_state.h"

#include <cstdint>
#include <string>

enum class RecorderAssistantPrimaryAction {
    kNone,
    kTalk,
    kSend,
    kPause,
    kResume,
};

enum class RecorderAssistantNotice {
    kNone,
    kWifiSetup,
    kAgentSetup,
    kDspFailure,
    kSaveFailure,
    kPlaybackFailure,
};

struct RecorderAssistantUiInput {
    RecorderControlMode mode = RecorderControlMode::kIdle;
    AgentVoicePhase voice_phase = AgentVoicePhase::kOffline;
    bool turn_pending = false;
    bool sd_ready = false;
    bool noise_reduction_ready = false;
    RecorderAssistantNotice notice = RecorderAssistantNotice::kNone;
    uint32_t elapsed_seconds = 0;
    int volume = 70;
};

struct RecorderAssistantUiModel {
    std::string connection_label;
    std::string title;
    std::string subtitle;
    std::string metric;
    std::string primary_label;
    uint32_t accent_rgb = 0x58D6FF;
    uint32_t connection_rgb = 0x58D6FF;
    RecorderAssistantPrimaryAction primary_action =
        RecorderAssistantPrimaryAction::kNone;
    bool primary_enabled = false;
    bool history_visible = false;
};

struct RecorderAssistantRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct RecorderAssistantLayout {
    RecorderAssistantRect menu;
    RecorderAssistantRect brand;
    RecorderAssistantRect connection;
    RecorderAssistantRect orb;
    RecorderAssistantRect title;
    RecorderAssistantRect subtitle;
    RecorderAssistantRect metric;
    RecorderAssistantRect primary;
    RecorderAssistantRect history;
};

RecorderAssistantUiModel RecorderBuildAssistantUi(
    const RecorderAssistantUiInput& input);
RecorderAssistantLayout RecorderBuildAssistantLayout(int width, int height);

#endif  // RECORDER_ASSISTANT_UI_H_
