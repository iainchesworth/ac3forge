#------------------------------------------------------------------------------
# Windows MSVC Toolchain Configuration
#
# Chainloaded by the config-windows-msvc* presets via
# VCPKG_CHAINLOAD_TOOLCHAIN_FILE. The compiler is pinned here rather than left
# to PATH discovery: cl.exe is never on PATH outside a Developer PowerShell,
# so "whatever CMake finds" resolves to a different compiler entirely.
#------------------------------------------------------------------------------

message(STATUS "Configuring Windows Toolchain (MSVC Variant)")

include("${CMAKE_CURRENT_LIST_DIR}/windows.msvc.environment.cmake")

set(_MSVC_BIN_DIR "${AC3_MSVC_TOOLS_DIR}/bin/Hostx64/x64")

find_program(CMAKE_C_COMPILER
    NAMES cl.exe
    PATHS "${_MSVC_BIN_DIR}"
    NO_DEFAULT_PATH
    REQUIRED)

find_program(CMAKE_CXX_COMPILER
    NAMES cl.exe
    PATHS "${_MSVC_BIN_DIR}"
    NO_DEFAULT_PATH
    REQUIRED)

# CMake drives the MSVC link step through CMAKE_LINKER directly. Pin it to the
# toolset's own link.exe: Git for Windows ships an unrelated /usr/bin/link.exe
# that shadows it whenever Git's tools are earlier on PATH.
find_program(CMAKE_LINKER
    NAMES link.exe
    PATHS "${_MSVC_BIN_DIR}"
    NO_DEFAULT_PATH
    REQUIRED)

unset(_MSVC_BIN_DIR)

# This toolchain is only selected when MSVC is the active compiler for both
# languages, so the flags apply unconditionally rather than behind a redundant
# per-language $<CXX_COMPILER_ID:MSVC> generator expression.
#
# /utf-8       sources carry spec citations with non-ASCII glyphs (section
#              marks, degrees, arrows); tell cl both the source and execution
#              charsets are UTF-8 so it neither mis-decodes them nor warns.
# /bigobj      the codec's constant tables (bit-allocation, E-AC-3, AHT, JOC)
#              are large enough that a translation unit including several of
#              them can exceed the default section limit (C1128).
add_compile_options("/utf-8")
add_compile_options("/bigobj")

message(STATUS "Using MSVC at: ${CMAKE_CXX_COMPILER}")
