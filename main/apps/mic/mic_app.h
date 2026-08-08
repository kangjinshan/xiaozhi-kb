#ifndef MIC_APP_H_
#define MIC_APP_H_

// Starts the real-time stereo Wi-Fi microphone on a dedicated stack large
// enough for the codec, ESP-SR and UDP packet working frames. The caller
// should return from app_main after this succeeds.
bool StartMicApp();

// Mic task entry; does not return during normal operation.
void RunMicApp();

#endif  // MIC_APP_H_
