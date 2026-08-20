"""Extract the A/52 bit-allocation tables (7.6-7.16) from the spec text.

Emits src/forge/include/ac3/core/bitalloc_tables.hpp (constexpr arrays,
citations included)
after self-verifying the parse: element counts, masktab identity against the
banding structure, latab monotonicity, and spot values. Import parse_tables()
from other tools (bitalloc_ref.py) so every consumer shares one source of
truth: the standard's own text.

Run from the repo root:  python tools/generators/gen_bitalloc_tables.py
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
SPEC_TXT = REPO / "docs" / "spec" / "A52-2018.txt"
OUT = REPO / "src" / "lib" / "include" / "ac3" / "core" / "bitalloc_tables.hpp"


def _lines():
    return SPEC_TXT.read_text(encoding="utf-8").splitlines()


def _find(lines, needle):
    # Take the LAST occurrence: each table title appears first in the table
    # of contents and then at the table body itself.
    found = -1
    for i, line in enumerate(lines):
        if needle in line:
            found = i
    if found < 0:
        raise SystemExit(f"marker not found: {needle}")
    return found


def _parse_pairs(lines, start, count, hex_vals=True):
    """Rows of 'index value' (possibly two column-pairs per line)."""
    values = {}
    pattern = re.compile(r"(\d+)\s+(0x[0-9a-fA-F]+|\d+)")
    for line in lines[start:start + count + 40]:
        for index, value in pattern.findall(line):
            index = int(index)
            if index in values or index >= count:
                continue
            values[index] = int(value, 16) if value.startswith("0x") else int(value)
        if len(values) == count:
            break
    if len(values) != count or sorted(values) != list(range(count)):
        raise SystemExit(f"pair parse failed at line {start}: got {len(values)}/{count}")
    return [values[i] for i in range(count)]


def _parse_grid(lines, start, count, base=10):
    """'A=n v v v ...' grid rows (Table 7.13 / 7.14 style)."""
    values = []
    for line in lines[start:start + 40]:
        stripped = line.strip()
        if stripped.startswith("A="):
            row = stripped.split()[1:]
            values.extend(int(v, 0) for v in row)
        if len(values) >= count:
            break
    if len(values) < count:
        raise SystemExit(f"grid parse failed at line {start}: got {len(values)}/{count}")
    return values[:count]


def parse_tables():
    lines = _lines()

    slowdec = _parse_pairs(lines, _find(lines, "Table 7.6 Slow Decay"), 4)
    fastdec = _parse_pairs(lines, _find(lines, "Table 7.7 Fast Decay"), 4)
    slowgain = _parse_pairs(lines, _find(lines, "Table 7.8 Slow Gain"), 4)
    dbpbtab = _parse_pairs(lines, _find(lines, "Table 7.9 dB/Bit"), 4)
    floortab = _parse_pairs(lines, _find(lines, "Table 7.10 Floor Table"), 8)
    fastgain = _parse_pairs(lines, _find(lines, "Table 7.11 Fast Gain"), 8)

    # Table 7.12: 'band bndtab bndsz  band bndtab bndsz' double columns.
    bnd_start = _find(lines, "Table 7.12 Banding Structure")
    bndtab, bndsz = {}, {}
    row = re.compile(r"(\d+)\s+(\d+)\s+(\d+)")
    for line in lines[bnd_start:bnd_start + 40]:
        for band, tab, size in row.findall(line):
            band = int(band)
            if band < 50 and band not in bndtab:
                bndtab[band] = int(tab)
                bndsz[band] = int(size)
        if len(bndtab) == 50:
            break
    if len(bndtab) != 50:
        raise SystemExit(f"Table 7.12 parse failed: {len(bndtab)}/50")
    bndtab = [bndtab[i] for i in range(50)]
    bndsz = [bndsz[i] for i in range(50)]

    masktab = _parse_grid(lines, _find(lines, "Table 7.13 Bin Number to Band"), 256)
    latab = _parse_grid(lines, _find(lines, "Table 7.14 Log-Addition"), 256)

    # Table 7.15: 'band h0 h1 h2  band h0 h1 h2' double columns, 50 bands.
    hth_start = _find(lines, "Table 7.15 Hearing Threshold")
    hth = {}
    row = re.compile(r"(\d+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)")
    for line in lines[hth_start:hth_start + 45]:
        for band, h0, h1, h2 in row.findall(line):
            band = int(band)
            if band < 50 and band not in hth:
                hth[band] = (int(h0, 16), int(h1, 16), int(h2, 16))
        if len(hth) == 50:
            break
    if len(hth) != 50:
        raise SystemExit(f"Table 7.15 parse failed: {len(hth)}/50")
    hth = [hth[i] for i in range(50)]

    baptab = _parse_pairs(lines, _find(lines, "Table 7.16 Bit Allocation Pointer"), 64,
                          hex_vals=False)

    # --- Self-verification ---
    # Banding structure is contiguous and spans exactly the 253 mantissa bins.
    for band in range(49):
        assert bndtab[band] + bndsz[band] == bndtab[band + 1], f"band {band} not contiguous"
    assert bndtab[49] + bndsz[49] == 253
    # Table 7.13 must equal the mapping derived from Table 7.12 (bins 0-252;
    # the printed table pads three trailing zeros to reach 256).
    derived = [0] * 256
    for band in range(50):
        for bin_ in range(bndtab[band], bndtab[band] + bndsz[band]):
            derived[bin_] = band
    assert masktab == derived, "Table 7.13 disagrees with Table 7.12"
    # latab decreases monotonically 0x40 -> 0; baptab is monotone 0 -> 15.
    assert latab[0] == 0x40 and latab[255] == 0
    assert all(a >= b for a, b in zip(latab, latab[1:]))
    assert baptab[0] == 0 and baptab[63] == 15
    assert all(a <= b for a, b in zip(baptab, baptab[1:]))
    # Spot values transcribed independently while reading the spec.
    assert slowdec == [0x0F, 0x11, 0x13, 0x15]
    assert floortab[7] == 0xF800
    assert fastgain == [0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400]
    assert hth[0] == (0x04D0, 0x04F0, 0x0580)
    assert hth[49] == (0x0840, 0x0840, 0x04E0)

    return {
        "slowdec": slowdec, "fastdec": fastdec, "slowgain": slowgain,
        "dbpbtab": dbpbtab, "floortab": floortab, "fastgain": fastgain,
        "bndtab": bndtab, "bndsz": bndsz, "masktab": masktab, "latab": latab,
        "hth": hth, "baptab": baptab,
    }


def _fmt(name, values, ctype, per_line=10, hexfmt=False):
    body = []
    fmt = (lambda v: f"0x{v:04x}") if hexfmt else str
    for i in range(0, len(values), per_line):
        body.append("    " + ", ".join(fmt(v) for v in values[i:i + per_line]) + ",")
    return (f"inline constexpr std::array<{ctype}, {len(values)}> {name} = {{\n"
            + "\n".join(body) + "\n}};\n".replace("}};", "};"))


def main():
    t = parse_tables()
    parts = [
        "// GENERATED by tools/generators/gen_bitalloc_tables.py from ATSC A/52:2018 - do not",
        "// edit by hand. Tables 7.6-7.16 of the standard, parsed verbatim from the",
        "// spec text and self-verified (banding contiguity, Table 7.13 == mapping",
        "// derived from Table 7.12, monotonicity, spot values).",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace ac3::tables {",
        "",
        "// Table 7.6 / 7.7: slow & fast decay.",
        _fmt("kSlowDec", t["slowdec"], "std::int32_t"),
        _fmt("kFastDec", t["fastdec"], "std::int32_t"),
        "// Table 7.8 / 7.9 / 7.10 / 7.11: slow gain, dB/bit knee, floor, fast gain.",
        _fmt("kSlowGain", t["slowgain"], "std::int32_t", hexfmt=True),
        _fmt("kDbPerBit", t["dbpbtab"], "std::int32_t", hexfmt=True),
        _fmt("kFloor", t["floortab"], "std::int32_t", hexfmt=True),
        _fmt("kFastGain", t["fastgain"], "std::int32_t", hexfmt=True),
        "// Table 7.12: banding structure (50 ~1/6-octave bands over 253 bins).",
        _fmt("kBandStart", t["bndtab"], "std::int32_t"),
        _fmt("kBandSize", t["bndsz"], "std::int32_t"),
        "// Table 7.13: bin -> band (3 trailing pad zeros as printed).",
        _fmt("kMaskTab", t["masktab"], "std::uint8_t", per_line=16),
        "// Table 7.14: log-addition table.",
        _fmt("kLogAdd", t["latab"], "std::int32_t", per_line=10, hexfmt=True),
        "// Table 7.15: hearing threshold, indexed [fscod][band].",
    ]
    for fscod in range(3):
        parts.append(_fmt(f"kHearingThreshold{fscod}", [h[fscod] for h in t["hth"]],
                          "std::int32_t", hexfmt=True))
    parts += [
        "inline constexpr std::array<const std::array<std::int32_t, 50>*, 3> kHearingThreshold = {",
        "    &kHearingThreshold0, &kHearingThreshold1, &kHearingThreshold2,",
        "};",
        "",
        "// Table 7.16: address -> bit allocation pointer.",
        _fmt("kBapTab", t["baptab"], "std::uint8_t", per_line=16),
        "}  // namespace ac3::tables",
    ]
    OUT.write_text("\n".join(parts) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes); all self-checks passed")


if __name__ == "__main__":
    main()
