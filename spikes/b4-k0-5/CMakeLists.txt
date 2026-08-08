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
# Build libchuck as a static library (vanilla mode: no audio I/O)
#
# The vanilla build compiles the ChucK core source tree with no platform
# audio drivers. We enable threads (do NOT define __DISABLE_THREADS__) so
# that CBufferSimple retains its XMutex — this is critical for our test,
# which exercises cross-thread compile/run concurrency.
#
# We DO disable: MIDI, network, OTF, HID, serial, keyboard, prompter, shell,
# and STK audio (no RtAudio). This matches the vanilla build configuration.
# ---------------------------------------------------------------------------

set(CHUCK_CORE_DIR ${chuck_SOURCE_DIR}/src/core)

set(CHUCK_DEFINES
    __DISABLE_MIDI__
    __DISABLE_NETWORK__
    __DISABLE_OTF_SERVER__
    __DISABLE_HID__
    __DISABLE_SERIAL__
    __DISABLE_KBHIT__
    __DISABLE_PROMPTER__
    __DISABLE_SHELL__
)

# C++ source files for the ChucK core library
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

# C source files for the ChucK core library (pre-generated parser + lex
# output from chuck_yacc.c / chuck_yacc.h are checked into the repo)
set(CHUCK_C_SRCS
    ${CHUCK_CORE_DIR}/chuck_yacc.c
    ${CHUCK_CORE_DIR}/chuck.yy.c
    ${CHUCK_CORE_DIR}/util_network.c
    ${CHUCK_CORE_DIR}/util_raw.c
    ${CHUCK_CORE_DIR}/util_xforms.c
)

# liblo sources (OSC library, used internally by ChucK)
file(GLOB CHUCK_LO_SRCS ${CHUCK_CORE_DIR}/lo/*.c)

# libsndfile is not needed in vanilla mode (no audio file I/O in core)
# but util_sndfile.c may be referenced; check and add if present
if(EXISTS ${CHUCK_CORE_DIR}/util_sndfile.c)
    list(APPEND CHUCK_C_SRCS ${CHUCK_CORE_DIR}/util_sndfile.c)
endif()

add_library(libchuck STATIC
    ${CHUCK_CXX_SRCS}
    ${CHUCK_C_SRCS}
    ${CHUCK_LO_SRCS}
)

target_include_directories(libchuck PUBLIC
    ${CHUCK_CORE_DIR}
    ${CHUCK_CORE_DIR}/lo
)

# Apply defines for vanilla build
foreach(DEF ${CHUCK_DEFINES})
    target_compile_definitions(libchuck PUBLIC ${DEF})
endforeach()

# C++ source files need C++14 at minimum
target_compile_features(libchuck PUBLIC cxx_std_14)

# The lo library needs HAVE_CONFIG_H
target_compile_definitions(libchuck PUBLIC HAVE_CONFIG_H)

# macOS frameworks needed by some ChucK utility code
if(APPLE)
    target_link_libraries(libchuck PUBLIC "-framework Foundation")
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
