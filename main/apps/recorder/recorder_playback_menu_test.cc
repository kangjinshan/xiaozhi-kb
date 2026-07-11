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
    Check(RecorderWavMatchesCodec(info, 24000, 1), "recorder wav matches codec");
}

void TestUnsupportedWavHeaderIsRejected() {
    uint8_t header[44];
    BuildCanonicalWav(header, 3, 1, 24000, 16, 48000);
    RecorderWavInfo info = {};
    Check(!RecorderParseWavHeader(header, sizeof(header), &info), "float wav is rejected");

    BuildCanonicalWav(header, 1, 2, 24000, 16, 48000);
    Check(!RecorderWavMatchesCodec(
              RecorderWavInfo{24000, 2, 16, 44, 48000}, 24000, 1),
          "channel mismatch is rejected");
}

}  // namespace

int main() {
    TestRecordingListSortsNewestFirst();
    TestCanonicalWavHeaderIsAccepted();
    TestUnsupportedWavHeaderIsRejected();
    std::puts("recorder_playback_menu_test passed");
    return 0;
}
