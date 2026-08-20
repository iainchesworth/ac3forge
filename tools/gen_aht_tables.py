"""Extract the Annex E adaptive hybrid transform tables from the spec text.

Emits src/forge/include/ac3/core/aht_tables.hpp: the high-efficiency bit
allocation pointers (Table E3.1) and the seven vector quantisation codebooks
(Tables E4.1-E4.7, 956 six-dimensional vectors between them).

Transcribing 5736 hex values by hand is not a thing a person does correctly,
so they are parsed out of the standard's own text and then self-checked:
element counts against the index width each hebap implies, index columns
against 0..N-1 in order, and the codebooks against the one structural
property they must have - no two entries the same, or the encoder's nearest
-neighbour search would have a tie it cannot resolve the same way twice.

Run from the repo root:  python tools/gen_aht_tables.py
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SPEC_TXT = REPO / "docs" / "spec" / "A52-2018.txt"
OUT = REPO / "src" / "lib" / "include" / "ac3" / "core" / "aht_tables.hpp"

# Table E3.2: bits in the VQ index for hebap 1-7, which fixes each
# codebook's size.
VQ_INDEX_BITS = {1: 2, 2: 3, 3: 4, 4: 5, 5: 7, 6: 8, 7: 9}

ROW = re.compile(r"^\s*(\d+)\s+((?:0x[0-9a-fA-F]{4}\s+){5}0x[0-9a-fA-F]{4})\s*$")
PAIR = re.compile(r"(\d+)\s+(\d+)")
REMAP = re.compile(
    r"^\s*(\d+)?\s*x\s*[><≥]=?\s*0\s+"
    r"((?:0x[0-9a-fA-F]{4}|N/A)(?:\s+(?:0x[0-9a-fA-F]{4}|N/A)){5})")

# Table E3.2 again, this time as the scalar mantissa width per hebap.
MANTISSA_BITS = {8: 3, 9: 4, 10: 5, 11: 6, 12: 7, 13: 8,
                 14: 9, 15: 10, 16: 11, 17: 12, 18: 14, 19: 16}


def gaq_reconstruction(hebap, gain, positive):
    """(a, b) of Table E3.6's y = x + a*x + b, from the quantizer's shape.

    The table is a restatement of three uniform quantizers, and deriving them
    rather than transcribing them is what makes the encoder's own arithmetic
    checkable: every constant below has to come back out.

      Gk = 1  symmetric, 2^m - 1 levels, step 2/(2^m - 1), m-bit codeword
      Gk = 2  dead zone at 1/2, (m-1)-bit codeword, step 1/(2^(m-1) - 1)
      Gk = 4  dead zone at 1/4, m-bit codeword,     step 3/(2^(m+1) - 2)

    x is the codeword read as a fractional two's complement value, so scaling
    a step into the (1 + a) slope means multiplying by 2^(codeword bits - 1).
    """
    m = MANTISSA_BITS[hebap]
    if gain == 1:
        return 1.0 / ((1 << m) - 1), 0.0
    dead = 1.0 / gain
    if gain == 2:
        step = 1.0 / ((1 << (m - 1)) - 1)
        half = 1 << (m - 2)          # codeword is m-1 bits
    else:
        step = 3.0 / ((1 << (m + 1)) - 2)
        half = 1 << (m - 1)          # codeword is m bits
    slope = half * step
    return slope - 1.0, (dead if positive else step - dead)


def verify_gaq_remap(text):
    """Check the derived quantizers against every entry of Table E3.6."""
    start = find_last(text, "Table E3.6 Large Mantissa Inverse Quantization")
    hebap = None
    checked = 0
    for line in text[start:start + 40]:
        match = REMAP.match(line)
        if not match:
            continue
        if match.group(1):
            hebap = int(match.group(1))
        positive = "≥" in line or ">" in line.split("x", 1)[1][:3]
        values = match.group(2).split()
        for index, gain in enumerate((1, 2, 4)):
            a_hex, b_hex = values[2 * index], values[2 * index + 1]
            if a_hex == "N/A":
                # Gains above 1 are only defined where GAQ reaches (hebap 16).
                if hebap <= 16:
                    raise SystemExit(f"E3.6: hebap {hebap} gain {gain} unexpectedly N/A")
                continue
            want = [(int(h, 16) - 0x10000 if int(h, 16) >= 0x8000 else int(h, 16)) / 32768.0
                    for h in (a_hex, b_hex)]
            got = gaq_reconstruction(hebap, gain, positive)
            # The table is 16-bit rounded, so one ulp of that is the tolerance.
            for name, w, g in zip("ab", want, got):
                if abs(w - g) > 1.0 / 32768.0:
                    raise SystemExit(
                        f"E3.6 mismatch hebap {hebap} Gk={gain} "
                        f"{'x>=0' if positive else 'x<0'} {name}: "
                        f"table {w:+.6f} derived {g:+.6f}")
            checked += 2
    if checked < 100:
        raise SystemExit(f"E3.6: only {checked} constants checked; the parse is wrong")
    return checked


def lines():
    return SPEC_TXT.read_text(encoding="utf-8").splitlines()


def find_last(text, needle):
    found = -1
    for i, line in enumerate(text):
        if needle in line:
            found = i
    if found < 0:
        raise SystemExit(f"marker not found: {needle}")
    return found


def parse_hebaptab(text):
    """Table E3.1, laid out as two address/value column pairs per line."""
    start = find_last(text, "Table E3.1 High Efficiency Bit Allocation Pointers")
    values = {}
    for line in text[start:start + 120]:
        if "hebaptab" in line or "Address" in line:
            continue
        for address, value in PAIR.findall(line):
            address, value = int(address), int(value)
            if address < 64 and address not in values:
                values[address] = value
        if len(values) == 64:
            break
    if len(values) != 64:
        raise SystemExit(f"hebaptab: got {len(values)} of 64 entries")
    table = [values[i] for i in range(64)]
    # The table is monotonically non-decreasing by construction: a louder bin
    # relative to its mask can never be allocated a coarser quantiser.
    if any(b < a for a, b in zip(table, table[1:])):
        raise SystemExit("hebaptab is not monotonic; the parse is wrong")
    if table[0] != 0 or table[-1] != 19:
        raise SystemExit(f"hebaptab endpoints wrong: {table[0]}, {table[-1]}")
    return table


def parse_vq(text, hebap):
    start = find_last(text, f"Table E4.{hebap} VQ Table for hebap {hebap}")
    want = 1 << VQ_INDEX_BITS[hebap]
    vectors = []
    for line in text[start:]:
        match = ROW.match(line)
        if not match:
            continue
        index = int(match.group(1))
        if index != len(vectors):
            # A page break repeats nothing, so an index that does not continue
            # the sequence means the parse has wandered into another table.
            raise SystemExit(
                f"E4.{hebap}: row index {index} where {len(vectors)} expected")
        row = [int(v, 16) for v in match.group(2).split()]
        vectors.append([v - 0x10000 if v >= 0x8000 else v for v in row])
        if len(vectors) == want:
            break
    if len(vectors) != want:
        raise SystemExit(f"E4.{hebap}: got {len(vectors)} of {want} vectors")
    if len(set(map(tuple, vectors))) != want:
        raise SystemExit(f"E4.{hebap}: duplicate codebook entries")
    return vectors


def main():
    text = lines()
    hebaptab = parse_hebaptab(text)
    books = {hebap: parse_vq(text, hebap) for hebap in VQ_INDEX_BITS}
    remaps = verify_gaq_remap(text)

    out = ['#pragma once', '', '#include <array>', '#include <cstdint>', '#include <span>', '']
    out += [
        '// Adaptive hybrid transform tables, ATSC A/52:2018 Annex E.',
        '// GENERATED by tools/gen_aht_tables.py from the standard\'s own text.',
        '// Do not edit by hand; re-run the generator instead.',
        '',
        'namespace ac3::tables {',
        '',
        '// Table E3.1. Addressed exactly as baptab is - (psd - mask) >> 5,',
        '// clamped to 0..63 - but with 20 outcomes instead of 16, which is the',
        '// finer granularity AHT trades its restrictions for.',
        'inline constexpr std::array<std::uint8_t, 64> kHeBapTab = {',
    ]
    for i in range(0, 64, 16):
        out.append('    ' + ', '.join(str(v) for v in hebaptab[i:i + 16]) + ',')
    out += ['};', '']
    out += [
        '// Tables E4.1-E4.7: the vector quantisation codebooks for hebap 1-7.',
        '// Six mantissas from one spectral bin - the same bin across all six',
        '// blocks of the frame - are coded together as one index into these,',
        '// which is where AHT gets its coding gain on stationary material.',
        '// Values are 16-bit signed fractions of full scale.',
        '',
    ]
    for hebap, vectors in sorted(books.items()):
        bits = VQ_INDEX_BITS[hebap]
        out.append(f'// hebap {hebap}: {bits}-bit index, {len(vectors)} vectors.')
        out.append(f'inline constexpr std::array<std::array<std::int16_t, 6>, {len(vectors)}> '
                   f'kAhtVq{hebap} = {{{{')
        for row in vectors:
            out.append('    {{' + ', '.join(str(v) for v in row) + '}},')
        out += ['}};', '']
    out += [
        '// Bits in the VQ index, by hebap. Zero outside the VQ range.',
        'inline constexpr std::array<int, 8> kAhtVqIndexBits = {0, 2, 3, 4, 5, 7, 8, 9};',
        '',
        '// The codebook for a hebap in 1..7, as a flat view.',
        '[[nodiscard]] constexpr std::span<const std::array<std::int16_t, 6>> aht_vq_table(',
        '    int hebap) {',
        '    switch (hebap) {',
    ]
    for hebap in sorted(books):
        out.append(f'        case {hebap}: return kAhtVq{hebap};')
    out += [
        '        default: return {};',
        '    }',
        '}',
        '',
        '}  // namespace ac3::tables',
        '',
    ]
    OUT.write_text('\n'.join(out), encoding='utf-8')
    total = sum(len(v) for v in books.values())
    print(f'wrote {OUT.relative_to(REPO)}: hebaptab (64) + {total} VQ vectors')
    print(f'verified {remaps} Table E3.6 constants against the derived quantizers')


if __name__ == '__main__':
    main()
