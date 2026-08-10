# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# ---------------------------------------------------------------------------
# hathor-chuck-diagnostics — JUCE-free ChucK source validation
#
# Built unconditionally (even when HATHOR_BUILD_APP=OFF) because the AI-5
# tests require the real compiler diagnostics path. Links libchuck when
# available; falls back to bracket-balancing heuristic when not.
# ---------------------------------------------------------------------------
add_library(hathor-chuck-diagnostics OBJECT
    ../app/audio-worker/ChuckDiagnostics.cpp
)

target_include_directories(hathor-chuck-diagnostics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/app/audio-worker
    PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/include
)

target_compile_features(hathor-chuck-diagnostics PUBLIC cxx_std_20)

if(CHUCK_AVAILABLE)
    target_link_libraries(hathor-chuck-diagnostics PUBLIC libchuck)
    target_compile_definitions(hathor-chuck-diagnostics PUBLIC CHUCK_AVAILABLE=1)
else()
    target_compile_definitions(hathor-chuck-diagnostics PUBLIC CHUCK_AVAILABLE=0)
endif()

if(WIN32)
    target_compile_options(hathor-chuck-diagnostics PRIVATE /W4 /WX)
else()
    target_compile_options(hathor-chuck-diagnostics PRIVATE -Wall -Wextra -Werror)
endif()

# ---------------------------------------------------------------------------
# hathor-control — CLI/ACP control interface + AI-2 read-only service layer
#
# The control layer is JUCE-free (only includes AudioEngineFacade.hpp, the
# abstract interface). It is built unconditionally so that tests can link
# against ChuckSessionService, ControlInterface, etc. without JUCE.
# ---------------------------------------------------------------------------

add_library(hathor-control OBJECT
    ControlInterface.cpp
    ProjectReadFacade.cpp
    WorkerThread.cpp
    SocketServer.cpp
    ChuckSessionService.cpp
    JobTracker.cpp
)

target_include_directories(hathor-control
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/engine/include
        ${CMAKE_SOURCE_DIR}/app/audio-worker
    PRIVATE
        ${CMAKE_SOURCE_DIR}/ui
)

target_compile_features(hathor-control PUBLIC cxx_std_20)

target_link_libraries(hathor-control
    PUBLIC
        hathor-engine
        hathor-chuck-diagnostics
        nlohmann_json::nlohmann_json
)

if(WIN32)
    target_compile_options(hathor-control PRIVATE /W4 /WX)
else()
    target_compile_options(hathor-control PRIVATE -Wall -Wextra -Werror)
endif()
