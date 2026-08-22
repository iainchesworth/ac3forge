# ---------------------------------------------------------------------------
# ac3::signing - the EMDF Atmos object signer.
#
# Clean-room and dependency-free (SHA-256/HMAC are implemented from FIPS 180-4/
# RFC 2104, not pulled from OpenSSL - the same "no third-party codec deps" rule
# vcpkg.json states). Kept a SEPARATE target from ac3::forge, not folded into
# it, so the codec still links nothing to encode/decode: signing is a strictly
# optional step a front end applies to finished frames, and ac3::forge has no
# dependency on it. It reuses the codec's PARSING primitives (BitReader,
# decode_exponents, compute_bit_allocation, the spx helpers) via ac3::forge but
# adds nothing to them.
#
# No embedded key: the signer takes a SigningKey the operator supplies at
# runtime (see signing_key.hpp). This is why the target is committed and built
# by default, unlike the old gitignored quarantine overlay it replaces - there
# is no secret in it to keep out of the tree.
#
# Built and installed as BOTH a static and a shared library, the same OBJECT-
# library-plus-two-thin-wrappers shape ac3::forge itself uses (see
# src/forge/CMakeLists.txt's header comment for the full reasoning) - unlike
# ac3::forge_c (src/capi/CMakeLists.txt), this is an ordinary C++ dependent
# rather than a self-contained C ABI, so signing_static links ac3::forge_static
# and signing_shared links ac3::forge_shared, exactly mirroring how
# forge_static/forge_shared each wrap forge_objects, rather than always
# embedding one specific variant regardless of BUILD_SHARED_LIBS the way capi
# does. Raw target names signing_objects/signing_static/signing_shared, not
# ac3_signing_*: same stutter-avoidance reasoning as forge_objects/forge_static/
# forge_shared's own header comment (install(EXPORT ... NAMESPACE ac3::)
# prepends the namespace to whatever the target is actually named).
# ---------------------------------------------------------------------------
add_library(signing_objects OBJECT)
add_library(signing_static STATIC)
add_library(signing_shared SHARED)

target_link_libraries(signing_static PUBLIC signing_objects ac3::forge_static)
target_link_libraries(signing_shared PUBLIC signing_objects ac3::forge_shared)

add_library(ac3::signing_static ALIAS signing_static)
add_library(ac3::signing_shared ALIAS signing_shared)
if(BUILD_SHARED_LIBS)
    add_library(ac3::signing ALIAS signing_shared)
else()
    add_library(ac3::signing ALIAS signing_static)
endif()

target_sources(signing_objects
    PRIVATE
        src/sha256.cpp
        src/hmac_sha256.cpp
        src/signing_key.cpp
        src/emdf_atmos_signer.cpp)

target_include_directories(signing_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(signing_objects PUBLIC cxx_std_23)

# ac3::forge (bare, BUILD_SHARED_LIBS-selected) for the parsing primitives, PRIVATE and compile-time
# only - signing_static/signing_shared above each PUBLICLY link their own matching forge_static/
# forge_shared variant instead, the same "propagation lives on the wrapper, not the shared object
# library" split forge_objects/capi_objects's own PRIVATE deps already use. ac3::warnings/
# ac3::coverage wrapped in $<BUILD_INTERFACE:...> for the same dev-only, never-exported reason
# forge_objects's identical treatment documents.
target_link_libraries(signing_objects
    PRIVATE
        ac3::forge
        "$<BUILD_INTERFACE:ac3::warnings>"
        "$<BUILD_INTERFACE:ac3::coverage>")

# --- Static+shared plumbing -------------------------------------------------
# Same PIC/hidden-visibility/AC3SIGNING_EXPORT shape as forge_objects's own
# identical block - see its header comment for the full reasoning.
set_target_properties(signing_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

target_compile_options(signing_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

include(GenerateExportHeader)
set_target_properties(signing_shared PROPERTIES DEFINE_SYMBOL "AC3SIGNING_BUILDING_SHARED")
generate_export_header(signing_shared
    BASE_NAME AC3SIGNING
    EXPORT_MACRO_NAME AC3SIGNING_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3/signing/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC3SIGNING_STATIC_DEFINE)
target_compile_definitions(signing_objects PRIVATE AC3SIGNING_BUILDING_SHARED)
target_compile_definitions(signing_static PUBLIC AC3SIGNING_STATIC_DEFINE)

set_target_properties(signing_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

# Same Windows same-directory-clash avoidance as forge_shared/forge_static's own OUTPUT_NAME split.
set_target_properties(signing_shared PROPERTIES OUTPUT_NAME "ac3signing")
set_target_properties(signing_static PROPERTIES OUTPUT_NAME "ac3signing_static")

# MSVC flags plain getenv() (signing_key.cpp's env-var fallbacks) as C4996
# "unsafe". The call is POSIX and portable - GCC/Clang raise nothing for it
# under -Wall -Wextra - and the suggested _dupenv_s is a Microsoft CRT
# extension with no NDK/POSIX equivalent, so "fix the code" is not an option
# that stays cross-platform. Scoped to this one compiler's opinion of a
# standard C function, the same way src/forge handles its own MSVC-only
# suppressions (see cmake/CompilerWarnings.cmake).
if(MSVC)
    target_compile_options(signing_objects PRIVATE /wd4996)
endif()
