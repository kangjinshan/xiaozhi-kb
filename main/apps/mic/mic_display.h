#ifndef MIC_DISPLAY_H_
#define MIC_DISPLAY_H_

#include <cstdint>
#include <string>

struct MicDisplayModel {
    bool recording = false;
    bool wifi_connected = false;
    bool wifi_streaming = false;
    bool noise_reduction = false;
    bool sd_ready = false;
    int gain_percent = 70;
    uint32_t elapsed_seconds = 0;
    std::string filename;
};

using MicDisplayCallback = void (*)(void* user_data);

struct MicDisplayCallbacks {
    MicDisplayCallback toggle_recording = nullptr;
    MicDisplayCallback exit = nullptr;
};

// Reuses the standalone recorder SH8601/CST9217 driver and replaces its UI
// tree with the microphone controls.
bool MicDisplayInit(const MicDisplayCallbacks& callbacks, void* user_data);
void MicDisplayRender(const MicDisplayModel& model);

#endif  // MIC_DISPLAY_H_
