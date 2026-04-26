# Changelog

All notable changes to **esp_mp3_player** will be documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] - 2026-04-26

### Fixed

- **Audible glitches / stutter on weak networks.** The single-task pipeline in
  v0.1.0 wrote decoded PCM straight into the codec, so any HTTP read stall
  (TLS retransmit, 4G handover, etc.) longer than the I2S DMA depth (~60 ms
  on most ESP32-S3 boards) caused a DMA underrun and an audible click.

### Changed

- Pipeline is now three stages: **Download → 128 KB compressed ring →
  Decode → 32 KB PCM ring → Output → audio sink**.
- New `OutputTask` (Core 0, priority 7, 4 KB internal-RAM stack) drains the
  PCM ring at codec-paced cadence. Decode jitter and HTTP read timeouts are
  fully absorbed by the ~340 ms cushion of the PCM ring on top of the codec's
  own DMA buffer.
- Decode task no longer calls `IAudioOutput::OutputData` directly; it pushes
  PCM bytes to the new ring instead.
- `AbortAndJoin` now waits for all three tasks (download / decode / output)
  before releasing the rings, preventing use-after-free during abort.

### Memory impact (vs. 0.1.0)

- PSRAM: +32 KB (PCM ring).
- Internal RAM: +4 KB (Output task stack).

## [0.1.0] - 2026-04-26

### Added

- Initial public release.
- `mydazy::Mp3Player` — single-instance streaming MP3 player.
- Abstract injection points: `IAudioOutput`, `IHttpClient`, `IHttpFactory`, `Callbacks`.
- Lifecycle: `Initialize()` → `Play(url, title)` → `Stop()` with cooperative abort and TLS-read tolerant join.
- Pipeline: HTTP chunked GET → 128 KB PSRAM ring buffer → `esp_audio_dec` MP3 decoder → optional `esp_ae_rate_cvt` resample → stereo→mono fold → consumer `IAudioOutput::OutputData`.
- Memory profile: zero increment to internal SRAM (all buffers and task stacks in PSRAM, except the decode task which runs on Core 0 with a stack on internal RAM at priority 7 to avoid I2S DMA underrun).
- Built and tested on ESP-IDF 5.3+ with ESP32-S3.
