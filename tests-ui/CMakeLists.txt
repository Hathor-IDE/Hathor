# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tests-ui/CMakeLists.txt
#
# Defines:
#   acp_spike         — Task 0.1 transport spike
#   hathor-ui-tests   — Task 1.5 PBT / headless tests (Req 31.3, 31.4)
#
# No JUCE audio device dependency — all tests run headless.

# ---------------------------------------------------------------------------
# acp_spike — Task 0.1
# ---------------------------------------------------------------------------
add_executable(acp_spike
    test_acp_spike.cpp
)

target_compile_features(acp_spike PRIVATE cxx_std_20)

target_compile_options(acp_spike PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(acp_spike PRIVATE nlohmann_json::nlohmann_json)

add_test(NAME acp_spike COMMAND acp_spike)

# ---------------------------------------------------------------------------
# hathor-ui-tests — headless PBT runner (Req 31.3, 31.4)
#
# Links hathor-engine + Catch2 only; no JUCE audio device.
# Sources: property tests P1 (SPSC ring buffer), P2 (HathorFile), P3 (tokeniser).
# ---------------------------------------------------------------------------
add_executable(hathor-ui-tests
    test_spsc_ring_buffer.cpp
    test_hathor_file_parser.cpp
    test_mini_tokeniser.cpp
    test_chuck_tokeniser.cpp
    test_tree_builder.cpp
    test_bake_to_song.cpp
    test_thread_state.cpp
    # AI-4: LSP unit tests (JUCE-free, req 31.1)
    test_lsp_protocol.cpp
    test_lsp_jsonrpc.cpp
    test_lsp_completion_logic.cpp
    ${CMAKE_SOURCE_DIR}/ui/HathorFileParser.cpp
    ${CMAKE_SOURCE_DIR}/ui/ExplorerFileTypes.cpp
    ${CMAKE_SOURCE_DIR}/ui/TreeBuilder.cpp
    ${CMAKE_SOURCE_DIR}/ui/ChuckKeywords.cpp
    ${CMAKE_SOURCE_DIR}/ui/LspJsonRpc.cpp
    ${CMAKE_SOURCE_DIR}/ui/LspCompletionLogic.cpp
    ${CMAKE_SOURCE_DIR}/ui/LspDiagnosticsDisplay.cpp
    ${CMAKE_SOURCE_DIR}/app/LiveJamSessionManager.cpp
)

target_compile_features(hathor-ui-tests PRIVATE cxx_std_20)

target_compile_options(hathor-ui-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-ui-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/app
        ${CMAKE_SOURCE_DIR}/engine/include
        ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(hathor-ui-tests
    PRIVATE
        hathor-engine
        Catch2::Catch2WithMain
)

# Register all Catch2 tests with CTest.
include(CTest)
include(Catch)
catch_discover_tests(hathor-ui-tests)
