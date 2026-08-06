"""Independent Python reference of the A/52 §7.2.2 bit-allocation routine.

A direct transcription of the spec pseudocode (integer arithmetic only),
sharing table extraction with gen_bitalloc_tables.py so both implementations
draw from the same source of truth: the standard's own text. Generates
bit-exact golden bap vectors for the C++ engine's unit tests.

Includes the known spec erratum fix in calc_lowcomp (§7.2.2.4 pseudocode has
a stray semicolon after `if ((b0 + 256) == b1)`; the universally implemented
intent - matching the bin >= 7 branch's structure - is else-if chaining).

Run from the repo root:  python tools/bitalloc_ref.py
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


def bit_alloc(exps, fscod, sdcycod, fdcycod, sgaincod, dbpbcod, floorcod,
              fgaincod, csnroffst, fsnroffst):
    """fbw-channel path (start = 0), §7.2.2.1 through §7.2.2.7."""
    end = len(exps)
    # 7.2.2.1.1 special case: all-zero SNR offsets -> all-zero bap.
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
    start = 0
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

    # 7.2.2.4: excitation function (fbw path, bndstrt == 0).
    bndstrt = T["masktab"][start]
    bndend = T["masktab"][end - 1] + 1
    excite = {}
    assert bndstrt == 0
    lowcomp = calc_lowcomp(lowcomp, bndpsd[0], bndpsd[1], 0)
    excite[0] = bndpsd[0] - fgain - lowcomp
    lowcomp = calc_lowcomp(lowcomp, bndpsd[1], bndpsd[2], 1)
    excite[1] = bndpsd[1] - fgain - lowcomp
    begin = 7
    fastleak = slowleak = 0
    for bin_ in range(2, 7):
        lowcomp = calc_lowcomp(lowcomp, bndpsd[bin_], bndpsd[bin_ + 1], bin_)
        fastleak = bndpsd[bin_] - fgain
        slowleak = bndpsd[bin_] - sgain
        excite[bin_] = fastleak - lowcomp
        if bndpsd[bin_] <= bndpsd[bin_ + 1]:
            begin = bin_ + 1
            break
    for bin_ in range(begin, min(bndend, 22)):
        lowcomp = calc_lowcomp(lowcomp, bndpsd[bin_], bndpsd[bin_ + 1], bin_)
        fastleak -= fdecay
        fastleak = max(fastleak, bndpsd[bin_] - fgain)
        slowleak -= sdecay
        slowleak = max(slowleak, bndpsd[bin_] - sgain)
        excite[bin_] = max(fastleak - lowcomp, slowleak)
    for bin_ in range(22, bndend):
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

    # 7.2.2.6: no delta bit allocation (deltbaie = 0).

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
            bap[i] = T["baptab"][address]
            i += 1
        j += 1
        if end <= lastbin:
            break
    return bap


def main():
    import random

    cases = []

    def add_case(name, exps, fscod=0, csnr=20, fsnr=6,
                 sd=2, fd=1, sg=1, db=2, fl=4, fg=4):
        bap = bit_alloc(exps, fscod, sd, fd, sg, db, fl, fg, csnr, fsnr)
        cases.append((name, exps, fscod, csnr, fsnr, sd, fd, sg, db, fl, fg, bap))

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

    parts = [
        "// GENERATED by tools/bitalloc_ref.py - do not edit by hand. Bit-exact",
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
        "};",
        "",
    ]

    def arr(values):
        padded = list(values) + [0xFF] * (253 - len(values))
        rows = []
        for i in range(0, 253, 23):
            rows.append("         " + ", ".join(str(v) for v in padded[i:i + 23]) + ",")
        return "{{\n" + "\n".join(rows) + "\n     }}"

    parts.append(f"inline constexpr std::array<BitAllocCase, {len(cases)}> kBitAllocCases = {{{{")
    for (name, exps, fscod, csnr, fsnr, sd, fd, sg, db, fl, fg, bap) in cases:
        parts.append(f"    {{\"{name}\", {fscod}, {csnr}, {fsnr}, {sd}, {fd}, {sg}, {db}, {fl}, {fg},")
        parts.append(f"     {arr(exps)},")
        parts.append(f"     {arr(bap)},")
        parts.append(f"     {len(exps)}}},")
    parts.append("}};")
    parts.append("")
    parts.append("}  // namespace ac3::golden")

    OUT.write_text("\n".join(parts) + "\n", encoding="utf-8", newline="\n")
    total_bits = sum(sum(b for b in c[-1]) for c in cases)
    print(f"wrote {OUT} ({len(cases)} cases; sanity: sum of all baps = {total_bits})")


if __name__ == "__main__":
    main()
