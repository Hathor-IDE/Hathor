# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# app/audio-worker/CMakeLists.txt — K2 AudioWorkerManager + worker executable
#
# The AudioWorkerManager is compiled as a JUCE-free static library so it can be
# linked by both the main app (when HATHOR_BUILD_APP=ON) and the test targets
# (which do not depend on JUCE).

# ---------------------------------------------------------------------------
# hathor-audio-worker-lib — JUCE-free static library (AudioWorkerManager + K3/K4)
# ---------------------------------------------------------------------------
add_library(hathor-audio-worker-lib STATIC
    AudioWorkerManager.cpp
    ChuckCompiler.cpp
    ChuckDiagnostics.cpp
    ChuckVM.cpp
    ResourcePolicy.cpp
    VMManager.cpp
    VmLifecycle.cpp
    VmWatchdog.cpp
)

target_include_directories(hathor-audio-worker-lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/engine/include
)

target_compile_features(hathor-audio-worker-lib PUBLIC cxx_std_20)
target_compile_options(hathor-audio-worker-lib PRIVATE -Wall -Wextra -Werror)

# ---------------------------------------------------------------------------
# hathor-audio-worker — standalone JUCE-free companion process executable
# ---------------------------------------------------------------------------
add_executable(hathor-audio-worker
    hathor-audio-worker.cpp
)

target_include_directories(hathor-audio-worker
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/engine/include
)

target_compile_features(hathor-audio-worker PRIVATE cxx_std_20)
target_compile_options(hathor-audio-worker PRIVATE -Wall -Wextra -Werror)

target_link_libraries(hathor-audio-worker PRIVATE hathor-audio-worker-lib)

# POSIX realtime libraries for shm_open / mmap / posix_spawn
if(APPLE)
    find_library(LIBRT rt)
    if(LIBRT)
        target_link_libraries(hathor-audio-worker PRIVATE ${LIBRT})
    endif()
elseif(UNIX)
    target_link_libraries(hathor-audio-worker PRIVATE rt)
endif()
