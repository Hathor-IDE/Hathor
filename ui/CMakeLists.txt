# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ui/CMakeLists.txt — Phase 2 UI targets (Req 31.1, 31.2, 31.5)
#
# Defines:
#   hathor-ui      — full JUCE GUI application (Req 31.1)
#   hathor-mcp     — JUCE-free standalone MCP server (Req 31.1, 32.7)

if(NOT HATHOR_BUILD_APP)
    message(STATUS "HATHOR_BUILD_APP=OFF — skipping ui/ targets")
    return()
endif()

# JUCE must already be fetched by app/CMakeLists.txt (same build graph).
# If app/ was skipped, we cannot build UI targets either.
if(NOT TARGET juce::juce_gui_basics)
    message(STATUS "JUCE not available — skipping ui/ targets")
    return()
endif()

# ---------------------------------------------------------------------------
# hathor-ui — full JUCE GUI application (Req 31.1, 31.5)
#
# Links: hathor-engine + juce_gui_basics + juce_gui_extra + juce_audio_devices
# Compile flags: -Wall -Wextra -Werror (consistent with engine, Req 31.5)
# ---------------------------------------------------------------------------
juce_add_gui_app(hathor-ui
    PRODUCT_NAME "HathorUI"
    VERSION      "0.2.0"
)

target_sources(hathor-ui
    PRIVATE
        HathorApplication.cpp
        MainWindow.cpp
        ActivityRibbon.cpp
        ExplorerPanel.cpp
        HathorFileParser.cpp
        HathorTab.cpp         # task 3.4
        EditorArea.cpp        # task 3.4
        MiniNotationTokeniser.cpp # task 3.5
        UITimer.cpp               # task 3.7
        VisualizerPanel.cpp       # task 3.8
        SliderPanel.cpp           # task 3.9
        AcpAgentSession.cpp           # task 4.2–4.5
        PermissionPromptComponent.cpp # task 4.4
        # App-layer sources (AudioEngine, SampleBank, VoicePool live in app/,
        # not in a linkable static lib — include them directly here)
        ${CMAKE_SOURCE_DIR}/app/AudioEngine.cpp
        ${CMAKE_SOURCE_DIR}/app/SampleBank.cpp
        ${CMAKE_SOURCE_DIR}/app/VoicePool.cpp
)

target_include_directories(hathor-ui
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/app
        ${CMAKE_SOURCE_DIR}/engine/include
        ${CMAKE_SOURCE_DIR}/control
)

target_compile_features(hathor-ui PRIVATE cxx_std_20)

target_compile_options(hathor-ui PRIVATE -Wall -Wextra -Werror)

target_link_libraries(hathor-ui
    PRIVATE
        hathor-engine
        hathor-control
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_basics
        juce::juce_core
        nlohmann_json::nlohmann_json
)

target_compile_definitions(hathor-ui
    PRIVATE
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
)

# ---------------------------------------------------------------------------
# hathor-mcp — standalone JUCE-free MCP server (Req 31.1, 32.7)
#
# Links: hathor-engine only — zero juce:: links enforced here.
# Compile flags: -Wall -Wextra -Werror
# ---------------------------------------------------------------------------
add_executable(hathor-mcp
    mcp/HathorMcpServer.cpp
)

target_include_directories(hathor-mcp
    PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/include
)

target_compile_features(hathor-mcp PRIVATE cxx_std_20)

target_compile_options(hathor-mcp PRIVATE -Wall -Wextra -Werror)

target_link_libraries(hathor-mcp
    PRIVATE
        hathor-engine
        nlohmann_json::nlohmann_json
)

# Verify no JUCE symbols slip in via a post-build nm check on macOS/Linux.
# This is a belt-and-suspenders guard — the real enforcement is "no juce:: in
# target_link_libraries above."
if(APPLE OR UNIX)
    add_custom_command(TARGET hathor-mcp POST_BUILD
        COMMAND sh -c
            "nm '$<TARGET_FILE:hathor-mcp>' 2>/dev/null | grep -i juce && echo 'ERROR: JUCE symbols found in hathor-mcp' && exit 1 || true"
        COMMENT "Verifying hathor-mcp contains zero JUCE symbols"
        VERBATIM
    )
endif()
