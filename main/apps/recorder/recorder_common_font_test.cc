#include "recorder_common_font.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct FakeAsset {
    bool available = true;
    bool decode_succeeds = true;
    int get_calls = 0;
    int decode_calls = 0;
    std::string requested_name;
    unsigned char bytes[4] = {1, 2, 3, 4};
    int decoded_font = 42;
};

bool GetAsset(const char* name,
              void** data,
              size_t* size,
              void* user_data) {
    auto* fake = static_cast<FakeAsset*>(user_data);
    ++fake->get_calls;
    fake->requested_name = name == nullptr ? "" : name;
    if (!fake->available || data == nullptr || size == nullptr) {
        return false;
    }
    *data = fake->bytes;
    *size = sizeof(fake->bytes);
    return true;
}

const void* DecodeFont(void* data, void* user_data) {
    auto* fake = static_cast<FakeAsset*>(user_data);
    ++fake->decode_calls;
    if (!fake->decode_succeeds || data != fake->bytes) {
        return nullptr;
    }
    return &fake->decoded_font;
}

void TestUnavailablePartitionShortCircuits() {
    FakeAsset fake;
    const auto result = RecorderLoadCommonFont(
        false, GetAsset, &fake, DecodeFont, &fake);
    Check(result.status == RecorderCommonFontStatus::kPartitionUnavailable,
          "invalid partition reports unavailable");
    Check(!result.loaded(), "invalid partition uses fallback");
    Check(fake.get_calls == 0 && fake.decode_calls == 0,
          "invalid partition invokes no callbacks");
}

void TestMissingAssetFallsBack() {
    FakeAsset fake;
    fake.available = false;
    const auto result = RecorderLoadCommonFont(
        true, GetAsset, &fake, DecodeFont, &fake);
    Check(result.status == RecorderCommonFontStatus::kAssetMissing,
          "missing asset reports missing");
    Check(!result.loaded(), "missing asset uses fallback");
    Check(fake.get_calls == 1 && fake.decode_calls == 0,
          "missing asset is not decoded");
    Check(fake.requested_name == "font_puhui_common_30_4.bin",
          "loader requests Xiaozhi common font");
}

void TestDecodeFailureFallsBack() {
    FakeAsset fake;
    fake.decode_succeeds = false;
    const auto result = RecorderLoadCommonFont(
        true, GetAsset, &fake, DecodeFont, &fake);
    Check(result.status == RecorderCommonFontStatus::kDecodeFailed,
          "decode failure reports failure");
    Check(!result.loaded(), "decode failure uses fallback");
    Check(fake.get_calls == 1 && fake.decode_calls == 1,
          "valid asset reaches decoder once");
}

void TestValidAssetReturnsDecodedFont() {
    FakeAsset fake;
    const auto result = RecorderLoadCommonFont(
        true, GetAsset, &fake, DecodeFont, &fake);
    Check(std::string(kRecorderCommonFontAsset) ==
              "font_puhui_common_30_4.bin",
          "public asset name matches board image");
    Check(result.status == RecorderCommonFontStatus::kLoaded,
          "valid asset reports loaded");
    Check(result.loaded(), "valid asset selects common font");
    Check(result.font == &fake.decoded_font, "decoded font pointer is returned");
    Check(result.asset_bytes == sizeof(fake.bytes), "asset size is retained");
}

}  // namespace

int main() {
    TestUnavailablePartitionShortCircuits();
    TestMissingAssetFallsBack();
    TestDecodeFailureFallsBack();
    TestValidAssetReturnsDecodedFont();
    std::puts("recorder_common_font_test passed");
    return 0;
}
