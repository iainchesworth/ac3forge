# ---------------------------------------------------------------------------
# The Shield app (platform/android) is a full Gradle/NDK project outside the
# ac3forge CMake configure entirely, reached from the other direction (its own
# CMakeLists.txt add_subdirectory()s the repo root). This platform doesn't
# need that indirection - there's no non-CMake build system in the way, so
# it's a normal add_subdirectory() from root CMakeLists.txt, gated on
# EMSCRIPTEN there rather than needing a whole separate entry point.
#
# Roadmap F3: a browser demo that decodes a real E-AC-3/Atmos-in-DD+
# elementary stream via ac3::forge compiled to WASM, plays the decoded bed
# audio through the Web Audio API, and visualizes real per-channel energy in
# time with playback. See docs/platforms/wasm.md.
#
# Produces a self-contained, servable directory at
# ${CMAKE_BINARY_DIR}/bin/wasm_decode_demo/: the compiled .js/.wasm, the
# static page and its JS, and the bundled demo bitstream. `python3 -m
# http.server` from that directory (or any static file server - ES modules
# and fetch() need http(s), not file://) is the entire "run the demo" story.
# ---------------------------------------------------------------------------

add_executable(ac3forge_wasm_decode decoder_bindings.cpp)
target_link_libraries(ac3forge_wasm_decode PRIVATE ac3::forge ac3::warnings)

# --bind: Embind, the JS<->C++ binding generator decoder_bindings.cpp's
# EMSCRIPTEN_BINDINGS macro uses. MODULARIZE+EXPORT_NAME: the loader becomes a
# plain factory function (createAc3ForgeModule()) rather than a global that
# would collide with a second WASM module on the same page - see index.html's
# import. ALLOW_MEMORY_GROWTH: an uploaded stream's decoded PCM size is not
# known ahead of time, unlike the bundled demo fixture. ENVIRONMENT=web: this
# is a browser page, not also a Node/worker target, so the smaller web-only
# runtime is enough. -fexceptions (compile AND link): ac3::forge itself never
# throws (std::expected throughout, matching every other target in this
# project), but transitively-called std library paths (bad_alloc, vector::at)
# still can - and Emscripten's default of exceptions compiled out turns any
# such throw into an immediate hard abort rather than the ordinary unwind a
# desktop build gets, which is a worse failure mode for a page a stranger's
# browser is running than the modest code-size/perf cost of leaving them on.
target_compile_options(ac3forge_wasm_decode PRIVATE "-fexceptions")
target_link_options(ac3forge_wasm_decode PRIVATE
    "-fexceptions"
    "--bind"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createAc3ForgeModule"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sENVIRONMENT=web"
    "-sNO_EXIT_RUNTIME=1")

set_target_properties(ac3forge_wasm_decode PROPERTIES
    OUTPUT_NAME "ac3forge_decode"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/wasm_decode_demo")

add_custom_command(TARGET ac3forge_wasm_decode POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/demo.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/favicon.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/favicon-32.png"
        "${CMAKE_BINARY_DIR}/bin/wasm_decode_demo"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_BINARY_DIR}/bin/wasm_decode_demo/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/demo.ec3"
        "${CMAKE_BINARY_DIR}/bin/wasm_decode_demo/assets"
    COMMENT "Assembling the servable wasm_decode_demo/ directory")
