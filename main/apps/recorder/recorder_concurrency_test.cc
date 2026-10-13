#include "recorder_turn_queue.h"
#include "recorder_playback_stream.h"
#include "recorder_amr_frame.h"

#include <cassert>
#include <cstring>
#include <unistd.h>

static void WriteWav(const std::string& path, const std::vector<int16_t>& pcm) {
    uint8_t header[44] = {};
    auto put = [&](size_t offset, uint32_t value, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) header[offset + i] = value >> (8 * i);
    };
    std::memcpy(header, "RIFF", 4);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    std::memcpy(header + 36, "data", 4);
    put(4, 36 + pcm.size() * 2, 4);
    put(16, 16, 4); put(20, 1, 2); put(22, 1, 2);
    put(24, 16000, 4); put(28, 32000, 4);
    put(32, 2, 2); put(34, 16, 2); put(40, pcm.size() * 2, 4);
    FILE* file = fopen(path.c_str(), "wb");
    assert(file);
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    assert(fwrite(pcm.data(), 2, pcm.size(), file) == pcm.size());
    fclose(file);
}

int main() {
    assert(RecorderAmrFrameSize(0x3c)==32 && RecorderAmrFrameSize(0x7c)==1);
    assert(RecorderAmrFrameSize(0x3d)==0 && RecorderAmrFrameSize(9<<3)==0);
    char pattern[] = "/tmp/shenyong-concurrency-XXXXXX";
    const char* root = mkdtemp(pattern);
    assert(root);
    AgentTurnStore store(root);
    RecorderTurnQueue queue;
    std::vector<int16_t> audio(5500);
    for (size_t i = 0; i < audio.size(); ++i) audio[i] = i;
    const std::string hash(64, 'a');
    const auto a = store.Create("20261013", "a");
    WriteWav(a.user_audio, audio);
    assert(store.MarkRecorded(a, audio.size() * 2 + 44, hash, 1));
    for (const auto& turn : store.ListPending()) queue.Push(turn);
    assert(store.BeginReply(a, 6, hash));
    const uint8_t reply[] = {1, 2, 3, 4, 5, 6};
    assert(store.AppendReply(reply, 3));

    // B and C finish while A's reply is still arriving. Appending must not
    // change A's ownership or invoke startup recovery on its live .part file.
    for (const char* name : {"b", "c"}) {
        auto paths = store.Create("20261013", name);
        WriteWav(paths.user_audio, audio);
        assert(store.MarkRecorded(paths, audio.size() * 2 + 44, hash, 2));
        queue.Push({paths, AgentTurnStatus::kRecorded, audio.size() * 2 + 44, hash, 2});
        assert(queue.active().paths.turn_id == "a");
        assert(access((a.assistant_wav + ".part").c_str(), F_OK) == 0);
    }
    queue.Push(queue.active());
    assert(queue.size() == 3);
    assert(store.AppendReply(reply + 3, 3));
    assert(store.CommitReply("hello", "reply", 6, hash, "2026-10-13T08:00:00Z"));
    queue.FinishActive();
    assert(queue.active().paths.turn_id == "b");
    assert(store.MarkFailed(queue.active().paths, "speech_not_recognized"));
    queue.FinishActive();
    assert(queue.active().paths.turn_id == "c");
    assert(store.ListPending().size() == 1);  // Restart keeps C, not A or B.

    RecorderPlaybackStream playback;
    assert(playback.Open(a.user_audio, 24000, 1));
    std::vector<int16_t> received, block;
    assert(playback.Read(&block) && block.size() <= 1024);
    received.insert(received.end(), block.begin(), block.end());
    // Capture a new turn while the open playback stream is suspended.
    auto d = store.Create("20261013", "d");
    WriteWav(d.user_audio, audio);
    assert(store.MarkRecorded(d, audio.size() * 2 + 44, hash, 3));
    while (!playback.done()) {
        assert(playback.Read(&block) && block.size() <= 1024);
        received.insert(received.end(), block.begin(), block.end());
    }
    assert(received == audio);  // Resume has no skipped/repeated samples.
    playback.Close();
    assert(!playback.Open(a.assistant_wav + ".part", 24000, 1));
    assert(truncate(a.user_audio.c_str(), 100) == 0);
    assert(!playback.Open(a.user_audio, 24000, 1));
    puts("recorder_concurrency_test passed");
}
