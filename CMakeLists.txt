cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(mxl-decklink
    VERSION 1.1.0
    DESCRIPTION "MXL MediaFunction container for Blackmagic DeckLink"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(MXL_DECKLINK_BUILD_TESTS "Build unit tests" ON)

find_package(mxl CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra)

# ------------------------------------------------------------------
# DeckLink API dispatch (loads libDeckLinkAPI.so at runtime via dlopen)
# ------------------------------------------------------------------
add_library(decklink-dispatch STATIC third_party/decklink/DeckLinkAPIDispatch.cpp)
target_include_directories(decklink-dispatch PUBLIC third_party/decklink)
target_link_libraries(decklink-dispatch PUBLIC ${CMAKE_DL_LIBS})
# The Blackmagic headers trip -Wmissing-field-initializers style warnings; keep them quiet.
target_compile_options(decklink-dispatch PRIVATE -w)

# ------------------------------------------------------------------
# Embedded web UI (§7.5): Vue SPA → single-file dist/index.html → header
# ------------------------------------------------------------------
# Docker multi-stage builds the SPA in a Node image and sets
# MXL_DECKLINK_USE_PREBUILT_WEBUI=ON. Local / CI builds run `npm ci &&
# npm run build` here so the embed always tracks web/src.
set(WEBUI_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/ops/webui_generated.hpp)

option(MXL_DECKLINK_USE_PREBUILT_WEBUI
    "Embed an existing web/dist/index.html instead of running npm" OFF)

# Explicit source list — avoid GLOB_RECURSE under web/ (it can pick up
# node_modules/.../src/... and create a ninja cycle on dist/index.html).
set(WEBUI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
    ${CMAKE_CURRENT_SOURCE_DIR}/web/package.json
    ${CMAKE_CURRENT_SOURCE_DIR}/web/package-lock.json
    ${CMAKE_CURRENT_SOURCE_DIR}/web/vite.config.js
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/main.js
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/App.vue
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/api.js
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/style.css
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/components/Pill.vue
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/components/SettingField.vue
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/components/Dashboard.vue
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/components/ChannelsTab.vue
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/components/AudioMatrixModal.vue
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/components/CardTab.vue
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/components/MxlTab.vue
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/components/SettingsTab.vue
)

if(MXL_DECKLINK_USE_PREBUILT_WEBUI)
    set(WEBUI_HTML ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html)
    if(NOT EXISTS ${WEBUI_HTML})
        message(FATAL_ERROR "MXL_DECKLINK_USE_PREBUILT_WEBUI=ON but ${WEBUI_HTML} is missing")
    endif()
    set(WEBUI_EMBED_COMMENT "Embedding prebuilt web/dist/index.html")
else()
    find_program(NPM_EXECUTABLE npm REQUIRED)
    # Build into the CMake binary dir so the source tree stays clean and the
    # OUTPUT never collides with a DEPENDS path under web/.
    set(WEBUI_HTML ${CMAKE_CURRENT_BINARY_DIR}/webui/index.html)
    add_custom_command(
        OUTPUT ${WEBUI_HTML}
        COMMAND ${NPM_EXECUTABLE} ci --no-fund --no-audit
        COMMAND ${NPM_EXECUTABLE} run build
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/webui
        COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html ${WEBUI_HTML}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web
        DEPENDS ${WEBUI_SOURCES}
        COMMENT "Building Vue web UI (npm run build)"
        VERBATIM
    )
    set(WEBUI_EMBED_COMMENT "Embedding built web UI")
endif()

add_custom_command(
    OUTPUT ${WEBUI_GENERATED}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${WEBUI_HTML}
        -DOUTPUT=${WEBUI_GENERATED}
        -DVAR=indexHtml
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${WEBUI_HTML} cmake/EmbedFile.cmake
    COMMENT ${WEBUI_EMBED_COMMENT}
)

# ------------------------------------------------------------------
# Core library (everything except main) so tests can link against it
# ------------------------------------------------------------------
add_library(mxl-decklink-core STATIC
    ${WEBUI_GENERATED}
    src/config/config.cpp
    src/config/schema.cpp
    src/config/store.cpp
    src/config/videomodes.cpp
    src/util/logging.cpp
    src/util/taiclock.cpp
    src/util/v210.cpp
    src/util/audioconv.cpp
    src/util/anc.cpp
    src/util/uuid.cpp
    src/util/threading.cpp
    src/mxlbridge/domain.cpp
    src/mxlbridge/domainscan.cpp
    src/mxlbridge/flowdef.cpp
    src/mxlbridge/videowriter.cpp
    src/mxlbridge/videoreader.cpp
    src/mxlbridge/audiowriter.cpp
    src/mxlbridge/audioreader.cpp
    src/mxlbridge/ancwriter.cpp
    src/decklink/devicemanager.cpp
    src/decklink/sdk/sdkbackend.cpp
    src/decklink/mock/mockbackend.cpp
    src/channel/input_channel.cpp
    src/channel/output_channel.cpp
    src/channel/channel_manager.cpp
    src/ops/metrics.cpp
    src/ops/httpserver.cpp
    src/ops/health.cpp
    src/ops/housekeeping.cpp
    src/ops/webapi.cpp
)
target_include_directories(mxl-decklink-core PUBLIC src third_party ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(mxl-decklink-core
    PUBLIC
        mxl::mxl
        decklink-dispatch
        Threads::Threads
)

add_executable(mxl-decklink src/main.cpp)
target_link_libraries(mxl-decklink PRIVATE mxl-decklink-core)

install(TARGETS mxl-decklink RUNTIME DESTINATION bin)

# ------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------
if(MXL_DECKLINK_BUILD_TESTS)
    enable_testing()
    add_executable(unit-tests
        tests/unit/test_main.cpp
        tests/unit/test_config.cpp
        tests/unit/test_videomodes.cpp
        tests/unit/test_v210.cpp
        tests/unit/test_audioconv.cpp
        tests/unit/test_anc.cpp
        tests/unit/test_metrics.cpp
        tests/unit/test_uuid.cpp
        tests/unit/test_taiclock.cpp
        tests/unit/test_flowdef.cpp
        tests/unit/test_store.cpp
        tests/unit/test_domainscan.cpp
        tests/unit/test_schema.cpp
    )
    target_link_libraries(unit-tests PRIVATE mxl-decklink-core)
    add_test(NAME unit-tests COMMAND unit-tests)
endif()
