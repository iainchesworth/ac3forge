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
# the rationale immediately below for generated Qt sources.
#
# CORRECTION: the "older Clang silently accepts an unrecognized -Wno-* flag"
# assumption this comment used to make is wrong, found compiling for Android
# with NDK r26's bundled Clang 17.0.2 (see docs/platforms/android.md) - that
# Clang errors with "-Werror,-Wunknown-warning-option" on -Wno-c2y-extensions
# because -Wc2y-extensions itself did not exist yet (C2y diagnostics landed
# upstream well after 17), so THIS project's own -Werror turns the unknown
# flag into a hard failure rather than a silent no-op. So this DOES need a
# version guard after all - real config-time if(), not a generator
# expression, since the compiler version is fixed at configure time and
# does not vary per-config the way COMPILER_ID conceivably could.
if(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 20)
    target_compile_options(ac3_warnings INTERFACE -Wno-c2y-extensions)
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
#
# Not "/w" on real MSVC, deliberately. cl has very little to say about this
# generated code under /W4 to begin with (unlike clang-cl and GCC, which
# reject more of it - see src/gui/CMakeLists.txt for the specific warning),
# so adding /w on top of the target's own /W4 would achieve nothing except a
# "D9025: overriding '/W4' with '/w'" on every generated file - a warning
# about the build, appearing on every build, to suppress warnings that were
# never going to fire.
#
# The one thing it DOES have to say: qmlcachegen falls back to interpreted
# (QJSValue-based) execution for a `var`-typed property whose function it
# cannot fully compile to native C++ - GuidedWizard.qml's data-driven step
# list is exactly that shape (a JS array of objects, filtered and searched
# by closures, not a fixed set of typed properties). The dispatch code Qt's
# own <QtQml/qjsprimitivevalue.h> generates for that path hits C4702
# (unreachable code) under cl's optimizer in Qt 6.8, on this MSVC toolset -
# real code Qt shipped, not a defect introduced here, so /wd4702 is scoped
# to only that one diagnostic rather than silencing the rest of /W4 the way
# non-MSVC's "-w" does. If a later Qt/cl combination stops emitting it, the
# flag becomes a harmless no-op rather than something that needs removing.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(AC3_WARNINGS_OFF_FLAG "/wd4702")
else()
    # clang-cl accepts the GNU spelling too, so this covers every non-cl case.
    set(AC3_WARNINGS_OFF_FLAG "-w")
endif()
