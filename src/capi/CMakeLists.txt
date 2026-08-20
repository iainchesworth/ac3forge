# ---------------------------------------------------------------------------
# ac3forge_c - roadmap item F1: a stable, minimal C-callable surface over
# ac3::forge's encode/decode core (AC-3, E-AC-3, Atmos/OAMD/JOC), for bindings
# and embedding by callers that cannot or do not want to link C++23. The
# public header (include/ac3forge_c/ac3forge.h) is plain C11 - no C++ type
# ever crosses it, only opaque handles and POD structs - so this is the one
# target in the tree meant to be consumed by a non-C++ toolchain directly.
#
# Built and installed as both a static and a shared library, same OBJECT-
# library-plus-two-thin-wrappers shape as ac3::forge itself - see
# src/forge/CMakeLists.txt's header comment for the full reasoning.
#
# Unlike forge_static/forge_shared, BOTH variants here statically embed
# ac3::forge_static (PRIVATE - forge's own types never appear in this
# target's public header, so nothing about that leaks to a consumer's
# include path) rather than tracking BUILD_SHARED_LIBS. A binding or embedder
# reaching for a C ABI wants exactly one library to load (dlopen/ctypes/
# ffi.dlopen all expect a single self-contained module) - making
# ac3forge_c_shared's own linkage depend on a separately-shipped
# libac3forge.so would defeat that. forge_static's objects are already built
# PIC (src/forge/CMakeLists.txt), so embedding them in a shared library is
# safe on every platform this project targets.
# ---------------------------------------------------------------------------
add_library(capi_objects OBJECT)
add_library(capi_static STATIC)
add_library(capi_shared SHARED)

target_link_libraries(capi_static PUBLIC capi_objects)
target_link_libraries(capi_shared PUBLIC capi_objects)

add_library(ac3::forge_c_static ALIAS capi_static)
add_library(ac3::forge_c_shared ALIAS capi_shared)
if(BUILD_SHARED_LIBS)
    add_library(ac3::forge_c ALIAS capi_shared)
else()
    add_library(ac3::forge_c ALIAS capi_static)
endif()

target_sources(capi_objects
    PRIVATE
        src/atmos.cpp
        src/common.cpp
        src/decoder.cpp
        src/eac3.cpp
        src/encoder.cpp)

target_include_directories(capi_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(capi_objects PUBLIC cxx_std_23)

# ac3::forge_static PRIVATE: see this file's header comment on why both
# capi_static and capi_shared embed the static codec rather than tracking
# BUILD_SHARED_LIBS. $<BUILD_INTERFACE:...> wraps ac3::warnings for the same
# "dev-only, must not enter the export graph" reason src/forge/CMakeLists.txt's
# identical forge_objects treatment documents - not needed for
# ac3::forge_static itself, which capi_static/capi_shared's install(EXPORT)
# already resolves via ac3forgeTargets.
target_link_libraries(capi_objects
    PRIVATE
        ac3::forge_static
        "$<BUILD_INTERFACE:ac3::warnings>")

set_target_properties(capi_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(capi_shared PROPERTIES DEFINE_SYMBOL "AC3FORGEC_BUILDING_SHARED")
generate_export_header(capi_shared
    BASE_NAME AC3FORGEC
    EXPORT_MACRO_NAME AC3FORGEC_API
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3forge_c/export.h"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC3FORGEC_STATIC_DEFINE)
target_compile_definitions(capi_objects PRIVATE AC3FORGEC_BUILDING_SHARED)
target_compile_definitions(capi_static PUBLIC AC3FORGEC_STATIC_DEFINE)

# generate_export_header() emits a C++-flavoured export.h ("#ifdef __cplusplus
# extern \"C\" {") by default only for the dllexport/dllimport attribute
# syntax itself, which is fine - the macro it defines (AC3FORGEC_API) is a
# plain object-like #define either way, usable from a .c translation unit
# exactly like from a .cpp one. ac3forge.h's own extern "C" block (guarded on
# __cplusplus) is what actually matters for C linkage, not this header.

set_target_properties(capi_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(capi_shared PROPERTIES OUTPUT_NAME "ac3forge_c")
set_target_properties(capi_static PROPERTIES OUTPUT_NAME "ac3forge_c_static")
