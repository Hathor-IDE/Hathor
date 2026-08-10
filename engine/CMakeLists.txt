# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# hathor-engine — pure pattern engine, NO JUCE dependency
add_library(hathor-engine STATIC
    src/Rational.cpp
    src/Combinators.cpp
    src/MiniParser.cpp
    src/PrettyPrinter.cpp
    src/PatternCompiler.cpp
    src/LanguageMetadata.cpp
)

target_include_directories(hathor-engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Explicitly declare public headers so dependents (e.g. hathor-ui) can include
# them without path manipulation. FILE_SET HEADERS makes the header visible
# in IDEs and install rules. (Req 27.1, 31.2)
target_sources(hathor-engine
    PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/hathor/Arc.hpp
        include/hathor/Combinators.hpp
        include/hathor/Event.hpp
        include/hathor/LanguageMetadata.hpp
        include/hathor/MiniParser.hpp
        include/hathor/MiniTokeniser.hpp
        include/hathor/ParamMap.hpp
        include/hathor/Pattern.hpp
        include/hathor/PatternCompiler.hpp
        include/hathor/PrettyPrinter.hpp
        include/hathor/Rational.hpp
        include/hathor/Value.hpp
)

target_compile_features(hathor-engine PUBLIC cxx_std_20)

target_link_libraries(hathor-engine PUBLIC nlohmann_json::nlohmann_json)

# Warnings as errors for the engine target
if(MSVC)
    target_compile_options(hathor-engine PRIVATE /W4 /WX)
else()
    target_compile_options(hathor-engine PRIVATE -Wall -Wextra -Werror)
endif()

# Engine has zero JUCE dependency — enforce this at the CMake level by
# linking nothing audio-related here.
