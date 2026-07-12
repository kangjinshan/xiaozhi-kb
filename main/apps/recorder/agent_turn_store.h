#ifndef AGENT_TURN_STORE_H_
#define AGENT_TURN_STORE_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

enum class AgentTurnStatus {
    kRecorded,
    kSending,
    kProcessing,
    kReceiving,
    kComplete,
    kFailed,
};

struct AgentTurnPaths {
    std::string turn_id;
    std::string date;
    std::string directory;
    std::string user_wav;
    std::string assistant_wav;
    std::string manifest;

    bool valid() const { return !directory.empty(); }
};

struct AgentPendingTurn {
    AgentTurnPaths paths;
    AgentTurnStatus status = AgentTurnStatus::kRecorded;
    uint64_t user_bytes = 0;
    std::string user_sha256;
    uint64_t created_at_ms = 0;
};

class AgentTurnStore {
public:
    explicit AgentTurnStore(std::string root);
    ~AgentTurnStore();

    AgentTurnPaths Create(const std::string& date, const std::string& turn_id);
    bool MarkRecorded(const AgentTurnPaths& paths,
                      uint64_t user_bytes,
                      const std::string& user_sha256,
                      uint64_t created_at_ms);
    bool UpdateState(const AgentTurnPaths& paths, AgentTurnStatus status);
    std::vector<AgentPendingTurn> ListPending() const;

    bool BeginReply(const AgentTurnPaths& paths,
                    uint64_t expected_bytes,
                    const std::string& expected_sha256);
    bool AppendReply(const uint8_t* data, size_t size);
    bool CommitReply(const std::string& transcript,
                     const std::string& reply_text,
                     uint64_t actual_bytes,
                     const std::string& actual_sha256,
                     const std::string& server_time);
    void AbortReply();

private:
    struct Record;

    std::string root_;
    FILE* reply_file_ = nullptr;
    AgentTurnPaths active_paths_;
    uint64_t expected_reply_bytes_ = 0;
    uint64_t received_reply_bytes_ = 0;
    std::string expected_reply_sha256_;

    bool LoadRecord(const AgentTurnPaths& paths, Record* record) const;
    bool WriteRecord(const AgentTurnPaths& paths, const Record& record) const;
    bool AppendIndex(const Record& record) const;
};

const char* AgentTurnStatusName(AgentTurnStatus status);

#endif  // AGENT_TURN_STORE_H_
