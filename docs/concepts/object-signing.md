# Object signing (the EMDF protection tag)

A clean-room Atmos stream from this encoder is a valid Atmos-in-E-AC-3 stream: the bed decodes
everywhere, and the JOC/OAMD object metadata is spec-correct. But a **Dolby-licensed decoder**
does one extra thing before it will reconstruct the objects — it checks a keyed authenticity tag
carried in the stream's EMDF *protection* field. Without a valid tag it plays the plain 5.1 bed
instead of the height-rendered objects (see [Atmos & JOC](atmos-joc.md#two-honest-limitations)).

`ac3::signing` computes that tag.

## The library has no key — you provide it

This is the one thing to take away from this page:

> **`ac3::signing` contains no key, and never will. Every user of the library supplies their own
> key at runtime — this project's own tools included, and any consumer outside this project
> equally.** The library cannot sign anything until *you* hand it a key.

That is a deliberate design boundary, not a gap to be filled later. The signer holds no embedded
key, reads none from the build, and has no way to obtain or derive one. What it ships is only the
*algorithm*:

| Part | Where it comes from | In the library? |
|---|---|---|
| **HMAC-SHA-256** | FIPS 180-4 / RFC 2104, public standards | Yes — `src/signing/`, dependency-free |
| **What gets authenticated** — which frame regions feed the HMAC, and where the tag is written | The public container layout this codec already emits (`src/lib/src/emdf/emdf.cpp`, the E-AC-3 syntax, TS 103 420) | Yes — `src/signing/src/emdf_atmos_signer.cpp` |
| **The key** | You provision it — exactly as a licensed tool (DEE) receives its own via iLok | **No — never** |

A stream signed with a key that does not match a given decoder's simply fails that decoder's check,
exactly as an unsigned one does. Providing a key that a particular licensed decoder accepts — and
being entitled to use it — is entirely the library user's responsibility, whether that user is this
project or someone building on `ac3::signing` elsewhere.

!!! note "The tag construction, precisely"
    `tag = HMAC-SHA-256(key, A ‖ B)`, truncated to the primary protection field's width.
    **A** is the frame bitstream with its framing/metadata/skip/CRC regions excised and the
    remainder packed and padded to a 16-bit word. **B** is the EMDF container content with the
    protection-tag bits zeroed. The tag is written into `protection_bits_primary` and `crc2` is
    recomputed. Frames with no object container are left untouched.

## Using the library (any consumer)

Any code that links `ac3::signing` gets a key-less signer and must construct a key to use it. The
whole API surface is the key type plus two sign calls:

```cpp
#include "ac3/signing/signing_key.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"

// You own the bytes. There is no default, no built-in, no fallback key.
ac3::signing::SigningKey key{ my_32_key_bytes };          // or:
auto loaded = ac3::signing::load_signing_key("/path/key"); // file/env resolver

// Sign a whole E-AC-3 elementary stream in place; returns the frames signed.
int n = ac3::signing::sign_atmos_stream(stream, key);
```

Full program: [`examples/object_signing.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/object_signing.cpp)
— encodes a two-object Atmos stream and signs it; see [Object signing](../library/signing.md)
in the library reference for the rest of the API surface.

- `SigningKey` owns the key bytes and **zeroizes them on destruction**; it is never persisted.
- An empty `SigningKey` (the default) signs nothing — `sign_atmos_stream` returns 0 and leaves the
  stream untouched. There is no way to end up signed without having supplied a key.
- `load_signing_key()` is a convenience resolver (a path argument, then `AC3FORGE_SIGNING_KEY_FILE`,
  then an inline `AC3FORGE_SIGNING_KEY`) for tools that take a key from the environment; a library
  consumer can ignore it and construct `SigningKey` directly from bytes it obtained however it likes.
- **Key format: base64 or raw bytes.** `decode_signing_key()` (which `load_signing_key()` uses, and
  which you can call yourself on bytes you already hold) base64-decodes its input when it is valid
  base64 — the form a GitHub secret must use, since a secret is text and can't carry a raw binary
  key — and otherwise takes it as raw key bytes. The two are unambiguous in practice; **hex is not a
  supported format** (a hex string is itself valid base64, so the two can't be auto-distinguished).
  So one base64 value works everywhere: as the CI secret, as `AC3FORGE_SIGNING_KEY`, or as a
  `signing-key=` file — and a raw binary key file decodes to the same bytes.

Everything below is just *how the two front ends in this repository supply their own key* — worked
examples of the rule above, not additional machinery.

## How this project supplies its key

### Desktop CLI

Signing is **off unless you both ask for it and provide a key**:

```bash
ac3cli atmos out.ec3 8 448 4 6 objects sign-objects signing-key=/path/to/atmos.key
```

- `sign-objects` requests signing.
- `signing-key=<path>` names the key file (preferred — a path doesn't leak into `ps`/shell history).
- Or, instead of `signing-key=`, set `AC3FORGE_SIGNING_KEY_FILE` (a path) or `AC3FORGE_SIGNING_KEY`
  (the key inline, base64 or raw) — the same resolver order `load_signing_key()` uses.

`sign-objects` with no key anywhere is a hard error (it won't silently ship an unsigned stream); no
`sign-objects` leaves the container unsigned, and — because an unsigned-but-present container is a
hard refusal on a validating decoder rather than a graceful fallback — you'll usually want
`mode bed51` there so the stream omits the container and plays as 5.1 everywhere. See
[CLI metadata options](../cli/metadata-options.md).

### Shield app (on-device signing)

The Shield demo signs each frame on the device, so its key has to be present in the APK as a bundled
asset, `app/src/main/assets/signing.key` — **gitignored, never committed**. It is materialized from
the `ATMOS_SIGNING_KEY` repository secret at build time, and the app decodes it (base64 or raw)
through the same `decode_signing_key()` the CLI uses:

- Both `ci.yml` and `release.yml` forward the secret to the `build-android` job, which writes the
  base64 secret verbatim into the asset before the Gradle build. Signing is gated purely on the
  secret being set — so **any** Shield build signs when it is, debug or release. With no secret (or
  on a fork PR, where secrets don't flow) the step is skipped and the build is the safe, unsigned app.
- Locally, drop your own `signing.key` (base64 or raw bytes) into `app/src/main/assets/` before
  building.

At startup the app loads the asset; present → the object container is emitted and signed, absent →
the app streams the unsigned `bed51`-equivalent, always safe on any receiver.

!!! warning "A signed APK contains the key"
    On-device signing means the key ships **inside** any signed-build APK as that asset — and since
    the secret gates every build, that includes the debug APK CI produces on each push, not just
    releases. Anyone with such an APK can extract the key, so it is as sensitive as the key itself:
    sideload it to your own device and never distribute it. See [Android](../platforms/android.md).

## What this is not

- **Not a key you get from us.** The library ships the machinery, never a key — see above. Whoever
  uses it, in this project or outside it, brings their own and is responsible for being entitled to
  use it.
- **Not a reconstruction of Dolby's own tag algorithm.** The construction here is derived from the
  public container layout, not from a licensed decoder. Whether a given decoder accepts a tag
  depends on the key *you* supply matching that decoder's expectation.
- **Not required for object motion to be audible.** `AtmosEncoder` pans every object into the
  transmitted 5.1 bed regardless of signing, so movement is audible on any decoder — signing is
  what lets a validating decoder reconstruct the objects as *discrete, height-rendered* sources
  rather than panned into the bed. See [Atmos & JOC](atmos-joc.md).
