#include "agent_turn_store.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool IsSafeComponent(const std::string& value, size_t exact_digits = 0) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    if (exact_digits != 0 && value.size() != exact_digits) {
        return false;
    }
    for (unsigned char ch : value) {
        if (exact_digits != 0) {
            if (!std::isdigit(ch)) {
                return false;
            }
        } else if (!std::isalnum(ch) && ch != '.' && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

bool EnsureDirectory(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] != '/' || current.size() == 1) {
            continue;
        }
        current.pop_back();
        if (!current.empty() && mkdir(current.c_str(), 0777) != 0 && errno != EEXIST) {
            return false;
        }
        current.push_back('/');
    }
    return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

bool IsDirectory(const std::string& path) {
    struct stat info = {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool IsFile(const std::string& path) {
    struct stat info = {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool UnlinkIfPresent(const std::string& path) {
    return unlink(path.c_str()) == 0 || errno == ENOENT;
}

bool PublishFile(const std::string& part_path,
                 const std::string& destination_path) {
    const std::string backup_path = destination_path + ".bak";
    const bool had_destination = IsFile(destination_path);
    if (had_destination) {
        if (!UnlinkIfPresent(backup_path) ||
            rename(destination_path.c_str(), backup_path.c_str()) != 0) {
            return false;
        }
    }
    if (rename(part_path.c_str(), destination_path.c_str()) != 0) {
        if (had_destination && !IsFile(destination_path)) {
            rename(backup_path.c_str(), destination_path.c_str());
        }
        return false;
    }
    UnlinkIfPresent(backup_path);
    return true;
}

std::string ReadFile(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    std::string output;
    char buffer[512];
    size_t size = 0;
    while ((size = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        output.append(buffer, size);
    }
    fclose(file);
    return output;
}

std::string JsonEscape(const std::string& value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (ch >= 0x20) {
                    output.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return output;
}

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

bool ParseStatus(const std::string& value, AgentTurnStatus* status) {
    const AgentTurnStatus values[] = {
        AgentTurnStatus::kRecorded,
        AgentTurnStatus::kSending,
        AgentTurnStatus::kProcessing,
        AgentTurnStatus::kReceiving,
        AgentTurnStatus::kComplete,
        AgentTurnStatus::kFailed,
    };
    for (AgentTurnStatus candidate : values) {
        if (value == AgentTurnStatusName(candidate)) {
            *status = candidate;
            return true;
        }
    }
    return false;
}

}  // namespace

struct AgentTurnStore::Record {
    std::string turn_id;
    std::string date;
    AgentTurnStatus status = AgentTurnStatus::kRecorded;
    uint64_t created_at_ms = 0;
    uint64_t user_bytes = 0;
    std::string user_sha256;
    uint64_t assistant_bytes = 0;
    std::string assistant_sha256;
    std::string transcript;
    std::string reply_text;
    std::string server_time;
    std::string last_error;
};

const char* AgentTurnStatusName(AgentTurnStatus status) {
    switch (status) {
        case AgentTurnStatus::kRecorded: return "recorded";
        case AgentTurnStatus::kSending: return "sending";
        case AgentTurnStatus::kProcessing: return "processing";
        case AgentTurnStatus::kReceiving: return "receiving";
        case AgentTurnStatus::kComplete: return "complete";
        case AgentTurnStatus::kFailed: return "failed";
    }
    return "failed";
}

AgentTurnStore::AgentTurnStore(std::string root) : root_(std::move(root)) {
    EnsureDirectory(root_);
}

AgentTurnStore::~AgentTurnStore() {
    AbortReply();
}

AgentTurnPaths AgentTurnStore::Create(const std::string& date,
                                      const std::string& turn_id) {
    if (!IsSafeComponent(date, 8) || !IsSafeComponent(turn_id)) {
        return {};
    }
    const std::string date_directory = root_ + "/" + date;
    const std::string directory = date_directory + "/" + turn_id;
    if (!EnsureDirectory(root_) || !EnsureDirectory(date_directory) ||
        !EnsureDirectory(directory)) {
        return {};
    }
    AgentTurnPaths paths;
    paths.turn_id = turn_id;
    paths.date = date;
    paths.directory = directory;
    paths.user_wav = directory + "/user.wav";
    paths.assistant_wav = directory + "/assistant.wav";
    paths.manifest = directory + "/turn.json";
    return paths;
}

bool AgentTurnStore::MarkRecorded(const AgentTurnPaths& paths,
                                  uint64_t user_bytes,
                                  const std::string& user_sha256,
                                  uint64_t created_at_ms) {
    if (!paths.valid() || !IsFile(paths.user_wav) || user_sha256.size() != 64) {
        return false;
    }
    Record record;
    record.turn_id = paths.turn_id;
    record.date = paths.date;
    record.status = AgentTurnStatus::kRecorded;
    record.created_at_ms = created_at_ms;
    record.user_bytes = user_bytes;
    record.user_sha256 = user_sha256;
    return WriteRecord(paths, record);
}

bool AgentTurnStore::UpdateState(const AgentTurnPaths& paths,
                                 AgentTurnStatus status) {
    Record record;
    if (!LoadRecord(paths, &record)) {
        return false;
    }
    record.status = status;
    return WriteRecord(paths, record);
}

std::vector<AgentPendingTurn> AgentTurnStore::ListPending() const {
    std::vector<AgentPendingTurn> pending;
    DIR* root = opendir(root_.c_str());
    if (root == nullptr) {
        return pending;
    }
    struct dirent* date_entry = nullptr;
    while ((date_entry = readdir(root)) != nullptr) {
        const std::string date = date_entry->d_name;
        if (!IsSafeComponent(date, 8)) {
            continue;
        }
        const std::string date_path = root_ + "/" + date;
        if (!IsDirectory(date_path)) {
            continue;
        }
        DIR* date_directory = opendir(date_path.c_str());
        if (date_directory == nullptr) {
            continue;
        }
        struct dirent* turn_entry = nullptr;
        while ((turn_entry = readdir(date_directory)) != nullptr) {
            const std::string turn_id = turn_entry->d_name;
            if (!IsSafeComponent(turn_id)) {
                continue;
            }
            AgentTurnPaths paths;
            paths.turn_id = turn_id;
            paths.date = date;
            paths.directory = date_path + "/" + turn_id;
            paths.user_wav = paths.directory + "/user.wav";
            paths.assistant_wav = paths.directory + "/assistant.wav";
            paths.manifest = paths.directory + "/turn.json";
            if (!IsDirectory(paths.directory) || !IsFile(paths.user_wav)) {
                continue;
            }
            Record record;
            if (!LoadRecord(paths, &record) ||
                record.status == AgentTurnStatus::kComplete) {
                continue;
            }
            unlink((paths.assistant_wav + ".part").c_str());
            pending.push_back({
                paths,
                record.status,
                record.user_bytes,
                record.user_sha256,
                record.created_at_ms,
            });
        }
        closedir(date_directory);
    }
    closedir(root);
    std::sort(pending.begin(), pending.end(),
              [](const AgentPendingTurn& left, const AgentPendingTurn& right) {
                  if (left.created_at_ms != right.created_at_ms) {
                      return left.created_at_ms < right.created_at_ms;
                  }
                  if (left.paths.date != right.paths.date) {
                      return left.paths.date < right.paths.date;
                  }
                  return left.paths.turn_id < right.paths.turn_id;
              });
    return pending;
}

bool AgentTurnStore::BeginReply(const AgentTurnPaths& paths,
                                uint64_t expected_bytes,
                                const std::string& expected_sha256) {
    AbortReply();
    if (!paths.valid() || expected_bytes == 0 || expected_sha256.size() != 64) {
        return false;
    }
    const std::string part_path = paths.assistant_wav + ".part";
    reply_file_ = fopen(part_path.c_str(), "wb");
    if (reply_file_ == nullptr) {
        return false;
    }
    active_paths_ = paths;
    expected_reply_bytes_ = expected_bytes;
    received_reply_bytes_ = 0;
    expected_reply_sha256_ = expected_sha256;
    if (!UpdateState(paths, AgentTurnStatus::kReceiving)) {
        AbortReply();
        return false;
    }
    return true;
}

bool AgentTurnStore::AppendReply(const uint8_t* data, size_t size) {
    if (reply_file_ == nullptr || data == nullptr || size == 0 || size > 4096 ||
        received_reply_bytes_ + size > expected_reply_bytes_) {
        return false;
    }
    if (fwrite(data, 1, size, reply_file_) != size) {
        return false;
    }
    received_reply_bytes_ += size;
    return true;
}

bool AgentTurnStore::CommitReply(const std::string& transcript,
                                 const std::string& reply_text,
                                 uint64_t actual_bytes,
                                 const std::string& actual_sha256,
                                 const std::string& server_time) {
    if (reply_file_ == nullptr) {
        return false;
    }
    const AgentTurnPaths paths = active_paths_;
    const std::string part_path = paths.assistant_wav + ".part";
    const bool flushed = fflush(reply_file_) == 0 && fsync(fileno(reply_file_)) == 0;
    const bool closed = fclose(reply_file_) == 0;
    reply_file_ = nullptr;
    const bool valid = flushed && closed &&
        received_reply_bytes_ == expected_reply_bytes_ &&
        actual_bytes == expected_reply_bytes_ &&
        actual_sha256 == expected_reply_sha256_;
    if (!valid || !PublishFile(part_path, paths.assistant_wav)) {
        unlink(part_path.c_str());
        active_paths_ = {};
        return false;
    }
    Record record;
    if (!LoadRecord(paths, &record)) {
        active_paths_ = {};
        return false;
    }
    record.status = AgentTurnStatus::kComplete;
    record.assistant_bytes = actual_bytes;
    record.assistant_sha256 = actual_sha256;
    record.transcript = transcript;
    record.reply_text = reply_text;
    record.server_time = server_time;
    record.last_error.clear();
    const bool saved = WriteRecord(paths, record) && AppendIndex(record);
    active_paths_ = {};
    expected_reply_bytes_ = 0;
    received_reply_bytes_ = 0;
    expected_reply_sha256_.clear();
    return saved;
}

void AgentTurnStore::AbortReply() {
    if (reply_file_ != nullptr) {
        fclose(reply_file_);
        reply_file_ = nullptr;
    }
    if (active_paths_.valid()) {
        unlink((active_paths_.assistant_wav + ".part").c_str());
    }
    active_paths_ = {};
    expected_reply_bytes_ = 0;
    received_reply_bytes_ = 0;
    expected_reply_sha256_.clear();
}

bool AgentTurnStore::LoadRecord(const AgentTurnPaths& paths, Record* record) const {
    if (record == nullptr) {
        return false;
    }
    std::string json = ReadFile(paths.manifest);
    if (json.empty()) {
        json = ReadFile(paths.manifest + ".bak");
    }
    std::string status;
    if (json.empty() ||
        !ExtractString(json, "turn_id", &record->turn_id) ||
        !ExtractString(json, "date", &record->date) ||
        !ExtractString(json, "status", &status) ||
        !ParseStatus(status, &record->status) ||
        !ExtractUint64(json, "created_at_ms", &record->created_at_ms) ||
        !ExtractUint64(json, "user_bytes", &record->user_bytes) ||
        !ExtractString(json, "user_sha256", &record->user_sha256)) {
        return false;
    }
    ExtractUint64(json, "assistant_bytes", &record->assistant_bytes);
    ExtractString(json, "assistant_sha256", &record->assistant_sha256);
    ExtractString(json, "transcript", &record->transcript);
    ExtractString(json, "reply_text", &record->reply_text);
    ExtractString(json, "server_time", &record->server_time);
    ExtractString(json, "last_error", &record->last_error);
    return record->turn_id == paths.turn_id && record->date == paths.date;
}

bool AgentTurnStore::WriteRecord(const AgentTurnPaths& paths,
                                 const Record& record) const {
    std::ostringstream json;
    json << "{\"protocol\":1"
         << ",\"turn_id\":\"" << JsonEscape(record.turn_id) << "\""
         << ",\"date\":\"" << JsonEscape(record.date) << "\""
         << ",\"status\":\"" << AgentTurnStatusName(record.status) << "\""
         << ",\"created_at_ms\":" << record.created_at_ms
         << ",\"user_path\":\"user.wav\""
         << ",\"user_bytes\":" << record.user_bytes
         << ",\"user_sha256\":\"" << JsonEscape(record.user_sha256) << "\""
         << ",\"assistant_path\":\"assistant.wav\""
         << ",\"assistant_bytes\":" << record.assistant_bytes
         << ",\"assistant_sha256\":\"" << JsonEscape(record.assistant_sha256) << "\""
         << ",\"transcript\":\"" << JsonEscape(record.transcript) << "\""
         << ",\"reply_text\":\"" << JsonEscape(record.reply_text) << "\""
         << ",\"server_time\":\"" << JsonEscape(record.server_time) << "\""
         << ",\"last_error\":\"" << JsonEscape(record.last_error) << "\"}\n";
    const std::string part_path = paths.manifest + ".part";
    FILE* file = fopen(part_path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const std::string content = json.str();
    const bool ok = fwrite(content.data(), 1, content.size(), file) == content.size() &&
                    fflush(file) == 0 && fsync(fileno(file)) == 0 && fclose(file) == 0;
    if (!ok || !PublishFile(part_path, paths.manifest)) {
        unlink(part_path.c_str());
        return false;
    }
    return true;
}

bool AgentTurnStore::AppendIndex(const Record& record) const {
    FILE* file = fopen((root_ + "/turns.jsonl").c_str(), "ab");
    if (file == nullptr) {
        return false;
    }
    std::ostringstream line;
    line << "{\"turn_id\":\"" << JsonEscape(record.turn_id)
         << "\",\"date\":\"" << JsonEscape(record.date)
         << "\",\"status\":\"complete\",\"created_at_ms\":"
         << record.created_at_ms << "}\n";
    const std::string content = line.str();
    const bool ok = fwrite(content.data(), 1, content.size(), file) == content.size() &&
                    fflush(file) == 0 && fsync(fileno(file)) == 0 && fclose(file) == 0;
    return ok;
}
