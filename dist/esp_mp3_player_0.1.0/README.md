# esp_mp3_player

[![Component Registry](https://components.espressif.com/components/mydazy/esp_mp3_player/badge.svg)](https://components.espressif.com/components/mydazy/esp_mp3_player)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

Streaming MP3 player for ESP32-S3: **HTTP chunked GET → 128 KB PSRAM ring buffer → MP3 decode → resample → mono PCM → your audio sink**, with cooperative abort and zero internal-SRAM growth.

The audio sink and HTTP transport are **injected via abstract interfaces**, so this component carries no project-specific dependencies (no `Application`/`Board`/codec singletons).

## Features

- ✅ **Streaming pipeline** — start playback as soon as the first MP3 frame arrives; no buffering of the whole file.
- ✅ **Zero internal-SRAM growth** — task stacks and ring buffer live in PSRAM. (The decode task is pinned to Core 0 with a *small* internal-RAM stack at priority 7, the same level as `opus_codec`, to avoid I2S DMA underrun under contention. See [Memory model](#memory-model).)
- ✅ **Cooperative abort** — `Stop()` and re-`Play()` set an abort flag and wait for both download and decode tasks to drain (TLS-read safe — no use-after-free of the ring buffer).
- ✅ **Stereo → mono fold** + **lazy resampler** that adapts to the codec's output rate (auto from first decoded frame's `sample_rate`).
- ✅ **Pluggable transports** — bring your own HTTP client (mbedTLS, esp_http_client, ML307 cellular wrapper, …) and your own audio sink (`AudioCodec`, raw I2S, ESP-ADF pipeline, …).
- ✅ **Error reporting via callback** — never blocks UI; never calls `Application` or `Display` directly.

## Hardware / IDF

| | |
|---|---|
| Targets | ESP32-S3 (PSRAM required) |
| ESP-IDF | ≥ 5.3 |
| External components | `espressif/esp_audio_codec ~2.4.1`, `espressif/esp_audio_effects ~1.2.1` |

## Installation

Add to your project's `idf_component.yml`:

```yaml
dependencies:
  mydazy/esp_mp3_player: "^0.1.0"
```

## Quick start

```cpp
#include "mp3_player.h"
#include <memory>

using namespace mydazy;

// 1. Implement IAudioOutput (wrap whatever your project uses)
class MyAudioOutput : public IAudioOutput {
public:
    int output_sample_rate() const override { return 16000; }
    int output_channels() const override { return 1; }
    bool output_enabled() const override { return enabled_; }
    void EnableOutput(bool enable) override { enabled_ = enable; }
    void OutputData(std::vector<int16_t>& data) override {
        // hand off to your codec / I2S / pipeline
    }
private:
    bool enabled_ = false;
};

// 2. Implement IHttpClient + IHttpFactory (wrap esp_http_client, ML307, …)
class MyHttp : public IHttpClient { /* ... */ };
class MyHttpFactory : public IHttpFactory {
public:
    std::unique_ptr<IHttpClient> CreateHttp() override {
        return std::make_unique<MyHttp>();
    }
};

// 3. Wire it up
static MyAudioOutput audio;
static MyHttpFactory http;

void app_main_cpp() {
    Mp3Player::Callbacks cb;
    cb.on_error = [](const char* status, const char* message) {
        ESP_LOGW("APP", "mp3 error: %s — %s", status, message);
    };

    Mp3Player::GetInstance().Initialize(&audio, &http, cb);

    std::string err;
    if (!Mp3Player::GetInstance().Play("https://example.com/song.mp3", "Demo", &err)) {
        ESP_LOGE("APP", "play failed: %s", err.c_str());
    }
    // ... later
    Mp3Player::GetInstance().Stop();
}
```

A complete project-agnostic example lives under `examples/basic/`.

## Memory model

| Resource | Size | Caps | Notes |
|---|---|---|---|
| Ring buffer | 128 KB | `MALLOC_CAP_SPIRAM` | ≈ 8 s @ 128 kbps; absorbs 4G / network jitter. |
| Download task stack | 6 KB | `MALLOC_CAP_SPIRAM` | Core 0, priority 1. One-shot HTTP I/O — PSRAM stack is safe here. |
| Decode task stack | 10 KB | **internal RAM** | Core 0, priority 7 — same lane as `opus_codec`. Continuous decode + I2S timing → must avoid the [PSRAM-stack ↔ flash-op deadlock](#why-internal-ram-stack-for-decode). |
| HTTP read scratch | 2 KB | `MALLOC_CAP_SPIRAM` | TLS handshake leaves little stack room — keep the buffer off the stack. |
| Decoder I/O buffers | 8 KB + ~4.6 KB | `MALLOC_CAP_SPIRAM` | One MP3 frame = 1152 samples × 2 ch × `int16_t`. |

**Net internal-SRAM increment vs. baseline: ~10 KB (decode task stack only).** Everything else is PSRAM-resident.

### Why internal-RAM stack for decode?

ESP32-S3 cache and PSRAM share the SPI bus. When any flash op happens (NVS write, OTA, partition op), `spi_flash_op_lock()` disables both cache and PSRAM **on both cores** for the duration. Any task scheduled with a PSRAM stack during that window will fault with a "Double Exception" (SP corruption like `SP=0x60100000`).

The decode task is on Core 0 (where flash ops are issued) and runs continuously, so a PSRAM stack would be a stable foot-gun. Internal RAM avoids it. The download task runs only briefly at low priority and tolerates the stall, so PSRAM is fine for it.

## API

### `mydazy::Mp3Player`

```cpp
class Mp3Player {
public:
    static Mp3Player& GetInstance();

    // One-time init. Pointers must outlive the player (typically static/singleton).
    void Initialize(IAudioOutput* audio, IHttpFactory* http,
                    const Callbacks& cb = {});

    // Start streaming. Auto-aborts any previous playback.
    // err_msg: synchronous failure reason (URL invalid / not initialized /
    //          PSRAM exhaustion / task creation). Async failures (HTTP 4xx/5xx,
    //          decoder errors) go through Callbacks::on_error.
    bool Play(const std::string& url,
              const std::string& title = "",
              std::string* err_msg = nullptr);

    void Stop();
    bool IsPlaying() const;
    std::string GetCurrentTitle() const;
};
```

### Injection interfaces

```cpp
struct IAudioOutput {
    virtual int  output_sample_rate() const = 0;
    virtual int  output_channels()    const = 0;
    virtual bool output_enabled()     const = 0;
    virtual void EnableOutput(bool enable) = 0;
    virtual void OutputData(std::vector<int16_t>& pcm) = 0;
};

struct IHttpClient {
    virtual bool   Open(const std::string& method, const std::string& url) = 0;
    virtual int    GetStatusCode() = 0;
    virtual size_t GetBodyLength() = 0;
    virtual int    Read(char* buf, size_t size) = 0;          // bytes, or <0 on error
    virtual void   Close() = 0;
    virtual void   SetTimeout(int ms) = 0;
};

struct IHttpFactory {
    virtual std::unique_ptr<IHttpClient> CreateHttp() = 0;
};

struct Callbacks {
    std::function<void(const char* status, const char* message)> on_error;
    std::function<void(const std::string& title)> on_started;
    std::function<void()> on_finished;
};
```

## Stability notes

- **TLS-read tolerance**: `Stop()` waits up to 2 s for tasks to drain. If a task is stuck in TLS read it will exit on the next read return; the ring buffer is **not** released until both tasks have exited (prevents use-after-free).
- **Re-entrant `Play()`**: a fresh `Play()` aborts and joins the previous one before allocating new buffers. Caller does not need to call `Stop()` first.
- **Decoder format strictness**: if no frame ever decodes (`sample_rate == 0`) and ≥ 16 consecutive errors accumulate, the player aborts with `"Audio format is not MP3 or corrupted"`.

## License

Apache-2.0. See [LICENSE](LICENSE).
