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

# -Wpedantic on a sufficiently new Clang (first seen on macOS via Homebrew's
# unpinned `llvm` formula, which tracks upstream head rather than the 21.1.8
# every other leg pins - see _build.yml's "Install LLVM (macOS)" step) flags
# Catch2's TEST_CASE/INFO macros for using __COUNTER__, which -Wc2y-extensions
# treats as a C2y-only construct even in C++. The "third-party headers are
# SYSTEM-included so warnings never fire on them" rule above does not save
# this: Clang attributes a macro's pedantic diagnostics to the *expansion
# site* (our test .cpp) rather than the system header the macro is defined
# in, so every TEST_CASE call across the whole suite trips it. Not ours to
# fix - it is Catch2's own macro body - so not ours to warn about, matching
# the rationale immediately below for generated Qt sources. Both GCC and
# older Clang silently accept an unrecognized -Wno-* flag (only positive -W
# flags warn as "unknown-warning-option"), so this needs no version guard.
target_compile_options(ac3_warnings INTERFACE
    "$<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wno-c2y-extensions>")

# ---------------------------------------------------------------------------
# AC3_WARNINGS_OFF_FLAG - switches every warning off for one source file.
#
# The note above is true for third-party *headers*, but not for third-party
# code generators. Qt's moc, rcc, qmltyperegistrar and qmlcachegen emit C++
# into the build tree and add it to our own target, where it inherits
# ac3::warnings - so a warning in a file nobody here wrote becomes a build
# failure under -Werror. It is not ours to fix, so it is not ours to warn
# about: see how src/gui/CMakeLists.txt applies this to the generated sources.
#
# Empty on real MSVC, deliberately - not "/w". cl has nothing to say about
# this generated code under /W4 to begin with (unlike clang-cl and GCC, which
# do reject some of it - see src/gui/CMakeLists.txt for the specific warning),
# so adding /w on top of the target's own /W4 achieves nothing except a
# "D9025: overriding '/W4' with '/w'" on every generated file - a warning
# about the build, appearing on every build, to suppress warnings that were
# never going to fire. A caller that appends an empty COMPILE_OPTIONS entry
# gets a harmless no-op, which is exactly what real MSVC should get here.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(AC3_WARNINGS_OFF_FLAG "")
else()
    # clang-cl accepts the GNU spelling too, so this covers every non-cl case.
    set(AC3_WARNINGS_OFF_FLAG "-w")
endif()
