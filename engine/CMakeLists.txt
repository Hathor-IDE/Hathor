# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# hathor-engine — pure pattern engine, NO JUCE dependency
add_library(hathor-engine STATIC
    src/Rational.cpp
    src/Combinators.cpp
    src/MiniParser.cpp
    src/PrettyPrinter.cpp
    src/PatternCompiler.cpp
)

target_include_directories(hathor-engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(hathor-engine PUBLIC cxx_std_20)

# Warnings as errors for the engine target
if(MSVC)
    target_compile_options(hathor-engine PRIVATE /W4 /WX)
else()
    target_compile_options(hathor-engine PRIVATE -Wall -Wextra -Werror)
endif()

# Engine has zero JUCE dependency — enforce this at the CMake level by
# linking nothing audio-related here.
