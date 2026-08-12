# Object signing (the EMDF protection tag)

A clean-room Atmos stream from this encoder is a valid Atmos-in-E-AC-3 stream: the bed decodes
everywhere, and the JOC/OAMD object metadata is spec-correct. But a **Dolby-licensed decoder**
does one extra thing before it will reconstruct the objects — it checks a keyed authenticity tag
carried in the stream's EMDF *protection* field. Without a valid tag it plays the plain 5.1 bed
instead of the height-rendered objects (see [Atmos & JOC](atmos-joc.md#two-honest-limitations)).

`ac3::signing` produces that tag. This page explains what is signed, what is and isn't in the
repository, and how to turn signing on for the CLI and the Shield app.

## What's the secret, and what isn't

The important split — and the reason the signer can live in the tree at all:

| Part | Where it comes from | In the repo? |
|---|---|---|
| **HMAC-SHA-256** | FIPS 180-4 / RFC 2104, public standards | Yes — `src/signing/` |
| **What gets authenticated** — which frame regions feed the HMAC, and where the tag is written | The public container layout this codec already emits (`src/lib/src/emdf/emdf.cpp`, the E-AC-3 syntax, TS 103 420) | Yes — `src/signing/src/emdf_atmos_signer.cpp` |
| **The key** | Provisioned by the operator, exactly as a licensed tool (DEE) receives its own via iLok | **No — never** |

So the *algorithm* is committed and dependency-free (no OpenSSL — the codec's "no third-party
dependencies" rule holds), and only the **key** is external. The signer holds no key of its own
and cannot obtain one: you pass it in at runtime. A stream signed with a key that does not match a
given decoder's simply fails that decoder's check, exactly as an unsigned one does — nothing here
reconstructs or guesses a key.

!!! note "The tag construction, precisely"
    `tag = HMAC-SHA-256(key, A ‖ B)`, truncated to the primary protection field's width.
    **A** is the frame bitstream with its framing/metadata/skip/CRC regions excised and the
    remainder packed and padded to a 16-bit word. **B** is the EMDF container content with the
    protection-tag bits zeroed. The tag is written into `protection_bits_primary` and `crc2` is
    recomputed. Frames with no object container are left untouched.

## The key, and how to provision it

The key is 32 bytes. Supply it as **raw bytes** or as **whitespace-delimited hex** (hex is tried
first; anything that isn't hex is taken verbatim). It is never written to disk by this code, and
`SigningKey` zeroizes its bytes when it is destroyed.

### Desktop CLI

Signing is **off unless you both ask for it and provide a key** — either alone is inert:

```bash
ac3cli atmos out.ec3 8 448 4 6 objects sign-objects signing-key=/path/to/atmos.key
```

- `sign-objects` requests signing.
- `signing-key=<path>` names the key file (preferred — a path doesn't leak into `ps`/shell history).
- Or, instead of `signing-key=`, set one of these environment variables (resolved in this order):
    - `AC3FORGE_SIGNING_KEY_FILE` — a path, same as `signing-key=`.
    - `AC3FORGE_SIGNING_KEY` — the key inline (hex or raw), convenient for CI secrets.

`sign-objects` with no key anywhere is a hard error (it won't silently ship an unsigned stream);
no `sign-objects` leaves the container unsigned, and — because an unsigned-but-present container is
a hard refusal on a validating decoder rather than a graceful fallback — you'll usually want
`mode bed51` there so the stream omits the container and plays as 5.1 everywhere. See
[CLI metadata options](../cli/metadata-options.md).

### Shield app (on-device signing)

The Shield demo signs each frame on the device, so the key has to be present in the APK as a
bundled asset, `app/src/main/assets/signing.key` (raw key bytes). That file is **gitignored** and
is written from a CI secret at build time — it is never committed:

- CI: set the repository secret `ATMOS_SIGNING_KEY` (the key, base64-encoded). Both `ci.yml` and
  `release.yml` forward it to the `build-android` job, which decodes it into the asset before the
  Gradle build — so **any** Shield build signs when the secret is set, debug or release. With no
  secret (or on a fork PR, where secrets don't flow) the step is skipped and the build is the safe,
  unsigned app.
- Locally: drop your own `signing.key` (raw bytes) into `app/src/main/assets/` before building.

At startup the app loads the asset; if it's present the object container is emitted and signed, and
if it's absent the app streams the unsigned `bed51`-equivalent — always safe on any receiver.

!!! warning "A signed APK contains the key"
    On-device signing means the key ships **inside** any signed-build APK as that asset — anyone
    with the APK can extract it. A signed APK is therefore as sensitive as the key: sideload it to
    your own device and never distribute it. See [Android](../platforms/android.md).

## What this is not

- **Not a key you get from us.** The repository ships the machinery, never a key. Provisioning a
  licensed key is your responsibility, the same as it is for any licensed Atmos toolchain.
- **Not a reconstruction of Dolby's own tag algorithm.** The construction here is derived from the
  public container layout, not from a licensed decoder. Whether a given decoder accepts a tag
  depends on the key you supply matching that decoder's expectation.
- **Not required for object motion to be audible.** `AtmosEncoder` pans every object into the
  transmitted 5.1 bed regardless of signing, so movement is audible on any decoder — signing is
  what lets a validating decoder reconstruct the objects as *discrete, height-rendered* sources
  rather than panned into the bed. See [Atmos & JOC](atmos-joc.md).
