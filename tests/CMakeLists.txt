# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# hathor-engine-tests — unit tests for the pure engine, NO JUCE dependency
add_executable(hathor-engine-tests
    test_rational.cpp
)

target_link_libraries(hathor-engine-tests
    PRIVATE
        hathor-engine
        Catch2::Catch2WithMain
)

target_compile_features(hathor-engine-tests PRIVATE cxx_std_20)

# Register tests with CTest
include(CTest)
include(Catch)
catch_discover_tests(hathor-engine-tests)
