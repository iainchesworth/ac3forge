# ---------------------------------------------------------------------------
# ac3adm::ac3adm - a standalone BW64/RF64 + Audio Definition Model (ADM)
# parser, built on top of two vendored third-party libraries: libbw64 (the
# BW64/RF64 container layer) and libadm (the ADM XML object model), both
# from github.com/ebu, Apache-2.0, maintained by the same BBC/IRT team that
# wrote the underlying ITU-R Recommendations (BS.2088-1, BS.2076-2)
# themselves.
#
# This is the one library in ac3forge that is NOT zero-third-party-dependency
# (docs/library/index.md's stated principle for everywhere else): libadm
# needs several Boost header-only libraries (Optional, Variant, Range,
# Iterator, Functional, Format, Integer, Rational - see vcpkg.json's "adm"
# feature comment for how this exact list was confirmed, rather than guessed,
# against libadm's own source). That dependency is DELIBERATELY opt-in, not
# default-on the way matroska/mp4/mpegts are - AC3FORGE_BUILD_ADM defaults
# OFF (see root CMakeLists.txt's own option()), and even turning it ON
# additionally needs -DVCPKG_MANIFEST_FEATURES=adm (see vcpkg.json's "adm"
# feature) to resolve the Boost packages - the guard immediately below fails
# clearly if that feature was not selected, rather than surfacing a bare
# "Boost not found" from deep inside libadm's own CMakeLists.txt.
#
# libbw64 is header-only and has no dependency of its own - it is vendored
# purely so this module does not have to hand-roll BW64/RF64 chunk-walking
# (an earlier attempt at exactly that inside this same module is what
# prompted using the EBU's own reference implementations instead).
#
# Neither library's own types appear in this module's public headers
# (ac3adm/ac3adm.hpp, ac3adm/model.hpp) - see model.hpp's own header comment
# for why (libadm's own C++ namespace is `adm`, which is also why this
# module's namespace is `ac3adm` and not `adm`).
#
# NOT wired into the Android/Shield CMake tree - see platform/android's own
# notes on why matroska/mp4/mpegts stay out of it (std::format on NDK r26),
# and this module adds an even less NDK-friendly dependency (Boost) on top
# of a capability (ADM ingest) the Android app has no use for at all.
# ---------------------------------------------------------------------------

# Only checked when this module is actually being configured - AC3FORGE_BUILD_ADM=OFF
# (the default) never touches find_package(Boost) at all.
find_package(Boost 1.57 QUIET)
if(NOT Boost_FOUND)
    message(FATAL_ERROR
        "AC3FORGE_BUILD_ADM=ON needs libadm's Boost header dependencies (Optional, "
        "Variant, Range, Iterator, Functional, Format, Integer, Rational), which were not "
        "found. If using vcpkg, reconfigure with -DVCPKG_MANIFEST_FEATURES=adm (see "
        "vcpkg.json's \"adm\" feature) so those packages resolve, or build with "
        "-DAC3FORGE_BUILD_ADM=OFF.")
endif()

include(FetchContent)

# libbw64 is header-only, and is deliberately NOT add_subdirectory()'d via
# FetchContent_MakeAvailable the way libadm is just below - its own src/CMakeLists.txt (as of
# 0.10.0) has a real bug that breaks that: its configure_package_config_file() call resolves
# "config/libbw64Config.cmake.in" against CMAKE_SOURCE_DIR (this whole build's top-level source
# dir - i.e. ac3forge's own repo root, once libbw64 is pulled in as a subdirectory of it) instead
# of PROJECT_SOURCE_DIR/CMAKE_CURRENT_SOURCE_DIR (libbw64's own tree), so it fails outright with
# a missing-file configure error ("File .../ac3forge/config/libbw64Config.cmake.in does not
# exist") the moment it is add_subdirectory()'d into a larger project rather than configured
# stand-alone - confirmed by actually trying it, not assumed. (Its EXAMPLES/UNIT_TESTS options
# also turned out to have no "BW64_" prefix, unlike what their own generic naming style might
# suggest - also confirmed by reading its CMakeLists.txt directly rather than guessing.)
#
# The workaround: populate the source only (FetchContent_Populate, not MakeAvailable - this
# fetches the repository without running its CMakeLists.txt at all) and define this module's own
# INTERFACE target over its include/ directory directly, which is all an INTERFACE library
# amounts to anyway (see its own src/CMakeLists.txt's target_include_directories call). The
# generated bw64/version.hpp its top-level CMakeLists.txt would otherwise produce is skipped
# entirely - grepping every public header under include/bw64/ confirms none of them include it;
# it exists only for libbw64's own examples/tests, neither of which this module builds.
#
# The fetch details are passed straight to FetchContent_Populate() below rather than through a
# separate FetchContent_Declare() + bare FetchContent_Populate(libbw64) pair: that two-call form
# (population "from previously declared details") is exactly what CMake 3.30+'s CMP0169 policy
# deprecates - and this CMake (4.4) defaults that policy to NEW, making it a hard configure error
# rather than a warning, confirmed by actually hitting it. Passing the GIT_REPOSITORY/GIT_TAG
# arguments directly here, the same way FetchContent_MakeAvailable does internally, is the
# CMP0169-unaffected form (see FetchContent.cmake's own FetchContent_Populate: it only warns/
# errors when called with nothing but a name).
FetchContent_Populate(libbw64
    QUIET
    GIT_REPOSITORY https://github.com/ebu/libbw64.git
    GIT_TAG "0.10.0"
    GIT_SHALLOW TRUE)

add_library(bw64 INTERFACE)
# SYSTEM (via the include-directories SYSTEM keyword, FetchContent's own SYSTEM option is not
# available on the Populate-only path used above) keeps libbw64's headers out of reach of
# ac3::warnings' -Werror, matching every other vendored dependency in this build.
target_include_directories(bw64 SYSTEM INTERFACE "${libbw64_SOURCE_DIR}/include")

# libadm has no such bug - its own CMakeLists.txt genuinely honors ADM_UNIT_TESTS/ADM_EXAMPLES/
# ADM_PACKAGE_AND_INSTALL (confirmed the same way, by reading it rather than guessing), so it is
# add_subdirectory()'d normally via FetchContent_MakeAvailable below. Also no vcpkg port of its
# own (confirmed via `vcpkg search`) - its Boost dependency is resolved separately, above, via
# vcpkg's "adm" manifest feature.
set(ADM_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(ADM_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ADM_PACKAGE_AND_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libadm
    GIT_REPOSITORY https://github.com/ebu/libadm.git
    GIT_TAG "0.14.0"
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)

# Populates the target `adm` (STATIC or SHARED, following BUILD_SHARED_LIBS like every other
# target in this build) - see its own src/CMakeLists.txt for why that is the exact name, not
# `libadm`.
FetchContent_MakeAvailable(libadm)

# ---------------------------------------------------------------------------
# ac3adm's own targets - built and installed as both a static and a shared
# library, same OBJECT-library-plus-two-thin-wrappers shape as every other
# component here; see src/lib/CMakeLists.txt's ac3::forge for the full
# explanation of why.
# ---------------------------------------------------------------------------
add_library(ac3adm_objects OBJECT)
add_library(ac3adm_static STATIC)
add_library(ac3adm_shared SHARED)

target_link_libraries(ac3adm_static PUBLIC ac3adm_objects)
target_link_libraries(ac3adm_shared PUBLIC ac3adm_objects)

add_library(ac3adm::ac3adm_static ALIAS ac3adm_static)
add_library(ac3adm::ac3adm_shared ALIAS ac3adm_shared)
if(BUILD_SHARED_LIBS)
    add_library(ac3adm::ac3adm ALIAS ac3adm_shared)
else()
    add_library(ac3adm::ac3adm ALIAS ac3adm_static)
endif()

target_sources(ac3adm_objects
    PRIVATE
        src/adm_model.cpp
        src/adm.cpp)

target_include_directories(ac3adm_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(ac3adm_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping ac3::warnings - see src/lib/CMakeLists.txt's identical
# forge_objects treatment for why a plain PRIVATE link isn't enough for an OBJECT
# library that's part of an install(EXPORT) set. bw64/adm are PRIVATE too: neither
# library's types cross ac3adm's own public API (see this file's header comment), so
# nothing downstream needs to link them transitively.
# -Wnull-dereference false positive, GNU only: adm.cpp's read_pcm() calls into libbw64's inlined
# chunk accessors (bw64::DataSize64Chunk::size(), etc. - plain member-variable getters, nothing
# genuinely null-unsafe), which GCC 15.2.0 at -O3 (Release) traces through several inlining levels
# into libstdc++'s std::map/set internals (bits/stl_tree.h) and misattributes an "impossible" path
# its own optimizer introduced as a real null dereference - confirmed by actually building this
# target at -O3 (config-linux-gcc, not just the -debug preset development normally used, which
# does not optimize enough to trigger it). cmake/CompilerWarnings.cmake already documents this
# exact false-positive category for GCC < 15 against libstdc++ internals reached from this
# project's own code (see its own comment, and gcc.gnu.org/bugzilla PR 108757 and neighbours) but
# stops the guard there because the CI-pinned GCC 15 did not exhibit it - true until this module's
# own interaction with libbw64 started reaching the same internals on that exact compiler. Scoped
# to this target rather than widening that project-wide guard: nothing else in this project has
# hit it on GCC 15, so silencing it everywhere would risk hiding a genuine one elsewhere.
#
# A separate INTERFACE library linked AFTER ac3::warnings, not a plain target_compile_options()
# call on ac3adm_objects itself - confirmed by inspecting the generated compile_commands.json that
# a target's own COMPILE_OPTIONS always land BEFORE the INTERFACE_COMPILE_OPTIONS propagated from
# its linked libraries on the actual command line, regardless of which order the CMakeLists.txt
# calls target_compile_options()/target_link_libraries() in - so a plain target_compile_options()
# call here would be overridden right back by ac3::warnings' own -Wnull-dereference. Putting the
# override in its own link dependency, listed after ac3::warnings, makes it win the last-flag-wins
# rule instead.
add_library(ac3adm_suppress_gcc15_null_deref INTERFACE)
target_compile_options(ac3adm_suppress_gcc15_null_deref INTERFACE
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-null-dereference>")

target_link_libraries(ac3adm_objects
    PRIVATE
        "$<BUILD_INTERFACE:ac3::warnings>"
        bw64
        adm
        ac3adm_suppress_gcc15_null_deref)

set_target_properties(ac3adm_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(ac3adm_shared PROPERTIES DEFINE_SYMBOL "AC3ADM_BUILDING_SHARED")
generate_export_header(ac3adm_shared
    BASE_NAME AC3ADM
    EXPORT_MACRO_NAME AC3ADM_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3adm/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC3ADM_STATIC_DEFINE)
target_compile_definitions(ac3adm_objects PRIVATE AC3ADM_BUILDING_SHARED)
target_compile_definitions(ac3adm_static PUBLIC AC3ADM_STATIC_DEFINE)

set_target_properties(ac3adm_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(ac3adm_shared PROPERTIES OUTPUT_NAME "ac3adm")
set_target_properties(ac3adm_static PROPERTIES OUTPUT_NAME "ac3adm_static")
