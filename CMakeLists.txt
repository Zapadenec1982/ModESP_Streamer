# ModESP Streamer — ESP32-P4 network stereo streamer on the ModESP v4 framework
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "drivers"
    "modules"
)

# ── Board selection ───────────────────────────────────────────────────────────
# The firmware name, board.json copy, and code generation below all need the board
# BEFORE project()/Kconfig confgen runs, so we mirror confgen's OWN resolution order.
# This matters because `idf.py set-target <t>` renames sdkconfig away before its
# configure pass — and the following `idf.py build` won't re-configure — so this pass
# must already pick the right board, in one shot:
#   1. sdkconfig exists → it is the resolved config; read CONFIG_MODESP_BOARD_DIR.
#   2. no sdkconfig yet  → read the same defaults confgen will apply:
#                          sdkconfig.defaults then sdkconfig.defaults.<target>
#                          (target overlay wins), deriving the board dir from the
#                          selected choice symbol (CONFIG_MODESP_BOARD_<X>=y → <x>),
#                          matching main/Kconfig.projbuild.
# Silently defaulting to "dev" when sdkconfig was merely absent was a footgun: it
# shipped a dev-GPIO binary onto an S3 board. The post-project guard below backstops
# any drift between this bootstrap and the authoritative Kconfig value.
set(_BOARD_DIR "")

if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig")
    file(STRINGS "${CMAKE_SOURCE_DIR}/sdkconfig" _sdkconfig_lines
         REGEX "^CONFIG_MODESP_BOARD_DIR=")
    foreach(_line ${_sdkconfig_lines})
        if(_line MATCHES "^CONFIG_MODESP_BOARD_DIR=\"(.+)\"$")
            set(_BOARD_DIR "${CMAKE_MATCH_1}")
        endif()
    endforeach()
else()
    # No sdkconfig yet (fresh checkout, `idf.py set-target`, or someone deleted it to
    # force a defaults re-apply). Resolve from the defaults overlay exactly as confgen
    # will, rather than guessing the wrong board.
    set(_IDF_TARGET "")
    if(DEFINED IDF_TARGET)
        set(_IDF_TARGET "${IDF_TARGET}")
    elseif(DEFINED ENV{IDF_TARGET})
        set(_IDF_TARGET "$ENV{IDF_TARGET}")
    endif()

    set(_defaults_files "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
    if(_IDF_TARGET)
        list(APPEND _defaults_files
             "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.${_IDF_TARGET}")
    endif()

    foreach(_defaults_file ${_defaults_files})
        if(EXISTS "${_defaults_file}")
            file(STRINGS "${_defaults_file}" _board_lines
                 REGEX "^CONFIG_MODESP_BOARD_[A-Z0-9_]+=y$")
            foreach(_line ${_board_lines})
                if(_line MATCHES "^CONFIG_MODESP_BOARD_([A-Z0-9_]+)=y$")
                    string(TOLOWER "${CMAKE_MATCH_1}" _BOARD_DIR)
                endif()
            endforeach()
        endif()
    endforeach()

    if(_BOARD_DIR)
        message(STATUS "ModESP board: resolved '${_BOARD_DIR}' from sdkconfig.defaults"
                       " (no sdkconfig yet, target='${_IDF_TARGET}')")
    endif()
endif()

if(NOT _BOARD_DIR)
    set(_BOARD_DIR "dev")
    message(WARNING
        "ModESP: could not determine the target board from sdkconfig or "
        "sdkconfig.defaults — falling back to '${_BOARD_DIR}'. If that is not "
        "intended, run 'idf.py set-target <target>' to regenerate sdkconfig from "
        "the defaults, then rebuild.")
endif()

set(_BOARD_PATH "${CMAKE_SOURCE_DIR}/boards/${_BOARD_DIR}")
if(NOT EXISTS "${_BOARD_PATH}/board.json")
    message(FATAL_ERROR "Board '${_BOARD_DIR}' not found in boards/")
endif()
file(COPY "${_BOARD_PATH}/board.json" DESTINATION "${CMAKE_SOURCE_DIR}/data")
file(COPY "${_BOARD_PATH}/bindings.json" DESTINATION "${CMAKE_SOURCE_DIR}/data")
message(STATUS "ModESP board: ${_BOARD_DIR}")

# player-fork: the device reads board.json and bindings.json into ONE fixed buffer, and a
# file bigger than it does not get parsed: the parser now refuses it outright and says so,
# but config is a CRITICAL module, so the board still goes to safe mode with no HAL, no audio
# and no screen. Before that refusal existed the failure was worse — fread stopped at the
# limit and jsmn reported "parse error: -3", with nothing about size — and it cost a flashed
# panel, from adding comments to a label. The limit is read out of the header
# so there is one copy of the number, the same one the firmware and POST /api/bindings use.
file(READ "${CMAKE_SOURCE_DIR}/components/modesp_services/include/modesp/services/config_service.h" _CFG_SRC)
string(REGEX MATCH "MAX_JSON_SIZE[ 	]*=[ 	]*([0-9]+)" _M "${_CFG_SRC}")
if(CMAKE_MATCH_1)
    set(_JSON_LIMIT ${CMAKE_MATCH_1})
    foreach(_f board.json bindings.json)
        file(SIZE "${_BOARD_PATH}/${_f}" _SZ)
        if(_SZ GREATER_EQUAL ${_JSON_LIMIT})
            message(FATAL_ERROR
                "boards/${_BOARD_DIR}/${_f} is ${_SZ} bytes; the on-device parser buffer is "
                "${_JSON_LIMIT}. It would be read truncated and config init would fail, which "
                "is a CRITICAL module — the panel would boot with no HAL. Shorten the labels "
                "(prose belongs in docs/), or raise ConfigService::MAX_JSON_SIZE in "
                "components/modesp_services/include/modesp/services/config_service.h.")
        endif()
        math(EXPR _LEFT "${_JSON_LIMIT} - ${_SZ}")
        message(STATUS "  ${_f}: ${_SZ} bytes, ${_LEFT} to spare")
    endforeach()
else()
    message(WARNING "Could not read ConfigService::MAX_JSON_SIZE from config_service.h — "
                    "board config size is NOT being checked.")
endif()

# ── Board profile: apply boards/<board>/sdkconfig.board on top of the shared
# sdkconfig.defaults (ESP-IDF also auto-applies each entry's .<target> overlay). Set
# BEFORE project(); the board dir is resolved above. So a new board ships its own
# config in its folder — zero edits to the global sdkconfig.defaults.<target>. NOTE:
# these seed only a FRESH sdkconfig (idf.py set-target / first build); an existing
# sdkconfig already holds the resolved values (change them via menuconfig).
set(_sdkconfig_defaults "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
if(EXISTS "${_BOARD_PATH}/sdkconfig.board")
    list(APPEND _sdkconfig_defaults "${_BOARD_PATH}/sdkconfig.board")
endif()
# -DMODESP_SOAK=ON adds the board's soak overlay (heap poisoning) on top. One long run
# before a release, not a shipping default — the overlay file says why. It only seeds a
# FRESH sdkconfig, like everything else here, so: delete sdkconfig, set-target, build.
option(MODESP_SOAK "Build the soak image: extra runtime checking, slower" OFF)
if(MODESP_SOAK AND EXISTS "${_BOARD_PATH}/sdkconfig.soak")
    list(APPEND _sdkconfig_defaults "${_BOARD_PATH}/sdkconfig.soak")
    message(STATUS "  SOAK build: ${_BOARD_PATH}/sdkconfig.soak applied")
endif()
# -DMODESP_NETTEST=ON adds the board's network-test overlay: the UDP probe that
# tools/net_probe.py talks to (components/net_probe). A bench image, never a shipping one —
# the probe's port is unauthenticated. Same rule: it seeds only a FRESH sdkconfig.
option(MODESP_NETTEST "Build the network-test image: UDP link probe" OFF)
if(MODESP_NETTEST AND EXISTS "${_BOARD_PATH}/sdkconfig.nettest")
    list(APPEND _sdkconfig_defaults "${_BOARD_PATH}/sdkconfig.nettest")
    message(STATUS "  NETTEST build: ${_BOARD_PATH}/sdkconfig.nettest applied")
endif()
set(SDKCONFIG_DEFAULTS "${_sdkconfig_defaults}")

# Re-run CMake configure (which re-copies the two files above into data/) whenever
# the active board's board.json/bindings.json/sdkconfig.board change. Without this,
# editing them does NOT refresh data/ — file(COPY) only runs at configure time and
# these are not tracked as configure inputs — so the LittleFS data image keeps the
# STALE hardware map and the device boots the old config until a manual reconfigure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_BOARD_PATH}/board.json"
    "${_BOARD_PATH}/bindings.json"
    "${_BOARD_PATH}/sdkconfig.board")

# Re-run CMake configure whenever ANY single-source-of-truth input changes.
# Without this, editing a manifest/project.json after the first configure silently
# builds with STALE generated artifacts (ui.json, state_meta.h, mqtt_topics.h,
# drivers.cmake, Kconfig) — breaking the manifest-driven contract.
# The GLOBs (with CONFIGURE_DEPENDS) retrigger on manifest ADD/REMOVE; listing the
# matched files in CMAKE_CONFIGURE_DEPENDS retriggers on CONTENT edits.
file(GLOB _modesp_module_manifests CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/modules/*/manifest.json")
file(GLOB _modesp_driver_manifests CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/drivers/*/manifest.json")
file(GLOB _modesp_board_manifests CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/boards/*/board.json")
file(GLOB _modesp_i18n_inputs CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/modules/*/i18n/*.json"
     "${CMAKE_SOURCE_DIR}/data/i18n/*.json")
file(GLOB _modesp_schemas CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/tools/schemas/*.schema.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/project.json"
    "${CMAKE_SOURCE_DIR}/tools/generate_ui.py"
    "${CMAKE_SOURCE_DIR}/tools/system_pages.json"
    ${_modesp_module_manifests}
    ${_modesp_driver_manifests}
    ${_modesp_board_manifests}
    ${_modesp_i18n_inputs}
    ${_modesp_schemas})

# ── Pre-build: generate UI schema and C++ headers from module manifests ──
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/generate_ui.py"
            --project     "${CMAKE_SOURCE_DIR}/project.json"
            --modules-dir "${CMAKE_SOURCE_DIR}/modules"
            --output-data "${CMAKE_SOURCE_DIR}/data"
            --output-gen  "${CMAKE_SOURCE_DIR}/generated"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE GEN_RESULT
    )
    if(NOT GEN_RESULT EQUAL 0)
        message(FATAL_ERROR "generate_ui.py failed (exit code ${GEN_RESULT})")
    endif()
    # GET /api/ui sends ui.json.gz (HttpService::handle_get_ui): the schema is ~86 KB as written
    # and ~12 KB packed, and every page load and every start of the phone app asks for it.
    # mtime=0, so the same schema packs to the same bytes and data.bin does not change for nothing.
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c
            "import gzip, sys; open(sys.argv[2], 'wb').write(gzip.compress(open(sys.argv[1], 'rb').read(), 9, mtime=0))"
            "${CMAKE_SOURCE_DIR}/data/ui.json" "${CMAKE_SOURCE_DIR}/data/ui.json.gz"
        RESULT_VARIABLE GZ_RESULT
    )
    if(NOT GZ_RESULT EQUAL 0)
        message(FATAL_ERROR "packing data/ui.json failed (exit code ${GZ_RESULT})")
    endif()
else()
    message(FATAL_ERROR "Python3 not found — cannot generate UI. Install Python 3.")
endif()

# Читаємо назву плати з board.json для ідентифікації прошивки
file(READ "${CMAKE_SOURCE_DIR}/data/board.json" _BOARD_JSON_CONTENT)
string(JSON _BOARD_NAME GET "${_BOARD_JSON_CONTENT}" "board")
message(STATUS "ModESP board name: ${_BOARD_NAME}")

# ── Trim the component graph to the product (single source of truth: project.json) ──
# generated/modules.cmake (written by generate_ui.py above) defines PRODUCT_MODULES.
# With COMPONENTS set, a module folder that is NOT in project.json no longer compiles;
# framework components enter the graph transitively via REQUIRES. Cloud/BLE components
# are listed explicitly because main links them via target_link_libraries(idf::...),
# which does not participate in component dependency discovery.
include("${CMAKE_SOURCE_DIR}/generated/modules.cmake")
# player-fork: espcoredump is not pulled in transitively — without it the CONFIG_ESP_COREDUMP_*
# symbols do not exist and the panic handler never writes the coredump partition.
set(COMPONENTS main ${PRODUCT_MODULES} modesp_mqtt modesp_ble espcoredump)

# ── Firmware version (semver) ──
# Used by esp_app_get_description()->version in heartbeat & OTA version matching.
# Single source of truth: project.json "version" — bump it there before a release.
file(READ "${CMAKE_SOURCE_DIR}/project.json" _PROJECT_JSON_CONTENT)
string(JSON PROJECT_VER GET "${_PROJECT_JSON_CONTENT}" "version")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# SharedState string values are etl::string<MODESP_MAX_STRING_VALUE_LENGTH>, and etl
# truncates silently. Upstream's 32 is right for a fridge, where every string is a short
# enum-ish word. A player puts FILE PATHS and TRACK TITLES through the same store:
# "/sd/Deep Night Deep House 9 (2026)/04. Apelislin - Slow Dance.mp3" reached the player as
# "/sd/Deep Night Deep House 9 (202" and could not be opened, and titles were being cut at
# 32 characters on screen for the same reason.
#
# That was fixed by making the inline string 256 — and every one of the 189 map slots then
# paid for it, whether it holds a path or the word "idle". Measured: 67140 bytes of internal
# .bss, on a board where a DLNA stream leaves 2535 bytes of DMA-capable internal RAM free.
#
# So the two numbers are now separate. 64 is the widest SHORT value in the manifests
# (radio.name is radio_core::NAME_LEN = 64), and it is what every slot pays. The keys that
# carry a path or a URI say `"long": true` in their manifest and keep their text in
# SharedState's side table, which has one entry per such key rather than one per slot; 256
# is that table's width, chosen to match the browser by construction — Browse::path (160)
# + '/' + Entry::name (96) is 255, so no path the file screen can build is longer than the
# value that carries it.
#
# The knobs are the two #ifndefs in modesp/types.h, set here rather than edited there so the
# core component stays untouched — and set for the whole build, because StateValue appears
# in headers and every translation unit must agree on its size.
idf_build_set_property(COMPILE_DEFINITIONS "MODESP_MAX_STRING_VALUE_LENGTH=64" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "MODESP_MAX_LONG_STRING_LENGTH=256" APPEND)

# ETL checks its own containers only if the build says so, and this build did not. Exceptions
# are off (CONFIG_COMPILER_CXX_EXCEPTIONS=n), and with none of ETL_LOG_ERRORS /
# ETL_USE_ASSERT_FUNCTION / ETL_DEBUG defined, ETL_ASSERT expands to NOTHING
# (etl/error_handler.h). So etl::unordered_map::insert on a full map walked into an exhausted
# pool, and etl::vector::push_back had no bounds check at all — that one needs
# ETL_CHECK_PUSH_POP even when asserts do fire (etl/vector.h). Silent memory corruption in the
# two containers the whole control plane is built from.
#
# ETL_LOG_ERRORS routes every failed assertion to the etl::error_handler callback that
# app_main installs; ETL_CHECK_PUSH_POP adds the push_back/pop_back bounds checks, which are
# ETL_ASSERT_OR_RETURN — they return safely once the callback has been told. The plain
# ETL_ASSERT sites do NOT: they carry on into the unsafe path when the callback returns, so
# the callback aborts and the coredump partition keeps the evidence.
#
# NOT ETL_VERBOSE_ERRORS: it puts __FILE__ into every assertion object, i.e. ETL's header
# paths into flash. The terse text ("vector:full") plus the line number identifies the site
# well enough, and the coredump has the backtrace.
#
# Build-wide, like the string length above: the containers appear in headers, and a
# translation unit that disagreed about whether they are checked would be an ODR violation.
idf_build_set_property(COMPILE_DEFINITIONS "ETL_LOG_ERRORS" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "ETL_CHECK_PUSH_POP" APPEND)

project(modesp_${_BOARD_NAME})

# ── Board-selection guard ─────────────────────────────────────────────────────
# CONFIG_MODESP_BOARD_DIR is the authoritative board, resolved by Kconfig/confgen
# during project() above (it reflects sdkconfig plus any sdkconfig.defaults.<target>
# overlay). Everything board-specific — the firmware name, data/board.json, and the
# generated headers — was emitted earlier from the PRE-project _BOARD_DIR bootstrap.
# If the two disagree, those artifacts were built for the wrong board (e.g. a stale or
# just-created sdkconfig), so fail loudly instead of silently shipping them. By now
# confgen has written a correct sdkconfig, so a single re-run resolves the mismatch.
if(DEFINED CONFIG_MODESP_BOARD_DIR AND
   NOT "${CONFIG_MODESP_BOARD_DIR}" STREQUAL "${_BOARD_DIR}")
    message(FATAL_ERROR
        "ModESP board mismatch: build artifacts (firmware 'modesp_${_BOARD_NAME}', "
        "data/board.json, generated headers) were produced for board '${_BOARD_DIR}', "
        "but Kconfig resolved CONFIG_MODESP_BOARD_DIR='${CONFIG_MODESP_BOARD_DIR}'. "
        "sdkconfig is now up to date — re-run 'idf.py build' so the resolved board is used.")
endif()

# Generate LittleFS image from data/ directory and flash to "data" partition.
# DEPENDS on the actual PAYLOAD so data.bin is regenerated (and `idf.py flash` picks it up)
# whenever any of it changes — board config copies (refreshed by file(COPY) above), the
# generated UI schema, AND the deployed WebUI bundle. Without the ui.json/www deps the image
# kept a STALE webui/ui.json after `npm run deploy` or a manifest change: the device booted
# the old page until a manual `Remove-Item build/data.bin` / fullclean.
littlefs_create_partition_image(data data FLASH_IN_PROJECT
    DEPENDS "${CMAKE_SOURCE_DIR}/data/board.json"
            "${CMAKE_SOURCE_DIR}/data/bindings.json"
            "${CMAKE_SOURCE_DIR}/data/ui.json"
            "${CMAKE_SOURCE_DIR}/data/ui.json.gz"
            "${CMAKE_SOURCE_DIR}/data/www/build.txt"             # every bundle, chunk and language
                                                                 # file webui deployed, with hashes
                                                                 # (webui/scripts/deploy.js)
            "${CMAKE_SOURCE_DIR}/data/www/index.html"
            "${CMAKE_SOURCE_DIR}/data/www/radio.html"            # now a redirect to #/stations/top
            "${CMAKE_SOURCE_DIR}/data/www/radio_genres.json")    # its genres, and the panel's

# There is deliberately NO littlefs_create_partition_image() for `userdata`. That partition
# is what a person's own files live in, and an image for it would be written by every
# `idf.py flash` — which is precisely the thing this split exists to stop. The firmware
# formats it on its first boot instead (UserStore::begin).
#
# Guard the table rather than trust it: without a `userdata` row the firmware still boots
# (it falls back to /data and says so), but the protection is silently gone and the next
# full flash takes the EQ with it again. Cheaper to fail here.
file(READ "${CMAKE_SOURCE_DIR}/partitions_p4_16mb.csv" _PART_CSV)
if(NOT _PART_CSV MATCHES "(^|\n)[ \t]*userdata[ \t]*,")
    message(FATAL_ERROR
        "partitions_p4_16mb.csv has no 'userdata' partition. Runtime user files (eq.json, "
        "stations.json, last_track, devices.json) would go back to living in 'data', which "
        "`idf.py flash` rewrites whole, user files and all. Restore the row, or delete this "
        "guard knowingly.")
endif()
if(_PART_CSV MATCHES "(^|\n)[ \t]*userdata[ \t]*,[^\n]*")
    message(STATUS "  userdata partition present; no image is built for it (that is the point)")
endif()

# ── LVGL: speed for the renderer, size everywhere else ───────────────────────────
# The firmware builds with -Os. LVGL's software renderer is per-pixel loops — fills, glyph
# blending, the layout of every label's text — the same kind of code
# components/audio_dsp/CMakeLists.txt raised to -O2 after measuring what -Os cost there.
# Scrolling a list redraws the whole content area every frame, which is where it shows.
# The 4 MB app slots leave the room.
idf_build_get_property(_build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST _build_components)
    idf_component_get_property(_lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_options(${_lvgl_lib} PRIVATE -O2)
endif()
