# ---------------------------------------------------------------------------
# matroska::matroska - a minimal MKV container writer.
#
# Deliberately standalone: it links nothing from ac3::forge and knows nothing
# about AC-3. It nests EBML elements and lays out clusters, taking every frame
# as opaque bytes, so it can be reasoned about, tested and reused on its own.
# The dependency runs one way only - front ends may use both, but the codec
# library never reaches for the container and vice versa.
#
# Built and installed as both a static and a shared library - see
# src/forge/CMakeLists.txt's ac3::forge for the full explanation of why an
# OBJECT library plus two thin wrapper targets, rather than one add_library().
# ---------------------------------------------------------------------------
add_library(matroska_objects OBJECT)
add_library(matroska_static STATIC)
add_library(matroska_shared SHARED)

target_link_libraries(matroska_static PUBLIC matroska_objects)
target_link_libraries(matroska_shared PUBLIC matroska_objects)

add_library(matroska::matroska_static ALIAS matroska_static)
add_library(matroska::matroska_shared ALIAS matroska_shared)
if(BUILD_SHARED_LIBS)
    add_library(matroska::matroska ALIAS matroska_shared)
else()
    add_library(matroska::matroska ALIAS matroska_static)
endif()

target_sources(matroska_objects
    PRIVATE
        src/matroska.cpp)

target_include_directories(matroska_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(matroska_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping - see src/forge/CMakeLists.txt's identical
# forge_objects treatment for why a plain PRIVATE link isn't enough for
# an OBJECT library that's part of an install(EXPORT) set.
target_link_libraries(matroska_objects PRIVATE "$<BUILD_INTERFACE:ac3::warnings>")

set_target_properties(matroska_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# MSVC's C4251 ("class X needs to have dll-interface...") fires for every
# PRIVATE STL-typed member of a MATROSKA_EXPORT class (matroska::Writer's own
# std::vector<std::byte> buffers) - pure implementation detail, never touched
# by a caller except through the class's own (exported) methods. Suppressed
# here rather than by scattering non-portable #pragma warning(disable:)
# through headers GCC/Clang also compile - see src/forge/CMakeLists.txt's
# identical forge_objects treatment, including why this is PUBLIC rather than
# PRIVATE (the warning fires wherever a translation unit parses the
# dllexport/dllimport-decorated class definition, not only inside this
# target's own compilation).
target_compile_options(matroska_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

include(GenerateExportHeader)
set_target_properties(matroska_shared PROPERTIES DEFINE_SYMBOL "MATROSKA_BUILDING_SHARED")
generate_export_header(matroska_shared
    BASE_NAME MATROSKA
    EXPORT_MACRO_NAME MATROSKA_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/matroska/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE MATROSKA_STATIC_DEFINE)
target_compile_definitions(matroska_objects PRIVATE MATROSKA_BUILDING_SHARED)
target_compile_definitions(matroska_static PUBLIC MATROSKA_STATIC_DEFINE)

set_target_properties(matroska_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(matroska_shared PROPERTIES OUTPUT_NAME "matroska")
set_target_properties(matroska_static PROPERTIES OUTPUT_NAME "matroska_static")
