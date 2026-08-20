"""Independent Python reference of the A/52 §7.2.2 bit-allocation routine.

A direct transcription of the spec pseudocode (integer arithmetic only),
sharing table extraction with gen_bitalloc_tables.py so both implementations
draw from the same source of truth: the standard's own text. Generates
bit-exact golden bap vectors for the C++ engine's unit tests.

Includes two known spec erratum fixes:

- calc_lowcomp (§7.2.2.4 pseudocode has a stray semicolon after
  `if ((b0 + 256) == b1)`; the universally implemented intent - matching the
  bin >= 7 branch's structure - is else-if chaining).
- The §7.2.2.6 delta bit allocation band cursor (see bit_alloc()'s own note):
  the pseudocode initializes `band = 0` literally, but on the coupling
  channel - whose own band range does not start at 0 - both FFmpeg and
  Dolby's own reference decoder (dlbac3dec, verified directly via gst-launch)
  require band to start at bndstrt instead, or they reject the stream.

Run from the repo root:  python tools/references/bitalloc_ref.py
"""

from pathlib import Path

from gen_bitalloc_tables import parse_tables

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "tests" / "golden" / "bitalloc_goldens.hpp"

T = parse_tables()


def logadd(a, b):
    c = a - b
    address = min(abs(c) >> 1, 255)
    return (a if c >= 0 else b) + T["latab"][address]


def calc_lowcomp(a, b0, b1, bin_):
    if bin_ < 7:
        if b0 + 256 == b1:
            a = 384
        elif b0 > b1:
            a = max(0, a - 64)
    elif bin_ < 20:
        if b0 + 256 == b1:
            a = 320
        elif b0 > b1:
            a = max(0, a - 64)
    else:
        a = max(0, a - 128)
    return a


# Table E3.1, hebaptab: the same address, 20 outcomes instead of 16.
HEBAPTAB = [0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8, 8, 9, 9, 9, 10,
            10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14,
            14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 17, 18,
            18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19]

# Table E3.2: bits per AHT bin FOR THE WHOLE FRAME - a VQ index covering six
# blocks below hebap 8, six scalar mantissas at and above it.
_AHT_VQ_BITS = [0, 2, 3, 4, 5, 7, 8, 9]
_AHT_MANT_BITS = [0, 0, 0, 0, 0, 0, 0, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16]


def aht_bin_bits(hebap):
    if hebap <= 0:
        return 0
    if hebap <= 7:
        return _AHT_VQ_BITS[hebap]
    return 6 * _AHT_MANT_BITS[hebap]


def bit_alloc(exps, fscod, sdcycod, fdcycod, sgaincod, dbpbcod, floorcod,
              fgaincod, csnroffst, fsnroffst, start=0, coupling=False,
              cplfleak=0, cplsleak=0, high_efficiency=False, deltnseg=0,
              deltoffst=(), deltlen=(), deltba=()):
    """§7.2.2.1 through §7.2.2.7 for one channel.

    start/coupling select the two shapes: fbw and LFE channels start at bin 0
    and run the lowcomp path; the coupling channel starts at cplstrtmant and
    seeds its leak state from cplfleak/cplsleak instead. high_efficiency picks
    §E3.4.3.1's hebaptab, which is the only change AHT makes to the routine.
    deltnseg/deltoffst/deltlen/deltba are §7.2.2.6's delta bit allocation -
    deltnseg == 0 (the default) is the spec's own "no delta" reset state.
    """
    end = len(exps)
    # 7.2.2.1.1: the all-zero-SNR mute spans EVERY channel's offsets. These
    # golden cases are single-channel, so the frame-wide condition reduces to
    # this channel's own offsets being zero.
    if csnroffst == 0 and fsnroffst == 0:
        return [0] * end

    sdecay = T["slowdec"][sdcycod]
    fdecay = T["fastdec"][fdcycod]
    sgain = T["slowgain"][sgaincod]
    dbknee = T["dbpbtab"][dbpbcod]
    floor = T["floortab"][floorcod]
    if floor >= 0x8000:
        floor -= 0x10000  # 0xf800 is a negative 16-bit value
    fgain = T["fastgain"][fgaincod]
    snroffset = ((csnroffst - 15) << 4) + fsnroffst << 2
    lowcomp = 0

    # 7.2.2.2: exponent -> psd.
    psd = [3072 - (e << 7) for e in exps]

    # 7.2.2.3: banded integration via log-addition.
    bndpsd = {}
    j = start
    k = T["masktab"][start]
    while True:
        lastbin = min(T["bndtab"][k] + T["bndsz"][k], end)
        bndpsd[k] = psd[j]
        j += 1
        for _ in range(j, lastbin):
            bndpsd[k] = logadd(bndpsd[k], psd[j])
            j += 1
        k += 1
        if end <= lastbin:
            break

    # 7.2.2.4: excitation function. Two shapes: fbw/LFE start at band 0 and
    # run lowcomp; the coupling channel starts higher and seeds its leaks.
    bndstrt = T["masktab"][start]
    bndend = T["masktab"][end - 1] + 1
    excite = {}
    fastleak = slowleak = 0
    begin_band = bndstrt
    if coupling:
        fastleak = (cplfleak << 8) + 768
        slowleak = (cplsleak << 8) + 768
    else:
        assert bndstrt == 0

        # LFE (bndend == 7): skip calc_lowcomp and the break check at bin 6.
        def not_lfe_last(bin_):
            return bndend != 7 or bin_ != 6

        lowcomp = calc_lowcomp(lowcomp, bndpsd[0], bndpsd[1], 0)
        excite[0] = bndpsd[0] - fgain - lowcomp
        lowcomp = calc_lowcomp(lowcomp, bndpsd[1], bndpsd[2], 1)
        excite[1] = bndpsd[1] - fgain - lowcomp
        begin = 7
        for bin_ in range(2, 7):
            if not_lfe_last(bin_):
                lowcomp = calc_lowcomp(lowcomp, bndpsd[bin_], bndpsd[bin_ + 1], bin_)
            fastleak = bndpsd[bin_] - fgain
            slowleak = bndpsd[bin_] - sgain
            excite[bin_] = fastleak - lowcomp
            if not_lfe_last(bin_) and bndpsd[bin_] <= bndpsd[bin_ + 1]:
                begin = bin_ + 1
                break
        for bin_ in range(begin, min(bndend, 22)):
            if not_lfe_last(bin_):
                lowcomp = calc_lowcomp(lowcomp, bndpsd[bin_], bndpsd[bin_ + 1], bin_)
            fastleak -= fdecay
            fastleak = max(fastleak, bndpsd[bin_] - fgain)
            slowleak -= sdecay
            slowleak = max(slowleak, bndpsd[bin_] - sgain)
            excite[bin_] = max(fastleak - lowcomp, slowleak)
        begin_band = 22

    # Common upper region: no lowcomp, plain dual-leak decay.
    for bin_ in range(begin_band, bndend):
        fastleak -= fdecay
        fastleak = max(fastleak, bndpsd[bin_] - fgain)
        slowleak -= sdecay
        slowleak = max(slowleak, bndpsd[bin_] - sgain)
        excite[bin_] = max(fastleak, slowleak)

    # 7.2.2.5: masking curve.
    mask = {}
    for bin_ in range(bndstrt, bndend):
        if bndpsd[bin_] < dbknee:
            excite[bin_] += (dbknee - bndpsd[bin_]) >> 2
        mask[bin_] = max(excite[bin_], T["hth"][bin_][fscod])

    # 7.2.2.6: delta bit allocation. mask[]/psd[] units are 128 per exponent
    # step, exactly one Table 5.17 6 dB step, so `delta` is added with no
    # unit conversion.
    #
    # band starts at bndstrt, not the pseudocode's literal 0: mask[] here is
    # this routine's own global-indexed array, so on the coupling channel (the
    # one channel whose bndstrt isn't 0) a literal band=0 would require every
    # deltoffst to encode an absolute band number - which both FFmpeg and
    # Dolby's reference decoder reject in practice (module docstring). For
    # fbw/LFE (bndstrt == 0) this is unchanged from the literal reading.
    band = bndstrt
    for seg in range(deltnseg):
        band += deltoffst[seg]
        code = deltba[seg]
        delta = (code - 3 if code >= 4 else code - 4) << 7
        for _ in range(deltlen[seg]):
            mask[band] += delta
            band += 1

    # 7.2.2.7: bap computation.
    bap = [0] * end
    i = start
    j = T["masktab"][start]
    while True:
        lastbin = min(T["bndtab"][j] + T["bndsz"][j], end)
        m = mask[j] - snroffset - floor
        if m < 0:
            m = 0
        m &= 0x1FE0
        m += floor
        for _ in range(i, lastbin):
            address = (psd[i] - m) >> 5
            address = min(63, max(0, address))
            bap[i] = HEBAPTAB[address] if high_efficiency else T["baptab"][address]
            i += 1
        j += 1
        if end <= lastbin:
            break
    return bap


def main():
    import random

    cases = []

    def add_case(name, exps, fscod=0, csnr=20, fsnr=6,
                 sd=2, fd=1, sg=1, db=2, fl=4, fg=4,
                 start=0, coupling=False, cplfleak=0, cplsleak=0,
                 deltoffst=(), deltlen=(), deltba=()):
        deltnseg = len(deltoffst)
        bap = bit_alloc(exps, fscod, sd, fd, sg, db, fl, fg, csnr, fsnr,
                        start=start, coupling=coupling, cplfleak=cplfleak,
                        cplsleak=cplsleak, deltnseg=deltnseg, deltoffst=deltoffst,
                        deltlen=deltlen, deltba=deltba)
        cases.append((name, exps, fscod, csnr, fsnr, sd, fd, sg, db, fl, fg, bap,
                      start, coupling, cplfleak, cplsleak, deltnseg,
                      list(deltoffst), list(deltlen), list(deltba)))

    rng = random.Random(0x52)

    silence = [24] * 253
    add_case("Silence", silence)
    add_case("SilenceZeroOffset", silence, csnr=0, fsnr=0)

    # A sine-like concentration: loud at low bins, quiet elsewhere.
    sine = [24] * 253
    for b, e in [(19, 6), (20, 2), (21, 0), (22, 2), (23, 6), (24, 10), (25, 14)]:
        sine[b] = e
    add_case("SineLike", sine)
    add_case("SineLikeHighOffset", sine, csnr=40, fsnr=12)

    ramp = [max(0, min(24, b // 11)) for b in range(253)]
    add_case("Ramp", ramp)

    rand73 = [rng.randint(0, 24) for _ in range(73)]
    add_case("Random73", rand73, fscod=1, csnr=25, fsnr=3, fl=7)

    rand253 = [rng.randint(0, 24) for _ in range(253)]
    add_case("Random253", rand253, fscod=2, csnr=15, fsnr=15, sd=0, fd=3, sg=3, db=0, fg=7)

    # LFE-sized sets (endmant 7): exercise the bndend==7 lowcomp skip.
    add_case("LfeLoud", [2, 3, 4, 6, 9, 14, 20])
    add_case("LfeRising", [20, 18, 14, 10, 6, 3, 1], csnr=30, fsnr=8)

    # Coupling channel: starts at cplstrtmant = 37 + 12*cplbegf and runs to
    # cplendmant = 37 + 12*(cplendf + 3), seeding the leaks from
    # cplfleak/cplsleak instead of the lowcomp path.
    for cplbegf, cplendf, fleak, sleak, tag in ((6, 12, 0, 0, "Mid"),
                                                (0, 14, 7, 7, "Wide"),
                                                (10, 4, 3, 5, "High")):
        strtmant = 37 + 12 * cplbegf
        endmant = 37 + 12 * (cplendf + 3)
        if endmant > 253 or strtmant >= endmant:
            continue
        exps = [24] * endmant
        for bin_ in range(strtmant, endmant):
            exps[bin_] = 4 + (bin_ * 5) % 19
        add_case(f"Coupling{tag}", exps, start=strtmant, coupling=True,
                 cplfleak=fleak, cplsleak=sleak, csnr=22, fsnr=9)

    # §7.2.2.6: delta bit allocation. One boost segment, one cut segment, and
    # a multi-segment case exercising the offset-from-previous-end encoding.
    add_case("DeltaBoost", ramp, deltoffst=(10,), deltlen=(5,), deltba=(7,))
    add_case("DeltaCut", ramp, deltoffst=(10,), deltlen=(5,), deltba=(0,))
    add_case("DeltaMultiSeg", rand253, fscod=2, csnr=15, fsnr=15, sd=0, fd=3,
             sg=3, db=0, fg=7, deltoffst=(2, 4, 3), deltlen=(3, 2, 6),
             deltba=(6, 1, 4))
    # deltoffst[0] is relative to the coupling channel's OWN start band
    # (bndstrt = masktab[37] = 31 here), not an absolute band number - see
    # bit_alloc()'s note on the delta cursor. A deltoffst[0] of 0 lands the
    # first segment right at bndstrt, the same target band the old
    # (absolute-band) reading of this case reached via deltoffst[0]=31.
    add_case("DeltaOnCoupling", [24] * 121, start=37, coupling=True, cplfleak=3,
             cplsleak=3, csnr=22, fsnr=9, deltoffst=(0, 2),
             deltlen=(8, 3), deltba=(5, 2))

    parts = [
        "// GENERATED by tools/references/bitalloc_ref.py - do not edit by hand. Bit-exact",
        "// golden bap vectors from an independent Python transcription of the",
        "// A/52 7.2.2 integer pseudocode (integer math: zero tolerance).",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace ac3::golden {",
        "",
        "struct BitAllocCase {",
        "    const char* name;",
        "    int fscod;",
        "    int csnroffst;",
        "    int fsnroffst;",
        "    int sdcycod, fdcycod, sgaincod, dbpbcod, floorcod, fgaincod;",
        "    std::array<std::uint8_t, 253> exps;   // padded with 0xFF past endmant",
        "    std::array<std::uint8_t, 253> bap;    // padded with 0xFF past endmant",
        "    int endmant;",
        "    int start;",
        "    bool coupling;",
        "    int cplfleak, cplsleak;",
        "    int deltnseg;",
        "    std::array<std::uint8_t, 8> deltoffst, deltlen, deltba;",
        "};",
        "",
    ]

    def arr(values):
        padded = list(values) + [0xFF] * (253 - len(values))
        rows = []
        for i in range(0, 253, 23):
            rows.append("         " + ", ".join(str(v) for v in padded[i:i + 23]) + ",")
        return "{{\n" + "\n".join(rows) + "\n     }}"

    def small_arr(values):
        padded = list(values) + [0] * (8 - len(values))
        return "{{" + ", ".join(str(v) for v in padded) + "}}"

    parts.append(f"inline constexpr std::array<BitAllocCase, {len(cases)}> kBitAllocCases = {{{{")
    for (name, exps, fscod, csnr, fsnr, sd, fd, sg, db, fl, fg, bap,
         start, coupling, cplfleak, cplsleak, deltnseg, deltoffst, deltlen,
         deltba) in cases:
        parts.append(f"    {{\"{name}\", {fscod}, {csnr}, {fsnr}, {sd}, {fd}, {sg}, {db}, {fl}, {fg},")
        parts.append(f"     {arr(exps)},")
        parts.append(f"     {arr(bap)},")
        parts.append(f"     {len(exps)}, {start}, {'true' if coupling else 'false'}, "
                     f"{cplfleak}, {cplsleak}, {deltnseg},")
        parts.append(f"     {small_arr(deltoffst)}, {small_arr(deltlen)}, {small_arr(deltba)}}},")
    parts.append("}};")
    parts.append("")
    parts.append("}  // namespace ac3::golden")

    OUT.write_text("\n".join(parts) + "\n", encoding="utf-8", newline="\n")
    total_bits = sum(sum(case[11]) for case in cases)  # index 11 is the bap list
    print(f"wrote {OUT} ({len(cases)} cases; sanity: sum of all baps = {total_bits})")


if __name__ == "__main__":
    main()
