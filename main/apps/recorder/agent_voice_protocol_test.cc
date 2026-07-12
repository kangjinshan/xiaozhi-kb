#include "agent_voice_protocol.h"

#include <cassert>
#include <string>

int main() {
    const std::string hash(64, 'a');
    const std::string turn_start = AgentVoiceBuildTurnStart("turn-1", 1024, hash);
    assert(turn_start.find("\"type\":\"turn_start\"") != std::string::npos);
    assert(turn_start.find("\"sample_rate\":16000") != std::string::npos);
    assert(turn_start.find(hash) != std::string::npos);
    assert(AgentVoiceBuildTurnStart("../escape", 1024, hash).empty());
    assert(AgentVoiceBuildTurnStart("turn-1", 4194305, hash).empty());

    AgentVoiceControl control;
    assert(AgentVoiceParseControl(
        R"({"type":"ready","protocol":1,"device_id":"device-one","heartbeat_seconds":25})",
        "",
        &control));
    assert(control.type == AgentVoiceControlType::kReady);
    assert(control.protocol == 1);
    assert(control.heartbeat_seconds == 25);

    assert(AgentVoiceParseControl(
        R"({"type":"turn_ready","turn_id":"turn-1","chunk_bytes":4096})",
        "turn-1",
        &control));
    assert(control.type == AgentVoiceControlType::kTurnReady);
    assert(control.chunk_bytes == 4096);

    assert(AgentVoiceParseControl(
        R"({"type":"turn_accepted","turn_id":"turn-1","replay":false})",
        "turn-1",
        &control));
    assert(control.type == AgentVoiceControlType::kTurnAccepted);
    assert(!control.replay);

    const std::string reply =
        std::string("{\"type\":\"reply_start\",\"turn_id\":\"turn-1\",") +
        "\"bytes\":2048,\"sha256\":\"" + std::string(64, 'b') +
        "\",\"audio_format\":\"wav-pcm-s16le\",\"sample_rate\":16000," +
        "\"channels\":1,\"transcript\":\"你好\",\"reply_text\":\"你好呀\"," +
        "\"server_time\":\"2026-07-12T10:00:00+08:00\"}";
    assert(AgentVoiceParseControl(reply, "turn-1", &control));
    assert(control.type == AgentVoiceControlType::kReplyStart);
    assert(control.bytes == 2048);
    assert(control.sha256 == std::string(64, 'b'));
    assert(control.transcript == "你好");
    assert(control.reply_text == "你好呀");

    const std::string oversize =
        std::string("{\"type\":\"reply_start\",\"turn_id\":\"turn-1\",") +
        "\"bytes\":4194305,\"sha256\":\"" + std::string(64, 'b') +
        "\",\"audio_format\":\"wav-pcm-s16le\",\"sample_rate\":16000," +
        "\"channels\":1,\"transcript\":\"\",\"reply_text\":\"\"}";
    assert(!AgentVoiceParseControl(oversize, "turn-1", &control));
    assert(!AgentVoiceParseControl(reply, "different-turn", &control));

    assert(AgentVoiceParseControl(
        R"({"type":"reply_end","turn_id":"turn-1"})",
        "turn-1",
        &control));
    assert(control.type == AgentVoiceControlType::kReplyEnd);

    assert(AgentVoiceBuildTurnEnd("turn-1") ==
           R"({"type":"turn_end","turn_id":"turn-1"})");
    assert(AgentVoiceBuildReplySaved("turn-1") ==
           R"({"type":"reply_saved","turn_id":"turn-1"})");
    assert(AgentVoiceBuildPing() == R"({"type":"ping"})");
    return 0;
}
