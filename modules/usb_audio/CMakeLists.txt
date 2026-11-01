# usb_audio — USB audio (UAC2) as the player sees it. The device itself is components/usb_uac;
# this module starts it under the panel's one name and publishes what it measures.
idf_component_register(
    SRCS "src/usb_audio_module.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_core audio_dsp   # audio_dsp: volume_follow.h, the slider rule AirPlay keeps too
    PRIV_REQUIRES usb_uac bt modesp_services   # bt: device_name.h; modesp_services: device_id.h
)
