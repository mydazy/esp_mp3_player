# esp_mp3_player — basic example

Minimal ESP32-S3 example that streams a hard-coded MP3 URL to I2S using injected `IAudioOutput` / `IHttpClient` shims.

```bash
idf.py set-target esp32s3
idf.py -DEXAMPLE_MP3_URL='"https://example.com/song.mp3"' build flash monitor
```

> The example is intentionally bare — see `main/basic_main.cc` for the smallest viable wiring.
