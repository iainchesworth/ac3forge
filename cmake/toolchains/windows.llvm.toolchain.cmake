#------------------------------------------------------------------------------
# Windows LLVM/Clang Toolchain Configuration
#
# clang-cl - the MSVC-compatible clang driver - against the MSVC CRT and the
# Windows SDK. ABI-compatible with the MSVC toolchain, so the two Windows
# presets can share vcpkg dependencies and a prebuilt msvc2022_64 Qt kit.
#------------------------------------------------------------------------------

message(STATUS "Configuring Windows Toolchain (LLVM/Clang Variant)")

# clang-cl finds the MSVC headers and import libraries the same way cl does -
# through INCLUDE/LIB - and links with MSVC's link.exe, so it needs the same
# developer environment.
include("${CMAKE_CURRENT_LIST_DIR}/windows.msvc.environment.cmake")

set(_LLVM_BIN_HINTS
    "C:/Program Files/LLVM/bin"
    "$ENV{ProgramFiles}/LLVM/bin"
    "C:/Program Files (x86)/LLVM/bin"
    "$ENV{LLVM_ROOT}/bin")

find_program(CMAKE_C_COMPILER
    NAMES clang-cl.exe
    HINTS ${_LLVM_BIN_HINTS}
    REQUIRED)

find_program(CMAKE_CXX_COMPILER
    NAMES clang-cl.exe
    HINTS ${_LLVM_BIN_HINTS}
    REQUIRED)

unset(_LLVM_BIN_HINTS)

# Link with MSVC's link.exe rather than lld-link. clang-cl emits MSVC-compatible
# objects, so link.exe works unmodified, and it is the linker the prebuilt Qt
# kits and vcpkg's MSVC-built dependencies were produced against. Found under
# VCToolsInstallDir explicitly so Git for Windows' unrelated /usr/bin/link.exe
# cannot shadow it.
find_program(CMAKE_LINKER
    NAMES link.exe
    PATHS "${AC3_MSVC_TOOLS_DIR}/bin/Hostx64/x64"
    NO_DEFAULT_PATH
    REQUIRED)

# See windows.msvc.toolchain.cmake for why these two are on.
add_compile_options("/utf-8")
add_compile_options("/bigobj")

message(STATUS "Using LLVM/Clang at: ${CMAKE_CXX_COMPILER}")
