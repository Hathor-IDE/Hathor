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
       # AI-4: Ghost text unit tests (JUCE-free, req 31.1)
         test_ghost_completion_logic.cpp
         test_completion_coordinator.cpp
         test_ghost_state_isolation.cpp
         test_ghost_candidates.cpp
          test_ghost_partial_accept.cpp
         test_ghost_completion_telemetry.cpp
       test_find_replace_model.cpp
       test_tab_reorder_model.cpp
       test_recently_closed_tabs.cpp
       test_navigation_history.cpp
       test_workspace_search_model.cpp
       test_symbol_search_model.cpp
       test_lsp_jsonrpc_navigation.cpp
        test_action_registry.cpp
        # L-3: Diagnostic registry unit tests
        test_diagnostic_registry.cpp
        # L-4: Terminal process + task runner unit tests (JUCE-free)
        test_terminal_process.cpp
        # L-5: Git process + repository unit tests (JUCE-free)
        test_git_process.cpp
        test_git_repository.cpp
        ${CMAKE_SOURCE_DIR}/ui/HathorFileParser.cpp
     ${CMAKE_SOURCE_DIR}/ui/ExplorerFileTypes.cpp
     ${CMAKE_SOURCE_DIR}/ui/TreeBuilder.cpp
     ${CMAKE_SOURCE_DIR}/ui/ChuckKeywords.cpp
     ${CMAKE_SOURCE_DIR}/ui/LspJsonRpc.cpp
     ${CMAKE_SOURCE_DIR}/ui/LspCompletionLogic.cpp
     ${CMAKE_SOURCE_DIR}/ui/LspDiagnosticsDisplay.cpp
      ${CMAKE_SOURCE_DIR}/ui/GhostJsonRpc.cpp
      ${CMAKE_SOURCE_DIR}/ui/GhostProviderConfig.cpp
       ${CMAKE_SOURCE_DIR}/ui/GhostCompletionLogic.cpp
        ${CMAKE_SOURCE_DIR}/ui/GhostTriggerPolicy.cpp
        ${CMAKE_SOURCE_DIR}/ui/CompletionCoordinator.cpp
        ${CMAKE_SOURCE_DIR}/ui/GhostCompletionTelemetry.cpp
     ${CMAKE_SOURCE_DIR}/ui/FindReplaceModel.cpp
       ${CMAKE_SOURCE_DIR}/ui/TabReorderModel.cpp
       ${CMAKE_SOURCE_DIR}/ui/RecentlyClosedTabs.cpp
      ${CMAKE_SOURCE_DIR}/ui/ActionRegistry.cpp
        ${CMAKE_SOURCE_DIR}/ui/NavigationHistory.cpp
        ${CMAKE_SOURCE_DIR}/ui/WorkspaceSearchModel.cpp
        ${CMAKE_SOURCE_DIR}/ui/SymbolSearchModel.cpp
      ${CMAKE_SOURCE_DIR}/control/DiagnosticRegistry.cpp
      ${CMAKE_SOURCE_DIR}/app/LiveJamSessionManager.cpp
      ${CMAKE_SOURCE_DIR}/ui/TerminalProcess.cpp
      ${CMAKE_SOURCE_DIR}/ui/TaskRunner.cpp
      # L-5: Git backend + repository model (JUCE-free)
      ${CMAKE_SOURCE_DIR}/ui/GitProcess.cpp
      ${CMAKE_SOURCE_DIR}/ui/GitRepository.cpp
      # L-6: Debugging + Hathor runtime inspection (JUCE-free)
      ${CMAKE_SOURCE_DIR}/ui/DebugSession.cpp
      ${CMAKE_SOURCE_DIR}/ui/RuntimeInspectorModel.cpp
      # Phase G — Petdex (D1): JUCE-free core (parser/cache/attribution)
      ${CMAKE_SOURCE_DIR}/ui/PetdexManifestParser.cpp
      ${CMAKE_SOURCE_DIR}/ui/PetdexCacheStore.cpp
      ${CMAKE_SOURCE_DIR}/ui/PetdexAttribution.cpp
      # Phase G — Petdex (D2–D3): JUCE-free sprite pipeline. PetdexHttp stays
      # out of the tests — it depends on juce::URL (juce_core).
      ${CMAKE_SOURCE_DIR}/ui/PetdexFrameGrid.cpp
      ${CMAKE_SOURCE_DIR}/ui/PetdexAnimation.cpp
      ${CMAKE_SOURCE_DIR}/ui/PetdexWebpDecoder.cpp
      ${CMAKE_SOURCE_DIR}/ui/PetdexResourceCache.cpp
      test_debug_output_parser.cpp
      test_debug_session.cpp
      test_runtime_inspector_model.cpp
      # Phase G — Petdex (D1): parser + cache + attribution gate (JUCE-free)
      test_petdex_manifest.cpp
      # Phase G — Petdex (D2–D3): grid/animation/webp/cache (JUCE-free)
        test_petdex_sprite.cpp
        # 20.7: Workspace session persistence (JUCE-free model)
        ${CMAKE_SOURCE_DIR}/ui/WorkspaceSession.cpp
        ${CMAKE_SOURCE_DIR}/ui/ChatSessionState.cpp
        test_workspace_session.cpp
        test_chat_session_state.cpp
)

target_compile_features(hathor-ui-tests PRIVATE cxx_std_20)

target_compile_options(hathor-ui-tests PRIVATE -Wall -Wextra -Werror)

target_compile_definitions(hathor-ui-tests PRIVATE
    HATHOR_ENABLE_GHOST_TELEMETRY=1
)

target_include_directories(hathor-ui-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/app
        ${CMAKE_SOURCE_DIR}/engine/include
        ${CMAKE_SOURCE_DIR}/ui
        ${CMAKE_SOURCE_DIR}/control
)

target_link_libraries(hathor-ui-tests
    PRIVATE
        hathor-engine
        Catch2::Catch2WithMain
        # Phase G / D2: WebP decoding for the sprite pipeline tests (libwebp)
        webp
)

# Register all Catch2 tests with CTest.
include(CTest)
include(Catch)
catch_discover_tests(hathor-ui-tests)
