cmake_minimum_required(VERSION 3.16)
project(ZeroTickSystems LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "The initial Kite Connect transport currently targets Windows (WinHTTP).")
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
    src/StateMachine.cpp
    src/Strategy.cpp
    src/KiteConnectApi.cpp
    src/Worker.cpp
    src/ThreadPool.cpp
)

target_include_directories(zerotick_core PUBLIC include)
target_link_libraries(
    zerotick_core
    PRIVATE nlohmann_json::nlohmann_json winhttp bcrypt ws2_32 shell32
)

add_executable(zerotick src/main.cpp)
target_link_libraries(zerotick PRIVATE zerotick_core)
