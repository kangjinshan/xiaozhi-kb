#include "recorder_file_list.h"
#include "recorder_wav_file.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void WriteBytes(const std::string& path, const char* bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    Check(f != nullptr, "create temp file");
    std::fwrite(bytes, 1, std::strlen(bytes), f);
    std::fclose(f);
}

void WriteText(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    Check(f != nullptr, "create temp text file");
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

void PutLe16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void PutLe32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void BuildCanonicalWav(uint8_t* header,
                       uint16_t format,
                       uint16_t channels,
                       uint32_t sample_rate,
                       uint16_t bits_per_sample,
                       uint32_t data_bytes) {
    std::memset(header, 0, 44);
    std::memcpy(header + 0, "RIFF", 4);
    PutLe32(header + 4, 36 + data_bytes);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    PutLe32(header + 16, 16);
    PutLe16(header + 20, format);
    PutLe16(header + 22, channels);
    PutLe32(header + 24, sample_rate);
    PutLe32(header + 28, sample_rate * channels * (bits_per_sample / 8));
    PutLe16(header + 32, channels * (bits_per_sample / 8));
    PutLe16(header + 34, bits_per_sample);
    std::memcpy(header + 36, "data", 4);
    PutLe32(header + 40, data_bytes);
}

void TestRecordingListSortsNewestFirst() {
    char dir_template[] = "/tmp/recorder-menu-test-XXXXXX";
    char* dir = mkdtemp(dir_template);
    Check(dir != nullptr, "mkdtemp");

    WriteBytes(std::string(dir) + "/rec2.wav", "two");
    WriteBytes(std::string(dir) + "/rec10.wav", "ten");
    WriteBytes(std::string(dir) + "/rec1.wav", "one");
    WriteBytes(std::string(dir) + "/note.wav", "ignored");
    WriteBytes(std::string(dir) + "/rec3.txt", "ignored");

    auto entries = RecorderListRecordings(dir, 8);
    Check(entries.size() == 3, "only recN.wav files are listed");
    Check(entries[0].name == "rec10.wav", "newest recording is first");
    Check(entries[1].name == "rec2.wav", "second newest recording is next");
    Check(entries[2].name == "rec1.wav", "oldest recording is last");
    Check(entries[0].path == std::string(dir) + "/rec10.wav", "entry has full path");
    Check(RecorderFormatRecordingDetail(entries[0]).find("B") != std::string::npos,
          "detail includes file size");

    unlink((std::string(dir) + "/rec2.wav").c_str());
    unlink((std::string(dir) + "/rec10.wav").c_str());
    unlink((std::string(dir) + "/rec1.wav").c_str());
    unlink((std::string(dir) + "/note.wav").c_str());
    unlink((std::string(dir) + "/rec3.txt").c_str());
    rmdir(dir);
}

void TestAgentTurnAudioStaysAdjacentNewestFirst() {
    char dir_template[] = "/tmp/recorder-agent-menu-test-XXXXXX";
    char* root = mkdtemp(dir_template);
    Check(root != nullptr, "mkdtemp agent root");
    const std::string date = std::string(root) + "/20260712";
    const std::string older = date + "/turn-100";
    const std::string newer = date + "/turn-200";
    Check(mkdir(date.c_str(), 0700) == 0, "create date directory");
    Check(mkdir(older.c_str(), 0700) == 0, "create older turn");
    Check(mkdir(newer.c_str(), 0700) == 0, "create newer turn");
    WriteBytes(older + "/user.wav", "old-user");
    WriteBytes(older + "/assistant.wav", "old-assistant");
    WriteBytes(newer + "/user.wav", "new-user");
    WriteBytes(newer + "/assistant.wav", "new-assistant");
    WriteBytes(newer + "/assistant.wav.part", "ignored");
    WriteBytes(newer + "/turn.json",
               "{\"transcript\":\"上海明天\\n天气怎么样\","
               "\"reply_text\":\"明天晴朗，最高 29℃\"}");

    auto entries = RecorderListAgentRecordings(root, 8);
    Check(entries.size() == 4, "both complete files from each turn are listed");
    Check(entries[0].turn_id == "turn-200" && entries[1].turn_id == "turn-200",
          "newest turn files remain adjacent");
    Check(entries[0].name == "assistant.wav" && entries[1].name == "user.wav",
          "assistant and user recordings have stable ordering");
    Check(RecorderConversationLabel(entries[0]) == "AI 回复",
          "assistant audio is labeled as an AI reply");
    Check(RecorderConversationLabel(entries[1]) == "你",
          "user audio is labeled as the user");
    Check(entries[0].conversation_text == "明天晴朗，最高 29℃",
          "assistant row carries reply text");
    Check(entries[1].conversation_text == "上海明天 天气怎么样",
          "user row carries normalized transcript");
    Check(RecorderFormatRecordingDetail(entries[0]) == entries[0].conversation_text,
          "conversation text replaces byte-size detail");
    Check(entries[2].turn_id == "turn-100" && entries[3].turn_id == "turn-100",
          "older turn follows newest turn");
    Check(entries[2].conversation_text.empty(),
          "turn without a published manifest keeps empty conversation text");
    Check(RecorderFormatRecordingDetail(entries[2]).find("B") != std::string::npos,
          "turn without conversation text keeps byte-size detail");

    RecorderFileEntry legacy;
    legacy.name = "rec7.wav";
    Check(RecorderConversationLabel(legacy) == "录音",
          "legacy audio has a neutral label");

    for (const auto& entry : entries) {
        unlink(entry.path.c_str());
    }
    unlink((newer + "/assistant.wav.part").c_str());
    unlink((newer + "/turn.json").c_str());
    rmdir(newer.c_str());
    rmdir(older.c_str());
    rmdir(date.c_str());
    rmdir(root);
}

void TestAgentHistoryUsesDurableCompletionOrderBeforeTruncating() {
    char dir_template[] = "/tmp/recorder-agent-index-menu-test-XXXXXX";
    char* root = mkdtemp(dir_template);
    Check(root != nullptr, "mkdtemp indexed history root");
    const std::string date = std::string(root) + "/19700101";
    const std::string older = date + "/turn-lu-ffffff00";
    const std::string newer = date + "/turn-lu-00000001";
    Check(mkdir(date.c_str(), 0700) == 0, "create indexed history date");
    Check(mkdir(older.c_str(), 0700) == 0, "create indexed older turn");
    Check(mkdir(newer.c_str(), 0700) == 0, "create indexed newer turn");

    WriteBytes(older + "/user.wav", "old-user");
    WriteBytes(older + "/assistant.wav", "old-assistant");
    WriteBytes(older + "/turn.json",
               "{\"transcript\":\"旧问题\",\"reply_text\":\"旧回答\"}");
    WriteBytes(newer + "/user.wav", "new-user");
    WriteBytes(newer + "/assistant.wav", "new-assistant");
    WriteBytes(newer + "/turn.json",
               "{\"transcript\":\"最近问题\",\"reply_text\":\"最近回答\"}");
    WriteText(std::string(root) + "/turns.jsonl",
              "{\"turn_id\":\"turn-lu-ffffff00\",\"date\":\"19700101\","
              "\"status\":\"complete\",\"created_at_ms\":100}\n"
              "{\"turn_id\":\"turn-lu-00000001\",\"date\":\"19700101\","
              "\"status\":\"complete\",\"created_at_ms\":200}\n");

    const auto entries = RecorderListAgentRecordings(root, 2);
    Check(entries.size() == 2, "row limit keeps one complete turn");
    Check(entries[0].turn_id == "turn-lu-00000001" &&
              entries[1].turn_id == "turn-lu-00000001",
          "durable index keeps the actual latest two messages");
    Check(entries[0].name == "assistant.wav" && entries[1].name == "user.wav",
          "latest AI and user messages stay adjacent at the row limit");

    const auto odd_limit_entries = RecorderListAgentRecordings(root, 3);
    Check(odd_limit_entries.size() == 2,
          "an odd row limit never exposes half of the next turn");
    Check(odd_limit_entries[0].turn_id == "turn-lu-00000001" &&
              odd_limit_entries[1].turn_id == "turn-lu-00000001",
          "pair-preserving truncation keeps the newest complete turn");

    for (const auto& entry : entries) {
        unlink(entry.path.c_str());
    }
    unlink((older + "/user.wav").c_str());
    unlink((older + "/assistant.wav").c_str());
    unlink((older + "/turn.json").c_str());
    unlink((newer + "/turn.json").c_str());
    unlink((std::string(root) + "/turns.jsonl").c_str());
    rmdir(newer.c_str());
    rmdir(older.c_str());
    rmdir(date.c_str());
    rmdir(root);
}

void TestInvalidManifestFallsBackToAudioSize() {
    char dir_template[] = "/tmp/recorder-agent-invalid-menu-test-XXXXXX";
    char* root = mkdtemp(dir_template);
    Check(root != nullptr, "mkdtemp invalid manifest root");
    const std::string date = std::string(root) + "/20260712";
    const std::string malformed = date + "/turn-malformed";
    const std::string oversized = date + "/turn-oversized";
    Check(mkdir(date.c_str(), 0700) == 0, "create invalid manifest date");
    Check(mkdir(malformed.c_str(), 0700) == 0, "create malformed turn");
    Check(mkdir(oversized.c_str(), 0700) == 0, "create oversized turn");

    WriteBytes(malformed + "/user.wav", "malformed-user");
    WriteBytes(malformed + "/turn.json", "{\"transcript\":\"unterminated");
    WriteBytes(oversized + "/assistant.wav", "oversized-assistant");
    WriteText(oversized + "/turn.json", std::string(16 * 1024 + 1, 'x'));

    const auto entries = RecorderListAgentRecordings(root, 8);
    Check(entries.size() == 2, "invalid manifest audio remains listed");
    for (const auto& entry : entries) {
        Check(entry.conversation_text.empty(),
              "invalid manifest never supplies conversation text");
        Check(RecorderFormatRecordingDetail(entry).find("B") != std::string::npos,
              "invalid manifest falls back to byte-size detail");
        unlink(entry.path.c_str());
    }

    unlink((malformed + "/turn.json").c_str());
    unlink((oversized + "/turn.json").c_str());
    rmdir(malformed.c_str());
    rmdir(oversized.c_str());
    rmdir(date.c_str());
    rmdir(root);
}

void TestCanonicalWavHeaderIsAccepted() {
    uint8_t header[44];
    BuildCanonicalWav(header, 1, 1, 24000, 16, 48000);

    RecorderWavInfo info = {};
    Check(RecorderParseWavHeader(header, sizeof(header), &info), "valid recorder wav is accepted");
    Check(info.sample_rate == 24000, "sample rate parsed");
    Check(info.channels == 1, "channel count parsed");
    Check(info.bits_per_sample == 16, "bits parsed");
    Check(info.data_offset == 44, "canonical data offset parsed");
    Check(info.data_bytes == 48000, "data bytes parsed");
    Check(RecorderWavCanPlay(info, 24000, 1), "recorder wav is playable");
}

void TestUnsupportedWavHeaderIsRejected() {
    uint8_t header[44];
    BuildCanonicalWav(header, 3, 1, 24000, 16, 48000);
    RecorderWavInfo info = {};
    Check(!RecorderParseWavHeader(header, sizeof(header), &info), "float wav is rejected");

    Check(RecorderWavCanPlay(
              RecorderWavInfo{24000, 1, 16, 44, 48000}, 24000, 1),
          "native 24k wav is playable");
    Check(RecorderWavCanPlay(
              RecorderWavInfo{16000, 1, 16, 44, 32000}, 24000, 1),
          "enhanced 16k wav is playable through resampler");
    Check(!RecorderWavCanPlay(
              RecorderWavInfo{22050, 1, 16, 44, 44100}, 24000, 1),
          "unsupported sample rate is rejected");
    Check(!RecorderWavCanPlay(
              RecorderWavInfo{16000, 2, 16, 44, 64000}, 24000, 1),
          "channel mismatch is rejected");
}

}  // namespace

int main() {
    TestRecordingListSortsNewestFirst();
    TestAgentTurnAudioStaysAdjacentNewestFirst();
    TestAgentHistoryUsesDurableCompletionOrderBeforeTruncating();
    TestInvalidManifestFallsBackToAudioSize();
    TestCanonicalWavHeaderIsAccepted();
    TestUnsupportedWavHeaderIsRejected();
    std::puts("recorder_playback_menu_test passed");
    return 0;
}
