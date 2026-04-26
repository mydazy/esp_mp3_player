// Minimal example: stream an MP3 URL and discard PCM.
// Replace the audio sink with your I2S / codec wiring.

#include "mp3_player.h"

#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <vector>

#ifndef EXAMPLE_MP3_URL
#define EXAMPLE_MP3_URL "https://example.com/song.mp3"
#endif

#define TAG "mp3_basic"

namespace {

// 1. Audio sink: discard. Plug your codec here.
class NullAudio : public mydazy::IAudioOutput {
public:
    int  output_sample_rate() const override { return 16000; }
    int  output_channels()    const override { return 1; }
    bool output_enabled()     const override { return enabled_; }
    void EnableOutput(bool enable) override  { enabled_ = enable; }
    void OutputData(std::vector<int16_t>& pcm) override {
        ESP_LOGD(TAG, "got %u samples", (unsigned)pcm.size());
    }
private:
    bool enabled_ = false;
};

// 2. Trivial HTTP wrapper around esp_http_client.
class EspHttp : public mydazy::IHttpClient {
public:
    bool Open(const std::string& method, const std::string& url) override {
        esp_http_client_config_t cfg = {};
        cfg.url = url.c_str();
        cfg.timeout_ms = timeout_ms_;
        cfg.crt_bundle_attach = nullptr;  // set if you need TLS bundle
        client_ = esp_http_client_init(&cfg);
        if (!client_) return false;
        esp_http_client_set_method(client_, method == "POST" ? HTTP_METHOD_POST : HTTP_METHOD_GET);
        esp_err_t err = esp_http_client_open(client_, 0);
        if (err != ESP_OK) return false;
        body_len_ = esp_http_client_fetch_headers(client_);
        return true;
    }
    int    GetStatusCode() override { return esp_http_client_get_status_code(client_); }
    size_t GetBodyLength() override { return body_len_ < 0 ? 0 : (size_t)body_len_; }
    int    Read(char* buf, size_t size) override {
        return esp_http_client_read_response(client_, buf, size);
    }
    void   Close() override {
        if (client_) {
            esp_http_client_cleanup(client_);
            client_ = nullptr;
        }
    }
    void   SetTimeout(int ms) override { timeout_ms_ = ms; }
private:
    esp_http_client_handle_t client_ = nullptr;
    int64_t body_len_ = 0;
    int timeout_ms_ = 15000;
};

class EspHttpFactory : public mydazy::IHttpFactory {
public:
    std::unique_ptr<mydazy::IHttpClient> CreateHttp() override {
        return std::make_unique<EspHttp>();
    }
};

NullAudio       g_audio;
EspHttpFactory  g_http;

}  // namespace

extern "C" void app_main(void) {
    mydazy::Mp3Player::Callbacks cb;
    cb.on_error    = [](const char* s, const char* m) { ESP_LOGW(TAG, "error: %s — %s", s, m); };
    cb.on_started  = [](const std::string& t)         { ESP_LOGI(TAG, "started: %s", t.c_str()); };
    cb.on_finished = []()                             { ESP_LOGI(TAG, "finished"); };

    mydazy::Mp3Player::GetInstance().Initialize(&g_audio, &g_http, cb);

    std::string err;
    if (!mydazy::Mp3Player::GetInstance().Play(EXAMPLE_MP3_URL, "demo", &err)) {
        ESP_LOGE(TAG, "play failed: %s", err.c_str());
        return;
    }

    while (mydazy::Mp3Player::GetInstance().IsPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "playback ended");
}
