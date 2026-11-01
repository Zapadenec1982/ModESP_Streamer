# jsmn_util — the helpers every jsmn parser here needs, in one header.
#
# Header-only and pure, depending on jsmn alone, so it sits below everything that parses JSON:
# the framework's HTTP server (modesp_net) and the player's components (user_store, which
# modesp_net itself depends on, stations, audio_dsp, ...) can all take it without a cycle, and
# tests/host compiles it unchanged.
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES jsmn
)
