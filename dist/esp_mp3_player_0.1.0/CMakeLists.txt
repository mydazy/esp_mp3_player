idf_component_register(
    SRCS "mp3_player.cc"
    INCLUDE_DIRS "include"
    # PUBLIC: types in mp3_player.h transitively include FreeRTOS + ring buffer headers,
    # so any consumer needs these include paths.
    REQUIRES freertos esp_ringbuf espressif__esp_audio_codec espressif__esp_audio_effects
    PRIV_REQUIRES esp_timer esp_common pthread
)

# C++17 for std::variant / std::optional usage in the public API
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
