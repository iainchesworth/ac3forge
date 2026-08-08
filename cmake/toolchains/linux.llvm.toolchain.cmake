#------------------------------------------------------------------------------
# Linux LLVM/Clang Toolchain Configuration
#
# Chainloaded by the config-linux-llvm* presets via
# VCPKG_CHAINLOAD_TOOLCHAIN_FILE. LLVM 21 is the pinned version; the older
# names are a fallback so a developer box one release behind still configures.
#
# Clang uses the system libstdc++ here rather than libc++, which keeps this
# preset ABI-compatible with the GCC preset's vcpkg dependency tree.
#------------------------------------------------------------------------------

message(STATUS "Configuring Linux Toolchain (LLVM/Clang Variant)")

set(CMAKE_SYSTEM_NAME Linux)

# Same host/target arch resolution as linux.gcc - see the comment there for why
# VCPKG_TARGET_ARCHITECTURE alone is not enough.
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

set(_LLVM_BIN_HINTS
    "/usr/bin"
    "/usr/local/bin"
    "/opt/llvm/bin"
    "$ENV{LLVM_ROOT}/bin")

find_program(CMAKE_C_COMPILER
    NAMES clang-21 clang clang-20 clang-19
    HINTS ${_LLVM_BIN_HINTS}
    REQUIRED)

find_program(CMAKE_CXX_COMPILER
    NAMES clang++-21 clang++ clang++-20 clang++-19
    HINTS ${_LLVM_BIN_HINTS}
    REQUIRED)

find_program(CMAKE_LINKER
    NAMES ld.lld-21 ld.lld ld.lld-20 ld.lld-19
    HINTS ${_LLVM_BIN_HINTS})

unset(_LLVM_BIN_HINTS)

if(CMAKE_LINKER)
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
endif()

# x86-only, for the same reason as in linux.gcc.
if(_LINUX_ARCH STREQUAL "x86_64")
    set(CMAKE_C_FLAGS_INIT "-m64")
    set(CMAKE_CXX_FLAGS_INIT "-m64")
endif()

add_compile_options(-fcolor-diagnostics)

message(STATUS "Using LLVM/Clang at: ${CMAKE_CXX_COMPILER}")
if(CMAKE_LINKER)
    message(STATUS "Using LLD linker at: ${CMAKE_LINKER}")
endif()
