#ifndef RECORDER_COMMON_FONT_H_
#define RECORDER_COMMON_FONT_H_

#include <cstddef>

inline constexpr char kRecorderCommonFontAsset[] =
    "font_puhui_common_30_4.bin";

enum class RecorderCommonFontStatus {
    kLoaded,
    kPartitionUnavailable,
    kAssetMissing,
    kDecodeFailed,
};

using RecorderCommonFontGetAsset = bool (*)(const char* name,
                                             void** data,
                                             size_t* size,
                                             void* user_data);
using RecorderCommonFontDecode = const void* (*)(void* data,
                                                  void* user_data);

struct RecorderCommonFontResult {
    RecorderCommonFontStatus status =
        RecorderCommonFontStatus::kPartitionUnavailable;
    const void* font = nullptr;
    size_t asset_bytes = 0;

    bool loaded() const {
        return status == RecorderCommonFontStatus::kLoaded && font != nullptr;
    }
};

RecorderCommonFontResult RecorderLoadCommonFont(
    bool partition_valid,
    RecorderCommonFontGetAsset get_asset,
    void* get_asset_user_data,
    RecorderCommonFontDecode decode,
    void* decode_user_data);

#endif  // RECORDER_COMMON_FONT_H_
