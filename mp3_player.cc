// mp3_player — streaming MP3 → PCM, project-agnostic.
//
// Pipeline:
//   DownloadTask (Core 0, PSRAM stack 6 KB, prio 1)
//     ├─ IHttpClient::Open/Read → xRingbufferSend
//     └─ end / abort → exit + set download_done_
//
//   DecodeTask (Core 0, internal-RAM stack 10 KB, prio 7)
//     ├─ xRingbufferReceiveUpTo → esp_audio_dec_process
//     ├─ resample (lazy, sample_rate from first frame)
//     ├─ stereo → mono fold
//     └─ IAudioOutput::OutputData

#include "mp3_player.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>

#include <cstring>

extern "C" {
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_mp3_dec.h"
#include "esp_ae_rate_cvt.h"
}

namespace mydazy {

namespace {

constexpr const char* TAG = "Mp3Player";

std::atomic<bool> g_mp3_registered{false};

void EnsureMp3DecoderRegistered() {
    bool expected = false;
    if (g_mp3_registered.compare_exchange_strong(expected, true)) {
        auto ret = esp_mp3_dec_register();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "esp_mp3_dec_register failed: %d", ret);
            g_mp3_registered.store(false);
        } else {
            ESP_LOGI(TAG, "MP3 decoder registered");
        }
    }
}

}  // namespace

Mp3Player& Mp3Player::GetInstance() {
    static Mp3Player instance;
    return instance;
}

void Mp3Player::Initialize(IAudioOutput* audio,
                           IHttpFactory* http,
                           const Callbacks& callbacks) {
    audio_ = audio;
    http_ = http;
    callbacks_ = callbacks;
    EnsureMp3DecoderRegistered();
}

std::string Mp3Player::GetCurrentTitle() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_title_;
}

void Mp3Player::EmitError(const char* status, const char* message) {
    if (callbacks_.on_error) callbacks_.on_error(status, message);
}

bool Mp3Player::Play(const std::string& url, const std::string& title, std::string* err_msg) {
    auto set_err = [err_msg](const char* s) { if (err_msg) *err_msg = s; };

    if (!audio_ || !http_) {
        ESP_LOGE(TAG, "Play failed: not initialized");
        set_err("player not initialized");
        return false;
    }
    if (url.empty()) {
        ESP_LOGW(TAG, "Play failed: empty url");
        set_err("URL is empty");
        return false;
    }
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
        ESP_LOGW(TAG, "Play failed: unsupported scheme (%s)", url.c_str());
        set_err("URL must start with http:// or https://");
        return false;
    }

    AbortAndJoin();

    // Belt-and-suspenders: AbortAndJoin already waits up to 2 s; if a TLS read
    // is still blocking, give it another ~20 s before giving up entirely.
    for (int i = 0; i < 2000 && active_tasks_.load(std::memory_order_acquire) > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (active_tasks_.load(std::memory_order_acquire) > 0) {
        ESP_LOGE(TAG, "Play failed: previous tasks still alive (stuck in TLS read)");
        set_err("previous playback not finished, retry later");
        return false;
    }
    if (ring_buf_) {
        vRingbufferDeleteWithCaps(ring_buf_);
        ring_buf_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_url_ = url;
        current_title_ = title;
    }

    ring_buf_ = xRingbufferCreateWithCaps(kRingBufSize, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!ring_buf_) {
        ESP_LOGE(TAG, "Play failed: PSRAM ring buffer alloc failed (%zu bytes)", kRingBufSize);
        set_err("PSRAM exhausted");
        return false;
    }

    abort_.store(false, std::memory_order_release);
    download_done_.store(false, std::memory_order_release);
    active_tasks_.store(0, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    // Download: PSRAM stack OK — one-shot I/O at low priority, tolerates flash-op stalls.
    TaskHandle_t download_task = nullptr;
    active_tasks_.fetch_add(1, std::memory_order_acq_rel);
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        DownloadThunk, "mp3_dl", 6 * 1024, this, 1, &download_task, 0, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Play failed: download task creation failed");
        active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
        vRingbufferDeleteWithCaps(ring_buf_);
        ring_buf_ = nullptr;
        running_.store(false, std::memory_order_release);
        set_err("failed to create download task");
        return false;
    }

    // Decode: prio 7 (== opus_codec), Core 0, internal-RAM stack — see README "Why internal-RAM".
    TaskHandle_t decode_task = nullptr;
    active_tasks_.fetch_add(1, std::memory_order_acq_rel);
    ret = xTaskCreatePinnedToCore(
        DecodeThunk, "mp3_dec", 10 * 1024, this, 7, &decode_task, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Play failed: decode task creation failed");
        active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
        abort_.store(true, std::memory_order_release);
        set_err("failed to create decode task");
        return false;
    }

    ESP_LOGI(TAG, "Play: %s (%s)", title.c_str(), url.c_str());
    if (err_msg) err_msg->clear();
    return true;
}

void Mp3Player::Stop() {
    AbortAndJoin();
    ESP_LOGI(TAG, "Stop");
}

void Mp3Player::AbortAndJoin() {
    if (!running_.load(std::memory_order_acquire)) {
        if (ring_buf_) {
            vRingbufferDeleteWithCaps(ring_buf_);
            ring_buf_ = nullptr;
        }
        return;
    }
    abort_.store(true, std::memory_order_release);

    for (int i = 0; i < 200 && active_tasks_.load(std::memory_order_acquire) > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    int remaining = active_tasks_.load(std::memory_order_acquire);
    if (remaining > 0) {
        ESP_LOGW(TAG, "AbortAndJoin: %d task(s) still alive after 2 s (likely TLS read)", remaining);
        // Don't free ring_buf_ — they may still be writing to it.
        return;
    }

    running_.store(false, std::memory_order_release);
    if (ring_buf_) {
        vRingbufferDeleteWithCaps(ring_buf_);
        ring_buf_ = nullptr;
    }
}

void Mp3Player::DownloadThunk(void* arg) {
    auto* self = static_cast<Mp3Player*>(arg);
    self->DownloadLoop();
    self->active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    vTaskDeleteWithCaps(nullptr);
}

void Mp3Player::DownloadLoop() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        url = current_url_;
    }

    auto http = http_->CreateHttp();
    if (!http) {
        ESP_LOGE(TAG, "Download failed: http factory returned null");
        EmitError("Playback failed", "HTTP transport unavailable");
        download_done_.store(true, std::memory_order_release);
        return;
    }
    http->SetTimeout(kHttpTimeoutMs);

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Download failed: HTTP open failed %s", url.c_str());
        EmitError("Playback failed", "Failed to connect to server");
        download_done_.store(true, std::memory_order_release);
        return;
    }

    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "Download failed: HTTP %d", status);
        char buf[64];
        if (status == 404)      snprintf(buf, sizeof(buf), "Audio file not found (404)");
        else if (status == 403) snprintf(buf, sizeof(buf), "Forbidden (403)");
        else if (status >= 500) snprintf(buf, sizeof(buf), "Server error (%d)", status);
        else                    snprintf(buf, sizeof(buf), "HTTP error (%d)", status);
        EmitError("Playback failed", buf);
        http->Close();
        download_done_.store(true, std::memory_order_release);
        return;
    }

    size_t total_len = http->GetBodyLength();
    ESP_LOGI(TAG, "Download started: %u bytes", (unsigned)total_len);

    // Read scratch in PSRAM — TLS handshake leaves limited stack room.
    constexpr size_t kReadChunk = 2048;
    char* buf = static_cast<char*>(heap_caps_malloc(kReadChunk, MALLOC_CAP_SPIRAM));
    if (!buf) {
        ESP_LOGE(TAG, "Download buffer alloc failed");
        EmitError("Playback failed", "PSRAM exhausted");
        http->Close();
        download_done_.store(true, std::memory_order_release);
        return;
    }
    size_t total_read = 0;

    while (!abort_.load(std::memory_order_acquire) &&
           running_.load(std::memory_order_acquire)) {
        int n = http->Read(buf, kReadChunk);
        if (n < 0) {
            ESP_LOGW(TAG, "Download: Read error %d", n);
            break;
        }
        if (n == 0) break;  // EOF

        size_t written = 0;
        while (written < (size_t)n && !abort_.load(std::memory_order_acquire)) {
            size_t chunk = (size_t)n - written;
            BaseType_t ok = xRingbufferSend(ring_buf_, buf + written, chunk,
                                             pdMS_TO_TICKS(200));
            if (ok == pdTRUE) written += chunk;
            // else: ring full, retry next iteration
        }
        total_read += written;
    }

    http->Close();
    heap_caps_free(buf);
    ESP_LOGI(TAG, "Download done: %u bytes read", (unsigned)total_read);
    download_done_.store(true, std::memory_order_release);
}

void Mp3Player::DecodeThunk(void* arg) {
    auto* self = static_cast<Mp3Player*>(arg);
    self->DecodeLoop();
    // ring_buf_ is freed by AbortAndJoin once both tasks have exited.
    self->active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    vTaskDelete(nullptr);
}

void Mp3Player::DecodeLoop() {
    esp_audio_dec_cfg_t dec_cfg = {};
    dec_cfg.type = ESP_AUDIO_TYPE_MP3;
    esp_audio_dec_handle_t dec = nullptr;
    if (esp_audio_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder");
        EmitError("Playback failed", "MP3 decoder init failed");
        return;
    }

    constexpr size_t kInBufSize  = 8192;
    constexpr size_t kOutBufSize = 2 * 1152 * sizeof(int16_t) + 512;
    uint8_t* in_buf  = static_cast<uint8_t*>(heap_caps_malloc(kInBufSize,  MALLOC_CAP_SPIRAM));
    uint8_t* out_buf = static_cast<uint8_t*>(heap_caps_malloc(kOutBufSize, MALLOC_CAP_SPIRAM));
    if (!in_buf || !out_buf) {
        ESP_LOGE(TAG, "Decode buffer alloc failed");
        EmitError("Playback failed", "PSRAM exhausted");
        if (in_buf)  heap_caps_free(in_buf);
        if (out_buf) heap_caps_free(out_buf);
        esp_audio_dec_close(dec);
        return;
    }
    size_t in_len = 0;

    esp_ae_rate_cvt_handle_t resampler = nullptr;
    uint32_t src_rate = 0;
    uint8_t  src_channel = 0;
    uint32_t dst_rate    = (uint32_t)audio_->output_sample_rate();
    uint8_t  dst_channel = (uint8_t)audio_->output_channels();

    if (audio_ && !audio_->output_enabled()) audio_->EnableOutput(true);

    int consecutive_errors = 0;
    bool started_emitted = false;
    bool finished_emitted = false;

    while (running_.load(std::memory_order_acquire) &&
           !abort_.load(std::memory_order_acquire)) {
        if (in_len < 1024) {
            size_t want = kInBufSize - in_len;
            size_t got = 0;
            char* recv = static_cast<char*>(
                xRingbufferReceiveUpTo(ring_buf_, &got, pdMS_TO_TICKS(200), want));
            if (recv) {
                memcpy(in_buf + in_len, recv, got);
                in_len += got;
                vRingbufferReturnItem(ring_buf_, recv);
            } else {
                if (download_done_.load(std::memory_order_acquire) && in_len == 0) {
                    ESP_LOGI(TAG, "Decode complete (data exhausted)");
                    finished_emitted = true;
                    break;
                }
                continue;
            }
        }
        if (in_len == 0) continue;

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = in_buf;
        raw.len = (uint32_t)in_len;
        esp_audio_dec_out_frame_t out = {};
        out.buffer = out_buf;
        out.len = (uint32_t)kOutBufSize;

        auto ret = esp_audio_dec_process(dec, &raw, &out);

        if (raw.consumed > 0) {
            memmove(in_buf, in_buf + raw.consumed, in_len - raw.consumed);
            in_len -= raw.consumed;
        }

        if (ret == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
            consecutive_errors = 0;

            if (src_rate == 0) {
                esp_audio_dec_info_t info = {};
                if (esp_audio_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                    src_rate = info.sample_rate;
                    src_channel = info.channel;
                    ESP_LOGI(TAG, "MP3: %u Hz, %u ch -> sink %u Hz, %u ch",
                             (unsigned)src_rate, src_channel,
                             (unsigned)dst_rate, dst_channel);
                    if (!started_emitted && callbacks_.on_started) {
                        callbacks_.on_started(GetCurrentTitle());
                        started_emitted = true;
                    }
                }
            }

            size_t sample_count = out.decoded_size / sizeof(int16_t);
            std::vector<int16_t> pcm(reinterpret_cast<int16_t*>(out_buf),
                                      reinterpret_cast<int16_t*>(out_buf) + sample_count);

            if (src_channel == 2 && dst_channel == 1) {
                std::vector<int16_t> mono(sample_count / 2);
                for (size_t i = 0; i < mono.size(); i++) {
                    int32_t avg = (pcm[2 * i] + pcm[2 * i + 1]) / 2;
                    mono[i] = (int16_t)avg;
                }
                pcm = std::move(mono);
            }

            if (src_rate != 0 && src_rate != dst_rate) {
                if (!resampler) {
                    esp_ae_rate_cvt_cfg_t cfg = {};
                    cfg.src_rate = src_rate;
                    cfg.dest_rate = dst_rate;
                    cfg.channel = dst_channel;
                    cfg.bits_per_sample = ESP_AUDIO_BIT16;
                    cfg.complexity = 2;
                    cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                    if (esp_ae_rate_cvt_open(&cfg, &resampler) != ESP_AE_ERR_OK) {
                        ESP_LOGE(TAG, "Resampler open failed");
                        resampler = nullptr;
                    }
                }
                if (resampler) {
                    uint32_t max_out = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, pcm.size(), &max_out);
                    std::vector<int16_t> out_pcm(max_out);
                    uint32_t actual = max_out;
                    esp_ae_rate_cvt_process(resampler,
                                            (esp_ae_sample_t)pcm.data(), pcm.size(),
                                            (esp_ae_sample_t)out_pcm.data(), &actual);
                    out_pcm.resize(actual);
                    pcm = std::move(out_pcm);
                }
            }

            if (!pcm.empty() && audio_) audio_->OutputData(pcm);

        } else if (ret == ESP_AUDIO_ERR_CONTINUE) {
            consecutive_errors = 0;
        } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "PCM buffer too small (need %u), skipping frame",
                     (unsigned)out.needed_size);
            consecutive_errors = 0;
        } else if (ret == ESP_AUDIO_ERR_DATA_LACK) {
            if (download_done_.load(std::memory_order_acquire) && in_len < 4) break;
        } else {
            if (++consecutive_errors >= 16) {
                ESP_LOGE(TAG, "Decode errors %d in a row, exiting", consecutive_errors);
                EmitError("Playback failed",
                          src_rate == 0 ? "Audio is not MP3 or corrupted"
                                        : "Decoder error");
                break;
            }
            if (in_len > 1) {
                memmove(in_buf, in_buf + 1, in_len - 1);
                in_len -= 1;
            } else {
                in_len = 0;
            }
        }
    }

    if (resampler) esp_ae_rate_cvt_close(resampler);
    esp_audio_dec_close(dec);
    heap_caps_free(in_buf);
    heap_caps_free(out_buf);

    if (finished_emitted && callbacks_.on_finished) callbacks_.on_finished();

    ESP_LOGI(TAG, "Decode task exit");
}

}  // namespace mydazy
