# Recorder Common-Font Conversation History Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show persisted user transcripts and AI replies in Jinshan AI history using Xiaozhi's existing common Chinese font without duplicating its flash payload.

**Architecture:** Add a bounded, host-tested reader for the already-published `turn.json` and attach each text field to its matching WAV history row. Add a dependency-injected common-font asset loader whose host tests cover every fallback, then adapt it to `Assets` and `LvglCBinFont` inside the display module; fixed controls keep the compiled 24 px subset.

**Tech Stack:** C++17, ESP-IDF 5.5.3, LVGL 9, `esp_mmap_assets`, FATFS/SDSPI, host C++ tests.

---

## File Structure

- Create `main/apps/recorder/agent_turn_manifest.h/.cc`: bounded manifest text reader and one-line UTF-8 preview normalization.
- Create `main/apps/recorder/recorder_common_font.h/.cc`: platform-neutral asset/decode orchestration with explicit failure status.
- Create `main/apps/recorder/recorder_common_font_test.cc`: exact asset-name, short-circuit, failure, and success tests.
- Modify `main/apps/recorder/recorder_file_list.h/.cc`: attach transcript/reply previews to matching Agent WAV entries.
- Modify `main/apps/recorder/recorder_playback_menu_test.cc`: fixture manifests and fallback assertions.
- Modify `main/apps/recorder/recorder_display.h/.cc`: carry the conversation-detail flag and use the mmap font only on those rows.
- Modify `main/apps/recorder/recorder_app.cc`: propagate the detail flag into the display model.
- Modify `main/CMakeLists.txt`: compile the two new production sources.
- Modify `README.md`, `docs/recorder-design-guardrails.md`, and `AGENTS.md`: document font reuse, bounded parsing, and new tests.

### Task 1: Read Persisted Conversation Text

**Files:**
- Create: `main/apps/recorder/agent_turn_manifest.h`
- Create: `main/apps/recorder/agent_turn_manifest.cc`
- Modify: `main/apps/recorder/recorder_file_list.h`
- Modify: `main/apps/recorder/recorder_file_list.cc`
- Modify: `main/apps/recorder/recorder_playback_menu_test.cc`

- [x] **Step 1: Write the failing history test**

Extend `TestAgentTurnAudioStaysAdjacentNewestFirst()` to write a published
`turn.json` containing escaped Chinese transcript/reply text and assert:

```cpp
WriteBytes(newer + "/turn.json",
    "{\"transcript\":\"上海明天\\n天气怎么样\"," 
    "\"reply_text\":\"明天晴朗，最高 29℃\"}");
auto entries = RecorderListAgentRecordings(root, 8);
Check(entries[0].conversation_text == "明天晴朗，最高 29℃",
      "assistant row carries reply text");
Check(entries[1].conversation_text == "上海明天 天气怎么样",
      "user row carries normalized transcript");
Check(RecorderFormatRecordingDetail(entries[0]) == entries[0].conversation_text,
      "conversation text replaces byte-size detail");
```

Add separate malformed and oversized manifest fixtures and assert their rows
have empty `conversation_text` and retain a `B`/`KB` size detail.

- [x] **Step 2: Run the test and verify RED**

Run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_playback_menu_test.cc \
  main/apps/recorder/recorder_file_list.cc \
  main/apps/recorder/agent_turn_manifest.cc \
  main/apps/recorder/recorder_wav_file.cc \
  -o /tmp/recorder_playback_menu_test
```

Expected: compilation fails because `conversation_text` and the manifest reader
do not exist.

- [x] **Step 3: Implement the bounded manifest reader**

Define:

```cpp
struct AgentTurnConversation {
    std::string transcript;
    std::string reply_text;
};

bool AgentReadTurnConversation(const std::string& manifest_path,
                               AgentTurnConversation* conversation);
```

Reject null output, non-regular files, empty files, and files larger than 16 KiB.
Read the exact stat size, decode the five escape forms emitted by
`AgentTurnStore`, and accept the manifest if either field parses. Normalize each
preview to one line, trim it, and cap it at 240 bytes without ending on a UTF-8
continuation byte.

- [x] **Step 4: Attach text to the matching WAV entries**

Add `std::string conversation_text` to `RecorderFileEntry`. Read
`turn.json` once per turn directory before iterating audio names, assign
`reply_text` to `assistant.wav` and `transcript` to `user.wav`, and make
`RecorderFormatRecordingDetail()` return non-empty conversation text before its
existing size formatting.

- [x] **Step 5: Run the test and verify GREEN**

Run the Step 2 command and `/tmp/recorder_playback_menu_test`.
Expected: `recorder_playback_menu_test passed`.

- [x] **Step 6: Commit**

```bash
git add main/apps/recorder/agent_turn_manifest.* \
  main/apps/recorder/recorder_file_list.* \
  main/apps/recorder/recorder_playback_menu_test.cc
git commit -m "feat(recorder): show persisted conversation text"
```

### Task 2: Load Xiaozhi's Common Font With Safe Fallback

**Files:**
- Create: `main/apps/recorder/recorder_common_font.h`
- Create: `main/apps/recorder/recorder_common_font.cc`
- Create: `main/apps/recorder/recorder_common_font_test.cc`
- Modify: `main/apps/recorder/recorder_display.h`
- Modify: `main/apps/recorder/recorder_display.cc`
- Modify: `main/apps/recorder/recorder_app.cc`
- Modify: `main/CMakeLists.txt`

- [x] **Step 1: Write the failing loader test**

Define fake asset and decoder callbacks, then assert that
`RecorderLoadCommonFont()`:

```cpp
Check(std::string(kRecorderCommonFontAsset) ==
          "font_puhui_common_30_4.bin",
      "loader reuses the board's Xiaozhi font");
Check(RecorderLoadCommonFont(false, get, nullptr, decode, nullptr).status ==
          RecorderCommonFontStatus::kPartitionUnavailable,
      "invalid partition short-circuits");
Check(RecorderLoadCommonFont(true, missing, nullptr, decode, nullptr).status ==
          RecorderCommonFontStatus::kAssetMissing,
      "missing asset falls back");
Check(RecorderLoadCommonFont(true, get, nullptr, failed_decode, nullptr).status ==
          RecorderCommonFontStatus::kDecodeFailed,
      "decode failure falls back");
Check(RecorderLoadCommonFont(true, get, nullptr, decode, nullptr).loaded(),
      "valid mmap font is selected");
```

- [x] **Step 2: Run the loader test and verify RED**

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_common_font_test.cc \
  main/apps/recorder/recorder_common_font.cc \
  -o /tmp/recorder_common_font_test
```

Expected: compilation fails because the loader contract does not exist.

- [x] **Step 3: Implement the platform-neutral loader**

Expose function-pointer types for `GetAssetData` and decode, an enum with
`kLoaded`, `kPartitionUnavailable`, `kAssetMissing`, and `kDecodeFailed`, and a
result carrying `const void* font` plus asset byte size. It must invoke callbacks
only when prerequisites succeeded and request exactly
`font_puhui_common_30_4.bin`.

- [x] **Step 4: Run the loader test and verify GREEN**

Run the Step 2 command and `/tmp/recorder_common_font_test`.
Expected: `recorder_common_font_test passed`.

- [x] **Step 5: Adapt the loader to Assets/LVGL**

In `recorder_display.cc`, after `lv_init()` and before building the widget tree:

```cpp
auto& assets = Assets::GetInstance();
const auto result = RecorderLoadCommonFont(
    assets.partition_valid(), GetRecorderFontAsset, &assets,
    DecodeRecorderFontAsset, nullptr);
```

The decode adapter owns one `std::unique_ptr<LvglCBinFont>` for the whole mode.
Default the dynamic detail font to `font_puhui_basic_20_4`; replace it with the
loaded `lv_font_t*` only for `kLoaded`. Log every fallback without failing display
initialization.

Add `bool conversation_detail` to `RecorderDisplayMenuItem`, set it from
`!entry.conversation_text.empty()` in `ShowPlaybackMenu()`, and choose the common
font only for those details. Existing size details and every fixed label retain
their current fonts.

- [x] **Step 6: Add firmware sources and commit**

Append `agent_turn_manifest.cc` and `recorder_common_font.cc` to the recorder
source list in `main/CMakeLists.txt`, then commit:

```bash
git add main/CMakeLists.txt main/apps/recorder/recorder_common_font.* \
  main/apps/recorder/recorder_display.* main/apps/recorder/recorder_app.cc
git commit -m "feat(recorder): reuse Xiaozhi common Chinese font"
```

### Task 3: Document and Verify the Integrated Result

**Files:**
- Modify: `README.md`
- Modify: `docs/recorder-design-guardrails.md`
- Modify: `AGENTS.md`

- [ ] **Step 1: Document the new invariant**

State that dynamic history text is read only from published, bounded
`turn.json`, uses the mmap common font, and falls back non-fatally. Add the common
font host-test command to the required gate and update the playback-menu command
to compile `agent_turn_manifest.cc`.

- [ ] **Step 2: Run all recorder host tests**

Run every command in `docs/recorder-design-guardrails.md`, including the new
common-font test. Expected: all tests exit 0.

- [ ] **Step 3: Run the complete firmware build and size checks**

```bash
source ~/esp/esp-idf/export.sh
idf.py build
nm build/esp-idf/main/libmain.a | rg ' U ns_(create|pro_create)$'
```

Expected: ESP32-C6 build succeeds, generated assets still contain
`font_puhui_common_30_4.bin`, application and assets images fit their partitions,
and the expected noise-suppression symbol remains linked.

- [ ] **Step 4: Flash and run safe serial verification**

```bash
idf.py -p /dev/cu.usbmodem1101 flash
/tmp/ser-venv/bin/python scripts/verify_agent_voice_runtime.py \
  --port-glob /dev/cu.usbmodem1101 --duration 30
```

Use `dtr=True, rts=False`. Verify successful assets font loading, SD mount, WSS
ready, no SPI assertion/reboot/stack fault, and a history row showing persisted
Chinese conversation text while still playing the correct WAV.

- [ ] **Step 5: Commit documentation**

```bash
git add README.md docs/recorder-design-guardrails.md AGENTS.md
git commit -m "docs(recorder): document common-font history"
```
