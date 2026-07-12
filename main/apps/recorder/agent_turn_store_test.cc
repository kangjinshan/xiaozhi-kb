#include "agent_turn_store.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::string MakeTempDir() {
    char pattern[] = "/tmp/agent-turn-store-XXXXXX";
    char* path = mkdtemp(pattern);
    assert(path != nullptr);
    return path;
}

void WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* file = fopen(path.c_str(), "wb");
    assert(file != nullptr);
    assert(fwrite(data.data(), 1, data.size(), file) == data.size());
    assert(fclose(file) == 0);
}

std::string ReadText(const std::string& path) {
    std::ifstream input(path);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    const std::string root = MakeTempDir();
    const std::vector<uint8_t> user_wav = {'R', 'I', 'F', 'F', 1, 2, 3};
    const std::vector<uint8_t> reply_wav = {'R', 'I', 'F', 'F', 4, 5, 6, 7};
    const std::string user_hash(64, 'a');
    const std::string reply_hash(64, 'b');

    AgentTurnStore store(root);
    AgentTurnPaths paths = store.Create("20260712", "turn-1");
    assert(paths.valid());
    WriteFile(paths.user_wav, user_wav);
    assert(store.MarkRecorded(paths, user_wav.size(), user_hash, 1783821600000ULL));

    auto pending = store.ListPending();
    assert(pending.size() == 1);
    assert(pending[0].paths.turn_id == "turn-1");
    assert(pending[0].user_bytes == user_wav.size());
    assert(pending[0].user_sha256 == user_hash);

    assert(store.UpdateState(paths, AgentTurnStatus::kSending));
    assert(store.BeginReply(paths, reply_wav.size(), reply_hash));
    assert(store.AppendReply(reply_wav.data(), 3));
    assert(store.AppendReply(reply_wav.data() + 3, reply_wav.size() - 3));
    assert(store.CommitReply(
        "用户说：你好",
        "助手说：你好呀",
        reply_wav.size(),
        reply_hash,
        "2026-07-12T10:00:00+08:00"));
    assert(access(paths.assistant_wav.c_str(), F_OK) == 0);
    assert(access((paths.assistant_wav + ".part").c_str(), F_OK) != 0);
    assert(store.ListPending().empty());

    const std::string manifest = ReadText(paths.manifest);
    assert(manifest.find("\"status\":\"complete\"") != std::string::npos);
    assert(manifest.find("用户说：你好") != std::string::npos);
    assert(manifest.find("助手说：你好呀") != std::string::npos);
    assert(manifest.find("2026-07-12T10:00:00+08:00") != std::string::npos);
    assert(ReadText(root + "/turns.jsonl").find("turn-1") != std::string::npos);

    AgentTurnPaths interrupted = store.Create("20260712", "turn-2");
    assert(interrupted.valid());
    WriteFile(interrupted.user_wav, user_wav);
    assert(store.MarkRecorded(interrupted, user_wav.size(), user_hash, 1783821660000ULL));
    assert(store.BeginReply(interrupted, reply_wav.size(), reply_hash));
    assert(store.AppendReply(reply_wav.data(), 3));
    store.AbortReply();
    assert(access((interrupted.assistant_wav + ".part").c_str(), F_OK) != 0);

    AgentTurnStore restarted(root);
    pending = restarted.ListPending();
    assert(pending.size() == 1);
    assert(pending[0].paths.turn_id == "turn-2");
    assert(access(pending[0].paths.user_wav.c_str(), F_OK) == 0);

    AgentTurnPaths missing_manifest = store.Create("20260712", "turn-3");
    assert(missing_manifest.valid());
    assert(!store.BeginReply(missing_manifest, reply_wav.size(), reply_hash));
    assert(access((missing_manifest.assistant_wav + ".part").c_str(), F_OK) != 0);

    AgentTurnPaths unsafe = store.Create("20260712", "../escape");
    assert(!unsafe.valid());
    return 0;
}
