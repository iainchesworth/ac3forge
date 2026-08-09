# AC-3 & E-AC-3

This page covers the two "core" formats in the family described in [Concepts](index.md):
**AC-3** (Dolby Digital) and **E-AC-3** (Dolby Digital Plus). Dolby Atmos builds on top of
E-AC-3 and gets its own page, [Atmos & JOC](atmos-joc.md).

## Frames

Both formats chop audio into fixed-size chunks called **frames**, sometimes called
**syncframes** because each one starts with a recognisable sync pattern a decoder can search
for. Each frame is compressed independently enough that a decoder can find one, decode it, and
start playing without needing any frame before it. That is what makes it possible to seek
partway into a stream or tune into a broadcast already in progress — the decoder just waits
for the next syncframe rather than needing to start from the very beginning.

Inside a frame, the audio goes through a short pipeline:

```mermaid
graph LR
    A[PCM samples] --> B["Transform (MDCT)"]
    B --> C[Bit allocation]
    C --> D["Packed bitstream<br/>(syncframe)"]
```

- **Transform** — the raw waveform (a list of sample values over time) is converted into
  frequency information: roughly, "how much energy is at each pitch," rather than "what was
  the air pressure at each instant." Audio compresses much better once it's expressed this
  way, because a lot of that frequency information turns out to be small or predictable. The
  tool that does this is called the **MDCT** (modified discrete cosine transform) — the exact
  maths doesn't matter here, only that it is a *transform*, not a compression step by itself.
- **Bit allocation** — having converted the audio to frequency information, the encoder
  decides how many bits to spend describing each part of it, spending more where the human ear
  is more sensitive and less where it is not.
- **Packed bitstream** — the results are packed into the syncframe format the standard
  defines, ready to be written to disc, broadcast, or streamed.

## Channel beds and layout

You'll often see surround sound described as "5.1" or "7.1." The number before the dot is the
count of ordinary, directional speaker channels; the number after the dot is the count of
**LFE** (low-frequency effects) channels — bass-only channels with no fixed direction, because
very low frequencies aren't directional to human hearing anyway.

**5.1** means five directional channels — left (L), centre (C), right (R), left-surround (LS),
right-surround (RS) — plus one LFE channel. This fixed set of channels, all mixed and placed
by the engineer ahead of time, is often called the **bed**.

<figure markdown>
<svg viewBox="0 0 420 400" xmlns="http://www.w3.org/2000/svg" role="img"
     aria-labelledby="fig-51-title fig-51-desc"
     style="width:100%;max-width:380px;height:auto;display:block;margin:0 auto;">
  <title id="fig-51-title">Top-down diagram of a 5.1 speaker layout around a listener</title>
  <desc id="fig-51-desc">
    A top-down view of a room. A listener sits at the centre facing a screen at the top.
    Left, centre and right speakers sit in front of the listener; left-surround and
    right-surround speakers sit behind and to the sides; the LFE bass channel has no fixed
    position and is drawn close to the listener.
  </desc>

  <!-- screen / front indicator -->
  <rect x="160" y="14" width="100" height="8" rx="2" fill="none" stroke="#888" stroke-width="2"/>
  <text x="210" y="10" text-anchor="middle" font-size="11" fill="currentColor">screen / front</text>

  <!-- room outline -->
  <circle cx="210" cy="212" r="172" fill="none" stroke="#888" stroke-width="1.5" stroke-dasharray="4 4"/>

  <!-- lines from listener to speakers -->
  <g stroke="#888" stroke-width="1" opacity="0.55">
    <line x1="210" y1="212" x2="210" y2="62"/>
    <line x1="210" y1="212" x2="135" y2="82.1"/>
    <line x1="210" y1="212" x2="285" y2="82.1"/>
    <line x1="210" y1="212" x2="69" y2="263.3"/>
    <line x1="210" y1="212" x2="351" y2="263.3"/>
  </g>

  <!-- listener -->
  <circle cx="210" cy="212" r="7" fill="#888"/>
  <text x="210" y="238" text-anchor="middle" font-size="11" fill="currentColor">listener</text>

  <!-- C -->
  <circle cx="210" cy="62" r="9" fill="#4C6EF5"/>
  <text x="210" y="48" text-anchor="middle" font-size="14" fill="currentColor">C</text>

  <!-- L -->
  <circle cx="135" cy="82.1" r="9" fill="#4C6EF5"/>
  <text x="115" y="78" text-anchor="middle" font-size="14" fill="currentColor">L</text>

  <!-- R -->
  <circle cx="285" cy="82.1" r="9" fill="#4C6EF5"/>
  <text x="305" y="78" text-anchor="middle" font-size="14" fill="currentColor">R</text>

  <!-- LS -->
  <circle cx="69" cy="263.3" r="9" fill="#7048E8"/>
  <text x="40" y="280" text-anchor="middle" font-size="14" fill="currentColor">LS</text>

  <!-- RS -->
  <circle cx="351" cy="263.3" r="9" fill="#7048E8"/>
  <text x="380" y="280" text-anchor="middle" font-size="14" fill="currentColor">RS</text>

  <!-- LFE -->
  <circle cx="238" cy="196" r="7" fill="#E8590C"/>
  <text x="272" y="192" text-anchor="middle" font-size="13" fill="currentColor">LFE</text>
  <text x="210" y="360" text-anchor="middle" font-size="10.5" fill="currentColor" opacity="0.85">
    LFE (the ".1") has no fixed direction — bass isn't directional
  </text>
</svg>
<figcaption>A 5.1 layout seen from above: L, C, R in front; LS, RS to the sides/rear; LFE
anywhere, because bass has no direction.</figcaption>
</figure>

E-AC-3 can describe larger layouts too — 7.1 and beyond — described in the next section.

## Bitrate

ac3forge encodes **CBR** (constant bit rate) only — no variable bit rate. Every frame of a
given stream spends the same number of bits, chosen from the 19 nominal rates the standard
defines, from 32 kbps up to 640 kbps. As with any lossy compressed format, the general rule
holds: a higher bitrate means more bits are spent describing each second of audio, which
generally means better quality, at the cost of a larger file or a bigger slice of a broadcast
pipe's bandwidth.

## What E-AC-3 adds over AC-3

E-AC-3 keeps everything AC-3 can do and adds more on top:

- **More channel layouts.** Beyond the plain 5.1-and-smaller layouts AC-3 supports, E-AC-3 can
  describe 7.1, 5.1.2, 5.1.4 and 7.1.4. It does this through **dependent substreams** — extra
  layers of channels riding alongside the main 5.1 bed, adding channels like extra height or
  rear speakers without redefining the whole stream format.
- **Better compression tools**, each recognised by name in the standard:
    - **Coupling** shares high-frequency detail across channels, because at high frequencies
      the ear is poor at telling *which* channel a sound is coming from anyway, so several
      channels can share one coded copy of that detail instead of each paying for their own.
    - **Spectral extension** predicts a channel's higher frequencies from its lower ones,
      instead of coding the high end directly — cheaper than describing every frequency band
      from scratch.
    - **Adaptive hybrid transform** swaps in a sharper transform for parts of the signal that
      need the extra precision, rather than using one fixed transform for everything.

Together, these are why E-AC-3 fits more channels and better quality into a given bitrate than
plain AC-3 can.

!!! example "See it in code"
    - [Encoding AC-3](../library/encoding-ac3.md)
    - [Encoding E-AC-3](../library/encoding-eac3.md)
    - [CLI commands](../cli/commands.md)

---

Next: [Atmos & JOC](atmos-joc.md), where E-AC-3 gains a layer of 3D-positioned sound objects.
