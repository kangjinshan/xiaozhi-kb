#include "recorder_assistant_ui.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

RecorderAssistantUiInput BaseInput() {
    RecorderAssistantUiInput input;
    input.mode = RecorderControlMode::kIdle;
    input.voice_phase = AgentVoicePhase::kOnline;
    input.sd_ready = true;
    input.noise_reduction_ready = true;
    input.volume = 70;
    return input;
}

void TestReadyAndOfflineActions() {
    auto model = RecorderBuildAssistantUi(BaseInput());
    Check(model.connection_label == "已连接", "ready connection label");
    Check(model.title == "准备好了", "ready title");
    Check(model.subtitle == "松开后自动发送", "ready guidance");
    Check(model.primary_label == "按住说话", "ready primary label");
    Check(model.primary_action == RecorderAssistantPrimaryAction::kTalk,
          "ready primary action");
    Check(model.primary_enabled, "ready primary enabled");

    auto offline = BaseInput();
    offline.voice_phase = AgentVoicePhase::kOffline;
    model = RecorderBuildAssistantUi(offline);
    Check(model.connection_label == "无网络", "offline connection label");
    Check(model.title == "离线可用", "offline title");
    Check(model.subtitle == "录音会在联网后发送", "offline guidance");
    Check(model.primary_action == RecorderAssistantPrimaryAction::kTalk,
          "offline recording remains available");
}

void TestListeningShowsTimerAndSendAction() {
    auto input = BaseInput();
    input.mode = RecorderControlMode::kRecording;
    input.elapsed_seconds = 65;
    const auto model = RecorderBuildAssistantUi(input);
    Check(model.title == "正在录音", "listening title");
    Check(model.subtitle == "说完松手即可", "listening guidance");
    Check(model.metric == "01:05", "listening timer");
    Check(model.primary_label == "松开发送", "listening primary label");
    Check(model.primary_action == RecorderAssistantPrimaryAction::kSend,
          "listening send action");
    Check(model.primary_enabled, "listening primary enabled");
}

void TestAgentBusyPhasesKeepRecordingAvailable() {
    struct Expected {
        AgentVoicePhase phase;
        const char* title;
    };
    for (const auto& expected : {
             Expected{AgentVoicePhase::kSending, "正在发送"},
             Expected{AgentVoicePhase::kThinking, "正在思考"},
             Expected{AgentVoicePhase::kReceiving, "准备回复"},
             Expected{AgentVoicePhase::kReadyToPlay, "准备播报"},
         }) {
        auto input = BaseInput();
        input.voice_phase = expected.phase;
        input.turn_pending = true;
        const auto model = RecorderBuildAssistantUi(input);
        Check(model.title == expected.title, "busy phase title");
        Check(model.primary_action == RecorderAssistantPrimaryAction::kTalk,
              "busy phase still offers push to talk");
        Check(model.primary_enabled, "busy phase keeps microphone available");
    }

    auto queued = BaseInput();
    queued.voice_phase = AgentVoicePhase::kOffline;
    queued.turn_pending = true;
    const auto model = RecorderBuildAssistantUi(queued);
    Check(model.title == "已排队", "offline pending title");
    Check(model.subtitle == "联网后自动发送", "offline pending guidance");
}

void TestSpeakingPauseResumeAndVolumeClamp() {
    auto input = BaseInput();
    input.mode = RecorderControlMode::kPlaying;
    input.volume = 108;
    auto model = RecorderBuildAssistantUi(input);
    Check(model.title == "神勇正在回答", "speaking title");
    Check(model.metric.empty(), "playback UI has no secondary volume panel");
    Check(model.primary_label == "按住说话", "speaking primary label");
    Check(model.primary_action == RecorderAssistantPrimaryAction::kTalk,
          "speaking pause action");

    input.mode = RecorderControlMode::kPaused;
    input.volume = -4;
    model = RecorderBuildAssistantUi(input);
    Check(model.title == "神勇正在回答", "paused title");
    Check(model.metric.empty(), "no secondary status metrics");
    Check(model.primary_label == "按住说话", "paused primary label");
    Check(model.primary_action == RecorderAssistantPrimaryAction::kTalk,
          "paused resume action");
}

void TestStorageSetupAndTransientFailuresTakePriority() {
    auto input = BaseInput();
    input.sd_ready = false;
    auto model = RecorderBuildAssistantUi(input);
    Check(model.title == "请插入存储卡", "missing storage title");
    Check(!model.primary_enabled, "missing storage blocks recording");

    input.sd_ready = true;
    input.notice = RecorderAssistantNotice::kWifiSetup;
    model = RecorderBuildAssistantUi(input);
    Check(model.title == "请设置网络", "Wi-Fi setup title");
    Check(model.subtitle == "先进入小智 AI 完成设置", "Wi-Fi setup guidance");

    input.notice = RecorderAssistantNotice::kAgentSetup;
    model = RecorderBuildAssistantUi(input);
    Check(model.title == "请设置助手", "Agent setup title");

    input.notice = RecorderAssistantNotice::kDspFailure;
    model = RecorderBuildAssistantUi(input);
    Check(model.title == "录音不可用", "DSP failure title");

    input.notice = RecorderAssistantNotice::kSaveFailure;
    model = RecorderBuildAssistantUi(input);
    Check(model.title == "保存失败", "save failure title");

    input.notice = RecorderAssistantNotice::kPlaybackFailure;
    model = RecorderBuildAssistantUi(input);
    Check(model.title == "播放失败", "playback failure title");

    input.notice = RecorderAssistantNotice::kSpeechNotRecognized;
    model = RecorderBuildAssistantUi(input);
    Check(model.title == "没有听清", "unrecognized speech title");
    Check(model.subtitle == "请再说一次", "unrecognized speech guidance");
    Check(model.primary_enabled, "unrecognized speech allows immediate retry");
    input.mode = RecorderControlMode::kRecording;
    model = RecorderBuildAssistantUi(input);
    Check(model.primary_action == RecorderAssistantPrimaryAction::kSend,
          "earlier turn errors cannot hide the release action");
}

void TestConnectingAndRetryingRemainUnderstandable() {
    auto input = BaseInput();
    input.voice_phase = AgentVoicePhase::kConnecting;
    auto model = RecorderBuildAssistantUi(input);
    Check(model.connection_label == "连接中", "connecting status");
    Check(model.title == "正在连接", "connecting title");
    Check(model.primary_action == RecorderAssistantPrimaryAction::kTalk,
          "recording remains available while connecting");

    input.voice_phase = AgentVoicePhase::kError;
    model = RecorderBuildAssistantUi(input);
    Check(model.connection_label == "重试中", "retry status");
    Check(model.title == "正在重试", "retry title");
    Check(model.primary_action == RecorderAssistantPrimaryAction::kTalk,
          "recording remains available while retrying");
}

bool Fits(const RecorderAssistantRect& rect, int width, int height) {
    return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 &&
           rect.x + rect.width <= width && rect.y + rect.height <= height;
}

void TestAssistantLayoutFitsTargetDisplay() {
    constexpr int kWidth = 480;
    constexpr int kHeight = 480;
    const auto layout = RecorderBuildAssistantLayout(kWidth, kHeight);
    for (const auto& rect : {
             layout.menu,
             layout.brand,
             layout.connection,
             layout.title,
             layout.subtitle,
             layout.metric,
             layout.primary,
         }) {
        Check(Fits(rect, kWidth, kHeight), "assistant widget fits display");
    }
    Check(layout.primary.width >= 320 && layout.primary.height >= 60,
          "primary talk target is prominent");
    Check(layout.primary.height >= 220 && layout.primary.width >= 380,
          "the single talk surface dominates the small display");
    Check(layout.subtitle.y >= layout.primary.y + layout.primary.height,
          "helper copy stays below the talk surface");
}

}  // namespace

int main() {
    TestReadyAndOfflineActions();
    TestListeningShowsTimerAndSendAction();
    TestAgentBusyPhasesKeepRecordingAvailable();
    TestSpeakingPauseResumeAndVolumeClamp();
    TestStorageSetupAndTransientFailuresTakePriority();
    TestConnectingAndRetryingRemainUnderstandable();
    TestAssistantLayoutFitsTargetDisplay();
    std::puts("recorder_assistant_ui_test passed");
    return 0;
}
