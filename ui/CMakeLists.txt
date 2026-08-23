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
# Version — single canonical source for the HathorUI beta (Phase 6.2)
# ---------------------------------------------------------------------------
set(HATHOR_UI_VERSION       "0.3.0-beta")
set(HATHOR_UI_BUILD_VERSION "1")

set(HATHOR_UI_ICON_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/resources/HathorAppIcon.icns")

juce_add_gui_app(hathor-ui
    PRODUCT_NAME "HathorUI"
    VERSION      "${HATHOR_UI_VERSION}"
    BUILD_VERSION "${HATHOR_UI_BUILD_VERSION}"
    BUNDLE_ID    "com.hathor.HathorUI"
    COMPANY_NAME "Hathor Contributors"
    COMPANY_COPYRIGHT "Copyright (C) 2026 Hathor Contributors"
)

# ---------------------------------------------------------------------------
# Application icon (Phase 6.2)
# ---------------------------------------------------------------------------
# The pre-built .icns (1024x1024 master) is shipped directly into the bundle's
# Resources directory.  CFBundleIconFile is set in the plist below.
add_custom_command(TARGET hathor-ui POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "${HATHOR_UI_ICON_ICNS}"
        "$<TARGET_BUNDLE_DIR:hathor-ui>/Contents/Resources/HathorAppIcon.icns"
    COMMENT "Copying HathorAppIcon.icns to bundle Resources"
    VERBATIM)

# ---------------------------------------------------------------------------
# Inject LSMinimumSystemVersion + CFBundleIconFile into the generated
# Info.plist (Phase 6.2).
# ---------------------------------------------------------------------------
# JUCE 8.0.4's juceaide plist generator does not emit LSMinimumSystemVersion,
# and we bypass ICON_BIG to ship a pre-built .icns.  The deployment target is
# pinned via CMAKE_OSX_DEPLOYMENT_TARGET in the top-level CMakeLists.txt; here
# we reflect it in the bundle plist so that the binary's LC_BUILD_VERSION and
# the plist value always agree.
if(APPLE)
    add_custom_command(TARGET hathor-ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E env
            plutil -replace LSMinimumSystemVersion -string "${CMAKE_OSX_DEPLOYMENT_TARGET}"
            "$<TARGET_BUNDLE_DIR:hathor-ui>/Contents/Info.plist"
        COMMAND ${CMAKE_COMMAND} -E env
            plutil -replace CFBundleIconFile -string "HathorAppIcon.icns"
            "$<TARGET_BUNDLE_DIR:hathor-ui>/Contents/Info.plist"
        COMMENT "Patching Info.plist: LSMinimumSystemVersion + CFBundleIconFile"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Phase 6.3: Bundle backend subprocesses into Contents/MacOS/
#
# hathor-audio-worker and hathor-mcp are resolved at runtime via
# File::getSpecialLocation(currentExecutableFile).getSiblingFile(...),
# so they MUST live inside Contents/MacOS/ of the .app bundle.  CMake copies
# them automatically from the built target output (target-based, not hard-coded
# paths).  chmod +x guarantees the executable bit survives the copy.
#
# hathor-ui links the *library* variant (hathor-audio-worker-lib), not the
# executable targets, so we must declare the dependency explicitly.
# ---------------------------------------------------------------------------
add_dependencies(hathor-ui
    hathor-audio-worker
    hathor-mcp
)

if(APPLE)
    add_custom_command(TARGET hathor-ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:hathor-audio-worker>"
            "$<TARGET_BUNDLE_DIR:hathor-ui>/Contents/MacOS/hathor-audio-worker"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:hathor-mcp>"
            "$<TARGET_BUNDLE_DIR:hathor-ui>/Contents/MacOS/hathor-mcp"
        COMMAND chmod +x
            "$<TARGET_BUNDLE_DIR:hathor-ui>/Contents/MacOS/hathor-audio-worker"
            "$<TARGET_BUNDLE_DIR:hathor-ui>/Contents/MacOS/hathor-mcp"
        COMMENT "Phase 6.3: Bundling hathor-audio-worker + hathor-mcp into HathorUI.app/Contents/MacOS/"
        VERBATIM)

    # Conditionally bundle the llm-ls ghost-text binary if it was downloaded
    # during configure (reference/llm-ls/download_llm_ls.cmake sets
    # HATHOR_LLM_LS_BINARY when the download succeeds).
    if(DEFINED HATHOR_LLM_LS_BINARY AND EXISTS "${HATHOR_LLM_LS_BINARY}")
        add_custom_command(TARGET hathor-ui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "${HATHOR_LLM_LS_BINARY}"
                "$<TARGET_BUNDLE_DIR:hathor-ui>/Contents/MacOS/llm-ls"
            COMMAND chmod +x
                "$<TARGET_BUNDLE_DIR:hathor-ui>/Contents/MacOS/llm-ls"
            COMMENT "Phase 6.3: Bundling llm-ls ghost-text binary into HathorUI.app/Contents/MacOS/"
            VERBATIM)
        message(STATUS "llm-ls binary found — will bundle into HathorUI.app/Contents/MacOS/")
    else()
        message(STATUS "llm-ls binary not available — ghost text remains disabled in the bundle")
    endif()

    # ---------------------------------------------------------------------------
    # Phase 6.3: Bundle LSP server resources (strudel-lsp script + node_modules)
    # and language metadata into Contents/Resources/ so the packaged .app does
    # not depend on the development reference/ directory.
    # ---------------------------------------------------------------------------
    add_custom_command(TARGET hathor-ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/reference/strudel-lsp"
            "$<TARGET_BUNDLE_DIR:hathor-ui>/Contents/Resources/strudel-lsp"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/reference/language-metadata"
            "$<TARGET_BUNDLE_DIR:hathor-ui>/Contents/Resources/language-metadata"
        COMMENT "Phase 6.3: Bundling strudel-lsp + language-metadata into HathorUI.app/Contents/Resources/"
        VERBATIM)
endif()

target_sources(hathor-ui
    PRIVATE
        HathorApplication.cpp
        MainWindow.cpp
        WelcomeScreen.cpp          # Agent 0.1: startup welcome overlay
        HathorLookAndFeel.cpp
        ActivityRibbon.cpp
        IconLibrary.cpp           # Agent 0.6: central SVG icon system
        ExplorerPanel.cpp
        ExplorerFileTypes.cpp
        ExplorerTreeItems.cpp
        TreeBuilder.cpp
         HathorFileParser.cpp
         HathorTab.cpp         # task 3.4
          EditorArea.cpp        # task 3.4
          EditorGroup.cpp       # L-1: extracted tab management
          EditorSplitSurface.cpp  # L-1: split layout container
          EditorSplitSurface.cpp  # L-1: split layout container
          TabReorderModel.cpp   # L-1: JUCE-free tab reorder model
          RecentlyClosedTabs.cpp # L-1: JUCE-free tab close history
          ActionRegistry.cpp    # L-1: JUCE-free action/keystroke registry
          FindReplaceModel.cpp # L-1: find/replace search engine (JUCE-free)
          FindReplacePanel.cpp  # L-1: find/replace UI
         CommandPalette.cpp    # L-1: command palette UI
         BreadcrumbsBar.cpp    # L-1: breadcrumbs navigation bar
         MiniNotationTokeniser.cpp # task 3.5
         ChuckTokeniser.cpp       # A5 — ChucK syntax highlighting
         ChuckKeywords.cpp       # A5 — JUCE-free ChucK keyword/class sets
         UITimer.cpp               # task 3.7
         VisualizerPanel.cpp       # task 3.8
         SliderPanel.cpp           # task 3.9
         AcpAgentSession.cpp           # task 4.2–4.5
         PermissionPromptComponent.cpp # task 4.4
          ChatSidebar.cpp               # task 5.1 — B6 multi-thread + C2 reconnect
          ChatThread.cpp                # B6: per-thread session + connection state
          MessageHistoryView.cpp        # extracted from ChatSidebar (B6)
          ChatSessionState.cpp          # B6: JUCE-free chat thread persistence model
          SettingsComponent.cpp         # A2
          WorkspaceSession.cpp         # 20.7: JUCE-free workspace persistence model
         # Phase G — Petdex (D1): JUCE-free core + service glue
         PetdexManifestParser.cpp      # D1: robust manifest/envelope parsing
         PetdexCacheStore.cpp          # D1: manifest cache store + staleness
         PetdexAttribution.cpp         # D4: licensing/attribution gate
         PetdexManifestService.cpp     # D1: async fetch/cache service
         # Phase G — Petdex (D2–D4): sprite pipeline + D4-gated mascot
         PetdexHttp.cpp                # D2: shared HTTP GET helper (juce::URL)
         PetdexFrameGrid.cpp           # D3: verified 8×9/192×208 frame slicing
         PetdexWebpDecoder.cpp         # D2: libwebp → RGBA decoding
         PetdexAnimation.cpp           # D3: deterministic animation driver
         PetdexResourceCache.cpp       # D2: per-pet sprite + attribution cache
         PetdexResourceService.cpp     # D2: async download → cache → decode
         PetWidget.cpp                 # D2–D4: D4-gated animated mascot widget
         WindowAppearanceController.mm  # B5
          BakeTargetDialog.cpp          # B8-K6
          BakeProgressDialog.cpp        # B8-K6
          BakeOrchestrator.cpp          # B8-K6
          GotoLineDialog.cpp            # L-1 §3: Go to Line modal dialog
          PeekDefinitionDialog.cpp      # L-2: Peek Definition modal surface
         EditorContextBridge.cpp       # AI-8: editor state snapshot bridge
         LspContextBridge.cpp          # AI-8: LSP diagnostics/status bridge
         # AI-4: LSP language integration
         HathorLspClient.cpp           # LSP client process management
         LspJsonRpc.cpp                # JSON-RPC serialization
         LspCompletionLogic.cpp        # Completion merge with metadata fallback
         LspCompletionPopup.cpp        # Completion popup UI
         LspHoverHandler.cpp           # Hover tooltip UI
         LspDiagnosticsDisplay.cpp     # Diagnostics data model
          LspCompletionLogic.cpp        # Completion merge with metadata fallback
          # L-2: Navigation & workspace search
          NavigationHistory.cpp          # L-2: JUCE-free navigation history
          WorkspaceSearchModel.cpp       # L-2: JUCE-free multi-file search model
          SymbolSearchModel.cpp          # L-2: JUCE-free symbol search model
          QuickOpenDialog.cpp            # L-2: Quick-open file dialog (Cmd+P)
          WorkspaceSearchPanel.cpp       # L-2: Workspace search/replace panel
          SymbolSearchPanel.cpp          # L-2: Symbol search results panel
          # L-3: Unified Problems / Diagnostics surface
          ProblemsPanel.cpp              # L-3: Problems panel
          StatusRibbon.cpp               # L-3: bottom status ribbon
          # L-4: Tasks + simple integrated terminal
          TerminalProcess.cpp            # L-4: subprocess lifecycle (JUCE-free)
          TaskRunner.cpp                 # L-4: lightweight task runner
          TerminalPanel.cpp              # L-4: native JUCE terminal UI
          # L-5: Git source control + history + graph
          GitProcess.cpp                 # L-5: async git subprocess runner (JUCE-free)
          GitRepository.cpp              # L-5: JUCE-free Git data model
          GitGraph.cpp                   # L-5: native JUCE commit graph
          GitDiffView.cpp                # L-5: native JUCE side-by-side diff view
          SourceControlPanel.cpp         # L-5: Changes/Commit + History panel
          # L-6: Debugging + Hathor runtime inspection
          DebugSession.cpp               # L-6: native debugger (LLDB/GDB) subprocess wrapper
          RuntimeInspectorModel.cpp      # L-6: JUCE-free runtime inspection model
          RuntimeInspectorPanel.cpp      # L-6: Hathor runtime inspector UI
          DebuggerPanel.cpp              # L-6: native debugger UI
          DebugPanel.cpp                 # L-6: Debug | Runtime two-tab container
         # AI-4: Ghost text (llm-ls inline completion)
         GhostJsonRpc.cpp              # llm-ls JSON-RPC serialization
         GhostProviderConfig.cpp       # provider config (env-var credentials)
          GhostCompletionLogic.cpp      # JUCE-free lifecycle logic
          GhostTriggerPolicy.cpp        # J-1: trigger policy engine
          GhostLlmClient.cpp            # JUCE process management for llm-ls
           GhostTextOverlay.cpp          # ghost-text renderer overlay
           CompletionCoordinator.cpp     # AI-G3: LSP + ghost coordination
          # J-6: Ghost completion telemetry (JUCE-free, quality feedback)
          GhostCompletionTelemetry.cpp
         # App-layer sources (AudioEngine, SampleBank, VoicePool live in app/,
         # not in a linkable static lib — include them directly here)
          ${CMAKE_SOURCE_DIR}/app/AudioEngine.cpp
          ${CMAKE_SOURCE_DIR}/app/ChuckRenderWriter.cpp
          ${CMAKE_SOURCE_DIR}/app/SampleBank.cpp
         ${CMAKE_SOURCE_DIR}/app/VoicePool.cpp
         ${CMAKE_SOURCE_DIR}/app/LiveJamSessionManager.cpp
)

target_include_directories(hathor-ui
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/app
        ${CMAKE_SOURCE_DIR}/engine/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../control
        ${CMAKE_SOURCE_DIR}
)

target_compile_features(hathor-ui PRIVATE cxx_std_20)

target_compile_options(hathor-ui PRIVATE -Wall -Wextra -Werror)

# ---------------------------------------------------------------------------
# Embedded font resources (Req 5) — JetBrains Mono, 4 weights
# ---------------------------------------------------------------------------
juce_add_binary_data(hathor_font_data
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/JetBrainsMono-Regular.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/JetBrainsMono-Medium.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/JetBrainsMono-SemiBold.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/JetBrainsMono-Bold.ttf
)

juce_add_binary_data(hathor_icon_data
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/activity.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/audio-waveform.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/bot.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/bug.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/columns-2.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/file-code-2.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/file.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/folder-open.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/folder.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/git-branch.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/music-2.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/play.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/search.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/settings.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/square.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/terminal.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/triangle-alert.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/x.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/zap.svg
)

target_link_libraries(hathor-ui
    PRIVATE
        hathor-engine
        hathor-control
        hathor-audio-worker-lib
        hathor_font_data
        hathor_icon_data
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_basics
        juce::juce_core
        nlohmann_json::nlohmann_json
        # Phase G / D2: WebP decoding (libwebp, BSD-3) — verified required
        webp
)

target_compile_definitions(hathor-ui
    PRIVATE
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        HATHOR_ENABLE_GHOST_TELEMETRY=1
        HATHOR_UI_VERSION="${HATHOR_UI_VERSION}"
        HATHOR_UI_BUILD_VERSION="${HATHOR_UI_BUILD_VERSION}"
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
