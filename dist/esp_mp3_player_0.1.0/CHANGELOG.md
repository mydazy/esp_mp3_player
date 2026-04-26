# Changelog

All notable changes to **esp_mp3_player** will be documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2026-04-26

### Added

- Initial public release.
- `mydazy::Mp3Player` — single-instance streaming MP3 player.
- Abstract injection points: `IAudioOutput`, `IHttpClient`, `IHttpFactory`, `Callbacks`.
- Lifecycle: `Initialize()` → `Play(url, title)` → `Stop()` with cooperative abort and TLS-read tolerant join.
- Pipeline: HTTP chunked GET → 128 KB PSRAM ring buffer → `esp_audio_dec` MP3 decoder → optional `esp_ae_rate_cvt` resample → stereo→mono fold → consumer `IAudioOutput::OutputData`.
- Memory profile: zero increment to internal SRAM (all buffers and task stacks in PSRAM, except the decode task which runs on Core 0 with a stack on internal RAM at priority 7 to avoid I2S DMA underrun).
- Built and tested on ESP-IDF 5.3+ with ESP32-S3.
