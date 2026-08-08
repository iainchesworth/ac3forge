"""Independent E-AC-3 (bsid 16) frame parser, from ATSC A/52:2018 Annex E.

Written to check our own encoder's field placement against a known-good
stream. Point it at an FFmpeg-produced .ec3 and any place this parser
diverges from reality is a place the spec tables were misread - which is
exactly the class of bug a silent test frame cannot expose, because a stray
bit there simply lands in zero-filled aux data and still "decodes".

Usage:  python tools/eac3_parse.py <file.ec3> [frame_index]
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import drc_ref  # noqa: E402  (independent section 7.7 word formats)
from bitalloc_ref import bit_alloc  # noqa: E402  (shares the spec's tables)

BLOCKS = 6
LFE_ENDMANT = 7

# Table E2.10: frmchexpstr / frmcplexpstr code -> the six blocks' strategies,
# as 0=reuse, 1=D15, 2=D25, 3=D45.
_E210 = """D15 R R R R R;D15 R R R R D45;D15 R R R D25 R;D15 R R R D45 D45;
D25 R R D25 R R;D25 R R D25 R D45;D25 R R D45 D25 R;D25 R R D45 D45 D45;
D25 R D15 R R R;D25 R D25 R R D45;D25 R D25 R D25 R;D25 R D25 R D45 D45;
D25 R D45 D25 R R;D25 R D45 D25 R D45;D25 R D45 D45 D25 R;D25 R D45 D45 D45 D45;
D45 D15 R R R R;D45 D15 R R R D45;D45 D25 R R D25 R;D45 D25 R R D45 D45;
D45 D25 R D25 R R;D45 D25 R D25 R D45;D45 D25 R D45 D25 R;D45 D25 R D45 D45 D45;
D45 D45 D15 R R R;D45 D45 D25 R R D45;D45 D45 D25 R D25 R;D45 D45 D25 R D45 D45;
D45 D45 D45 D25 R R;D45 D45 D45 D25 R D45;D45 D45 D45 D45 D25 R;
D45 D45 D45 D45 D45 D45"""
_NAME = {'R': 0, 'D15': 1, 'D25': 2, 'D45': 3}
FRM_EXP_STRATEGY = [[_NAME[t] for t in row.split()]
                    for row in _E210.replace('\n', '').split(';')]
assert len(FRM_EXP_STRATEGY) == 32 and all(len(r) == 6 for r in FRM_EXP_STRATEGY)


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def bits(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v


def fullbw_channels(acmod):
    return (2, 1, 2, 3, 3, 4, 4, 5)[acmod]


def dynrng_db(word):
    return drc_ref.to_db(drc_ref.dynrng_gain(word))


def compr_db(word):
    return drc_ref.to_db(drc_ref.compr_gain(word))


def parse_frame(data, verbose=True):
    r = Reader(data)
    log = (lambda *a: print(*a)) if verbose else (lambda *a: None)

    sync = r.bits(16)
    assert sync == 0x0B77, f'bad syncword {sync:#06x}'

    # --- bsi (Table E1.2) ---
    strmtyp = r.bits(2)
    substreamid = r.bits(3)
    frmsiz = r.bits(11)
    fscod = r.bits(2)
    if fscod == 3:
        r.bits(2)                    # fscod2
        numblkscod = 3
    else:
        numblkscod = r.bits(2)
    acmod = r.bits(3)
    lfeon = r.bits(1)
    bsid = r.bits(5)
    assert bsid == 16, f'not E-AC-3 (bsid {bsid})'
    dialnorm = r.bits(5)
    compr = None
    if r.bits(1):                    # compre
        compr = r.bits(8)
    if acmod == 0:
        r.bits(5)
        if r.bits(1):
            r.bits(8)
    chanmap = None
    if strmtyp == 1:
        if r.bits(1):                # chanmape
            chanmap = r.bits(16)
    nfchans = fullbw_channels(acmod)
    nblks = (1, 2, 3, 6)[numblkscod]

    mix = {}
    if r.bits(1):                    # mixmdate
        if acmod > 2:
            mix['dmixmod'] = r.bits(2)
        if (acmod & 1) and acmod > 2:
            mix['ltrtcmixlev'] = r.bits(3)
            mix['lorocmixlev'] = r.bits(3)
        if acmod & 4:
            mix['ltrtsurmixlev'] = r.bits(3)
            mix['lorosurmixlev'] = r.bits(3)
        if lfeon:
            if r.bits(1):            # lfemixlevcode
                mix['lfemixlevcod'] = r.bits(5)
        # The rest is gated on strmtyp == 0: a dependent substream carries only
        # the downmix/mix-level group above.
        if strmtyp == 0:
            if r.bits(1):
                mix['pgmscl'] = r.bits(6)
            if acmod == 0:
                if r.bits(1):
                    r.bits(6)        # pgmscl2
            if r.bits(1):
                mix['extpgmscl'] = r.bits(6)
            mixdef = r.bits(2)
            if mixdef == 1:
                r.bits(1); r.bits(1); r.bits(3)
            elif mixdef == 2:
                r.bits(12)
            elif mixdef == 3:
                mixdeflen = r.bits(5)
                r.bits((mixdeflen + 2) * 8)
            if acmod < 2:
                if r.bits(1):        # paninfoe
                    r.bits(8)        # panmean
                    r.bits(6)        # paninfo (reserved, E2.3.1.55)
                if acmod == 0:
                    if r.bits(1):
                        r.bits(8); r.bits(6)
            # frmmixcfginfoe gates the whole per-block mixing configuration.
            # Reading the blkmixcfginfoe loop unconditionally consumes five bits
            # too many at numblkscod 3, which lands mid-audfrm and shows up as a
            # bogus ahte - the failure that first exposed this.
            if r.bits(1):            # frmmixcfginfoe
                if numblkscod == 0:
                    r.bits(5)        # blkmixcfginfo[0]
                else:
                    for _ in range(nblks):
                        if r.bits(1):
                            r.bits(5)
    if r.bits(1):                    # infomdate
        r.bits(3)                    # bsmod
        r.bits(1); r.bits(1)         # copyrightb, origbs
        if acmod == 2:
            r.bits(2); r.bits(2)     # dsurmod, dheadphonmod
        if acmod >= 6:
            r.bits(2)                # dsurexmod
        if r.bits(1):                # audprodie
            r.bits(5); r.bits(2); r.bits(1)
        if acmod == 0:
            if r.bits(1):
                r.bits(5); r.bits(2); r.bits(1)
        if fscod < 3:
            r.bits(1)                # sourcefscod
    if strmtyp == 0 and numblkscod != 3:
        r.bits(1)                    # convsync
    if strmtyp == 2:
        blkid = 1 if numblkscod == 3 else r.bits(1)
        if blkid:
            r.bits(6)                # frmsizecod
    if r.bits(1):                    # addbsie
        addbsil = r.bits(6)
        r.bits((addbsil + 1) * 8)

    log(f'bsi: strmtyp={strmtyp} substreamid={substreamid} frmsiz={frmsiz} '
        f'fscod={fscod} numblkscod={numblkscod} acmod={acmod} lfeon={lfeon} '
        f'dialnorm={dialnorm}  -> {r.pos} bits')
    if compr is not None:
        # In a DEPENDENT substream compre is the end-of-programme marker
        # (E3.8.5), not a gain, so the word it carries means nothing there.
        role = 'last-dependent marker' if strmtyp == 1 else f'{compr_db(compr):+.2f} dB'
        log(f'  compr: 0x{compr:02X}  {role}')
    if mix:
        log('  mixmdate: ' + '  '.join(f'{k}={v}' for k, v in mix.items()))

    # --- audfrm (Table E1.3) ---
    if numblkscod == 3:
        expstre = r.bits(1)
        ahte = r.bits(1)
    else:
        expstre, ahte = 1, 0
    snroffststr = r.bits(2)
    transproce = r.bits(1)
    blkswe = r.bits(1)
    dithflage = r.bits(1)
    bamode = r.bits(1)
    frmfgaincode = r.bits(1)
    dbaflde = r.bits(1)
    skipflde = r.bits(1)
    spxattene = r.bits(1)

    cplinu = [0] * nblks
    if acmod > 1:
        cplinu[0] = r.bits(1)
        for blk in range(1, nblks):
            if r.bits(1):            # cplstre[blk]
                cplinu[blk] = r.bits(1)
            else:
                cplinu[blk] = cplinu[blk - 1]

    chexpstr = [[0] * nfchans for _ in range(nblks)]
    cplexpstr = [0] * nblks
    if expstre:
        for blk in range(nblks):
            if cplinu[blk]:
                cplexpstr[blk] = r.bits(2)
            for ch in range(nfchans):
                chexpstr[blk][ch] = r.bits(2)
    else:
        # Table E2.10: one 5-bit code expands to all six blocks' strategies.
        ncplblks = sum(cplinu)
        if acmod > 1 and ncplblks > 0:
            code = r.bits(5)
            for blk in range(nblks):
                cplexpstr[blk] = FRM_EXP_STRATEGY[code][blk]
        for ch in range(nfchans):
            code = r.bits(5)
            for blk in range(nblks):
                chexpstr[blk][ch] = FRM_EXP_STRATEGY[code][blk]
    lfeexpstr = [0] * nblks
    if lfeon:
        for blk in range(nblks):
            lfeexpstr[blk] = r.bits(1)
    if strmtyp == 0:
        convexpstre = 1 if numblkscod == 3 else r.bits(1)
        if convexpstre:
            for _ in range(nfchans):
                r.bits(5)            # convexpstr
    if ahte:
        raise SystemExit('AHT not modelled')
    frmcsnroffst = frmfsnroffst = 0
    if snroffststr == 0:
        frmcsnroffst = r.bits(6)
        frmfsnroffst = r.bits(4)
    if transproce:
        for _ in range(nfchans):
            if r.bits(1):
                r.bits(10); r.bits(8)
    if spxattene:
        for _ in range(nfchans):
            if r.bits(1):
                r.bits(5)
    if numblkscod != 0:
        if r.bits(1):                # blkstrtinfoe
            # 2.3.2.27: (numblks - 1) * (4 + ceil(log2(words_per_frame))).
            # bit_length() is floor(log2) + 1, which is one too many at an
            # exact power of two.
            nblkstrtbits = (nblks - 1) * (4 + (frmsiz).bit_length())
            r.bits(nblkstrtbits)
    log(f'audfrm: expstre={expstre} ahte={ahte} snroffststr={snroffststr} '
        f'blkswe={blkswe} dithflage={dithflage} bamode={bamode} '
        f'frmfgaincode={frmfgaincode} dbaflde={dbaflde} skipflde={skipflde} '
        f'cplinu={cplinu}  -> {r.pos} bits')

    # --- audblk x N (Table E1.4) ---
    endmant = [0] * nfchans
    exps = [None] * nfchans
    lfeexps = None
    codes = dict(sdcycod=2, fdcycod=1, sgaincod=1, dbpbcod=2, floorcod=7)
    fgaincod = [4] * (nfchans + 1)
    csnroffst = 0
    fsnroffst = [0] * (nfchans + 1)
    spxinu = 0
    # Section 7.7.1.2: an absent word inherits the previous BLOCK's, and block 0
    # without one is unity - never the previous frame's value.
    dynrng = 0x00
    dynrng_blocks = []

    for blk in range(nblks):
        start = r.pos
        if blkswe:
            for _ in range(nfchans):
                r.bits(1)
        if dithflage:
            for _ in range(nfchans):
                r.bits(1)
        if r.bits(1):                # dynrnge
            dynrng = r.bits(8)
        dynrng_blocks.append(dynrng)
        if acmod == 0:
            if r.bits(1):
                r.bits(8)

        spxstre = 1 if blk == 0 else r.bits(1)
        if spxstre:
            spxinu = r.bits(1)
            if spxinu:
                raise SystemExit('spectral extension not modelled')
        if cplinu[blk]:
            raise SystemExit('coupling not modelled')

        if acmod == 2:
            rematstr = 1 if blk == 0 else r.bits(1)
            if rematstr:
                for _ in range(4):   # nrematbd == 4 with no coupling or spx
                    r.bits(1)

        for ch in range(nfchans):
            if chexpstr[blk][ch] != 0:
                chbwcod = r.bits(6)
                endmant[ch] = ((chbwcod + 12) * 3) + 37
        for ch in range(nfchans):
            if chexpstr[blk][ch] != 0:
                grpsize = (0, 1, 2, 4)[chexpstr[blk][ch]]
                ngrps = {1: (endmant[ch] - 1) // 3,
                         2: (endmant[ch] - 1 + 3) // 6,
                         4: (endmant[ch] - 1 + 9) // 12}[grpsize]
                absexp = r.bits(4)
                groups = [r.bits(7) for _ in range(ngrps)]
                r.bits(2)            # gainrng
                exps[ch] = expand(absexp, groups, grpsize, endmant[ch])
        if lfeon and lfeexpstr[blk] != 0:
            absexp = r.bits(4)
            groups = [r.bits(7) for _ in range(2)]
            lfeexps = expand(absexp, groups, 1, LFE_ENDMANT)

        if bamode:
            if r.bits(1):            # baie
                codes = dict(sdcycod=r.bits(2), fdcycod=r.bits(2), sgaincod=r.bits(2),
                             dbpbcod=r.bits(2), floorcod=r.bits(3))
        if snroffststr == 0:
            csnroffst = frmcsnroffst
            fsnroffst = [frmfsnroffst] * (nfchans + 1)
        else:
            snroffste = 1 if blk == 0 else r.bits(1)
            if snroffste:
                csnroffst = r.bits(6)
                if snroffststr == 1:
                    blkfsnroffst = r.bits(4)
                    fsnroffst = [blkfsnroffst] * (nfchans + 1)
                elif snroffststr == 2:
                    fsnroffst = [r.bits(4) for _ in range(nfchans)] + \
                                ([r.bits(4)] if lfeon else [0])
        fgaincode = r.bits(1) if frmfgaincode else 0
        if fgaincode:
            fgaincod = [r.bits(3) for _ in range(nfchans)] + \
                       ([r.bits(3)] if lfeon else [4])
        if strmtyp == 0:
            if r.bits(1):            # convsnroffste
                r.bits(10)
        if dbaflde:
            if r.bits(1):            # deltbaie
                raise SystemExit('delta bit allocation not modelled')
        if skipflde:
            if r.bits(1):            # skiple
                skipl = r.bits(9)
                r.bits(skipl * 8)

        side = r.pos - start
        # Mantissas, using the same allocation the decoder computes.
        total_mant_bits = 0
        counts = {1: 0, 2: 0, 4: 0}
        for ch in list(range(nfchans)) + ([nfchans] if lfeon else []):
            e = exps[ch] if ch < nfchans else lfeexps
            end = endmant[ch] if ch < nfchans else LFE_ENDMANT
            bap = bit_alloc(e[:end], fscod, codes['sdcycod'], codes['fdcycod'],
                            codes['sgaincod'], codes['dbpbcod'], codes['floorcod'],
                            fgaincod[ch], csnroffst, fsnroffst[ch])
            for b in bap:
                if b in counts:
                    counts[b] += 1
                elif b:
                    total_mant_bits += (0, 0, 0, 3, 0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16)[b]
        total_mant_bits += 5 * ((counts[1] + 2) // 3)
        total_mant_bits += 7 * ((counts[2] + 2) // 3)
        total_mant_bits += 7 * ((counts[4] + 1) // 2)
        log(f'  blk {blk}: side {side} bits, csnroffst={csnroffst} '
            f'fsnroffst={fsnroffst[0]}, mantissas {total_mant_bits} bits '
            f'-> ends at {r.pos + total_mant_bits}')
        if r.pos + total_mant_bits > len(r.data) * 8:
            raise SystemExit(
                f'  OVERRUN: block {blk} wants {total_mant_bits} mantissa bits but only '
                f'{len(r.data) * 8 - r.pos} remain in the frame. The encoder and an '
                f'independent allocation disagree.')
        r.bits(total_mant_bits)

    if any(w != 0x00 for w in dynrng_blocks):
        log('  dynrng: ' + ' '.join(f'{dynrng_db(w):+.2f}' for w in dynrng_blocks) + ' dB')

    total_bits = (frmsiz + 1) * 16
    log(f'consumed {r.pos} of {total_bits} bits; {total_bits - r.pos} left for '
        f'aux + errorcheck (needs >= 18)')
    return r.pos, total_bits, {'strmtyp': strmtyp, 'substreamid': substreamid,
                               'fscod': fscod, 'numblkscod': numblkscod,
                               'acmod': acmod, 'lfeon': lfeon, 'chanmap': chanmap,
                               'dialnorm': dialnorm, 'compr': compr,
                               'mixmdate': mix or None, 'dynrng': dynrng_blocks}


def expand(absexp, groups, grpsize, end):
    out = [absexp]
    prev = absexp
    for g in groups:
        for d in (g // 25, (g % 25) // 5, (g % 25) % 5):
            prev += d - 2
            out.extend([prev] * grpsize)
    return out[:end] + [24] * max(0, end - len(out))


# Table E2.5 locations that name a PAIR of channels rather than one, so a
# map's population count is not its channel count.
CHANMAP_PAIRS = 0x0400 | 0x0200 | 0x0040 | 0x0020 | 0x0010 | 0x0004


def chanmap_channels(m):
    return bin(m).count('1') + bin(m & CHANMAP_PAIRS).count('1')


def split_access_units(data):
    """Group syncframes into access units. A new one starts at each strmtyp 0."""
    units, offset = [], 0
    while offset + 4 <= len(data):
        assert data[offset] == 0x0B and data[offset + 1] == 0x77, 'lost sync'
        # Byte 2 is strmtyp(2) | substreamid(3) | the top 3 bits of frmsiz.
        strmtyp = data[offset + 2] >> 6
        substreamid = (data[offset + 2] >> 3) & 0x07
        frmsiz = ((data[offset + 2] & 0x07) << 8) | data[offset + 3]
        size = (frmsiz + 1) * 2
        if strmtyp == 0 or not units:
            units.append([])
        units[-1].append((offset, size, strmtyp, substreamid))
        offset += size
    return units


def main():
    path = Path(sys.argv[1])
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    data = path.read_bytes()
    units = split_access_units(data)
    if want >= len(units):
        raise SystemExit(f'only {len(units)} access units in {path}')
    unit = units[want]
    print(f'access unit {want}: {len(unit)} substream(s), '
          f'{sum(s for _, s, _, _ in unit)} bytes')

    ok = True
    parent = None
    for offset, size, strmtyp, substreamid in unit:
        kind = ('independent', 'dependent', 'independent (AC-3 convertible)',
                'reserved')[strmtyp]
        print(f'-- {kind} substreamid={substreamid} at byte {offset}, {size} bytes')
        used, total, info = parse_frame(data[offset:offset + size])
        slack = total - used
        if slack < 18:
            print(f'   OVERRUN by {18 - slack} bits')
            ok = False
        # Cross-substream invariants. A dependent that disagrees with its
        # parent about the sample rate or the block count silently desynchronises
        # the program rather than failing to parse.
        if strmtyp == 0:
            parent = info
        elif parent is not None:
            for field in ('fscod', 'numblkscod'):
                if info[field] != parent[field]:
                    print(f'   MISMATCH {field}: {info[field]} vs parent {parent[field]}')
                    ok = False
        # E2.3.1.8: the locations a chanmap names must equal the channels the
        # substream's acmod and lfeon actually code.
        if info['chanmap'] is not None:
            coded = fullbw_channels(info['acmod']) + info['lfeon']
            named = chanmap_channels(info['chanmap'])
            state = 'ok' if named == coded else f'MISMATCH: codes {coded}'
            print(f'   chanmap 0x{info["chanmap"]:04X} names {named} channels ({state})')
            ok = ok and named == coded
    print('VERDICT:', 'consistent' if ok else 'INCONSISTENT')


if __name__ == '__main__':
    main()
