# bt — the control link to the Bluetooth satellite (docs/CONCEPT.md, "J5 — UART to the satellite",
# docs/LINK_PROTOCOL.md).
# The wire is a bound driver (drivers/uart_link) behind modesp/hal/byte_link.h, so this
# module no longer depends on a UART driver — only on what is said over the wire.
idf_component_register(
    SRCS "src/bt_module.cpp" "src/bt_cover.cpp" "src/bt_settings.cpp" "src/bt_web.cpp" "src/device_name.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_core modesp_hal
    # modesp_net + esp_http_server + jsmn: GET/POST /api/bt (src/bt_http.inc, src/bt_web.cpp),
    # the web's Bluetooth page and its answer to a pairing request.
    # modesp_services: nvs_helper, for the panel's one name (src/device_name.cpp).
    PRIV_REQUIRES cover sat_link esp_app_format esp_timer esp_ringbuf modesp_net esp_http_server jsmn
                  jsmn_util modesp_services
)
