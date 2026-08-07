# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tests-ui/CMakeLists.txt
#
# Currently defines the acp_spike target only (Task 0.1).
# The full hathor-ui-tests target (linking hathor-engine + Catch2) is defined
# in Task 1.5.
#
# Build the spike:
#   mkdir -p build && cd build && cmake .. && cmake --build . --target acp_spike
#
# Run the spike:
#   ./build/tests-ui/acp_spike

# ---------------------------------------------------------------------------
# acp_spike — Task 0.1
#
# Validates:
#   (a) Bidirectional stdio with a subprocess via posix_spawn + pipes
#   (b) JSON-RPC 2.0 ACP message serialisation / parsing with nlohmann/json
#   (c) Unix domain socket listener/client round-trip
#
# Deliberately does NOT link JUCE — validates the hathor-mcp no-JUCE constraint
# (Requirement 31).
# ---------------------------------------------------------------------------

add_executable(acp_spike
    test_acp_spike.cpp
)

target_compile_features(acp_spike PRIVATE cxx_std_20)

target_compile_options(acp_spike PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(acp_spike PRIVATE
    nlohmann_json::nlohmann_json
)

# Register with CTest so `ctest` can run it
add_test(NAME acp_spike COMMAND acp_spike)
