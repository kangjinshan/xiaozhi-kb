#include "recorder_assistant_ui.h"

#include <algorithm>
#include <cstdio>

namespace {

constexpr uint32_t kCyan = 0x58D6FF;
constexpr uint32_t kCoral = 0xFF667A;
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
    model->primary_label = "点击说话";
    model->primary_action = RecorderAssistantPrimaryAction::kTalk;
    model->primary_enabled = true;
    model->history_visible = true;
}

void SetDisabled(RecorderAssistantUiModel* model) {
    model->primary_label = "请稍等";
    model->primary_action = RecorderAssistantPrimaryAction::kNone;
    model->primary_enabled = false;
    model->history_visible = false;
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

    if (input.notice != RecorderAssistantNotice::kNone) {
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
        model.title = "正在聆听";
        model.subtitle = "说完后点击发送";
        model.metric = timer;
        model.primary_label = "发送";
        model.primary_action = RecorderAssistantPrimaryAction::kSend;
        model.primary_enabled = true;
        model.history_visible = false;
        model.accent_rgb = kCoral;
        return model;
    }

    if (input.mode == RecorderControlMode::kPlaying ||
        input.mode == RecorderControlMode::kPaused) {
        char volume[20];
        std::snprintf(volume, sizeof(volume), "音量 %d",
                      std::clamp(input.volume, 0, 100));
        const bool paused = input.mode == RecorderControlMode::kPaused;
        model.title = paused ? "已暂停" : "正在播报";
        model.subtitle = paused ? "点击继续播放" : "正在回答你的问题";
        model.metric = volume;
        model.primary_label = paused ? "继续" : "暂停";
        model.primary_action = paused
            ? RecorderAssistantPrimaryAction::kResume
            : RecorderAssistantPrimaryAction::kPause;
        model.primary_enabled = true;
        model.history_visible = false;
        model.accent_rgb = paused ? kAmber : kMint;
        return model;
    }

    if (input.turn_pending || IsBusyPhase(input.voice_phase)) {
        SetDisabled(&model);
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
                model.subtitle = "请稍等";
                break;
            case AgentVoicePhase::kReceiving:
                model.title = "准备回复";
                model.subtitle = "正在保存到存储卡";
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
            model.subtitle = input.noise_reduction_ready
                ? "请说话"
                : "请说话 降噪未启用";
            break;
    }
    return model;
}

RecorderAssistantLayout RecorderBuildAssistantLayout(int width, int height) {
    RecorderAssistantLayout layout;
    layout.menu = {18, 18, 76, 42};
    layout.brand = {(width - 180) / 2, 20, 180, 40};
    layout.connection = {width - 138, 22, 120, 36};
    layout.orb = {(width - 168) / 2, 76, 168, 168};
    layout.title = {30, 248, width - 60, 40};
    layout.subtitle = {30, 289, width - 60, 32};
    layout.metric = {30, 321, width - 60, 28};
    layout.primary = {(width - 340) / 2, 354, 340, 64};
    layout.history = {(width - 150) / 2, height - 50, 150, 40};
    return layout;
}
