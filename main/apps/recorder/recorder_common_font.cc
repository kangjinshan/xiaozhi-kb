#include "recorder_common_font.h"

RecorderCommonFontResult RecorderLoadCommonFont(
    bool partition_valid,
    RecorderCommonFontGetAsset get_asset,
    void* get_asset_user_data,
    RecorderCommonFontDecode decode,
    void* decode_user_data) {
    RecorderCommonFontResult result;
    if (!partition_valid) {
        result.status = RecorderCommonFontStatus::kPartitionUnavailable;
        return result;
    }

    void* data = nullptr;
    size_t size = 0;
    if (get_asset == nullptr ||
        !get_asset(kRecorderCommonFontAsset, &data, &size,
                   get_asset_user_data) ||
        data == nullptr || size == 0) {
        result.status = RecorderCommonFontStatus::kAssetMissing;
        return result;
    }

    result.asset_bytes = size;
    if (decode == nullptr) {
        result.status = RecorderCommonFontStatus::kDecodeFailed;
        return result;
    }
    result.font = decode(data, decode_user_data);
    if (result.font == nullptr) {
        result.status = RecorderCommonFontStatus::kDecodeFailed;
        return result;
    }
    result.status = RecorderCommonFontStatus::kLoaded;
    return result;
}
