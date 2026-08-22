#------------------------------------------------------------------------------
# Linux GCC Toolchain Configuration
#
# Chainloaded by the config-linux-gcc* presets via
# VCPKG_CHAINLOAD_TOOLCHAIN_FILE. GCC 16 is the pinned version; the older names
# are a fallback so a developer box one release behind still configures.
#------------------------------------------------------------------------------

message(STATUS "Configuring Linux Toolchain (GCC Variant)")

set(CMAKE_SYSTEM_NAME Linux)

# Resolve the target architecture. vcpkg sets VCPKG_TARGET_ARCHITECTURE only
# while it is building a port for a triplet; in the project's own configure
# scope it is empty, so fall back to the host. Without the fallback a native
# aarch64 build would assume x86_64 and emit -m64, which an aarch64 gcc rejects.
if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    set(_LINUX_ARCH "aarch64")
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    set(_LINUX_ARCH "x86_64")
else()
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE _HOST_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_HOST_ARCH MATCHES "aarch64|arm64")
        set(_LINUX_ARCH "aarch64")
    else()
        set(_LINUX_ARCH "x86_64")
    endif()
endif()

set(CMAKE_SYSTEM_PROCESSOR ${_LINUX_ARCH})
message(STATUS "Target architecture: ${CMAKE_SYSTEM_PROCESSOR}")

set(_GCC_BIN_HINTS
    "/usr/bin"
    "/usr/local/bin"
    "$ENV{GCC_ROOT}/bin")

find_program(CMAKE_C_COMPILER
    NAMES gcc-16 gcc gcc-15 gcc-14 gcc-13
    HINTS ${_GCC_BIN_HINTS}
    REQUIRED)

find_program(CMAKE_CXX_COMPILER
    NAMES g++-16 g++ g++-15 g++-14 g++-13
    HINTS ${_GCC_BIN_HINTS}
    REQUIRED)

unset(_GCC_BIN_HINTS)

# -m64 selects the 64-bit x86 ABI and is rejected by an aarch64 gcc, so it is
# x86-only; the arm host compiler already targets aarch64 natively.
if(_LINUX_ARCH STREQUAL "x86_64")
    set(CMAKE_C_FLAGS_INIT "-m64")
    set(CMAKE_CXX_FLAGS_INIT "-m64")
endif()

add_compile_options(-fdiagnostics-color=always)

message(STATUS "Using GCC at: ${CMAKE_CXX_COMPILER}")
