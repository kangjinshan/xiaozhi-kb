#include "recorder_assistant_ui.h"

#include <algorithm>
#include <cstdio>

namespace {

constexpr uint32_t kCyan = 0xBFF4D4;
constexpr uint32_t kCoral = 0xFF9E9E;
constexpr uint32_t kViolet = 0x9D7CFF;
constexpr uint32_t kMint = 0x55E6A5;
constexpr uint32_t kAmber = 0xF5B94C;

void SetConnection(const RecorderAssistantUiInput& input,
                   RecorderAssistantUiModel* model) {
    if (model == nullptr) {
        return;
    }
    switch (input.voice_phase) {
        case AgentVoicePhase::kOffline:
            model->connection_label = "无网络";
            model->connection_rgb = kAmber;
            break;
        case AgentVoicePhase::kConnecting:
            model->connection_label = "连接中";
            model->connection_rgb = kViolet;
            break;
        case AgentVoicePhase::kError:
            model->connection_label = "重试中";
            model->connection_rgb = kAmber;
            break;
        default:
            model->connection_label = "已连接";
            model->connection_rgb = kMint;
            break;
    }
}

void SetTalkAction(RecorderAssistantUiModel* model) {
    model->primary_label = "按住说话";
    model->primary_action = RecorderAssistantPrimaryAction::kTalk;
    model->primary_enabled = true;
}

void SetDisabled(RecorderAssistantUiModel* model) {
    model->primary_label = "请稍等";
    model->primary_action = RecorderAssistantPrimaryAction::kNone;
    model->primary_enabled = false;
}

bool IsBusyPhase(AgentVoicePhase phase) {
    return phase == AgentVoicePhase::kSending ||
           phase == AgentVoicePhase::kThinking ||
           phase == AgentVoicePhase::kReceiving ||
           phase == AgentVoicePhase::kReadyToPlay ||
           phase == AgentVoicePhase::kPlaying;
}

}  // namespace

RecorderAssistantUiModel RecorderBuildAssistantUi(
    const RecorderAssistantUiInput& input) {
    RecorderAssistantUiModel model;
    SetConnection(input, &model);

    if (!input.sd_ready) {
        model.title = "请插入存储卡";
        model.subtitle = "存储卡用于保存每次对话";
        model.accent_rgb = kAmber;
        SetDisabled(&model);
        return model;
    }

    if (input.notice != RecorderAssistantNotice::kNone &&
        input.mode != RecorderControlMode::kRecording) {
        model.accent_rgb = kAmber;
        SetDisabled(&model);
        switch (input.notice) {
            case RecorderAssistantNotice::kWifiSetup:
                model.title = "请设置网络";
                model.subtitle = "先进入小智 AI 完成设置";
                break;
            case RecorderAssistantNotice::kAgentSetup:
                model.title = "请设置助手";
                model.subtitle = "设备尚未绑定";
                break;
            case RecorderAssistantNotice::kDspFailure:
                model.title = "录音不可用";
                model.subtitle = "请稍后重试";
                break;
            case RecorderAssistantNotice::kSaveFailure:
                model.title = "保存失败";
                model.subtitle = "请检查存储卡";
                break;
            case RecorderAssistantNotice::kPlaybackFailure:
                model.title = "播放失败";
                model.subtitle = "请检查录音文件";
                break;
            case RecorderAssistantNotice::kSpeechNotRecognized:
                model.title = "没有听清";
                model.subtitle = "请再说一次";
                SetTalkAction(&model);
                break;
            case RecorderAssistantNotice::kNone:
                break;
        }
        return model;
    }

    if (input.mode == RecorderControlMode::kRecording) {
        char timer[16];
        std::snprintf(timer, sizeof(timer), "%02u:%02u",
                      static_cast<unsigned>(input.elapsed_seconds / 60),
                      static_cast<unsigned>(input.elapsed_seconds % 60));
        model.title = "正在录音";
        model.subtitle = "说完松手即可";
        model.metric = timer;
        model.primary_label = "松开发送";
        model.primary_action = RecorderAssistantPrimaryAction::kSend;
        model.primary_enabled = true;
        model.accent_rgb = kCoral;
        return model;
    }

    if (input.mode == RecorderControlMode::kPlaying ||
        input.mode == RecorderControlMode::kPaused) {
        model.title = "神勇正在回答";
        SetTalkAction(&model);
        model.subtitle = "按住可继续说话";
        model.accent_rgb = kCyan;
        return model;
    }

    if (input.turn_pending || IsBusyPhase(input.voice_phase)) {
        SetTalkAction(&model);
        model.accent_rgb = kViolet;
        switch (input.voice_phase) {
            case AgentVoicePhase::kOffline:
            case AgentVoicePhase::kError:
            case AgentVoicePhase::kConnecting:
                model.title = "已排队";
                model.subtitle = "联网后自动发送";
                model.accent_rgb = kAmber;
                break;
            case AgentVoicePhase::kSending:
                model.title = "正在发送";
                model.subtitle = "录音已保存";
                break;
            case AgentVoicePhase::kThinking:
                model.title = "正在思考";
                model.subtitle = "可以继续按住说话";
                break;
            case AgentVoicePhase::kReceiving:
                model.title = "准备回复";
                model.subtitle = "可以继续按住说话";
                break;
            case AgentVoicePhase::kReadyToPlay:
            case AgentVoicePhase::kPlaying:
                model.title = "准备播报";
                model.subtitle = "回复已保存";
                break;
            case AgentVoicePhase::kOnline:
                model.title = "已排队";
                model.subtitle = "准备发送";
                break;
        }
        return model;
    }

    model.accent_rgb = kCyan;
    SetTalkAction(&model);
    switch (input.voice_phase) {
        case AgentVoicePhase::kOffline:
            model.title = "离线可用";
            model.subtitle = "录音会在联网后发送";
            model.accent_rgb = kAmber;
            break;
        case AgentVoicePhase::kConnecting:
            model.title = "正在连接";
            model.subtitle = "现在也可以说话";
            model.accent_rgb = kViolet;
            break;
        case AgentVoicePhase::kError:
            model.title = "正在重试";
            model.subtitle = "现在也可以说话";
            model.accent_rgb = kAmber;
            break;
        default:
            model.title = "准备好了";
            model.subtitle = "松开后自动发送";
            break;
    }
    return model;
}

RecorderAssistantLayout RecorderBuildAssistantLayout(int width, int height) {
    RecorderAssistantLayout layout;
    layout.menu = {20, 18, 48, 48};
    layout.brand = {(width - 160) / 2, 20, 160, 46};
    layout.connection = {width - 52, 32, 18, 18};
    layout.title = {24, 92, width - 48, 38};
    layout.metric = {24, 128, width - 48, 30};
    layout.primary = {36, 170, width - 72, height - 234};
    layout.subtitle = {24, height - 45, width - 48, 32};
    return layout;
}
