# ---------------------------------------------------------------------------
# matroska::matroska - a minimal MKV container writer.
#
# Deliberately standalone: it links nothing from ac3::forge and knows nothing
# about AC-3. It nests EBML elements and lays out clusters, taking every frame
# as opaque bytes, so it can be reasoned about, tested and reused on its own.
# The dependency runs one way only - front ends may use both, but the codec
# library never reaches for the container and vice versa.
# ---------------------------------------------------------------------------
add_library(matroska)
add_library(matroska::matroska ALIAS matroska)

target_sources(matroska
    PRIVATE
        src/matroska.cpp)

target_include_directories(matroska
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(matroska PUBLIC cxx_std_23)

target_link_libraries(matroska PRIVATE ac3::warnings)
