"""Independent reference for the A/52 dynamic-range word formats.

Transcribes Table 7.29 (dynrng, section 7.7.1.2) and Table 7.30 (compr,
section 7.7.2.2) as literal table lookups of the "Arithmetic Shifts" column,
then applies the mantissa fraction the same sections define. That is a
different derivation from the encoder's, which computes the exponent in closed
form from the signed field - so agreement between the two is evidence about the
spec reading, not just about arithmetic.

Quantisation (dB -> nearest word) is likewise done here by exhaustive search
over all 256 codes, against the encoder's frexp-based closed form.

Run from the repo root:
    python tools/references/drc_ref.py            # self-check, print a summary
    python tools/references/drc_ref.py --emit     # write tests/golden/drc_goldens.hpp
"""

import argparse
import math
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "tests" / "golden" / "drc_goldens.hpp"

# Table 7.29, "Arithmetic Shifts" column: left shifts positive, right negative.
# Keyed by the raw 3-bit X field exactly as transmitted (X0 first).
DYNRNG_SHIFT = {
    0b011: 4,   # integer  3, +24.08 dB, 4 left
    0b010: 3,   # integer  2, +18.06 dB, 3 left
    0b001: 2,   # integer  1, +12.04 dB, 2 left
    0b000: 1,   # integer  0,  +6.02 dB, 1 left
    0b111: 0,   # integer -1,   0    dB, none
    0b110: -1,  # integer -2,  -6.02 dB, 1 right
    0b101: -2,  # integer -3, -12.04 dB, 2 right
    0b100: -3,  # integer -4, -18.06 dB, 3 right
}

# Table 7.30, same column, 4-bit X field.
COMPR_SHIFT = {
    0b0111: 8,   # +48.16 dB
    0b0110: 7,
    0b0101: 6,
    0b0100: 5,
    0b0011: 4,
    0b0010: 3,
    0b0001: 2,
    0b0000: 1,   # +6.02 dB
    0b1111: 0,   # 0 dB
    0b1110: -1,
    0b1101: -2,
    0b1100: -3,
    0b1011: -4,
    0b1010: -5,
    0b1001: -6,
    0b1000: -7,  # -42.14 dB
}


def dynrng_gain(word: int) -> float:
    """Linear gain of an 8-bit dynrng word (section 7.7.1.2)."""
    shift = DYNRNG_SHIFT[word >> 5]
    # "Y is considered to be an unsigned fractional integer, with a leading
    # value of 1, or: 0.1 Y3 Y4 Y5 Y6 Y7" - so 32..63 over 64.
    mantissa = (32 + (word & 0x1F)) / 64.0
    return (2.0**shift) * mantissa


def compr_gain(word: int) -> float:
    """Linear gain of an 8-bit compr word (section 7.7.2.2)."""
    shift = COMPR_SHIFT[word >> 4]
    mantissa = (16 + (word & 0x0F)) / 32.0
    return (2.0**shift) * mantissa


def to_db(gain: float) -> float:
    return 20.0 * math.log10(gain)


def nearest(gain_of, gain_db: float):
    """Exhaustively nearest code in LINEAR gain, or None if two codes tie."""
    target = 10.0 ** (gain_db / 20.0)
    scored = sorted(((abs(gain_of(w) - target), w) for w in range(256)))
    if abs(scored[0][0] - scored[1][0]) < 1e-12 * max(target, 1e-12):
        return None
    return scored[0][1]


def self_check() -> None:
    # Section 7.7.1.2: "The bit code of '0000 0000' indicates 0 dB (unity) gain."
    assert dynrng_gain(0x00) == 1.0, dynrng_gain(0x00)
    # Section 7.7.2.2 gives no unity code, but 0x00 is X=0 (+6.02) with Y=0
    # (-6.02), so it lands on unity too.
    assert compr_gain(0x00) == 1.0, compr_gain(0x00)

    # "gain changes from 24.08 - 0.14 = +23.95 dB, to -18.06 - 6.02 = -24.08 dB"
    assert abs(to_db(dynrng_gain(0x7F)) - 23.95) < 0.01
    assert abs(to_db(dynrng_gain(0x80)) + 24.08) < 0.01
    # "gain changes from 48.16 - 0.28 = +47.89 dB, to -42.14 - 6.02 = -48.16 dB"
    assert abs(to_db(compr_gain(0x7F)) - 47.89) < 0.01
    assert abs(to_db(compr_gain(0x80)) + 48.16) < 0.01

    # Y alone spans -6.02 dB to -0.14 dB for dynrng (five bits) and -6.02 to
    # -0.28 dB for compr (four bits) - the sections state both.
    assert abs(to_db(dynrng_gain(0xE0)) + 6.02) < 0.01   # X=-1, Y=0
    assert abs(to_db(dynrng_gain(0xFF)) + 0.14) < 0.01   # X=-1, Y=31
    assert abs(to_db(compr_gain(0xF0)) + 6.02) < 0.01    # X=-1, Y=0
    assert abs(to_db(compr_gain(0xFF)) + 0.28) < 0.01    # X=-1, Y=15

    # Both maps must be injective, or "nearest code" would be ill-defined.
    assert len({dynrng_gain(w) for w in range(256)}) == 256
    assert len({compr_gain(w) for w in range(256)}) == 256

    # Every code must round-trip through the quantiser.
    for w in range(256):
        assert nearest(dynrng_gain, to_db(dynrng_gain(w))) == w
        assert nearest(compr_gain, to_db(compr_gain(w))) == w

    # compr's range is twice dynrng's at half the resolution (section 7.7.2.1).
    dyn = sorted(to_db(dynrng_gain(w)) for w in range(256))
    cmp_ = sorted(to_db(compr_gain(w)) for w in range(256))
    assert abs((cmp_[-1] - cmp_[0]) - 2 * (dyn[-1] - dyn[0])) < 0.5


def sweep():
    """Unambiguous quantiser cases over the union of both formats' ranges."""
    cases = []
    step = 0.31  # deliberately not a round fraction of either step size
    value = -50.0
    while value <= 50.0:
        d = nearest(dynrng_gain, value)
        c = nearest(compr_gain, value)
        if d is not None and c is not None:
            cases.append((value, d, c))
        value += step
    return cases


def emit() -> None:
    cases = sweep()
    lines = [
        "// GENERATED by tools/references/drc_ref.py - do not edit by hand. An independent",
        "// Python transcription of Table 7.29 (dynrng) and Table 7.30 (compr) as",
        "// arithmetic-shift lookups, plus exhaustive nearest-code quantisation.",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace ac3::golden {",
        "",
        "// Linear gain of every dynrng word (A/52 section 7.7.1.2).",
        "inline constexpr std::array<double, 256> kDynrngGain = {{",
    ]

    def rows(values, per_line=4):
        out = []
        for i in range(0, len(values), per_line):
            chunk = ", ".join(f"{v!r}" for v in values[i : i + per_line])
            out.append(f"    {chunk},")
        return out

    lines += rows([dynrng_gain(w) for w in range(256)])
    lines += [
        "}};",
        "",
        "// Linear gain of every compr word (A/52 section 7.7.2.2).",
        "inline constexpr std::array<double, 256> kComprGain = {{",
    ]
    lines += rows([compr_gain(w) for w in range(256)])
    lines += [
        "}};",
        "",
        "// Nearest-code quantisation, by exhaustive search over all 256 codes.",
        "// Ambiguous inputs (two codes equidistant) are excluded at generation",
        "// time, so a mismatch here is a real disagreement and not a tie-break.",
        "struct DrcQuantCase {",
        "    double gain_db;",
        "    std::uint8_t dynrng;",
        "    std::uint8_t compr;",
        "};",
        "",
        f"inline constexpr std::array<DrcQuantCase, {len(cases)}> kDrcQuantCases = {{{{",
    ]
    for db, d, c in cases:
        lines.append(f"    {{{db!r}, 0x{d:02X}, 0x{c:02X}}},")
    lines += ["}};", "", "}  // namespace ac3::golden", ""]

    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT} ({len(cases)} quantiser cases)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emit", action="store_true", help="write the golden header")
    args = parser.parse_args()

    self_check()
    print("self-check passed: Table 7.29 / 7.30 endpoints, injectivity, round-trip")
    print(f"  dynrng: {to_db(dynrng_gain(0x80)):+.2f} .. {to_db(dynrng_gain(0x7F)):+.2f} dB")
    print(f"  compr : {to_db(compr_gain(0x80)):+.2f} .. {to_db(compr_gain(0x7F)):+.2f} dB")
    if args.emit:
        emit()


if __name__ == "__main__":
    main()
