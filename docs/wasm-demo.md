# Live decode demo (WASM)

`ac3::forge`'s decoder, compiled to WebAssembly, decoding a real E-AC-3
elementary stream entirely in your browser — no server-side decode, no
upload. This is the same C++ decode path `ac3cli decode` uses, running as
WASM instead of a native binary.

<!-- The iframe src below is raw HTML (md_in_html), which mkdocs passes through
     verbatim rather than rewriting the way it rewrites real markdown links -
     it has to be relative to this PAGE's own built URL (wasm-demo/index.html,
     directory URLs are on), hence "../". The markdown link further down is
     the opposite case: mkdocs rewrites markdown-syntax links itself, relative
     to this SOURCE file's own location (docs/wasm-demo.md, at the docs root),
     so it does NOT get a "../" even though it points at the same target. -->
<div style="border:1px solid var(--md-default-fg-color--lightest); border-radius:0.4em; overflow:hidden;">
  <iframe
    src="../assets/wasm-decode-demo/index.html"
    title="ac3forge WASM decode demo"
    style="width:100%; height:900px; border:0; display:block;"
    loading="lazy">
  </iframe>
</div>

[Open the demo in its own tab](assets/wasm-decode-demo/index.html){ target="_blank" }

## What's real, and what isn't

Real: the decode, the audio, and the per-channel energy driving the two
speaker rings (solid = ear-level, dashed = ceiling — ported from the desktop
GUI's `SoundfieldView.qml`). Drop in your own `.ec3`/`.ac3` file to decode
something other than the bundled fixture. Not real yet: individual Atmos
object positions — `ac3::forge` can *write* OAMD/JOC metadata (see
[Atmos & JOC](concepts/atmos-joc.md)) but can't read it back out of a stream,
so there's nothing object-level to show. Tracked as follow-up work.

## Source and how it's built

Source: [`examples/wasm_decode_demo/`](https://github.com/iainchesworth/ac3forge/tree/develop/examples/wasm_decode_demo)
— an Embind wrapper (`decoder_bindings.cpp`) around `ac3::forge`'s existing
`FrameDecoder`/`Eac3Decoder` API, built via the `config-wasm-emscripten`
CMake preset (needs [Emscripten](https://emscripten.org/) on `PATH`, see
`cmake/toolchains/wasm.emscripten.toolchain.cmake`).

The files under `docs/assets/wasm-decode-demo/` embedded above are a
**prebuilt copy** of that target's output, committed directly rather than
rebuilt by CI: the docs-publishing workflow (`.github/workflows/docs.yml`)
only runs `mkdocs build`, with no C++ toolchain or Emscripten SDK installed,
so there is nowhere in the current pipeline for an `emcc` build to happen.
Regenerate them by building the `ac3forge_wasm_decode` target with the
`config-wasm-emscripten` preset and copying
`build/config-wasm-emscripten/bin/wasm_decode_demo/` over this directory —
teaching CI to do that automatically is tracked as follow-up work, not done
as part of landing this page.
