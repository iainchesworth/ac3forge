# Object signing: `ac3::signing`

`ac3/signing/emdf_atmos_signer.hpp`, `signing_key.hpp` — a separate library, `ac3::signing`, not
part of `ac3::forge`: signing is a strictly optional step a front end applies to already-encoded
frames, so the codec itself has no dependency on it. See
[Object signing](../concepts/object-signing.md) for what this is *for* — a licensed decoder
treats the EMDF container's `emdf_protection` field as a commitment to object decoding and
refuses the whole stream if the tag does not check out; this computes the keyed tag that
satisfies it.

```cpp
const ac3::signing::SigningKey key{key_bytes};   // the operator's own, at runtime
const int signed_count = ac3::signing::sign_atmos_stream(stream, key);
```

Full program: [`examples/object_signing.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/object_signing.cpp)
— encodes a two-object Atmos stream, then signs it. The key used there is a literal stand-in
for the example's own sake, not a real key for any real decoder — a real one is always the
operator's to provision at runtime (an environment variable or a `signing-key=<path>` file in
the CLI), never compiled in.

`sign_atmos_stream` signs, in place, every syncframe in a stream that carries an EMDF object
container — frames without one are left untouched — and returns how many it signed.
`sign_atmos_frame` does the same for one syncframe at a time, returning whether that frame
carried a container at all. Scope is deliberately narrow: the ac3forge "atmos" output (a single
independent 5.1 substream, frame-level exponent strategy and SNR, no coupling) — a frame outside
that subset asserts in debug and is left unsigned in release rather than signed wrong.

`SigningKey` owns its bytes and zeroizes them on destruction, so a supplied key does not linger
in freed heap after signing finishes. `load_signing_key(explicit_path)` resolves one from, in
order, an explicit path, `$AC3FORGE_SIGNING_KEY_FILE`, then `$AC3FORGE_SIGNING_KEY` — the same
resolution the CLI's `signing-key=<path>` uses. `decode_signing_key` is the shared decode underneath
all three: base64 when the content is valid base64 (the CI/secret-transport form — a GitHub
secret is text and cannot carry a raw binary key), otherwise taken as raw bytes.

The HMAC-SHA-256 construction (RFC 2104 / FIPS 180-4) and the choice of which frame regions are
authenticated are clean-room: derived from this codec's own public container layout
(`ac3::emdf`, `ETSI TS 103 420`) and built on this project's own parsing primitives. The *key*
is the one piece never carried here — the same posture a licensed tool takes with its own
(iLok-provisioned) key.

---

See also: [Object signing](../concepts/object-signing.md) — the concept and why the key can
never be embedded; [Spatial & Atmos objects](spatial-and-atmos.md) — where the container being
signed comes from; [Header map](header-map.md).
