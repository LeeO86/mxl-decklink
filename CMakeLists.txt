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
# Core library (everything except main) so tests can link against it
# ------------------------------------------------------------------
add_library(mxl-decklink-core STATIC
    src/config/config.cpp
    src/config/videomodes.cpp
    src/util/logging.cpp
    src/util/taiclock.cpp
    src/util/v210.cpp
    src/util/audioconv.cpp
    src/util/anc.cpp
    src/util/uuid.cpp
    src/util/threading.cpp
    src/mxlbridge/domain.cpp
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
)
target_include_directories(mxl-decklink-core PUBLIC src third_party)
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
    )
    target_link_libraries(unit-tests PRIVATE mxl-decklink-core)
    add_test(NAME unit-tests COMMAND unit-tests)
endif()
