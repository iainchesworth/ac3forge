# ---------------------------------------------------------------------------
# mpegts::mpegts - a minimal MPEG-2 Transport Stream muxer.
#
# Deliberately standalone: it links nothing from ac3::forge and knows nothing
# about AC-3 beyond the two stream_type/descriptor choices ac3::io::StreamKind
# maps onto. It packetizes PAT + PMT + one PES-wrapped elementary stream into
# 188-byte TS packets, taking every access unit as opaque bytes, so it can be
# reasoned about, tested and reused on its own - the same shape as
# matroska::matroska (src/matroska/), which this file mirrors structurally.
# The dependency runs one way only - front ends may use both, but the codec
# library never reaches for the container and vice versa.
#
# Built and installed as both a static and a shared library - see
# src/forge/CMakeLists.txt's ac3::forge for the full explanation of why an
# OBJECT library plus two thin wrapper targets, rather than one add_library().
# ---------------------------------------------------------------------------
add_library(mpegts_objects OBJECT)
add_library(mpegts_static STATIC)
add_library(mpegts_shared SHARED)

target_link_libraries(mpegts_static PUBLIC mpegts_objects)
target_link_libraries(mpegts_shared PUBLIC mpegts_objects)

add_library(mpegts::mpegts_static ALIAS mpegts_static)
add_library(mpegts::mpegts_shared ALIAS mpegts_shared)
if(BUILD_SHARED_LIBS)
    add_library(mpegts::mpegts ALIAS mpegts_shared)
else()
    add_library(mpegts::mpegts ALIAS mpegts_static)
endif()

target_sources(mpegts_objects
    PRIVATE
        src/mpegts.cpp)

target_include_directories(mpegts_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(mpegts_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping - see src/forge/CMakeLists.txt's identical
# forge_objects treatment for why a plain PRIVATE link isn't enough for
# an OBJECT library that's part of an install(EXPORT) set.
target_link_libraries(mpegts_objects PRIVATE "$<BUILD_INTERFACE:ac3::warnings>")

set_target_properties(mpegts_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(mpegts_shared PROPERTIES DEFINE_SYMBOL "MPEGTS_BUILDING_SHARED")
generate_export_header(mpegts_shared
    BASE_NAME MPEGTS
    EXPORT_MACRO_NAME MPEGTS_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/mpegts/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE MPEGTS_STATIC_DEFINE)
target_compile_definitions(mpegts_objects PRIVATE MPEGTS_BUILDING_SHARED)
target_compile_definitions(mpegts_static PUBLIC MPEGTS_STATIC_DEFINE)

set_target_properties(mpegts_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(mpegts_shared PROPERTIES OUTPUT_NAME "mpegts")
set_target_properties(mpegts_static PROPERTIES OUTPUT_NAME "mpegts_static")
