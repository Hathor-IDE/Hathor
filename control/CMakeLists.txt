# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# hathor-control is only needed when building the full app
if(NOT HATHOR_BUILD_APP)
    return()
endif()

# hathor-control — CLI/ACP control interface + AI-2 read-only service layer,
# compiled as an object library so its translation units are linked directly
# into the hathor executable.
add_library(hathor-control OBJECT
    ControlInterface.cpp
    ProjectReadFacade.cpp
    WorkerThread.cpp
    SocketServer.cpp
)

# ChuckDiagnostics.cpp is a JUCE-free translation unit that provides the
# real ChucK validation path used by ProjectReadFacade::getDiagnostics (AI-2).
add_library(hathor-chuck-diagnostics OBJECT
    ../app/audio-worker/ChuckDiagnostics.cpp
)

target_include_directories(hathor-control
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/engine/include
        ${CMAKE_SOURCE_DIR}/app/audio-worker
)

target_compile_features(hathor-control PUBLIC cxx_std_20)

target_link_libraries(hathor-control
    PUBLIC
        hathor-engine
        nlohmann_json::nlohmann_json
)
