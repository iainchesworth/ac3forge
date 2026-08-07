# ---------------------------------------------------------------------------
# ac3::forge - the whole codec: bitstream, DSP, encoder, decoder, the spatial
# object layer and the IEC 61937 sink. No GUI, no platform UI, no third-party
# codec code (see docs/RESEARCH.md on the clean-room rule).
# ---------------------------------------------------------------------------
add_library(ac3forge)
add_library(ac3::forge ALIAS ac3forge)

target_sources(ac3forge
    PRIVATE
        src/core/bitalloc.cpp
        src/core/exponents.cpp
        src/core/mantissas.cpp
        src/core/mdct.cpp
        src/decoder/decoder.cpp
        src/encoder/encoder.cpp
        src/io/wav.cpp
        src/sinks/iec61937.cpp
        src/spatial/spatial.cpp)

target_include_directories(ac3forge
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(ac3forge PUBLIC cxx_std_23)

target_link_libraries(ac3forge PRIVATE ac3::warnings)
