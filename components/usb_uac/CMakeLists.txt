# usb_uac — the panel as a USB Audio Class 2 device on the P4's high-speed port
# (docs/CONCEPT.md, "USB").
# TinyUSB is used directly, as espressif/usb_device_uac does: this component owns the PHY, the
# descriptors and tusb_config.h. TinyUSB's own sources must be compiled against that config
# header, so its directory goes onto the tinyusb component's include path below.
idf_component_register(
    SRCS "src/usb_uac.cpp"
         "src/uac_usb.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "tusb"
    PRIV_REQUIRES usb esp_timer freertos espressif__tinyusb audio_dsp   # audio_dsp: pcm_ring.h
)

idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${COMPONENT_DIR}/tusb")

# The producer converts and measures every frame a computer sends, at a priority above the
# audio task's: built -O2 like the other components that run per sample.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
