# ---------------------------------------------------------------------------
# CompilerWarnings.cmake
#
# Defines an INTERFACE target `ac3::warnings` that turns on a strict,
# cross-compiler warning set with "warnings as errors". Link it PRIVATE-ly
# into every first-party target (library, apps, tests). Third-party code (Qt,
# Catch2) is pulled in as SYSTEM headers by their package configs, so these
# flags never fire on dependency code.
# ---------------------------------------------------------------------------

add_library(ac3_warnings INTERFACE)
add_library(ac3::warnings ALIAS ac3_warnings)

set(AC3_GNU_CLANG_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
    -Wdouble-promotion
    -Wimplicit-fallthrough
    -Wformat=2)

set(AC3_MSVC_WARNINGS
    /W4
    /WX
    /permissive-)

# clang-cl reports CXX_COMPILER_ID "Clang" but binds -Wall to MSVC's /Wall
# semantics ("everything, including off-by-default groups"), which fails on
# ordinary C++23 code. /W4 is what clang-cl itself recommends instead.
set(AC3_CLANG_CL_WARNINGS
    /W4
    -Wextra
    -Wpedantic
    -Werror
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
    -Wdouble-promotion
    -Wimplicit-fallthrough
    -Wformat=2)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(ac3_warnings INTERFACE ${AC3_CLANG_CL_WARNINGS})
else()
    target_compile_options(ac3_warnings INTERFACE
        "$<$<CXX_COMPILER_ID:MSVC>:${AC3_MSVC_WARNINGS}>"
        "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:${AC3_GNU_CLANG_WARNINGS}>")
endif()

# ---------------------------------------------------------------------------
# AC3_WARNINGS_OFF_FLAG - switches every warning off for one source file.
#
# The note above is true for third-party *headers*, but not for third-party
# code generators. Qt's moc, rcc, qmltyperegistrar and qmlcachegen emit C++
# into the build tree and add it to our own target, where it inherits
# ac3::warnings - so a warning in a file nobody here wrote becomes a build
# failure under -Werror. It is not ours to fix, so it is not ours to warn
# about: see how src/gui/CMakeLists.txt applies this to the generated sources.
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(AC3_WARNINGS_OFF_FLAG "/w")
else()
    # clang-cl accepts the GNU spelling too, so this covers every non-cl case.
    set(AC3_WARNINGS_OFF_FLAG "-w")
endif()
