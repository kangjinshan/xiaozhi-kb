# Recorder Common-Font Conversation History Design

**Date:** 2026-07-12
**Target:** Waveshare ESP32-C6-Touch-AMOLED-2.16 Jinshan AI mode

## Goal

Reuse the Chinese font already shipped for Xiaozhi so Jinshan AI history can
show the user's transcript and the assistant's reply instead of looking like a
list of audio files. Preserve the current recording, WSS, SD durability, and
automatic playback behavior.

## Chosen Direction

Three approaches were evaluated:

1. Expand the compiled 24 px assistant subset for every possible reply. This is
   impossible for dynamic text and would keep requiring firmware rebuilds.
2. Link or package a second full Puhui font in the application. The current app
   partition does not have enough headroom, and duplicating the font wastes
   flash already reserved in the assets partition.
3. Memory-map Xiaozhi's existing `font_puhui_common_30_4.bin` from the assets
   partition and use it only for dynamic conversation text. This adds no font
   payload and keeps fixed 24 px labels unchanged.

Direction 3 is selected. The shipped common font covers 5,259 Han characters,
which is suitable for ordinary Mandarin conversation but is not claimed to be
a complete Unicode CJK font.

## Data Flow

The service already returns `transcript` and `reply_text`. `AgentTurnStore`
commits both fields to each completed turn's `turn.json` beside `user.wav` and
`assistant.wav`.

When the user opens `历史`, the existing recorder main task scans SD while LVGL
is paused. For every turn directory it reads the bounded manifest once and maps:

- `user.wav` → label `你`, detail = `transcript`;
- `assistant.wav` → label `AI 回复`, detail = `reply_text`.

If the manifest is absent, malformed, too large, or does not contain usable
text, the row keeps its current byte-size detail. Legacy `/sdcard/rec/recN.wav`
rows are unchanged.

No network callback, LVGL callback, or codec callback reads SD. No new data is
written and the turn schema remains unchanged.

## Font Loading

After LVGL initializes and before history rows are created,
`recorder_display.cc` obtains `font_puhui_common_30_4.bin` through the existing
`Assets` mmap API and constructs an `LvglCBinFont`. The wrapper remains alive for
the lifetime of Jinshan AI mode.

Only the history detail line uses this font. Its 38 px line height fits together
with the 25 px semantic label in the existing 82 px row. Fixed controls continue
using `font_puhui_assistant_24_4`, preserving current geometry and appearance.

Failure is non-fatal. Missing/invalid assets leave the detail line on the built-in
20 px font and emit a serial warning; recording, storage, networking, history
playback, and menu navigation continue to work.

## Parsing and Bounds

Manifest reads are capped at 16 KiB. The parser accepts the JSON string escaping
written by `AgentTurnStore` (`\\`, `\"`, `\n`, `\r`, and `\t`) and rejects
unterminated strings. Embedded control characters are normalized to spaces for
the one-line history preview. Text remains UTF-8 and LVGL's long-dot mode clips
it to the row width without allocating a second unbounded display buffer.

Incomplete `.part` and recovery `.bak` files are never read or listed. Only the
published `turn.json`, `user.wav`, and `assistant.wav` names participate.

## Testing

Host tests prove that:

- completed manifests populate the correct user and assistant detail;
- JSON escapes are decoded for display;
- missing/malformed/oversized manifests fall back to file size;
- legacy recordings and existing newest-first ordering remain unchanged;
- `.part` files remain hidden.

The ESP-IDF build proves the Assets/LVGL integration and partition fit. Device
acceptance verifies that history renders ordinary Chinese transcript/reply text,
rows still play the correct WAV, and startup has no mmap, SPI2, stack, or reboot
failure.

## Non-Goals

- Complete rare-character CJK coverage.
- Streaming transcript or response text while a turn is active.
- Changing the Agent protocol or SD manifest schema.
- Adding a second font payload to the app or assets partition.
- Reading SD from display or network callbacks.
