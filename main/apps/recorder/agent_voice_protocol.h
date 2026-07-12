#ifndef AGENT_VOICE_PROTOCOL_H_
#define AGENT_VOICE_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <string>

constexpr uint64_t kAgentVoiceMaxBytes = 4ULL * 1024ULL * 1024ULL;
constexpr uint32_t kAgentVoiceMaxChunkBytes = 4096;

enum class AgentVoiceControlType {
    kUnknown,
    kReady,
    kTurnReady,
    kTurnAccepted,
    kReplyStart,
    kReplyEnd,
    kError,
    kPong,
};

struct AgentVoiceControl {
    AgentVoiceControlType type = AgentVoiceControlType::kUnknown;
    std::string turn_id;
    uint32_t protocol = 0;
    uint32_t heartbeat_seconds = 0;
    uint32_t chunk_bytes = 0;
    bool replay = false;
    uint64_t bytes = 0;
    std::string sha256;
    std::string transcript;
    std::string reply_text;
    std::string server_time;
    std::string error_code;
    bool retryable = false;
};

std::string AgentVoiceBuildTurnStart(const std::string& turn_id,
                                     uint64_t bytes,
                                     const std::string& sha256);
std::string AgentVoiceBuildTurnEnd(const std::string& turn_id);
std::string AgentVoiceBuildReplyChunkSaved(const std::string& turn_id,
                                           uint64_t saved_bytes);
std::string AgentVoiceBuildReplySaved(const std::string& turn_id);
std::string AgentVoiceBuildPing();

bool AgentVoiceParseControl(const std::string& json,
                            const std::string& expected_turn_id,
                            AgentVoiceControl* control);

bool AgentVoiceSafeTurnId(const std::string& turn_id);
bool AgentVoiceSha256Text(const std::string& sha256);
size_t AgentVoiceClampPcmSamples(uint64_t current_data_bytes,
                                size_t requested_samples);

#endif  // AGENT_VOICE_PROTOCOL_H_
