cmake_minimum_required(VERSION 3.16)
project(modesp_host_tests C CXX)   # C: components/audio_resampler is plain C

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ── Root of ModESP_v4 project ──
get_filename_component(ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)

# ── ETL library — pure C++, no platform dependencies (git submodule components/etl/etl) ──
set(ETL_INCLUDE "${ROOT}/components/etl/etl/include")
if(NOT EXISTS "${ETL_INCLUDE}/etl/version.h")
    message(FATAL_ERROR "ETL library not found. Run 'git submodule update --init' first.")
endif()
message(STATUS "  ETL path: ${ETL_INCLUDE}")

# ── Include directories ──
# CRITICAL: mocks/ must be FIRST so that stub headers intercept ESP-IDF includes.
include_directories(
    "${CMAKE_SOURCE_DIR}/mocks"                             # freertos_mock.h, esp_log_mock.h, etc.
    "${CMAKE_SOURCE_DIR}/mocks/freertos"                    # freertos/FreeRTOS.h, semphr.h (via relative path)
    "${CMAKE_SOURCE_DIR}/mocks/driver"                      # driver/gpio.h, driver/i2c_master.h
    "${ETL_INCLUDE}"                                        # etl/*.h (pure C++, no stubs needed)
    "${ROOT}/components/modesp_core/include"                # modesp/shared_state.h, base_module.h, types.h
    "${ROOT}/components/modesp_services/include"            # modesp/services/config_service.h, error_service.h
    "${ROOT}/generated"                                     # features_config.h, state_meta.h, etc.
    "${ROOT}/components/modesp_hal/include"                  # modesp/hal/driver_interfaces.h, hal_types.h
    "${ROOT}/components/modesp_net/include"                 # modesp/net/log_source.h (ILogSource contract)
    "${ROOT}/components/audio_dsp/include"                   # dsp_chain.h + eq_config.h (shared, pure)
    "${ROOT}/components/audio_tags/include"                  # audio_tags.h — no IDF dependency at all
    "${ROOT}/components/stations/include"                    # stations.h — the radio list, also pure
    "${ROOT}/components/radio_catalog/include"               # radio_catalog.h — the station catalog, pure
    "${ROOT}/components/cover/include"                       # cover.h — for cover_tone.cpp, which is pure
    "${ROOT}/components/jsmn"                                # header-only JSON parser
    "${ROOT}/components/audio_resampler"                     # resampler.h — vendored dbry, plain C
    "${ROOT}/components/audio_pipe/include"                  # audio_pipe.h — decoder-to-ring arithmetic
    "${ROOT}/components/sat_link/include"                    # sat_link.h — the BT satellite wire format
    "${ROOT}/components/mic_capture/include"                 # bounded capture + WAV, pure C++
    "${ROOT}/components/safe_file/include"                   # safe_file.h — write/read-back/rename, pure stdio
    "${ROOT}/components/user_store/include"                  # user_store.h — the userdata partition and its migration
    "${ROOT}/components/net_probe/include"                   # net_probe/probe_stats.h — the UDP probe's accounting, pure C
    "${ROOT}/modules/bt/include"                             # bt_web.h / bt_settings.h — /api/bt bodies, pure
    "${ROOT}/modules/airplay/include"                        # airplay_meta.h — pure
    "${ROOT}/modules/ui_lvgl/include"                        # i18n.h — the panel's strings, free of LVGL
    "${CMAKE_SOURCE_DIR}"                                   # for #include "doctest.h"
)

# ── Core host-build sources ──
#
# shared_state.cpp and base_module.cpp are the FIRMWARE files, compiled as they are. They
# used to have host copies here, and the copies had drifted: the host SharedState bumped
# version_ on an untracked set and had no persist callback, so the suite was green about
# behaviour the device does not have. mocks/ is first on the include path, so the esp_log.h
# and freertos/*.h those two files include resolve to the stubs without either file knowing.
#
# module_manager and driver_manager still have host stand-ins: those call esp_timer and the
# driver registry in ways the stubs do not cover yet.
set(CORE_SOURCES
    "${ROOT}/components/modesp_core/src/shared_state.cpp"
    "${ROOT}/components/modesp_core/src/base_module.cpp"
    "${CMAKE_SOURCE_DIR}/module_manager_host.cpp"
    "${CMAKE_SOURCE_DIR}/driver_manager_host.cpp"
)

# ── Framework sources compiled directly (only need esp_log.h which is mocked) ──
set(MODULE_SOURCES
    "${ROOT}/components/modesp_hal/src/char_grid.cpp"
    "${ROOT}/components/modesp_hal/src/driver_registry.cpp"
    "${ROOT}/components/modesp_services/src/config_service.cpp"  # fopen + mocked esp_log/littlefs/heap_caps
    "${ROOT}/components/modesp_services/src/error_service.cpp"   # portMUX → std::mutex in freertos_mock.h
    "${ROOT}/components/modesp_services/src/nvs_helper.cpp"      # over the in-memory NVS in mocks/nvs.h
    "${ROOT}/components/modesp_services/src/persist_service.cpp" # restore + the one-shot positional-key pass
    "${ROOT}/components/audio_dsp/src/dsp_chain.cpp"         # band design, the EQ/width/limiter chain, the tone
    "${ROOT}/components/audio_dsp/src/dsp_kernels.cpp"       # the EQ's stereo SVF loops (fmaf spelled out: same rounding as the P4)
    "${ROOT}/components/audio_dsp/src/leveler.cpp"           # «Вирівнювання гучності» and its schedule
    "${ROOT}/components/audio_dsp/src/eq_config.cpp"         # eq.json parse/serialise (jsmn, header-only)
    "${ROOT}/components/audio_dsp/src/eq_presets.cpp"        # the DSP page's presets and the "Свій" rules
    "${ROOT}/modules/ui_lvgl/src/i18n.cpp"                   # the panel's strings: tr, plural, fmt, num
    "${ROOT}/modules/ui_lvgl/src/i18n_tables.cpp"            # ...and their tables, generated from webui/src/i18n
    "${ROOT}/components/audio_dsp/src/eq_store.cpp"          # the EQ document the panel and the web share
    "${ROOT}/components/audio_dsp/src/eq_web.cpp"            # GET/POST /api/eq bodies
    "${ROOT}/modules/bt/src/bt_web.cpp"                      # GET/POST /api/bt bodies (bt_settings types only)
    "${ROOT}/components/audio_dsp/src/spectrum.cpp"          # FFT kernel mocked in mocks/dsps_fft_ref.h
    "${ROOT}/components/audio_dsp/src/pcm_ring.cpp"          # SPSC ring, decoder -> audio task
    "${ROOT}/components/audio_dsp/src/port_finder.cpp"       # the port meter's lock-in and dip finder
    "${ROOT}/components/audio_tags/src/audio_tags.cpp"       # ID3v2 / Vorbis / RIFF INFO, pure parsing
    "${ROOT}/components/stations/src/stations.cpp"           # station list JSON (jsmn, header-only)
    "${ROOT}/components/radio_catalog/src/radio_catalog.cpp" # Radio Browser paths, page parser, filters
    "${ROOT}/components/radio_catalog/src/radio_art.cpp"     # a cover for a station's ICY title
    "${ROOT}/components/cover/src/cover_tone.cpp"            # the colour of a cover tile, pure
    "${ROOT}/components/radio_catalog/src/radio_catalog_ui.cpp" # what the catalog screen and the radio module share
    "${ROOT}/components/audio_resampler/resampler.c"         # vendored dbry resampler, plain C
    "${ROOT}/components/audio_pipe/src/audio_pipe.cpp"       # PCM conversion + stream length/seek arithmetic
    "${ROOT}/components/sat_link/src/sat_link.cpp"           # COBS + CRC16 framing for the BT satellite
    "${ROOT}/components/modesp_core/src/liveness.cpp"        # liveness registry — <atomic> only, no IDF
    "${ROOT}/components/safe_file/src/safe_file.cpp"         # write -> read back -> rename, plain stdio
    "${ROOT}/components/user_store/src/user_store.cpp"       # userdata mount + migration (esp_littlefs mocked)
    "${ROOT}/components/user_store/src/user_backup.cpp"      # the user's files as one JSON document (A6)
)

# ── Test sources ──
set(TEST_SOURCES
    "${CMAKE_SOURCE_DIR}/test_main.cpp"
    "${CMAKE_SOURCE_DIR}/test_char_grid.cpp"
    "${CMAKE_SOURCE_DIR}/test_display_backend_registry.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_chain.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_svf.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_fade.cpp"
    "${CMAKE_SOURCE_DIR}/test_out_gate.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_guard.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_limiter.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_levels.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_bass.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_design.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_loudness.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_tone.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_leveler.cpp"
    "${CMAKE_SOURCE_DIR}/test_dsp_passes.cpp"        # the filter sections two to a pass, bit for bit
    "${CMAKE_SOURCE_DIR}/test_level_latch.cpp"
    "${CMAKE_SOURCE_DIR}/test_port_finder.cpp"
    "${CMAKE_SOURCE_DIR}/test_mono_bass_retune.cpp"
    "${CMAKE_SOURCE_DIR}/test_bt_clock_health.cpp"
    "${CMAKE_SOURCE_DIR}/test_eq_config.cpp"
    "${CMAKE_SOURCE_DIR}/test_eq_store.cpp"
    "${CMAKE_SOURCE_DIR}/test_eq_curve_vectors.cpp"
    "${CMAKE_SOURCE_DIR}/test_bt_web.cpp"
    "${CMAKE_SOURCE_DIR}/test_bt_volume_gate.cpp"
    "${CMAKE_SOURCE_DIR}/test_bt_stall_kick.cpp"
    "${CMAKE_SOURCE_DIR}/test_airplay_meta.cpp"
    "${CMAKE_SOURCE_DIR}/test_spectrum.cpp"
    "${CMAKE_SOURCE_DIR}/test_pcm_ring.cpp"
    "${CMAKE_SOURCE_DIR}/test_pcm_session.cpp"
    "${CMAKE_SOURCE_DIR}/test_pcm_producer.cpp"
    "${CMAKE_SOURCE_DIR}/test_audio_seek.cpp"
    "${CMAKE_SOURCE_DIR}/test_mic_capture.cpp"
    "${CMAKE_SOURCE_DIR}/test_startup_probe.cpp"
    "${CMAKE_SOURCE_DIR}/test_audio_tags.cpp"
    "${CMAKE_SOURCE_DIR}/test_stations.cpp"
    "${CMAKE_SOURCE_DIR}/test_radio_catalog.cpp"
    "${CMAKE_SOURCE_DIR}/test_radio_art.cpp"
    "${CMAKE_SOURCE_DIR}/test_resampler.cpp"
    "${CMAKE_SOURCE_DIR}/test_resampler_fixed_phase.cpp"
    "${CMAKE_SOURCE_DIR}/test_resampler_safety.cpp"
    "${CMAKE_SOURCE_DIR}/test_resampler_alloc.c"
    "${CMAKE_SOURCE_DIR}/test_stream_pos.cpp"
    "${CMAKE_SOURCE_DIR}/test_dlna_transport.cpp"
    "${CMAKE_SOURCE_DIR}/test_dlna_stream.cpp"
    "${CMAKE_SOURCE_DIR}/test_dlna_soap.cpp"
    "${CMAKE_SOURCE_DIR}/test_dlna_browser_json.cpp" # components/dlna_client/include/browser_json.h — GET /api/dlna/browser
    "${CMAKE_SOURCE_DIR}/test_sat_link.cpp"
    "${CMAKE_SOURCE_DIR}/test_cover_assembly.cpp"
    "${CMAKE_SOURCE_DIR}/test_cover_guard.cpp"       # components/cover/include/cover_guard.h — the software decoder's budget
    "${CMAKE_SOURCE_DIR}/test_cover_png_zlib.cpp"    # components/cover/src/stb_image.h — the PNG DEFLATE-block cap
    "${CMAKE_SOURCE_DIR}/test_cover_tone.cpp"        # components/cover/src/cover_tone.cpp — the Player's background
    "${CMAKE_SOURCE_DIR}/test_uac_tone.cpp"          # components/usb_uac/include/uac_tone.h — the USB stream check (U0)
    "${CMAKE_SOURCE_DIR}/test_usb_volume.cpp"        # modules/usb_audio/include/usb_volume.h — the computer's slider (U1)
    "${CMAKE_SOURCE_DIR}/test_i2s_clock.cpp"
    "${CMAKE_SOURCE_DIR}/test_stream_control.cpp"
    "${CMAKE_SOURCE_DIR}/test_audio_pipe.cpp"
    "${CMAKE_SOURCE_DIR}/test_etl_asserts.cpp"
    "${CMAKE_SOURCE_DIR}/test_config_service.cpp"
    "${CMAKE_SOURCE_DIR}/test_error_service.cpp"
    "${CMAKE_SOURCE_DIR}/test_persist_service.cpp"
    "${CMAKE_SOURCE_DIR}/test_liveness.cpp"
    "${CMAKE_SOURCE_DIR}/test_base_module.cpp"
    "${CMAKE_SOURCE_DIR}/test_module_manager.cpp"    # request_stop_all(): on_stop() on the update_all() task
    "${CMAKE_SOURCE_DIR}/test_shared_state.cpp"
    "${CMAKE_SOURCE_DIR}/test_json_escape.cpp"
    "${CMAKE_SOURCE_DIR}/test_utf8.cpp"              # modesp/utf8.h — cutting text on a character boundary
    "${CMAKE_SOURCE_DIR}/test_ws_slots.cpp"          # modesp/net/ws_slots.h — WsService's slot rules
    "${CMAKE_SOURCE_DIR}/test_module_commands.cpp"
    "${CMAKE_SOURCE_DIR}/test_safe_file.cpp"
    "${CMAKE_SOURCE_DIR}/test_user_store.cpp"
    "${CMAKE_SOURCE_DIR}/test_user_backup.cpp"
    "${CMAKE_SOURCE_DIR}/test_service_readout.cpp"   # modules/ui_lvgl/include/service_readout.h
    "${CMAKE_SOURCE_DIR}/test_i18n.cpp"              # modules/ui_lvgl/include/i18n.h — the panel in four languages
    "${CMAKE_SOURCE_DIR}/test_vlist_window.cpp"      # modules/ui_lvgl/include/vlist_window.h
    "${CMAKE_SOURCE_DIR}/test_touch_gesture.cpp"     # modules/ui_lvgl/include/touch_gesture.h — a finger that scrolls sets nothing
    "${CMAKE_SOURCE_DIR}/test_ppa_geometry.cpp"      # modules/ui_lvgl/include/ppa_geometry.h
    "${CMAKE_SOURCE_DIR}/test_web_media.cpp"         # modules/ui_lvgl/include/web_media.h — /api/library, /api/cover
    "${CMAKE_SOURCE_DIR}/test_net_probe_stats.cpp"   # components/net_probe/include/net_probe/probe_stats.h
    "${CMAKE_SOURCE_DIR}/test_stream_watch.cpp"      # modules/radio/include/stream_watch.h
)

add_executable(test_runner
    ${CORE_SOURCES}
    ${MODULE_SOURCES}
    ${TEST_SOURCES}
)

# ── Force-include prefix header path ──
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/mocks/host_prefix.h" HOST_PREFIX)

# ── Compile options ──
#
# MSVC is the compiler this suite meets on a Windows machine without MinGW (tools/tests/
# test_cpp_host.py looks for it after g++), and it takes none of the GCC spellings below:
# cl.exe rejects -Wno-* outright (D8021), forces a header with /FI rather than -include,
# reads sources in the ANSI code page unless told /utf-8, and only defines M_PI on request.
if(MSVC)
    target_compile_options(test_runner PRIVATE
        /W3
        /utf-8
        /bigobj
        /wd4244 /wd4267 /wd4305      # float/size_t narrowing — deliberate throughout the DSP code
        /wd4996                      # POSIX names (strdup, fileno) the mocks use
    )
    target_compile_definitions(test_runner PRIVATE
        HOST_BUILD=1
        ETL_NO_PROFILE_HEADER
        ETL_LOG_ERRORS
        ETL_CHECK_PUSH_POP
        _USE_MATH_DEFINES
    )
    target_compile_options(test_runner PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/FI${HOST_PREFIX}>")
else()
target_compile_options(test_runner PRIVATE
    -Wall
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-unused-function
    -DHOST_BUILD=1
    -DETL_NO_PROFILE_HEADER
    # The same two the firmware builds with (root CMakeLists.txt), so a container misuse
    # shows up here rather than on the panel. ETL_LOG_ERRORS routes failed assertions to the
    # callback test_main.cpp installs; ETL_CHECK_PUSH_POP adds the push_back/pop_back bounds
    # checks that are absent otherwise.
    #
    # NOTE the difference in what an assertion then DOES. g++ compiles with exceptions on and
    # ETL does not define ETL_THROW_EXCEPTIONS, so ETL_USING_EXCEPTIONS is 0 here as on the
    # target (etl/platform.h) and ETL itself never throws: with ETL_LOG_ERRORS the macro is
    # "call the handler", full stop. The handler decides — the host one records, and throws
    # only inside a test's ThrowScope; the firmware's aborts.
    -DETL_LOG_ERRORS
    -DETL_CHECK_PUSH_POP
    # The prefix header is C++ (it pulls <mutex>), so only force it into C++ sources.
    # components/audio_resampler is plain C and needs none of it.
    "$<$<COMPILE_LANGUAGE:CXX>:-include${HOST_PREFIX}>"
)
endif()

# Repo root as an absolute path, so a test that reads a shipped data file (data/eq.json)
# does not depend on the runner's working directory.
target_compile_definitions(test_runner PRIVATE MODESP_ROOT="${ROOT}")

# The same two the root CMakeLists sets for the firmware. Without them the host suite built
# StateValue with the 32-char default from modesp/types.h while the device built it with 64
# plus a side table — a suite that cannot see a truncation because it is not testing the
# sizes that truncate. Keep these in step with the idf_build_set_property() lines in
# ../../CMakeLists.txt; test_shared_state.cpp asserts the values it was told.
target_compile_definitions(test_runner PRIVATE
    MODESP_MAX_STRING_VALUE_LENGTH=64
    MODESP_MAX_LONG_STRING_LENGTH=256
)
# A directory the tests may write in. safe_file and UserStore are about what survives a
# power cut, so testing them means actually creating, replacing and removing files. Out of
# the source tree and out of the system temp dir: reproducible, and `rm -rf` on the build
# directory cleans it up.
set(TEST_TMP "${CMAKE_BINARY_DIR}/tmp")
file(MAKE_DIRECTORY "${TEST_TMP}")
target_compile_definitions(test_runner PRIVATE MODESP_TEST_TMP="${TEST_TMP}")

# ── Windows-specific definitions ──
if(WIN32)
    target_compile_definitions(test_runner PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
    # Static link runtime libs — eliminates MSYS2 DLL dependencies
    # (libstdc++, libgcc, libwinpthread, libmpc etc.). MSVC links its own runtime and
    # has no -static.
    if(NOT MSVC)
        target_link_options(test_runner PRIVATE -static)
    endif()
endif()

# ── Link with system threading library (for std::mutex in freertos_mock.h) ──
if(UNIX)
    target_link_libraries(test_runner PRIVATE pthread)
endif()

# ── Output binary name ──
set_target_properties(test_runner PROPERTIES OUTPUT_NAME "test_runner")

message(STATUS "ModESP host tests configured")
message(STATUS "  ETL:     ${ETL_INCLUDE}")
message(STATUS "  ROOT:    ${ROOT}")
message(STATUS "  MOCKS:   ${CMAKE_SOURCE_DIR}/mocks")
