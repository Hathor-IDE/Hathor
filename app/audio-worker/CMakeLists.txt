# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# worker/CMakeLists.txt — hathor-audio-worker companion process (B4-K2)
#
# This is a JUCE-free standalone executable that owns ChucK VM lifecycle
# (filled in by B4-K3) and produces audio samples into shared memory.
# It is built unconditionally (independent of HATHOR_BUILD_APP) so the
# AudioWorkerManager tests can run it as a child process.

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

# The worker does not link JUCE — it is a pure POSIX process.
# It only needs the standard C/C++ library and POSIX realtime extensions.
if(APPLE)
    find_library(LIBRT rt)
    if(LIBRT)
        target_link_libraries(hathor-audio-worker PRIVATE ${LIBRT})
    endif()
elseif(UNIX)
    target_link_libraries(hathor-audio-worker PRIVATE rt)
endif()
