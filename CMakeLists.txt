cmake_minimum_required(VERSION 3.16)
project(ZeroTickSystems LANGUAGES CXX)
include(CTest)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR "The Kite Connect transport supports Windows and macOS.")
endif()

if(APPLE)
    find_package(CURL REQUIRED)
endif()

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(zerotick_core
    src/BacktestEngine.cpp
    src/KiteConnectApi.cpp
    src/EngineTypes.cpp
    src/Log.cpp
    src/StructuredEventWriter.cpp
    src/RuntimeConfig.cpp
    src/StockSelectionEngine.cpp
    src/TradingDecisionEngine.cpp
    src/TradeHandlingEngine.cpp
)

target_include_directories(zerotick_core PUBLIC include)
target_link_libraries(
    zerotick_core
    PUBLIC nlohmann_json::nlohmann_json
)
if(WIN32)
    target_link_libraries(zerotick_core PRIVATE winhttp bcrypt ws2_32 shell32)
elseif(APPLE)
    target_link_libraries(zerotick_core PRIVATE CURL::libcurl)
endif()

add_executable(zerotick src/main.cpp)
target_link_libraries(zerotick PRIVATE zerotick_core)

if(BUILD_TESTING)
    add_executable(engine_tests tests/EngineTests.cpp)
    target_link_libraries(engine_tests PRIVATE zerotick_core)
    add_test(NAME engine_tests COMMAND engine_tests)
    set_tests_properties(
        engine_tests
        PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
