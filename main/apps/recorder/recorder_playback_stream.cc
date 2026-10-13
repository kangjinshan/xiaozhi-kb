#include "recorder_playback_stream.h"
#include <algorithm>

bool RecorderPlaybackStream::Open(const std::string& path,
                                  int output_rate, int output_channels) {
    Close();
    // Only fully published WAVs are playable, never a partial download/backup.
    if (path.size() < 4 || path.substr(path.size() - 4) != ".wav") return false;
    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) return false;
    uint8_t header[44];
    const bool valid = std::fread(header, 1, sizeof(header), file_) == sizeof(header) &&
        RecorderParseWavHeader(header, sizeof(header), &info_) &&
        RecorderWavCanPlay(info_, output_rate, output_channels) &&
        info_.data_bytes % (info_.channels * sizeof(int16_t)) == 0;
    if (!valid || std::fseek(file_, 0, SEEK_END) != 0 ||
        std::ftell(file_) != static_cast<long>(info_.data_offset + info_.data_bytes) ||
        std::fseek(file_, info_.data_offset, SEEK_SET) != 0) {
        Close();
        return false;
    }
    remaining_ = info_.data_bytes;
    return true;
}

bool RecorderPlaybackStream::Read(std::vector<int16_t>* samples) {
    if (file_ == nullptr || samples == nullptr) return false;
    const auto bytes = std::min<size_t>(remaining_, 1024 * sizeof(int16_t));
    samples->resize(bytes / sizeof(int16_t));
    if (bytes != 0 && std::fread(samples->data(), 1, bytes, file_) != bytes) {
        samples->clear();
        Close();
        return false;
    }
    remaining_ -= bytes;
    return true;
}

void RecorderPlaybackStream::Close() {
    if (file_ != nullptr) std::fclose(file_);
    file_ = nullptr;
    remaining_ = 0;
}
