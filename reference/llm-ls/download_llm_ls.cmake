# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# reference/llm-ls/download_llm_ls.cmake
#
# Downloads the prebuilt llm-ls binary at CMake configure time.
#
# Upstream project:  https://github.com/huggingface/llm-ls  (Rust, Apache-2.0)
# Latest release:    0.5.3  (tag "0.5.3", published 2024-05-24)
#
# Upstream distributes separate per-architecture binaries as single-file
# gzip-compressed Mach-O executables (NOT tarballs):
#   llm-ls-aarch64-apple-darwin.gz   (Apple Silicon, arm64)
#   llm-ls-x86_64-apple-darwin.gz    (Intel, x86_64)
#   llm-ls-x86_64-unknown-linux-gnu.gz
#   llm-ls-aarch64-unknown-linux-gnu.gz
#
# There is no single universal macOS binary from upstream.  On macOS the
# default Hathor build is universal (arm64 + x86_64), so this script downloads
# both architecture binaries, verifies each, and combines them into one
# universal binary via `lipo -create`.
#
# The resulting binary is placed at <dest>/llm-ls and made executable.
#
# Cache variables:
#   HATHOR_LLM_LS_URL      — override: a single download URL.  When set, the
#                            binary is downloaded and decompressed from that
#                            URL only (architecture detection and lipo are
#                            skipped).  Intended for developer/test override
#                            or a custom pre-built universal binary.
#   HATHOR_LLM_LS_BINARY   — set to the resolved binary path so that
#                            ui/CMakeLists.txt can conditionally bundle it
#                            into Contents/MacOS/ of HathorUI.app.
#
# Requirement references: AI-4

cmake_minimum_required(VERSION 3.24)

set(llm_ls_version "0.5.3")
set(dest_dir       "${CMAKE_CURRENT_LIST_DIR}")
set(binary_path    "${dest_dir}/llm-ls")

set(_hf_base "https://github.com/huggingface/llm-ls/releases/download/${llm_ls_version}")

# ---------------------------------------------------------------------------
# Helper: download a single .gz artifact and decompress it to _out_path.
#
# llm-ls ships as a gzip-compressed standalone binary (not a tar archive),
# so decompression is a single `gzip -dc` step — no tar/zlib dependency.
#
# On any failure the helper aborts with FATAL_ERROR so the configure step
# fails loudly instead of silently shipping ghost text disabled.
# ---------------------------------------------------------------------------
function(_hathor_download_llm_ls _url _out_path)
    set(_archive "${_out_path}.gz")

    message(STATUS "Downloading llm-ls v${llm_ls_version}: ${_url}")

    file(DOWNLOAD
        "${_url}"
        "${_archive}"
        STATUS dl_status
        TIMEOUT 300
        SHOW_PROGRESS
    )

    list(GET dl_status 0 dl_code)
    if(NOT dl_code EQUAL 0)
        list(GET dl_status 1 dl_msg)
        message(FATAL_ERROR
            "Failed to download llm-ls from:\n"
            "  ${_url}\n"
            "CMake status: ${dl_msg}\n"
            "Verify the upstream release exists at:\n"
            "  https://github.com/huggingface/llm-ls/releases")
    endif()

    # Guard against empty / redirected downloads (e.g. a stale URL that
    # 302-redirects to a 404 error page body).
    file(SIZE "${_archive}" _dl_bytes)
    if(_dl_bytes EQUAL 0)
        message(FATAL_ERROR
            "Downloaded llm-ls archive is 0 bytes (${_url}). "
            "The upstream artifact may be unavailable or the URL is stale.")
    endif()

    # Decompress: llm-ls artifacts are gzip-compressed single Mach-O/Rust
    # binaries, NOT tarballs.
    find_program(GZIP_CMD gzip)
    if(NOT GZIP_CMD)
        message(FATAL_ERROR "gzip not found in PATH — cannot decompress llm-ls binary")
    endif()

    execute_process(
        COMMAND ${GZIP_CMD} -dc "${_archive}"
        OUTPUT_FILE "${_out_path}"
        RESULT_VARIABLE _gz_rc
        OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT _gz_rc EQUAL 0)
        message(FATAL_ERROR
            "Failed to decompress llm-ls archive ${_archive} (gzip exit ${_gz_rc}).")
    endif()

    # Verify the decompressed binary is non-empty.
    file(SIZE "${_out_path}" _out_bytes)
    if(_out_bytes EQUAL 0)
        message(FATAL_ERROR
            "Decompressed llm-ls binary is 0 bytes — extraction produced an empty file.")
    endif()

    # Clean up the intermediate archive
    file(REMOVE "${_archive}")
endfunction()

# ---------------------------------------------------------------------------
# Idempotent: reuse an existing, non-empty binary (supports re-runs)
# ---------------------------------------------------------------------------
if(EXISTS "${binary_path}")
    file(SIZE "${binary_path}" _existing_bytes)
    if(_existing_bytes EQUAL 0)
        message(WARNING "Existing llm-ls at ${binary_path} is 0 bytes — re-downloading.")
        file(REMOVE "${binary_path}")
    else()
        message(STATUS "llm-ls binary already present at ${binary_path} (${_existing_bytes} bytes)")
        set(HATHOR_LLM_LS_BINARY "${binary_path}" CACHE PATH
            "Path to the llm-ls binary (for bundling into HathorUI.app/Contents/MacOS/)")
        return()
    endif()
endif()

# ---------------------------------------------------------------------------
# Acquire the binary
# ---------------------------------------------------------------------------
if(DEFINED HATHOR_LLM_LS_URL)
    # Developer override — single binary from the supplied URL.
    # lipo combination is skipped; the caller is responsible for providing
    # the correct architecture(s).
    _hathor_download_llm_ls("${HATHOR_LLM_LS_URL}" "${binary_path}")

elseif(APPLE)
    # macOS — determine the target architecture set
    if(DEFINED CMAKE_OSX_ARCHITECTURES)
        # CMAKE_OSX_ARCHITECTURES is a CMake list (semicolon-separated)
        set(_arch_list ${CMAKE_OSX_ARCHITECTURES})
    elseif(DEFINED CMAKE_SYSTEM_PROCESSOR)
        set(_arch_list "${CMAKE_SYSTEM_PROCESSOR}")
    else()
        set(_arch_list "x86_64")
    endif()

    list(FIND _arch_list "arm64"  _arm64_idx)
    list(FIND _arch_list "x86_64" _x86_idx)

    set(_is_universal FALSE)
    if(NOT _arm64_idx EQUAL -1 AND NOT _x86_idx EQUAL -1)
        set(_is_universal TRUE)
    endif()

    if(_is_universal)
        # Download both architecture binaries and create a universal binary
        # via `lipo -create`.  This produces a single `llm-ls` binary that
        # contains both x86_64 and arm64 slices, matching the universal Hathor
        # app bundle.
        set(_arm64_path "${dest_dir}/llm-ls-arm64")
        set(_x86_path   "${dest_dir}/llm-ls-x86_64")

        _hathor_download_llm_ls(
            "${_hf_base}/llm-ls-aarch64-apple-darwin.gz"
            "${_arm64_path}")
        _hathor_download_llm_ls(
            "${_hf_base}/llm-ls-x86_64-apple-darwin.gz"
            "${_x86_path}")

        # Combine into a universal binary
        find_program(LIPO_CMD lipo)
        if(NOT LIPO_CMD)
            message(FATAL_ERROR "lipo not found in PATH — cannot create universal llm-ls binary")
        endif()

        message(STATUS "Creating universal llm-ls binary (x86_64 + arm64) via lipo")
        execute_process(
            COMMAND ${LIPO_CMD} -create -output "${binary_path}" "${_arm64_path}" "${_x86_path}"
            RESULT_VARIABLE _lipo_rc
            OUTPUT_QUIET ERROR_QUIET
        )
        if(NOT _lipo_rc EQUAL 0)
            message(FATAL_ERROR
                "lipo -create failed (exit ${_lipo_rc}). Cannot produce universal llm-ls binary.")
        endif()

        # Verify the universal binary has both architectures
        execute_process(
            COMMAND ${LIPO_CMD} -archs "${binary_path}"
            OUTPUT_VARIABLE _lipo_archs
            RESULT_VARIABLE _lipo_archs_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_lipo_archs_rc EQUAL 0)
            if(NOT _lipo_archs MATCHES "x86_64" OR NOT _lipo_archs MATCHES "arm64")
                message(FATAL_ERROR
                    "Universal llm-ls binary is missing an architecture: ${_lipo_archs}")
            endif()
            message(STATUS "llm-ls universal binary architectures: ${_lipo_archs}")
        else()
            message(WARNING "Could not verify llm-ls binary architectures via lipo -archs.")
        endif()

        # Clean up per-architecture intermediates
        file(REMOVE "${_arm64_path}" "${_x86_path}")

    else()
        # Single-architecture macOS build
        if(NOT _arm64_idx EQUAL -1)
            _hathor_download_llm_ls(
                "${_hf_base}/llm-ls-aarch64-apple-darwin.gz"
                "${binary_path}")
        else()
            _hathor_download_llm_ls(
                "${_hf_base}/llm-ls-x86_64-apple-darwin.gz"
                "${binary_path}")
        endif()
    endif()

elseif(UNIX)
    # Linux — single binary
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        _hathor_download_llm_ls(
            "${_hf_base}/llm-ls-aarch64-unknown-linux-gnu.gz"
            "${binary_path}")
    else()
        _hathor_download_llm_ls(
            "${_hf_base}/llm-ls-x86_64-unknown-linux-gnu.gz"
            "${binary_path}")
    endif()

else()
    message(FATAL_ERROR "llm-ls binary download: unsupported platform")
endif()

# ---------------------------------------------------------------------------
# Make the binary executable and expose the path to the parent scope
# ---------------------------------------------------------------------------
file(CHMOD "${binary_path}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE
)

# Expose the resolved binary path to the parent scope so that
# ui/CMakeLists.txt can conditionally bundle it into Contents/MacOS/.
set(HATHOR_LLM_LS_BINARY "${binary_path}" CACHE PATH
    "Path to the llm-ls binary (for bundling into HathorUI.app/Contents/MacOS/)")

message(STATUS "llm-ls v${llm_ls_version} installed at ${binary_path}")
