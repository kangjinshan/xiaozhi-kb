#ifndef RECORDER_PLAYBACK_STREAM_H_
#define RECORDER_PLAYBACK_STREAM_H_

#include "recorder_wav_file.h"
#include <cstdio>
#include <string>
#include <vector>

// Each Read consumes at most 1024 samples. Not calling Read pauses at the exact
// next sample, leaving the application free to capture audio and service WSS.
// The caller owns the display/SD lock and the separate rate converter.
class RecorderPlaybackStream {
public:
    ~RecorderPlaybackStream() { Close(); }
    bool Open(const std::string& path, int output_rate, int output_channels);
    bool Read(std::vector<int16_t>* samples);
    void Close();
    bool is_open() const { return file_ != nullptr; }
    bool done() const { return remaining_ == 0; }
    int sample_rate() const { return info_.sample_rate; }

private:
    FILE* file_ = nullptr;
    RecorderWavInfo info_{};
    uint32_t remaining_ = 0;
};

#endif
