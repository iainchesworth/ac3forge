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
# ---------------------------------------------------------------------------
add_library(ac3_signing STATIC
    src/sha256.cpp
    src/hmac_sha256.cpp
    src/signing_key.cpp
    src/emdf_atmos_signer.cpp)
add_library(ac3::signing ALIAS ac3_signing)

target_include_directories(ac3_signing
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(ac3_signing PUBLIC cxx_std_23)

# ac3::forge for the parsing primitives; ac3::warnings for the same -Werror
# every other target builds under. PIC so this static lib can link into
# ac3gui/the Android shared object without a text-relocation complaint.
target_link_libraries(ac3_signing
    PUBLIC ac3::forge
    PRIVATE "$<BUILD_INTERFACE:ac3::warnings>")

set_target_properties(ac3_signing PROPERTIES POSITION_INDEPENDENT_CODE ON)

# MSVC flags plain getenv() (signing_key.cpp's env-var fallbacks) as C4996
# "unsafe". The call is POSIX and portable - GCC/Clang raise nothing for it
# under -Wall -Wextra - and the suggested _dupenv_s is a Microsoft CRT
# extension with no NDK/POSIX equivalent, so "fix the code" is not an option
# that stays cross-platform. Scoped to this one compiler's opinion of a
# standard C function, the same way src/lib handles its own MSVC-only
# suppressions (see cmake/CompilerWarnings.cmake).
if(MSVC)
    target_compile_options(ac3_signing PRIVATE /wd4996)
endif()
