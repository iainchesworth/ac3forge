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

Source: [`platform/wasm/`](https://github.com/iainchesworth/ac3forge/tree/develop/platform/wasm) —
see [WebAssembly](platforms/wasm.md) for the build/toolchain details and what's reused vs. new. CI
rebuilds this embed fresh from source on every deploy to `main`; see
[Release / CI](platforms/wasm.md#release-ci).
