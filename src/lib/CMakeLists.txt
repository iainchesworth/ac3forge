# ---------------------------------------------------------------------------
# ac3::forge - the whole codec: bitstream, DSP, encoder, decoder, the spatial
# object layer and the IEC 61937 sink. No GUI, no platform UI, no third-party
# codec code (see docs/RESEARCH.md on the clean-room rule).
# ---------------------------------------------------------------------------
add_library(ac3forge)
add_library(ac3::forge ALIAS ac3forge)

# Everything here is portable: bitstream, DSP, containers, measurement. The
# OS-specific code is selected further down, out of this list.
target_sources(ac3forge
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
        src/encoder/coupling.cpp
        src/encoder/eac3_frame.cpp
        src/encoder/eac3_tools.cpp
        src/encoder/encoder.cpp
        src/encoder/plan.cpp
        src/io/elementary.cpp
        src/io/wav.cpp
        src/meta/drc.cpp
        src/meta/loudness.cpp
        src/meta/mixing.cpp
        src/oba/atmos.cpp
        src/oba/joc.cpp
        src/oba/oamd.cpp
        src/sinks/iec61937.cpp
        src/spatial/spatial.cpp)

target_include_directories(ac3forge
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(ac3forge PUBLIC cxx_std_23)

target_link_libraries(ac3forge PRIVATE ac3::warnings)

# ---------------------------------------------------------------------------
# The platform tree.
#
# Live capture, IEC 61937 passthrough, and the report saying whether either
# exists are the only OS-specific code in the library. Each is one file of the
# SAME name under src/platform/<os>/, and CMake compiles exactly one directory
# of them - so no translation unit ever asks which system it is being built
# for, and adding a platform means adding a directory rather than editing
# conditionals scattered through the sources.
#
# posix/ is the shared Unix implementation; give linux/ or macos/ a file of
# that name only where the two genuinely diverge, and it overrides for that
# platform alone.
# ---------------------------------------------------------------------------
if(NOT WIN32 AND NOT LINUX AND NOT APPLE)
    message(FATAL_ERROR
        "ac3forge has no platform directory for this system. Add "
        "src/lib/src/platform/<os>/{audio_backend,capture,passthrough}.cpp - "
        "src/lib/src/platform/posix/ is the no-backend implementation and is a "
        "working starting point - then select it here.")
endif()

if(WIN32)
    target_sources(ac3forge
        PRIVATE
            src/platform/windows/audio_backend.cpp
            src/platform/windows/capture.cpp
            src/platform/windows/passthrough.cpp)

    # How <windows.h> is to be configured wherever it is included: the lean
    # header set, and no min/max macros to collide with <algorithm>. Stated
    # here rather than #defined at the top of each backend, so the two cannot
    # drift apart and a third backend cannot forget.
    target_compile_definitions(ac3forge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

    # WASAPI: COM for the device enumerator, MMCSS for capture-thread priority.
    target_link_libraries(ac3forge PRIVATE ole32 avrt)
endif(WIN32)

if(LINUX)
    target_sources(ac3forge
        PRIVATE
            src/platform/posix/audio_backend.cpp
            src/platform/posix/capture.cpp
            src/platform/posix/passthrough.cpp)
endif(LINUX)

if(APPLE)
    target_sources(ac3forge
        PRIVATE
            src/platform/posix/audio_backend.cpp
            src/platform/posix/capture.cpp
            src/platform/posix/passthrough.cpp)
endif(APPLE)
