# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# hathor-control is only needed when building the full app
if(NOT HATHOR_BUILD_APP)
    return()
endif()

# hathor-control — CLI/ACP control interface, compiled as an object library
# so its translation units are linked directly into the hathor executable.
add_library(hathor-control OBJECT
    ControlInterface.cpp
    WorkerThread.cpp
    SocketServer.cpp
)

target_include_directories(hathor-control
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/engine/include
)

target_compile_features(hathor-control PUBLIC cxx_std_20)

target_link_libraries(hathor-control
    PUBLIC
        hathor-engine
        nlohmann_json::nlohmann_json
)
