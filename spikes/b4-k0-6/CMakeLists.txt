cmake_minimum_required(VERSION 3.24)
project(b4_k0_6_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(hathor-audio-worker
    hathor-audio-worker.cpp
)
target_compile_features(hathor-audio-worker PRIVATE cxx_std_20)
target_compile_options(hathor-audio-worker PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(hathor-audio-worker PRIVATE nlohmann_json::nlohmann_json)

add_executable(b4_k0_6_ipc_spike
    test_b4_k0_6.cpp
)
target_compile_features(b4_k0_6_ipc_spike PRIVATE cxx_std_20)
target_compile_options(b4_k0_6_ipc_spike PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(b4_k0_6_ipc_spike PRIVATE nlohmann_json::nlohmann_json)
