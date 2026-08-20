"""Turn TS 103 420's JOC Huffman trees into encoder-side code tables.

Annex A.1 does not print the trees. It prints only their names, modes and
types, and ships the contents in the companion archive ts_103420v010201p0.zip
as ts_103420_tables.c - so docs/spec/ts_103420_tables.c IS the normative table,
not a transcription of one.

The tables are decoder-shaped: joc_huff_code[node][bit] holds the next node
index, or a non-positive value meaning "leaf, decoding to -value-1"
(§6.6.3 Pseudocode 4). An encoder needs the inverse, so this walks each tree
and records the bit path to every leaf.

Self-checks before writing, because a silently truncated tree would still
produce a plausible-looking header: every tree must decode to exactly the
values [0, n-1] with no gaps, and its code lengths must satisfy the Kraft
equality sum(2^-len) == 1, which holds only for a complete prefix code.

Run from the repo root:  python tools/generators/gen_joc_tables.py
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TABLES_C = REPO / "docs" / "spec" / "ts_103420_tables.c"
SPEC_TXT = REPO / "docs" / "spec" / "TS103420-2018.txt"
OUT = REPO / "src" / "lib" / "include" / "ac3" / "oba" / "joc_tables.hpp"

# Table 50: joc_num_bands_idx -> joc_num_bands. Table 54's columns run the
# other way round, widest first, so the two are transposed against each other.
NUM_BANDS = [1, 3, 5, 7, 9, 12, 15, 23]

# Annex A.1's own naming: the code table, the mode it is selected by, and the
# bitstream element it codes.
TABLES = [
    ("joc_huff_code_coarse_generic", "kMtxCoarse",
     "Table A.1 - joc_num_quant_idx 0, whole-matrix mode"),
    ("joc_huff_code_fine_generic", "kMtxFine",
     "Table A.2 - joc_num_quant_idx 1, whole-matrix mode"),
    ("joc_huff_code_coarse_coeff_sparse", "kVecCoarse",
     "Table A.3 - joc_num_quant_idx 0, sparse mode coefficients"),
    ("joc_huff_code_fine_coeff_sparse", "kVecFine",
     "Table A.4 - joc_num_quant_idx 1, sparse mode coefficients"),
    ("joc_huff_code_5ch_pos_index_sparse", "kIdx5ch",
     "Table A.5 - sparse mode channel index, 5-channel downmix"),
    ("joc_huff_code_7ch_pos_index_sparse", "kIdx7ch",
     "Table A.6 - sparse mode channel index, 7-channel downmix"),
]


def parse_trees():
    src = TABLES_C.read_text(encoding="utf-8")
    trees = {}
    for match in re.finditer(r"const int (\w+)\[\]\[2\]\s*=\s*\{(.*?)\};", src, re.S):
        pairs = re.findall(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", match.group(2))
        trees[match.group(1)] = [(int(a), int(b)) for a, b in pairs]
    return trees


def codes_from_tree(name, nodes):
    """{value: (code, bit length)} by walking every path from the root."""
    codes = {}

    def walk(node, bits, length):
        assert length <= 32, f"{name}: path longer than a uint32 codeword"
        for bit in (0, 1):
            nxt = nodes[node][bit]
            path = (bits << 1) | bit
            if nxt <= 0:
                value = -nxt - 1
                assert value not in codes, f"{name}: value {value} reached twice"
                codes[value] = (path, length + 1)
            else:
                walk(nxt, path, length + 1)

    walk(0, 0, 0)

    values = sorted(codes)
    assert values == list(range(len(values))), \
        f"{name}: decodes to {values[:3]}..{values[-3:]}, not a dense range"
    kraft = sum(2.0 ** -codes[v][1] for v in values)
    assert abs(kraft - 1.0) < 1e-12, \
        f"{name}: Kraft sum {kraft}, so the tree is not a complete prefix code"
    return codes


def parse_band_mapping():
    """Table 54, as mapping[joc_num_bands_idx][qmf subband] -> parameter band.

    Rows name either a single subband or an inclusive range, and the eight
    columns are the joc_num_bands values in DESCENDING order (23 first, 1
    last), which is the reverse of joc_num_bands_idx.
    """
    text = SPEC_TXT.read_text(encoding="utf-8")
    start = text.index("Table 54: Mapping of quadrature mirror filter bank")
    end = text.index("6.6", start)
    row_re = re.compile(r"^(\d+)(?:\s*-\s*(\d+))?((?:\s+\d+){8})\s*$")

    mapping = [[None] * 64 for _ in NUM_BANDS]
    seen = set()
    for line in text[start:end].splitlines():
        match = row_re.match(line.strip())
        if not match:
            continue
        first = int(match.group(1))
        last = int(match.group(2)) if match.group(2) else first
        bands = [int(v) for v in match.group(3).split()]
        for subband in range(first, last + 1):
            assert subband not in seen, f"Table 54: subband {subband} listed twice"
            seen.add(subband)
            # Column 0 is joc_num_bands 23, which is joc_num_bands_idx 7.
            for column, band in enumerate(bands):
                mapping[len(NUM_BANDS) - 1 - column][subband] = band

    assert seen == set(range(64)), \
        f"Table 54 covers {len(seen)} subbands, not 64: missing {sorted(set(range(64)) - seen)}"
    for idx, bands in enumerate(NUM_BANDS):
        row = mapping[idx]
        assert set(row) == set(range(bands)), \
            f"joc_num_bands {bands}: mapping reaches {sorted(set(row))}"
        assert all(a <= b for a, b in zip(row, row[1:])), \
            f"joc_num_bands {bands}: mapping is not monotonic in subband"
    return mapping


def main():
    trees = parse_trees()
    mapping = parse_band_mapping()
    missing = [name for name, _, _ in TABLES if name not in trees]
    assert not missing, f"not in {TABLES_C.name}: {missing}"

    out = ['#pragma once', '', '#include <array>', '#include <cstdint>', '',
           '// JOC Huffman code tables - ETSI TS 103 420 Annex A.1.',
           '//',
           '// GENERATED by tools/generators/gen_joc_tables.py from the normative',
           '// ts_103420_tables.c that ships in the standard\'s companion archive.',
           '// Do not edit by hand.',
           '//',
           '// The standard gives decoder trees; these are the inverse, one entry per',
           '// value, holding the codeword in the low `bits` bits, MSB first - which is',
           '// the order §6.6.3 reads them in ("starting with the MSB").',
           '',
           'namespace ac3::joc {', '',
           'struct HuffCode {',
           '    std::uint32_t code;',
           '    std::uint8_t bits;',
           '};', '']

    for name, cxx, citation in TABLES:
        codes = codes_from_tree(name, trees[name])
        longest = max(bits for _, bits in codes.values())
        out.append(f'// {citation}.')
        out.append(f'// {len(codes)} values, longest codeword {longest} bits.')
        out.append(f'inline constexpr std::array<HuffCode, {len(codes)}> {cxx} = {{{{')
        for start in range(0, len(codes), 4):
            row = []
            for value in range(start, min(start + 4, len(codes))):
                code, bits = codes[value]
                row.append(f'{{0x{code:06X}, {bits:2d}}}')
            out.append('    ' + ', '.join(row) + ',')
        out.append('}};')
        out.append('')

    out.append('// Table 50 - joc_num_bands_idx to the number of parameter bands.')
    out.append(f'inline constexpr std::array<int, {len(NUM_BANDS)}> kNumBands = '
               f'{{{{{", ".join(str(n) for n in NUM_BANDS)}}}}};')
    out.append('')
    out.append('// Table 54 - which parameter band each of the 64 QMF subbands belongs')
    out.append('// to, per joc_num_bands_idx. This is sb_to_pb() in §6.6.5.')
    out.append(f'inline constexpr std::array<std::array<std::uint8_t, 64>, '
               f'{len(NUM_BANDS)}> kSubbandToBand = {{{{')
    for idx, row in enumerate(mapping):
        out.append(f'    // joc_num_bands = {NUM_BANDS[idx]}')
        out.append('    {{' + ', '.join(str(v) for v in row) + '}},')
    out.append('}};')
    out.append('')
    out.append('}  // namespace ac3::joc')
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text('\n'.join(out) + '\n', encoding='utf-8')
    print(f'wrote {OUT.relative_to(REPO)} ({len(TABLES)} tables)')


if __name__ == '__main__':
    main()
