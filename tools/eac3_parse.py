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
    if r.bits(1):                    # compre
        r.bits(8)
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

    if r.bits(1):                    # mixmdate
        if acmod > 2:
            r.bits(2)                # dmixmod
        if (acmod & 1) and acmod > 2:
            r.bits(3); r.bits(3)     # ltrtcmixlev, lorocmixlev
        if acmod & 4:
            r.bits(3); r.bits(3)     # ltrtsurmixlev, lorosurmixlev
        if lfeon:
            if r.bits(1):
                r.bits(5)            # lfemixlevcod
        if strmtyp == 0:
            if r.bits(1):
                r.bits(6)            # pgmscl
            if acmod == 0:
                if r.bits(1):
                    r.bits(6)        # pgmscl2
            if r.bits(1):
                r.bits(6)            # extpgmscl
            mixdef = r.bits(2)
            if mixdef == 1:
                r.bits(1); r.bits(1); r.bits(3)
            elif mixdef == 2:
                r.bits(12)
            elif mixdef == 3:
                mixdeflen = r.bits(5)
                r.bits((mixdeflen + 2) * 8)
            if acmod < 2:
                if r.bits(1):
                    r.bits(6)        # panmean
                    r.bits(8)        # paninfo
                if acmod == 0:
                    if r.bits(1):
                        r.bits(6); r.bits(8)
            if numblkscod == 0:
                r.bits(5)            # blkmixcfginfo[0]
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
    # TS 103 420 §8.3: an object-audio stream puts its only decoder-visible
    # marker here. addbsil counts bytes minus one.
    oba = None
    if r.bits(1):                    # addbsie
        addbsil = r.bits(6)
        first = r.bits(8)
        if first & 1:                # flag_ec3_extension_type_a
            oba = r.bits(8)          # complexity_index_type_a = object count
            r.bits((addbsil + 1 - 2) * 8)
        else:
            r.bits(addbsil * 8)

    log(f'bsi: strmtyp={strmtyp} substreamid={substreamid} frmsiz={frmsiz} '
        f'fscod={fscod} numblkscod={numblkscod} acmod={acmod} lfeon={lfeon} '
        f'dialnorm={dialnorm}  -> {r.pos} bits')

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
            nblkstrtbits = (nblks - 1) * (4 + (frmsiz + 1).bit_length())
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
    skip_fields = []

    for blk in range(nblks):
        start = r.pos
        if blkswe:
            for _ in range(nfchans):
                r.bits(1)
        if dithflage:
            for _ in range(nfchans):
                r.bits(1)
        if r.bits(1):                # dynrnge
            r.bits(8)
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
                # Where the metadata actually lives. Dolby's own DD+ JOC
                # streams put the EMDF container here, not in the aux field:
                # theirs read auxdatae=0 with the container a third of the way
                # into the frame, which is a block skip field and nothing else.
                skip_fields.append((r.pos, skipl * 8))
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

    total_bits = (frmsiz + 1) * 16
    log(f'consumed {r.pos} of {total_bits} bits; {total_bits - r.pos} left for '
        f'aux + errorcheck (needs >= 18)')

    # --- aux data, read from the back of the frame -------------------------
    # A/52 §5.4.4.1 puts user data at the END of auxbits precisely so it can be
    # found without knowing nauxbits, which is only knowable once the audio has
    # been decoded. So this walks backwards from crc2 rather than forwards from
    # where the blocks happened to stop.
    emdf = None
    aux_start = None
    # A skip field is the first place to look: it is where Dolby's own streams
    # carry the container, and unlike the aux field its position is already
    # known exactly from parsing the blocks.
    for at, length in skip_fields:
        emdf = parse_emdf(data, at, length, log)
        if emdf is not None:
            emdf['in_skip'] = True
            aux_start = at
            break
    if emdf is None and total_bits >= 32:
        tail = Reader(data)
        tail.pos = total_bits - 18       # auxdatae, crcrsv, crc2
        if tail.bits(1):                 # auxdatae
            tail.pos = total_bits - 32
            auxdatal = tail.bits(14)
            # §5.4.4.1: "backup auxdatal bits from the beginning of auxdatal",
            # so the user data ends where auxdatal starts - 32 bits from the
            # end of the frame, not 18. auxdatal is inside the backup point.
            aux_start = total_bits - 32 - auxdatal
            log(f'auxdata: {auxdatal} bits starting at bit {aux_start}')
            if aux_start < r.pos:
                log('  AUX OVERLAPS the audio blocks')
            emdf = parse_emdf(data, aux_start, auxdatal, log)

    return r.pos, total_bits, {'strmtyp': strmtyp, 'substreamid': substreamid,
                               'fscod': fscod, 'numblkscod': numblkscod,
                               'acmod': acmod, 'lfeon': lfeon, 'chanmap': chanmap,
                               'oba': oba, 'emdf': emdf, 'aux_start': aux_start}


def variable_bits(r, n):
    """TS 102 366 §H.2.1.2.1."""
    value = 0
    while True:
        value += r.bits(n)
        if not r.bits(1):
            return value
        value <<= n
        value += 1 << n


def parse_emdf(data, start_bit, length_bits, log):
    """TS 102 366 Annex H, over the aux field located by the caller."""
    r = Reader(data)
    r.pos = start_bit
    if r.bits(16) != 0x5838:
        log('  aux data is not an EMDF container')
        return None
    container_length = r.bits(16)
    log(f'EMDF: container {container_length} bytes '
        f'(aux field holds {length_bits // 8})')
    version = r.bits(2)
    if version == 3:
        version += variable_bits(r, 2)
    key_id = r.bits(3)
    if key_id == 7:
        key_id += variable_bits(r, 3)
    payloads = []
    while True:
        payload_id = r.bits(5)
        if payload_id == 0x1F:
            payload_id += variable_bits(r, 5)
        if payload_id == 0:
            break
        # §H.2.1.3, and TS 103 420 Table 56 for what object audio must send.
        cfg = {}
        cfg['smploffste'] = r.bits(1)
        if cfg['smploffste']:
            r.bits(11); r.bits(1)
        cfg['duratione'] = r.bits(1)
        if cfg['duratione']:
            variable_bits(r, 11)
        cfg['groupide'] = r.bits(1)
        if cfg['groupide']:
            cfg['groupid'] = variable_bits(r, 2)
        cfg['codecdatae'] = r.bits(1)
        if cfg['codecdatae']:
            r.bits(8)
        cfg['discard_unknown_payload'] = r.bits(1)
        aligned = None
        if not cfg['discard_unknown_payload']:
            if not cfg['smploffste']:
                aligned = r.bits(1)
                if aligned:
                    cfg['create_duplicate'] = r.bits(1)
                    cfg['remove_duplicate'] = r.bits(1)
            if cfg['smploffste'] or aligned:
                cfg['priority'] = r.bits(5)
                cfg['proc_allowed'] = r.bits(2)
        size = variable_bits(r, 8)
        payload = bytes(r.bits(8) for _ in range(size))
        name = {11: 'OAMD', 14: 'JOC'}.get(payload_id, f'id {payload_id}')
        log(f'  payload {name}: {size} bytes, config {cfg}')
        payloads.append((payload_id, payload))
    prim = r.bits(2)
    sec = r.bits(2)
    r.bits((0, 8, 32, 128)[prim])
    r.bits((0, 8, 32, 128)[sec])
    used = r.pos - start_bit
    # §H.2.2.1.2 measures emdf_container(), which emdf_sync() precedes.
    declared = 32 + container_length * 8
    if used > declared:
        log(f'  EMDF OVERRUN: parsed {used} bits, container declares {declared}')
    elif declared > length_bits:
        log(f'  EMDF does not fit: declares {declared} bits, aux field has {length_bits}')
    else:
        log(f'  EMDF ok: {used} bits parsed, {declared} declared, '
            f'{declared - used} padding')
    return {'payloads': payloads, 'ok': used <= declared <= length_bits}


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
        if info['oba'] is not None:
            print(f'   addbsi: object audio, {info["oba"]} objects')
        if info['emdf'] is not None:
            names = ', '.join({11: 'OAMD', 14: 'JOC'}.get(pid, str(pid))
                              for pid, _ in info['emdf']['payloads']) or 'none'
            state = 'ok' if info['emdf']['ok'] else 'MALFORMED'
            print(f'   EMDF container ({state}): payloads {names}')
            ok = ok and info['emdf']['ok']
            where = 'a block skip field' if info['emdf'].get('in_skip') else 'the aux field'
            print(f'   EMDF carried in {where}')
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
