#include "agent_voice_protocol.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace {

bool ExtractString(const std::string& json, const char* key, std::string* output) {
    const std::string marker = std::string("\"") + key + "\":\"";
    size_t position = json.find(marker);
    if (position == std::string::npos) {
        return false;
    }
    position += marker.size();
    std::string value;
    bool escaped = false;
    for (; position < json.size(); ++position) {
        const char ch = json[position];
        if (escaped) {
            switch (ch) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(ch); break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            *output = value;
            return true;
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

bool ExtractUint64(const std::string& json, const char* key, uint64_t* output) {
    const std::string marker = std::string("\"") + key + "\":";
    size_t position = json.find(marker);
    if (position == std::string::npos) {
        return false;
    }
    position += marker.size();
    char* end = nullptr;
    const unsigned long long value = strtoull(json.c_str() + position, &end, 10);
    if (end == json.c_str() + position) {
        return false;
    }
    *output = static_cast<uint64_t>(value);
    return true;
}

bool ExtractInt64(const std::string& json, const char* key, int64_t* output) {
    const std::string marker = std::string("\"") + key + "\":";
    size_t position = json.find(marker);
    if (position == std::string::npos) {
        return false;
    }
    position += marker.size();
    char* end = nullptr;
    const long long value = strtoll(json.c_str() + position, &end, 10);
    if (end == json.c_str() + position) {
        return false;
    }
    *output = static_cast<int64_t>(value);
    return true;
}

bool HasValue(const std::string& json, const char* key) {
    return json.find(std::string("\"") + key + "\":") != std::string::npos;
}

bool ExtractBool(const std::string& json, const char* key, bool* output) {
    const std::string marker = std::string("\"") + key + "\":";
    size_t position = json.find(marker);
    if (position == std::string::npos) {
        return false;
    }
    position += marker.size();
    if (json.compare(position, 4, "true") == 0) {
        *output = true;
        return true;
    }
    if (json.compare(position, 5, "false") == 0) {
        *output = false;
        return true;
    }
    return false;
}

bool ParseTurnId(const std::string& json,
                 const std::string& expected,
                 std::string* turn_id) {
    if (!ExtractString(json, "turn_id", turn_id) || !AgentVoiceSafeTurnId(*turn_id)) {
        return false;
    }
    return expected.empty() || *turn_id == expected;
}

}  // namespace

size_t AgentVoiceClampPcmSamples(uint64_t current_data_bytes,
                                size_t requested_samples) {
    constexpr uint64_t kWavHeaderBytes = 44;
    constexpr uint64_t kFlushReserveBytes = 640;
    constexpr uint64_t kDataLimit =
        kAgentVoiceMaxBytes - kWavHeaderBytes - kFlushReserveBytes;
    if (current_data_bytes >= kDataLimit) {
        return 0;
    }
    const uint64_t remaining_samples =
        (kDataLimit - current_data_bytes) / sizeof(int16_t);
    return static_cast<size_t>(std::min<uint64_t>(
        requested_samples, remaining_samples));
}

bool AgentVoiceSafeTurnId(const std::string& turn_id) {
    if (turn_id.empty() || turn_id.size() > 64) {
        return false;
    }
    for (unsigned char ch : turn_id) {
        if (!std::isalnum(ch) && ch != '.' && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

bool AgentVoiceSha256Text(const std::string& sha256) {
    if (sha256.size() != 64) {
        return false;
    }
    for (unsigned char ch : sha256) {
        if (!std::isdigit(ch) && !(ch >= 'a' && ch <= 'f')) {
            return false;
        }
    }
    return true;
}

std::string AgentVoiceBuildTurnStart(const std::string& turn_id,
                                     uint64_t bytes,
                                     const std::string& sha256) {
    if (!AgentVoiceSafeTurnId(turn_id) || bytes < 44 ||
        bytes > kAgentVoiceMaxBytes || !AgentVoiceSha256Text(sha256)) {
        return {};
    }
    std::ostringstream json;
    json << "{\"type\":\"turn_start\",\"turn_id\":\"" << turn_id
         << "\",\"audio_format\":\"wav-pcm-s16le\",\"sample_rate\":16000"
         << ",\"channels\":1,\"bytes\":" << bytes
         << ",\"sha256\":\"" << sha256 << "\"}";
    return json.str();
}

std::string AgentVoiceBuildTurnEnd(const std::string& turn_id) {
    if (!AgentVoiceSafeTurnId(turn_id)) {
        return {};
    }
    return "{\"type\":\"turn_end\",\"turn_id\":\"" + turn_id + "\"}";
}

std::string AgentVoiceBuildReplyChunkSaved(const std::string& turn_id,
                                           uint64_t saved_bytes) {
    if (!AgentVoiceSafeTurnId(turn_id) || saved_bytes == 0 ||
        saved_bytes > kAgentVoiceMaxBytes) {
        return {};
    }
    std::ostringstream json;
    json << "{\"type\":\"reply_chunk_saved\",\"turn_id\":\"" << turn_id
         << "\",\"bytes\":" << saved_bytes << "}";
    return json.str();
}

std::string AgentVoiceBuildReplySaved(const std::string& turn_id) {
    if (!AgentVoiceSafeTurnId(turn_id)) {
        return {};
    }
    return "{\"type\":\"reply_saved\",\"turn_id\":\"" + turn_id + "\"}";
}

std::string AgentVoiceBuildPing() {
    return "{\"type\":\"ping\"}";
}

bool AgentVoiceParseControl(const std::string& json,
                            const std::string& expected_turn_id,
                            AgentVoiceControl* control) {
    if (control == nullptr || json.size() > 8192) {
        return false;
    }
    AgentVoiceControl parsed;
    std::string type;
    if (!ExtractString(json, "type", &type)) {
        return false;
    }
    if (type == "ready") {
        uint64_t protocol = 0;
        uint64_t heartbeat = 0;
        uint64_t server_time_ms = 0;
        int64_t timezone_offset_minutes = 0;
        if (!ExtractUint64(json, "protocol", &protocol) || protocol != 1 ||
            !ExtractUint64(json, "heartbeat_seconds", &heartbeat) ||
            heartbeat < 5 || heartbeat > 120) {
            return false;
        }
        const bool has_server_time = HasValue(json, "server_time_ms");
        const bool has_timezone = HasValue(json, "timezone_offset_minutes");
        if (has_server_time != has_timezone ||
            (has_server_time &&
             (!ExtractUint64(json, "server_time_ms", &server_time_ms) ||
              server_time_ms < 1577836800000ULL ||
              server_time_ms > 4102444800000ULL ||
              !ExtractInt64(json, "timezone_offset_minutes",
                            &timezone_offset_minutes) ||
              timezone_offset_minutes < -720 ||
              timezone_offset_minutes > 840))) {
            return false;
        }
        parsed.type = AgentVoiceControlType::kReady;
        parsed.protocol = static_cast<uint32_t>(protocol);
        parsed.heartbeat_seconds = static_cast<uint32_t>(heartbeat);
        parsed.server_time_ms = server_time_ms;
        parsed.timezone_offset_minutes =
            static_cast<int32_t>(timezone_offset_minutes);
    } else if (type == "turn_ready") {
        uint64_t chunk_bytes = 0;
        if (!ParseTurnId(json, expected_turn_id, &parsed.turn_id) ||
            !ExtractUint64(json, "chunk_bytes", &chunk_bytes) ||
            chunk_bytes == 0 || chunk_bytes > kAgentVoiceMaxChunkBytes) {
            return false;
        }
        parsed.type = AgentVoiceControlType::kTurnReady;
        parsed.chunk_bytes = static_cast<uint32_t>(chunk_bytes);
    } else if (type == "turn_accepted") {
        if (!ParseTurnId(json, expected_turn_id, &parsed.turn_id) ||
            !ExtractBool(json, "replay", &parsed.replay)) {
            return false;
        }
        parsed.type = AgentVoiceControlType::kTurnAccepted;
    } else if (type == "reply_start") {
        uint64_t sample_rate = 0;
        uint64_t channels = 0;
        std::string audio_format;
        if (!ParseTurnId(json, expected_turn_id, &parsed.turn_id) ||
            !ExtractUint64(json, "bytes", &parsed.bytes) ||
            parsed.bytes < 44 || parsed.bytes > kAgentVoiceMaxBytes ||
            !ExtractString(json, "sha256", &parsed.sha256) ||
            !AgentVoiceSha256Text(parsed.sha256) ||
            !ExtractString(json, "audio_format", &audio_format) ||
            audio_format != "wav-pcm-s16le" ||
            !ExtractUint64(json, "sample_rate", &sample_rate) || sample_rate != 16000 ||
            !ExtractUint64(json, "channels", &channels) || channels != 1 ||
            !ExtractString(json, "transcript", &parsed.transcript) ||
            !ExtractString(json, "reply_text", &parsed.reply_text) ||
            !ExtractString(json, "server_time", &parsed.server_time) ||
            parsed.server_time.empty()) {
            return false;
        }
        parsed.type = AgentVoiceControlType::kReplyStart;
    } else if (type == "reply_end") {
        if (!ParseTurnId(json, expected_turn_id, &parsed.turn_id)) {
            return false;
        }
        parsed.type = AgentVoiceControlType::kReplyEnd;
    } else if (type == "error") {
        if (!ExtractString(json, "code", &parsed.error_code) ||
            !ExtractBool(json, "retryable", &parsed.retryable)) {
            return false;
        }
        ExtractString(json, "turn_id", &parsed.turn_id);
        if (!expected_turn_id.empty() && !parsed.turn_id.empty() &&
            parsed.turn_id != expected_turn_id) {
            return false;
        }
        parsed.type = AgentVoiceControlType::kError;
    } else if (type == "pong") {
        parsed.type = AgentVoiceControlType::kPong;
    } else {
        return false;
    }
    *control = std::move(parsed);
    return true;
}
