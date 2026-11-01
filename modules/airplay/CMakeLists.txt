# airplay — AirPlay 2 as audio.source 6. The receiver itself is components/airplay2 (vendored
# upstream code, UPSTREAM.md there); this is the player's side of it: lifecycle, events into
# SharedState, and the source contract of docs/CONCEPT.md, "SharedState contract: sources".
idf_component_register(
    SRCS "src/airplay_module.cpp"
         "src/mdns_query_guard.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_core audio_dsp   # audio_dsp: volume_follow.h, the slider rule USB keeps too
    PRIV_REQUIRES airplay2 cover modesp_services modesp_net esp_timer bt   # bt: device_name.h
    # The only reference to __wrap_mdns_query_ptr comes from libairplay2.a, which the linker
    # reaches after this archive, and an archive member nobody has asked for yet is not
    # pulled in. Same cure as components/lwip_dns_compat and components/esp_dlna.
    WHOLE_ARCHIVE
)

# DACP's mDNS queries take Wi-Fi's mDNS guard (src/mdns_query_guard.cpp); upstream's
# dacp_client.c is the only caller and stays as published.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mdns_query_ptr")
