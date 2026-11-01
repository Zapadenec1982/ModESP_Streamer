# radio — internet radio: an HTTP stream decoded into the same PCM ring the card plays
# through. The stretch from decoder to ring is components/audio_pipe, shared with the
# player rather than written twice; the station list is components/stations, and the
# station catalog (Radio Browser) is components/radio_catalog with its task in catalog.cpp; the
# presets' logos kept on the panel are logos.cpp, with their rules in radio_catalog too.
idf_component_register(
    SRCS "src/radio_module.cpp" "src/next_stream.cpp" "src/catalog.cpp" "src/art.cpp" "src/radio_http.cpp"
         "src/logos.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_core modesp_hal
    PRIV_REQUIRES esp_http_client audio_pipe audio_dsp stations safe_file user_store tcp_transport esp-tls mbedtls esp_wifi esp_timer
                  espressif__esp_audio_codec radio_catalog esp_app_format cover modesp_net esp_http_server
)
