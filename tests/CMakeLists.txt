# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# hathor-engine-tests — unit tests for the pure engine, NO JUCE dependency
add_executable(hathor-engine-tests
    test_rational.cpp
    test_parammap.cpp
    test_pattern.cpp
    test_combinators.cpp
    test_miniparser.cpp
    test_prettyprinter.cpp
    test_strudel_differential.cpp
    golden/FixtureLoader.cpp
)

target_link_libraries(hathor-engine-tests
    PRIVATE
        hathor-engine
        Catch2::Catch2WithMain
        nlohmann_json::nlohmann_json
)

target_compile_features(hathor-engine-tests PRIVATE cxx_std_20)

# Add golden/ as an include directory for FixtureLoader.hpp.
target_include_directories(hathor-engine-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/golden
)

# Pass the golden-fixture directory path as a compile definition so the test
# can locate reference/strudel-golden/ regardless of working-directory.
target_compile_definitions(hathor-engine-tests PRIVATE
    GOLDEN_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/reference/strudel-golden"
)

# Ensure -Wall -Wextra -Werror applies to the golden test sources too.
if(MSVC)
    target_compile_options(hathor-engine-tests PRIVATE /W4 /WX)
else()
    target_compile_options(hathor-engine-tests PRIVATE -Wall -Wextra -Werror)
endif()

# Register tests with CTest.
# DISCOVERY_MODE PRE_TEST avoids the bracket-escaping problem that occurs
# when test names contain "[" or "]" characters (e.g. "query over [0,1)").
# With POST_BUILD (the default) Catch2 escapes those names and CTest then
# fails to match them back. PRE_TEST discovers tests at test-run time,
# which never has this problem.
include(CTest)
include(Catch)
catch_discover_tests(hathor-engine-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# Integration tests (require the full hathor binary; skipped if app is off)
# ---------------------------------------------------------------------------
add_subdirectory(integration)
