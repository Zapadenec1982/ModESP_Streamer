# radio_catalog — the station catalog (Radio Browser): request paths, the page parser, the
# filters and the page the screen reads; radio_catalog_ui, what the screens and the radio
# module share besides that page; radio_art, which finds a cover for a station's ICY title;
# and station_logo, the rules of the presets' logos kept on the panel. Pure: no IDF, no
# network, no filesystem, so it is tested on the host against answers recorded from the real
# services (tests/host).
idf_component_register(
    SRCS "src/radio_catalog.cpp" "src/radio_art.cpp" "src/radio_catalog_ui.cpp" "src/station_logo.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES jsmn jsmn_util stations
)
