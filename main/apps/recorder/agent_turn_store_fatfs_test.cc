#include "agent_turn_store.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#undef rename

extern "C" int AgentTurnStoreFatRename(const char* source,
                                        const char* destination) {
    struct stat info = {};
    if (stat(destination, &info) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) {
        return -1;
    }
    return renameat(AT_FDCWD, source, AT_FDCWD, destination);
}

namespace {

std::string MakeTempDir() {
    char pattern[] = "/tmp/agent-turn-store-fatfs-XXXXXX";
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

std::vector<uint8_t> ReadBytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    const std::string root = MakeTempDir();
    const std::vector<uint8_t> user_wav(128, 0x31);
    const std::string user_hash(64, 'a');

    AgentTurnStore store(root);
    const AgentTurnPaths paths = store.Create("20260712", "turn-fatfs");
    assert(paths.valid());
    WriteFile(paths.user_wav, user_wav);
    assert(store.MarkRecorded(
        paths, user_wav.size(), user_hash, 1783821600000ULL));

    assert(store.UpdateState(paths, AgentTurnStatus::kSending));
    assert(ReadText(paths.manifest).find("\"status\":\"sending\"") !=
           std::string::npos);

    const std::vector<uint8_t> old_reply = {'o', 'l', 'd'};
    const std::vector<uint8_t> new_reply = {'R', 'I', 'F', 'F', 1, 2, 3, 4};
    const std::string reply_hash(64, 'b');
    WriteFile(paths.assistant_wav, old_reply);
    assert(store.BeginReply(paths, new_reply.size(), reply_hash));
    assert(store.AppendReply(new_reply.data(), new_reply.size()));
    assert(store.CommitReply(
        "transcript",
        "reply",
        new_reply.size(),
        reply_hash,
        "2026-07-12T11:30:00+08:00"));
    assert(ReadBytes(paths.assistant_wav) == new_reply);
    assert(access((paths.assistant_wav + ".part").c_str(), F_OK) != 0);
    assert(access((paths.assistant_wav + ".bak").c_str(), F_OK) != 0);

    const AgentTurnPaths interrupted =
        store.Create("20260712", "turn-interrupted-swap");
    assert(interrupted.valid());
    WriteFile(interrupted.user_wav, user_wav);
    assert(store.MarkRecorded(
        interrupted, user_wav.size(), user_hash, 1783821660000ULL));
    const std::string manifest_backup = interrupted.manifest + ".bak";
    assert(renameat(AT_FDCWD,
                    interrupted.manifest.c_str(),
                    AT_FDCWD,
                    manifest_backup.c_str()) == 0);

    AgentTurnStore restarted(root);
    const auto pending = restarted.ListPending();
    assert(pending.size() == 1);
    assert(pending[0].paths.turn_id == interrupted.turn_id);
    assert(restarted.UpdateState(
        interrupted, AgentTurnStatus::kSending));
    assert(access(interrupted.manifest.c_str(), F_OK) == 0);
    assert(access(manifest_backup.c_str(), F_OK) != 0);
    return 0;
}
