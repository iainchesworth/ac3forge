# ---------------------------------------------------------------------------
# ac3::forge - the whole codec: bitstream, DSP, encoder, decoder, the spatial
# object layer and the IEC 61937 sink. No GUI, no third-party codec code (see
# docs/RESEARCH.md on the clean-room rule) - and, deliberately, no
# platform-specific code at all: this target builds identically on any
# C++23 platform, with no WASAPI/ALSA dependency, so a third party linking it
# purely to encode/decode AC-3/E-AC-3/Atmos streams needs nothing else. Live
# capture, monitor playback and IEC 61937 delivery are ac3::audio
# (src/audio/), a separate target ac3cli/ac3gui link alongside this one -
# see that target's own header comment.
#
# Built and installed as BOTH a static and a shared library - a distributed
# package can't know in advance which a downstream consumer wants. One
# OBJECT library compiles the sources exactly once; forge_static and
# forge_shared are thin archive/DLL wrappers around the same object code
# (a single add_library() can't be both STATIC and SHARED at once). In-tree
# consumers (ac3cli/ac3gui/tests) link the plain ac3::forge alias, which
# resolves to whichever variant BUILD_SHARED_LIBS asks for; an installed
# package has no such ambient default, so it exports both variants under
# their own names, ac3::forge_static / ac3::forge_shared.
#
# The raw target names are forge_objects/forge_static/forge_shared, not
# ac3forge_*: install(EXPORT ... NAMESPACE ac3::) prepends the namespace to
# whatever the target is actually named, so an ac3forge_static target would
# export as the stuttering ac3::ac3forge_static, not ac3::forge_static - the
# leading "ac3" only needs to appear once, either in the target's own name or
# the namespace, never both. (matroska::matroska's own targets don't have
# this problem: matroska_static under the matroska:: namespace already lands
# on the intended matroska::matroska_static with no stutter.)
# ---------------------------------------------------------------------------
add_library(forge_objects OBJECT)
add_library(forge_static STATIC)
add_library(forge_shared SHARED)

target_link_libraries(forge_static PUBLIC forge_objects)
target_link_libraries(forge_shared PUBLIC forge_objects)

add_library(ac3::forge_static ALIAS forge_static)
add_library(ac3::forge_shared ALIAS forge_shared)
if(BUILD_SHARED_LIBS)
    add_library(ac3::forge ALIAS forge_shared)
else()
    add_library(ac3::forge ALIAS forge_static)
endif()

# --- Version + git stamp ---------------------------------------------------
find_package(Git QUIET)
set(AC3FORGE_VERSION_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/include/ac3/version.hpp.in")
set(AC3FORGE_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3/version.hpp")

# OS/architecture/compiler this binary is built for, for version_details()'s
# --version/About-dialog output. Computed here (CMake already knows all three
# from the toolchain it configured) rather than read back with predefined
# macros at compile time, so version.cpp needs no #ifdef - see ac3::audio's
# platform tree (src/audio/CMakeLists.txt) and scripts/check-platform-macros.ps1.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(AC3FORGE_BUILD_OS "macOS")
else()
    set(AC3FORGE_BUILD_OS "${CMAKE_SYSTEM_NAME}")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(AC3FORGE_BUILD_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(AC3FORGE_BUILD_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|i686|X86)$")
    set(AC3FORGE_BUILD_ARCH "x86")
else()
    set(AC3FORGE_BUILD_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(AC3FORGE_BUILD_COMPILER "GCC ${CMAKE_CXX_COMPILER_VERSION}")
else()
    # AppleClang, Clang and MSVC already spell their own CMAKE_CXX_COMPILER_ID
    # the way version_details() wants it.
    set(AC3FORGE_BUILD_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

set(AC3FORGE_BUILD_TARGET "${AC3FORGE_BUILD_OS} ${AC3FORGE_BUILD_ARCH} (${AC3FORGE_BUILD_COMPILER})")

# Generate once at configure time so the header exists for the first build.
set(AC3FORGE_VERSION "${PROJECT_VERSION}")
set(AC3FORGE_VERSION_FULL "${PROJECT_VERSION_FULL}")
set(SRC "${AC3FORGE_VERSION_TEMPLATE}")
set(DST "${AC3FORGE_VERSION_HEADER}")
set(WORKDIR "${PROJECT_SOURCE_DIR}")
include("${PROJECT_SOURCE_DIR}/cmake/GenerateVersion.cmake")

# Re-stamp on every build so the git information tracks new commits without a
# reconfigure. configure_file only rewrites when the content changes, so this
# does not needlessly recompile dependants.
add_custom_target(ac3forge_version ALL
    BYPRODUCTS "${AC3FORGE_VERSION_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
        "-DAC3FORGE_VERSION=${PROJECT_VERSION}"
        "-DAC3FORGE_VERSION_FULL=${PROJECT_VERSION_FULL}"
        "-DAC3FORGE_BUILD_TARGET=${AC3FORGE_BUILD_TARGET}"
        "-DSRC=${AC3FORGE_VERSION_TEMPLATE}"
        "-DDST=${AC3FORGE_VERSION_HEADER}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DWORKDIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/GenerateVersion.cmake"
    COMMENT "Stamping version and git information"
    VERBATIM)

# Everything here is portable: bitstream, DSP, containers, measurement. The
# OS-specific code is selected further down, out of this list.
target_sources(forge_objects
    PRIVATE
        src/analysis/levels.cpp
        src/core/bitalloc.cpp
        src/core/eac3_tables.cpp
        src/core/exponents.cpp
        src/core/mantissas.cpp
        src/core/mdct.cpp
        src/decoder/decoder.cpp
        src/decoder/eac3_decoder.cpp
        src/emdf/emdf.cpp
        src/encoder/assignment.cpp
        src/encoder/coupling.cpp
        src/encoder/eac3_frame.cpp
        src/encoder/eac3_tools.cpp
        src/encoder/encoder.cpp
        src/encoder/plan.cpp
        src/encoder/transient.cpp
        src/io/elementary.cpp
        src/mlp/sync.cpp
        src/io/wav.cpp
        src/meta/drc.cpp
        src/meta/loudness.cpp
        src/meta/mixing.cpp
        src/oba/atmos.cpp
        src/oba/joc.cpp
        src/oba/motion.cpp
        src/oba/oamd.cpp
        src/sinks/iec61937.cpp
        src/spatial/spatial.cpp
        src/version.cpp)

add_dependencies(forge_objects ac3forge_version)

target_include_directories(forge_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(forge_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping, not a plain PRIVATE link: ac3::warnings/
# ac3::coverage are dev-only interface targets that must never propagate to
# an installed consumer regardless - but OBJECT libraries specifically
# require install(EXPORT) to be able to resolve every dependency they have,
# PRIVATE included, since a .o file's compile-time requirements aren't fully
# self-contained the way a finished .lib/.dll's are. Wrapping makes the
# dependency invisible to the export graph outright, which is what "dev-only,
# never for a consumer" actually means here, not "somehow also exported."
target_link_libraries(forge_objects PRIVATE
    "$<BUILD_INTERFACE:ac3::warnings>"
    "$<BUILD_INTERFACE:ac3::coverage>")

# --- Static+shared plumbing -------------------------------------------------
# PIC is required for any object code that might end up in a shared library
# (mandatory on Linux/macOS, ignored by MSVC) - the same objects feed
# forge_static too, but a static archive being built PIC costs nothing.
# Hidden visibility by default + explicit AC3FORGE_EXPORT annotation (see
# ac3/export.hpp's callers) is the standard pattern: only the intentionally
# public API surfaces across the .so/.dll boundary, everything else - helper
# functions, implementation detail classes - stays invisible outside it.
set_target_properties(forge_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# MSVC's C4251 ("class X needs to have dll-interface...") fires for every
# PRIVATE STL-typed member of an AC3FORGE_EXPORT class - std::vector/std::map/
# std::optional fields that are pure implementation detail, never touched by
# a caller except through the class's own (exported) methods. That's exactly
# the case this warning can't tell apart from a genuinely unsafe one, so it's
# suppressed here rather than by scattering non-portable
# #pragma warning(disable:) through headers GCC/Clang also compile (see
# cmake/CompilerWarnings.cmake's own MSVC-vs-portable split for the same
# reasoning). Scoped to this target, not ac3::warnings globally: it's a
# consequence of exporting a library, not a project-wide warning policy call.
#
# PUBLIC, not PRIVATE: the warning fires wherever a translation unit parses
# the dllexport/dllimport-decorated class definition, not only inside
# forge_objects's own compilation - a consumer built against
# ac3::forge_shared (ac3cli/ac3gui/tests/examples, once BUILD_SHARED_LIBS=ON)
# includes these same headers under the dllimport branch and hits the
# identical warning in its own translation units. ac3::forge_static's
# consumers never see it (AC3FORGE_STATIC_DEFINE strips the decoration
# entirely), so this is a no-op for them, not an unwanted suppression.
target_compile_options(forge_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

# generate_export_header() normally adds the "building the DLL" compile
# definition to the target it's called on (here, forge_shared) - but
# forge_shared has no sources of its own, everything actually compiles
# via forge_objects, so that auto-added definition would never reach the
# code it's meant to affect. Setting DEFINE_SYMBOL explicitly and adding it
# to forge_objects by hand closes that gap. The object code is therefore
# always compiled as if building the DLL (dllexport, not dllimport) - this
# is harmless for the objects that end up in forge_static too: a plain
# (non-decorated) consumer of the static archive still resolves the same
# underlying symbol names, dllexport just also lets those symbols export
# through a DLL if one were ever built from them, which forge_static
# itself isn't. AC3FORGE_STATIC_DEFINE is what a static consumer needs
# instead, so the generated header emits no import/export decoration at
# all - see the target_compile_definitions on forge_static below.
#
# The generated header keeps the .hpp extension and generated/ac3/ layout
# every other generated header here already uses (ac3/version.hpp), rather
# than generate_export_header's own .h default, so nothing that already
# includes "ac3/export.hpp" (every annotated header) needs to change.
include(GenerateExportHeader)
# DEFINE_SYMBOL is a target property generate_export_header() reads (not one
# of its own function keywords) to decide which #ifdef the generated header
# guards the dllexport branch with - set explicitly since it would otherwise
# default to forge_shared_EXPORTS, and it's forge_objects that must
# actually define it (see above).
set_target_properties(forge_shared PROPERTIES DEFINE_SYMBOL "AC3FORGE_BUILDING_SHARED")
generate_export_header(forge_shared
    BASE_NAME AC3FORGE
    EXPORT_MACRO_NAME AC3FORGE_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC3FORGE_STATIC_DEFINE)
target_compile_definitions(forge_objects PRIVATE AC3FORGE_BUILDING_SHARED)
target_compile_definitions(forge_static PUBLIC AC3FORGE_STATIC_DEFINE)

# SOVERSION is the full version, not just the major component: pre-1.0, no
# ABI-compatibility promise holds across any two releases, so there is no
# meaningful "compatible" version range narrower than an exact match.
set_target_properties(forge_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

# On Windows, forge_shared's import library and forge_static's archive
# would otherwise both be named ac3forge.lib in the same output directory -
# name the static one distinctly so installing both side by side doesn't
# clobber either. forge_shared keeps the plain "ac3forge" name (-> the
# ac3forge.dll a consumer actually expects). These are the installed
# filenames a consumer sees, deliberately unrelated to the CMake target
# names above.
set_target_properties(forge_shared PROPERTIES OUTPUT_NAME "ac3forge")
set_target_properties(forge_static PROPERTIES OUTPUT_NAME "ac3forge_static")
