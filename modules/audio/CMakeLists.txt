# audio — the audio core (docs/CONCEPT.md, "Signal path and clocking",
# "SharedState contract: sources"): its own task on core 1, sources
# (tone / AUX / BT), ASRC (components/audio_resampler), PEQ (esp-dsp biquads), volume,
# limiter → I2S-A. Transports are DRIVERS (drivers/i2s_std) resolved from bindings.json
# through the audio seam (modesp/hal/audio_sink.h, IAudioSink::as_port); the module
# never touches pins or i2s_* APIs. ModESP only sees SharedState keys and slow status.
get_filename_component(_modesp_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

idf_component_register(
    SRCS "src/audio_module.cpp"
    INCLUDE_DIRS "include" "${_modesp_root}/generated"
    # audio_dsp is PUBLIC: audio_module.h exposes audio_core types in its interface.
    REQUIRES modesp_core modesp_hal audio_dsp
    # modesp_net + esp_http_server: GET/POST /api/eq (src/eq_http.inc), the web's EQ editor.
    PRIV_REQUIRES audio_resampler esp_timer sat_link mic_capture user_store
                  modesp_net esp_http_server
)

# Bench-only A/B isolation; normal builds retain the real audio output.
option(MODESP_AUDIO_DIAGNOSTIC_CONSTANT "Transmit DAC keepalive only, retaining normal format/clock policy" OFF)
if(MODESP_AUDIO_DIAGNOSTIC_CONSTANT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MODESP_AUDIO_DIAGNOSTIC_CONSTANT=1)
endif()
option(MODESP_AUDIO_DIAGNOSTIC_FIXED_TRIM "Disable live P4 clock trimming for bench A/B" OFF)
option(MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT "Bench: bypass BT ASRC at equal rates, fixed P4 trim, at most 60 seconds" OFF)
option(MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO "Bench: BT ASRC ratio 1, fixed P4 trim, at most 60 seconds" OFF)
# The Bluetooth path by default since link protocol 1.9: the panel clocks the link from the
# DAC's own I2S, so stream and output run on one clock and the satellite's samples go to the DAC
# untouched -- no ASRC, no ring servo, no trim. OFF brings back the resampling path, which only
# a satellite older than 1.9 (one that clocks the link itself) needs; with ON such a satellite
# gets a rebuffer every few minutes instead.
option(MODESP_AUDIO_BT_DIRECT_CONTINUOUS "Continuous BT direct PCM at equal rates, fixed P4 trim, automatic rebuffer" ON)
if(MODESP_AUDIO_BT_DIRECT_CONTINUOUS AND (MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT OR MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO))
    message(FATAL_ERROR "Continuous Bluetooth PCM cannot use a bounded diagnostic path")
endif()
if(MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT AND MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO)
    message(FATAL_ERROR "Select only one Bluetooth diagnostic PCM path")
endif()
if(MODESP_AUDIO_DIAGNOSTIC_CONSTANT AND (MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT OR MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO OR MODESP_AUDIO_BT_DIRECT_CONTINUOUS))
    message(FATAL_ERROR "Bluetooth diagnostic listening requires real PCM output")
endif()
if(MODESP_AUDIO_DIAGNOSTIC_FIXED_TRIM OR MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT OR MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO OR MODESP_AUDIO_BT_DIRECT_CONTINUOUS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MODESP_AUDIO_DIAGNOSTIC_FIXED_TRIM=1)
endif()
if(MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT=1)
endif()
if(MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO=1)
endif()
if(MODESP_AUDIO_BT_DIRECT_CONTINUOUS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MODESP_AUDIO_BT_DIRECT_CONTINUOUS=1)
endif()
