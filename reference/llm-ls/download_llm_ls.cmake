# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# reference/llm-ls/download_llm_ls.cmake
#
# Downloads the prebuilt llm-ls v0.5.3 binary at CMake configure time.
# The binary is placed at <dest>/llm-ls and made executable.
#
# The URL is controlled by the HATHOR_LLM_LS_URL cmake variable.
# If not set, the script resolves a platform-appropriate release URL
# from the llm-ls GitHub releases page.
#
# Requirement references: AI-4

cmake_minimum_required(VERSION 3.24)

set(dest_dir "${CMAKE_CURRENT_LIST_DIR}")

if(DEFINED HATHOR_LLM_LS_URL)
    set(url "${HATHOR_LLM_LS_URL}")
else()
    # Auto-detect platform and architecture for the release tag.
    # llm-ls v0.5.3 release naming convention:
    #   linux-x86_64 / linux-aarch64
    #   macos-x86_64 / macos-aarch64
    if(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(platform_tag "macos-aarch64")
        else()
            set(platform_tag "macos-x86_64")
        endif()
    elseif(UNIX)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(platform_tag "linux-aarch64")
        else()
            set(platform_tag "linux-x86_64")
        endif()
    else()
        message(FATAL_ERROR "llm-ls binary download: unsupported platform")
    endif()

    set(url "https://github.com/teddybot/llm-ls/releases/download/v0.5.3/llm-ls-0.5.3-${platform_tag}.tar.gz")
endif()

set(binary_path "${dest_dir}/llm-ls")
set(archive_path "${dest_dir}/llm-ls.tar.gz")

# Skip download if the binary already exists (idempotent — supports re-runs)
if(EXISTS "${binary_path}")
    message(STATUS "llm-ls binary already present at ${binary_path}")
    return()
endif()

message(STATUS "Downloading llm-ls v0.5.3 from ${url}")

file(DOWNLOAD
    "${url}"
    "${archive_path}"
    STATUS download_status
    TIMEOUT 120
    SHOW_PROGRESS
)

list(GET download_status 0 status_code)
if(NOT status_code EQUAL 0)
    list(GET download_status 1 status_message)
    message(WARNING "Failed to download llm-ls binary: ${status_message}")
    message(STATUS "Ghost text will be disabled (GHOST_ENABLED must be set manually).")
    return()
endif()

# Extract the binary from the tarball (gzip-compressed).
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    # Use tar if available
    execute_process(
        COMMAND tar -xzf "${archive_path}" -C "${dest_dir}"
        RESULT_VARIABLE tar_result
        OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT tar_result EQUAL 0)
        message(WARNING "Failed to extract llm-ls archive. Ghost text will be disabled.")
        return()
    endif()

    # Verify the binary was extracted
    if(EXISTS "${binary_path}")
        message(STATUS "llm-ls v0.5.3 installed at ${binary_path}")
    else()
        message(WARNING "llm-ls binary not found after extraction. Ghost text will be disabled.")
    endif()
else()
    message(WARNING "zlib/tar not available — cannot extract llm-ls binary. Ghost text will be disabled.")
endif()

# Clean up the archive
file(REMOVE "${archive_path}")

# Make the binary executable
if(EXISTS "${binary_path}")
    file(CHMOD "${binary_path}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE
    )
endif()
