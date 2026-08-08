# ---------------------------------------------------------------------------
# ac3::forge - the whole codec: bitstream, DSP, encoder, decoder, the spatial
# object layer and the IEC 61937 sink. No GUI, no platform UI, no third-party
# codec code (see docs/RESEARCH.md on the clean-room rule).
# ---------------------------------------------------------------------------
add_library(ac3forge)
add_library(ac3::forge ALIAS ac3forge)

# Select the single platform implementation of capture and passthrough for
# the target OS. No #ifdefs: CMake decides which translation unit is compiled.
if(WIN32)
    set(AC3_CAPTURE_SOURCE src/capture/capture_wasapi.cpp)
    set(AC3_PASSTHROUGH_SOURCE src/sinks/passthrough_wasapi.cpp)
else()
    set(AC3_CAPTURE_SOURCE src/capture/capture_stub.cpp)
    set(AC3_PASSTHROUGH_SOURCE src/sinks/passthrough_stub.cpp)
endif()

target_sources(ac3forge
    PRIVATE
        ${AC3_CAPTURE_SOURCE}
        ${AC3_PASSTHROUGH_SOURCE}
        src/core/bitalloc.cpp
        src/core/exponents.cpp
        src/core/mantissas.cpp
        src/core/mdct.cpp
        src/decoder/decoder.cpp
        src/encoder/coupling.cpp
        src/encoder/eac3_frame.cpp
        src/encoder/encoder.cpp
        src/io/wav.cpp
        src/meta/drc.cpp
        src/meta/loudness.cpp
        src/meta/mixing.cpp
        src/sinks/iec61937.cpp
        src/spatial/spatial.cpp)

target_include_directories(ac3forge
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(ac3forge PUBLIC cxx_std_23)

target_link_libraries(ac3forge PRIVATE ac3::warnings)

# WASAPI: COM for the device enumerator, MMCSS for capture-thread priority.
if(WIN32)
    target_link_libraries(ac3forge PRIVATE ole32 avrt)
endif()
