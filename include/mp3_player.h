#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>

namespace mydazy {

// Audio sink. The component never owns the codec; the caller keeps it alive.
struct IAudioOutput {
    virtual ~IAudioOutput() = default;
    virtual int  output_sample_rate() const = 0;
    virtual int  output_channels()    const = 0;
    virtual bool output_enabled()     const = 0;
    virtual void EnableOutput(bool enable) = 0;
    virtual void OutputData(std::vector<int16_t>& pcm) = 0;
};

// HTTP transport. One IHttpClient per request; component closes & destroys it.
struct IHttpClient {
    virtual ~IHttpClient() = default;
    virtual bool   Open(const std::string& method, const std::string& url) = 0;
    virtual int    GetStatusCode() = 0;
    virtual size_t GetBodyLength() = 0;
    virtual int    Read(char* buf, size_t size) = 0;
    virtual void   Close() = 0;
    virtual void   SetTimeout(int ms) = 0;
};

struct IHttpFactory {
    virtual ~IHttpFactory() = default;
    virtual std::unique_ptr<IHttpClient> CreateHttp() = 0;
};

// Streaming MP3 player. Single-instance (`GetInstance()`); MP3 decoder is
// process-global in esp_audio_codec, so a second player would not buy parallelism.
class Mp3Player {
public:
    struct Callbacks {
        // Async error from download / decode. Always invoked from a worker task —
        // the caller is responsible for thread-hopping back to UI if needed.
        std::function<void(const char* status, const char* message)> on_error;
        // Optional: called once decode produces its first frame.
        std::function<void(const std::string& title)> on_started;
        // Optional: called on graceful end-of-stream (not on Stop()/abort).
        std::function<void()> on_finished;
    };

    static Mp3Player& GetInstance();

    // One-time. Pointers must outlive the player.
    void Initialize(IAudioOutput* audio,
                    IHttpFactory* http,
                    const Callbacks& callbacks = {});

    // Auto-aborts any previous playback before starting.
    bool Play(const std::string& url,
              const std::string& title = "",
              std::string* err_msg = nullptr);

    void Stop();
    bool IsPlaying() const { return running_.load(std::memory_order_acquire); }
    std::string GetCurrentTitle() const;

    Mp3Player(const Mp3Player&) = delete;
    Mp3Player& operator=(const Mp3Player&) = delete;

private:
    Mp3Player() = default;
    ~Mp3Player() = default;

    static void DownloadThunk(void* arg);
    static void DecodeThunk(void* arg);
    void DownloadLoop();
    void DecodeLoop();

    void AbortAndJoin();
    void EmitError(const char* status, const char* message);

    IAudioOutput* audio_ = nullptr;
    IHttpFactory* http_ = nullptr;
    Callbacks callbacks_;

    std::atomic<bool> running_{false};
    std::atomic<bool> abort_{false};
    std::atomic<bool> download_done_{false};
    std::atomic<int>  active_tasks_{0};

    mutable std::mutex state_mutex_;
    std::string current_url_;
    std::string current_title_;

    RingbufHandle_t ring_buf_ = nullptr;
    static constexpr size_t kRingBufSize = 128 * 1024;
    static constexpr int    kHttpTimeoutMs = 15000;
};

}  // namespace mydazy
