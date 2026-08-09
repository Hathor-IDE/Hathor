cmake_minimum_required(VERSION 3.24)
project(b4_k0_5_spike LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Fetch ChucK source (ccrma/chuck) for the libchuck embedding API
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    chuck
    GIT_REPOSITORY https://github.com/ccrma/chuck.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(chuck)

# ---------------------------------------------------------------------------
# Build libchuck as a static library (vanilla mode: no platform audio I/O)
#
# The vanilla build compiles the ChucK core source tree with no platform
# audio drivers. This matches `make vanilla` in the upstream ChucK build.
#
# We enable threads (do NOT define __DISABLE_THREADS__) so that CBufferSimple
# retains its XMutex — this is critical for our test, which exercises
# cross-thread compile/run concurrency.
# ---------------------------------------------------------------------------

set(CHUCK_CORE_DIR ${chuck_SOURCE_DIR}/src/core)

# Generate parser with bison and lexer with flex.
#
# The ChucK source tree ships pre-generated chuck_yacc.c/.h (bison output with
# a custom prefix), but chuck.lex includes "chuck.tab.h" (the standard bison
# output header name). We run bison ourselves from chuck.y to produce the
# chuck.tab.c/.h pair that the lexer expects. Similarly, flex produces
# chuck.yy.c from chuck.lex.
find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

add_custom_command(
    OUTPUT ${CHUCK_CORE_DIR}/chuck.tab.c ${CHUCK_CORE_DIR}/chuck.tab.h
    COMMAND ${BISON_EXECUTABLE} -d -o ${CHUCK_CORE_DIR}/chuck.tab.c ${CHUCK_CORE_DIR}/chuck.y
    DEPENDS ${CHUCK_CORE_DIR}/chuck.y
    COMMENT "Generating Chuck parser with bison"
)

add_custom_command(
    OUTPUT ${CHUCK_CORE_DIR}/chuck.yy.c
    COMMAND ${FLEX_EXECUTABLE} --nounistd -o ${CHUCK_CORE_DIR}/chuck.yy.c ${CHUCK_CORE_DIR}/chuck.lex
    DEPENDS ${CHUCK_CORE_DIR}/chuck.lex ${CHUCK_CORE_DIR}/chuck.tab.h
    COMMENT "Generating Chuck lexer with flex"
)

# C++ source files for the ChucK core library (matches the vanilla makefile's
# CXXOBJS_CORE list exactly)
set(CHUCK_CXX_SRCS
    ${CHUCK_CORE_DIR}/chuck_absyn.cpp
    ${CHUCK_CORE_DIR}/chuck_parse.cpp
    ${CHUCK_CORE_DIR}/chuck_errmsg.cpp
    ${CHUCK_CORE_DIR}/chuck_frame.cpp
    ${CHUCK_CORE_DIR}/chuck_symbol.cpp
    ${CHUCK_CORE_DIR}/chuck_table.cpp
    ${CHUCK_CORE_DIR}/chuck_vm.cpp
    ${CHUCK_CORE_DIR}/chuck_instr.cpp
    ${CHUCK_CORE_DIR}/chuck_scan.cpp
    ${CHUCK_CORE_DIR}/chuck_type.cpp
    ${CHUCK_CORE_DIR}/chuck_emit.cpp
    ${CHUCK_CORE_DIR}/chuck_compile.cpp
    ${CHUCK_CORE_DIR}/chuck_dl.cpp
    ${CHUCK_CORE_DIR}/chuck_oo.cpp
    ${CHUCK_CORE_DIR}/chuck_lang.cpp
    ${CHUCK_CORE_DIR}/chuck_ugen.cpp
    ${CHUCK_CORE_DIR}/chuck_otf.cpp
    ${CHUCK_CORE_DIR}/chuck_stats.cpp
    ${CHUCK_CORE_DIR}/chuck_shell.cpp
    ${CHUCK_CORE_DIR}/chuck_io.cpp
    ${CHUCK_CORE_DIR}/chuck.cpp
    ${CHUCK_CORE_DIR}/chuck_carrier.cpp
    ${CHUCK_CORE_DIR}/chuck_globals.cpp
    ${CHUCK_CORE_DIR}/hidio_sdl.cpp
    ${CHUCK_CORE_DIR}/midiio_rtmidi.cpp
    ${CHUCK_CORE_DIR}/rtmidi.cpp
    ${CHUCK_CORE_DIR}/ugen_osc.cpp
    ${CHUCK_CORE_DIR}/ugen_filter.cpp
    ${CHUCK_CORE_DIR}/ugen_stk.cpp
    ${CHUCK_CORE_DIR}/ugen_xxx.cpp
    ${CHUCK_CORE_DIR}/ulib_ai.cpp
    ${CHUCK_CORE_DIR}/ulib_doc.cpp
    ${CHUCK_CORE_DIR}/ulib_machine.cpp
    ${CHUCK_CORE_DIR}/ulib_math.cpp
    ${CHUCK_CORE_DIR}/ulib_std.cpp
    ${CHUCK_CORE_DIR}/ulib_opsc.cpp
    ${CHUCK_CORE_DIR}/util_buffers.cpp
    ${CHUCK_CORE_DIR}/util_console.cpp
    ${CHUCK_CORE_DIR}/util_math.cpp
    ${CHUCK_CORE_DIR}/util_platforms.cpp
    ${CHUCK_CORE_DIR}/util_string.cpp
    ${CHUCK_CORE_DIR}/util_thread.cpp
    ${CHUCK_CORE_DIR}/util_opsc.cpp
    ${CHUCK_CORE_DIR}/util_serial.cpp
    ${CHUCK_CORE_DIR}/util_hid.cpp
    ${CHUCK_CORE_DIR}/uana_xform.cpp
    ${CHUCK_CORE_DIR}/uana_extract.cpp
)

# C source files for the ChucK core library (bison-generated parser +
# flex-generated lexer + utility C sources)
set(CHUCK_C_SRCS
    ${CHUCK_CORE_DIR}/chuck.tab.c
    ${CHUCK_CORE_DIR}/chuck.yy.c
    ${CHUCK_CORE_DIR}/util_network.c
    ${CHUCK_CORE_DIR}/util_raw.c
    ${CHUCK_CORE_DIR}/util_xforms.c
)

# liblo sources (OSC library, used internally by ChucK)
file(GLOB CHUCK_LO_SRCS ${CHUCK_CORE_DIR}/lo/*.c)

add_library(libchuck STATIC
    ${CHUCK_CXX_SRCS}
    ${CHUCK_C_SRCS}
    ${CHUCK_LO_SRCS}
)

target_include_directories(libchuck PUBLIC
    ${CHUCK_CORE_DIR}
    ${CHUCK_CORE_DIR}/lo
)

# C++ source files need C++14 at minimum (ChucK core requirement)
target_compile_features(libchuck PUBLIC cxx_std_14)

# The lo library needs HAVE_CONFIG_H
target_compile_definitions(libchuck PUBLIC HAVE_CONFIG_H)

# macOS frameworks needed by some ChucK utility code
if(APPLE)
    target_link_libraries(libchuck PUBLIC
        "-framework Foundation"
        "-framework CoreFoundation"
        "-framework CoreMIDI"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework IOKit"
        "-framework ApplicationServices"
    )

    # Stub out private framework symbols (MultitouchSupport) that are only
    # referenced by HID code paths we never exercise in the spike.
    # These are weak symbols so they only resolve if actually used.
    target_link_options(libchuck PUBLIC "-Wl,-undefined,dynamic_lookup")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE IMPORTED_TARGET sndfile)
if(SNDFILE_FOUND)
    target_link_libraries(libchuck PUBLIC PkgConfig::SNDFILE)
endif()

# ---------------------------------------------------------------------------
# Build the spike test executable
# ---------------------------------------------------------------------------
add_executable(b4_k0_5_concurrency_spike spike_test.cpp)
target_link_libraries(b4_k0_5_concurrency_spike PRIVATE libchuck)

# Warnings
if(MSVC)
    target_compile_options(b4_k0_5_concurrency_spike PRIVATE /W4 /WX)
else()
    target_compile_options(b4_k0_5_concurrency_spike PRIVATE -Wall -Wextra)
endif()
