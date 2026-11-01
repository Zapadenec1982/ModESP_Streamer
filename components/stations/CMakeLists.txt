# stations — the radio station list and its presets, as JSON. Pure: no IDF, no filesystem,
# no network, so the parser is tested on the host alongside the rest of tests/host.
idf_component_register(
    SRCS "src/stations.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES jsmn jsmn_util
)
