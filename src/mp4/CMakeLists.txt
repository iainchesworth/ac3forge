# ---------------------------------------------------------------------------
# mp4::mp4 - a minimal MP4 (ISOBMFF) container writer, plus fragmented
# MP4/CMAF segmenting and HLS/DASH signaling helpers.
#
# Deliberately standalone: it links nothing from ac3::forge and knows nothing
# about AC-3. It nests ISOBMFF boxes and lays out one audio track's samples,
# taking every frame - and the codec's sample-entry configuration box - as
# opaque bytes, so it can be reasoned about, tested and reused on its own.
# The dependency runs one way only - front ends may use both this and
# ac3::forge, but the codec library never reaches for the container and vice
# versa. Mirrors src/matroska/CMakeLists.txt in every particular; see that
# file's own header for the OBJECT/STATIC/SHARED shape's full rationale.
#
# mp4.cpp (mux()) and fragment.cpp (fragment()) share their ISOBMFF
# box-writing plumbing through src/isobmff_detail.hpp; hls.cpp and dash.cpp
# share their segment-duration/bandwidth arithmetic through
# src/manifest_detail.hpp - neither is a public header, both are `inline`
# functions included by more than one .cpp in this same target (see either
# file's own comment).
# ---------------------------------------------------------------------------
add_library(mp4_objects OBJECT)
add_library(mp4_static STATIC)
add_library(mp4_shared SHARED)

target_link_libraries(mp4_static PUBLIC mp4_objects)
target_link_libraries(mp4_shared PUBLIC mp4_objects)

add_library(mp4::mp4_static ALIAS mp4_static)
add_library(mp4::mp4_shared ALIAS mp4_shared)
if(BUILD_SHARED_LIBS)
    add_library(mp4::mp4 ALIAS mp4_shared)
else()
    add_library(mp4::mp4 ALIAS mp4_static)
endif()

target_sources(mp4_objects
    PRIVATE
        src/mp4.cpp
        src/fragment.cpp
        src/hls.cpp
        src/dash.cpp)

target_include_directories(mp4_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(mp4_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping - see src/forge/CMakeLists.txt's identical
# forge_objects treatment for why a plain PRIVATE link isn't enough for
# an OBJECT library that's part of an install(EXPORT) set.
target_link_libraries(mp4_objects PRIVATE "$<BUILD_INTERFACE:ac3::warnings>")

set_target_properties(mp4_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(mp4_shared PROPERTIES DEFINE_SYMBOL "MP4_BUILDING_SHARED")
generate_export_header(mp4_shared
    BASE_NAME MP4
    EXPORT_MACRO_NAME MP4_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/mp4/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE MP4_STATIC_DEFINE)
target_compile_definitions(mp4_objects PRIVATE MP4_BUILDING_SHARED)
target_compile_definitions(mp4_static PUBLIC MP4_STATIC_DEFINE)

set_target_properties(mp4_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(mp4_shared PROPERTIES OUTPUT_NAME "mp4")
set_target_properties(mp4_static PROPERTIES OUTPUT_NAME "mp4_static")
