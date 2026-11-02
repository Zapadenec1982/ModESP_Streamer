# modesp_net - Phase 5a: WiFi + HTTP + WebSocket
# Optional: when CONFIG_MODESP_NET_ENABLE is off, contributes zero sources
# (empty component) so offline/BLE-only builds carry no network code.
set(net_srcs "")
if(CONFIG_MODESP_NET_ENABLE)
    list(APPEND net_srcs
         "src/wifi_service.cpp"
         "src/mdns_guard.cpp"
         "src/http_service.cpp"
         "src/ws_service.cpp"
         "src/state_json.cpp")
endif()

set(net_reqs modesp_core modesp_services modesp_hal
             esp_wifi esp_netif esp_http_server esp_event nvs_flash app_update espressif__mdns)
# Targets without native Wi-Fi (ESP32-P4): the esp_wifi component only provides the headers and
# Kconfig; the implementation is esp_wifi_remote, which drives the ESP-Hosted co-processor over
# SDIO (docs/CONCEPT.md, "Network"). Gated on the target so other boards keep the native stack.
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND net_reqs esp_wifi_remote)
endif()

idf_component_register(
    SRCS ${net_srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${net_reqs}
    PRIV_REQUIRES jsmn jsmn_util mbedtls safe_file user_store esp_timer
)

# Доступ до згенерованих headers (state_meta.h для валідації POST /api/settings)
if(CONFIG_MODESP_NET_ENABLE)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_SOURCE_DIR}/generated")
endif()
