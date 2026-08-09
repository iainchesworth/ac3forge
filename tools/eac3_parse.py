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
from bitalloc_ref import aht_bin_bits, bit_alloc  # noqa: E402  (the spec's tables)

BLOCKS = 6
LFE_ENDMANT = 7

# Table E2.12: the coupling banding structure a decoder assumes when
# cplbndstrce is 0 in the first coupled block. Note this is NOT "one band per
# sub-band" - that is the AC-3 reading, and assuming it here computes a
# different ncplbnd from the decoder.
DEF_CPL_BNDSTRC = [0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1]

# Table E2.11. Unlike coupling's, this one is indexed by the ABSOLUTE spectral
# extension sub-band number - the transmitted loop runs from
# spx_begin_subbnd + 1, not from 1.
DEF_SPX_BNDSTRC = [0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1]


def spx_band_start(subbnd):
    return 25 + 12 * subbnd


def group_bands(first_bin, subbands, bins_per_subband, structure):
    """Merge sub-bands into bands; returns [(start_bin, size), ...]."""
    bands = [[first_bin, bins_per_subband]]
    for sbnd in range(1, subbands):
        if structure[sbnd]:
            bands[-1][1] += bins_per_subband
        else:
            bands.append([first_bin + sbnd * bins_per_subband, bins_per_subband])
    return [tuple(b) for b in bands]

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
        # Sec E2.3.1.3: fscod2 replaces numblkscod outright - a reduced-rate
        # frame is implicitly always six blocks. Sec E2.3.1.4: the reduced
        # rate reuses the SAME bit-allocation tables as its fscod == fscod2
        # double-rate parent, so fscod2's own value already is the family
        # index bit_alloc() below needs.
        fscod_family = r.bits(2)      # fscod2
        numblkscod = 3
    else:
        fscod_family = fscod
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
                r.bits(1); r.bits(1); r.bits(3)  # premixcmpsel, drcsrc, premixcmpscl
            elif mixdef == 2:
                r.bits(12)           # mixdata
            elif mixdef == 3:
                # §E2.3.1.22: mixdeflen 0-31 means a mixdata field of 2-33
                # BYTES, and whatever the sub-structures below do not use is
                # mixdatafill. So the length is authoritative and the parse of
                # the contents only has to be good enough to not overrun it -
                # skipping to the end is both simpler and more robust than
                # trusting a field-by-field walk of a rarely used element.
                mixdeflen = r.bits(5)
                mixdata_start = r.pos
                if r.bits(1):        # mixdata2e
                    r.bits(1); r.bits(1); r.bits(3)
                    for _ in range(6):   # L, C, R, Ls, Rs and LFE scale factors
                        if r.bits(1):
                            r.bits(4)
                    if r.bits(1):    # dmixscle
                        r.bits(4)
                    if r.bits(1):    # addche
                        for _ in range(2):   # two auxiliary channels
                            if r.bits(1):
                                r.bits(4)
                if r.bits(1):        # mixdata3e
                    r.bits(5)        # spchdat
                    if r.bits(1):    # addspchdate
                        r.bits(5); r.bits(2)      # spchdat1, spchan1att
                        if r.bits(1):             # addspchdat1e
                            r.bits(5); r.bits(3)  # spchdat2, spchan2att
                used = r.pos - mixdata_start
                r.bits(8 * (mixdeflen + 2) - used)   # mixdata remainder + fill
            if acmod < 2:
                if r.bits(1):        # paninfoe
                    r.bits(8)        # panmean
                    r.bits(6)        # paninfo
                if acmod == 0:
                    if r.bits(1):    # paninfo2e
                        r.bits(8); r.bits(6)
            # §E2.3.1.59: the per-block mixing configuration is gated by ONE
            # frame-level bit. Reading the block flags without it costs five
            # bits or more, and everything downstream lands adrift - which is
            # exactly how this went unnoticed until a real encoder's 5.1
            # stream, the first one here to carry mixing metadata at all,
            # failed to parse.
            if r.bits(1):            # frmmixcfginfoe
                if numblkscod == 0:
                    r.bits(5)        # blkmixcfginfo[0]
                else:
                    for _ in range(nblks):
                        if r.bits(1):    # blkmixcfginfoe
                            r.bits(5)    # blkmixcfginfo[blk]
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
        f'fscod={fscod}{" fscod2=" + str(fscod_family) if fscod == 3 else ""} '
        f'numblkscod={numblkscod} acmod={acmod} lfeon={lfeon} '
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
    cplstre = [0] * nblks
    if acmod > 1:
        cplstre[0] = 1               # implied
        cplinu[0] = r.bits(1)
        for blk in range(1, nblks):
            cplstre[blk] = r.bits(1)
            if cplstre[blk]:
                cplinu[blk] = r.bits(1)
            else:
                cplinu[blk] = cplinu[blk - 1]

    ncplblks = sum(cplinu)
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
    cplahtinu = chahtinu = lfeahtinu = 0
    chahtinu = [0] * nfchans
    if ahte:
        # §E2.2.3: each flag exists only where that channel's exponents are
        # transmitted exactly once in the frame, since AHT spans the frame and
        # cannot straddle a change of exponent set.
        ncplregs = sum(1 for blk in range(nblks)
                       if cplstre[blk] or cplexpstr[blk] != 0)
        if ncplblks == nblks and ncplregs == 1:
            cplahtinu = r.bits(1)
        for ch in range(nfchans):
            nchregs = sum(1 for blk in range(nblks) if chexpstr[blk][ch] != 0)
            if nchregs == 1:
                chahtinu[ch] = r.bits(1)
        if lfeon:
            nlferegs = sum(1 for blk in range(nblks) if lfeexpstr[blk] != 0)
            if nlferegs == 1:
                lfeahtinu = r.bits(1)
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
            # §E2.3.2.27: ceiling(log2(words_per_frame)), which is NOT
            # bit_length - the two differ by one at every exact power of two,
            # and a 256 kbps frame is exactly 512 words.
            words = frmsiz + 1
            nblkstrtbits = (nblks - 1) * (4 + (words - 1).bit_length())
            r.bits(nblkstrtbits)
    log(f'audfrm: expstre={expstre} ahte={ahte} snroffststr={snroffststr} '
        f'blkswe={blkswe} dithflage={dithflage} bamode={bamode} '
        f'frmfgaincode={frmfgaincode} dbaflde={dbaflde} skipflde={skipflde} '
        f'spxattene={spxattene} transproce={transproce} '
        f'cplinu={cplinu}  -> {r.pos} bits')

    # --- audblk x N (Table E1.4) ---
    endmant = [0] * nfchans
    exps = [None] * nfchans
    lfeexps = None
    cplexps = None
    codes = dict(sdcycod=2, fdcycod=1, sgaincod=1, dbpbcod=2, floorcod=7)
    fgaincod = [4] * (nfchans + 1)
    csnroffst = 0
    fsnroffst = [0] * (nfchans + 1)
    spxinu = 0
    # Section 7.7.1.2: an absent word inherits the previous BLOCK's, and block 0
    # without one is unity - never the previous frame's value.
    dynrng = 0x00
    dynrng_blocks = []
    spxbegf = 0
    spxstart = 0
    spxbnds = []
    chinspx = [0] * nfchans
    spxbndstrc = list(DEF_SPX_BNDSTRC)
    firstspxcos = [1] * nfchans
    chincpl = [0] * nfchans
    phsflginu = 0
    cplbnds = []
    cplbegf = 0
    cplstrtmant = cplendmant = 0
    cplbndstrc = list(DEF_CPL_BNDSTRC)
    # §E2.3.2.28-30: the "first time this frame" states, all set at audfrm's
    # end. They are what makes block 0 cheaper than AC-3's - cplcoe and
    # cplleake are implied there rather than transmitted.
    firstcplcos = [1] * nfchans
    firstcplleak = 1
    cplfleak = cplsleak = 0
    # The coupling channel's own allocation parameters. Under snroffststr 0 and
    # frmfgaincode 0 they simply follow the frame values, but both can be sent
    # separately and ahead of the per-channel ones.
    cplfsnroffst = 0
    cplfgaincod = 4
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
            dynrng = r.bits(8)
        dynrng_blocks.append(dynrng)
        if acmod == 0:
            if r.bits(1):
                r.bits(8)

        spxstre = 1 if blk == 0 else r.bits(1)
        if spxstre:
            spxinu = r.bits(1)
            if spxinu:
                chinspx = [1] if acmod == 1 else [r.bits(1) for _ in range(nfchans)]
                spxstrtf = r.bits(2)
                spxbegf = r.bits(3)
                spxendf = r.bits(3)
                spx_begin = spxbegf + 2 if spxbegf < 6 else spxbegf * 2 - 3
                spx_end = spxendf + 5 if spxendf < 3 else spxendf * 2 + 3
                if r.bits(1):    # spxbndstrce
                    for bnd in range(spx_begin + 1, spx_end):
                        spxbndstrc[bnd] = r.bits(1)
                spxbnds = group_bands(spx_band_start(spx_begin), spx_end - spx_begin,
                                      12, spxbndstrc[spx_begin:])
                spxstart = spx_band_start(spx_begin)
            else:
                chinspx = [0] * nfchans
                firstspxcos = [1] * nfchans
        if spxinu:
            for ch in range(nfchans):
                if not chinspx[ch]:
                    firstspxcos[ch] = 1
                    continue
                if firstspxcos[ch]:
                    spxcoe = 1
                    firstspxcos[ch] = 0
                else:
                    spxcoe = r.bits(1)
                if spxcoe:
                    r.bits(5)    # spxblnd
                    r.bits(2)    # mstrspxco
                    for _ in spxbnds:
                        r.bits(4)  # spxcoexp
                        r.bits(2)  # spxcomant

        # cplstre[blk] came from audfrm: block 0's is implied 1, the rest
        # were transmitted there.
        if cplstre[blk]:
            if cplinu[blk]:
                ecplinu = r.bits(1)
                if ecplinu:
                    raise SystemExit('enhanced coupling not modelled')
                if acmod == 2:
                    chincpl = [1, 1]
                else:
                    chincpl = [r.bits(1) for _ in range(nfchans)]
                if acmod == 2:
                    phsflginu = r.bits(1)
                cplbegf = r.bits(4)
                if spxinu:
                    # §E3.3.1: cplendf is derived from spxbegf, not sent, so
                    # that coupling ends exactly where synthesis begins.
                    cplendf = spxbegf - 2 if spxbegf < 6 else spxbegf * 2 - 7
                else:
                    cplendf = r.bits(4)
                ncplsubnd = 3 + cplendf - cplbegf
                cplstrtmant = 37 + 12 * cplbegf
                cplendmant = 37 + 12 * (cplendf + 3)
                if r.bits(1):        # cplbndstrce
                    cplbndstrc = [0] + [r.bits(1) for _ in range(1, ncplsubnd)]
                cplbnds = group_bands(cplstrtmant, ncplsubnd, 12, cplbndstrc)
            else:
                chincpl = [0] * nfchans
                firstcplcos = [1] * nfchans
                firstcplleak = 1
                phsflginu = 0

        cplcoe = [0] * nfchans
        if cplinu[blk]:
            for ch in range(nfchans):
                if not chincpl[ch]:
                    firstcplcos[ch] = 1
                    continue
                if firstcplcos[ch]:
                    cplcoe[ch] = 1
                    firstcplcos[ch] = 0
                else:
                    cplcoe[ch] = r.bits(1)
                if cplcoe[ch]:
                    r.bits(2)        # mstrcplco
                    for _ in cplbnds:
                        r.bits(4)    # cplcoexp
                        r.bits(4)    # cplcomant
            if acmod == 2 and phsflginu and (cplcoe[0] or cplcoe[1]):
                for _ in cplbnds:
                    r.bits(1)        # phsflg

        if acmod == 2:
            rematstr = 1 if blk == 0 else r.bits(1)
            if rematstr:
                for _ in range(nrematbd(cplinu[blk], cplbegf if cplinu[blk] else 0,
                                        spxinu, spxbegf)):
                    r.bits(1)

        for ch in range(nfchans):
            if chexpstr[blk][ch] != 0:
                # §E3.3.3: whichever tool takes the spectrum over first sets
                # the coded band, and chbwcod is not sent for such a channel.
                if chincpl[ch]:
                    endmant[ch] = cplstrtmant
                elif chinspx[ch]:
                    endmant[ch] = spxstart
                else:
                    chbwcod = r.bits(6)
                    endmant[ch] = ((chbwcod + 12) * 3) + 37
        if cplinu[blk] and cplexpstr[blk] != 0:
            grpsize = (0, 1, 2, 4)[cplexpstr[blk]]
            ncplgrps = (cplendmant - cplstrtmant) // (3 * grpsize)
            absexp = r.bits(4)
            groups = [r.bits(7) for _ in range(ncplgrps)]
            # cplabsexp is a reference, not a coefficient's exponent, and is
            # transmitted halved (§7.1.3).
            cplexps = [24] * cplstrtmant + expand_cpl(absexp * 2, groups, grpsize,
                                                      cplendmant - cplstrtmant)
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
            cplfsnroffst = frmfsnroffst
        else:
            snroffste = 1 if blk == 0 else r.bits(1)
            if snroffste:
                csnroffst = r.bits(6)
                if snroffststr == 1:
                    blkfsnroffst = r.bits(4)
                    fsnroffst = [blkfsnroffst] * (nfchans + 1)
                    cplfsnroffst = blkfsnroffst
                elif snroffststr == 2:
                    # The coupling channel gets its own offset, ahead of the
                    # per-channel ones.
                    if cplinu[blk]:
                        cplfsnroffst = r.bits(4)
                    fsnroffst = [r.bits(4) for _ in range(nfchans)] + \
                                ([r.bits(4)] if lfeon else [0])
        fgaincode = r.bits(1) if frmfgaincode else 0
        if fgaincode:
            # Likewise the coupling channel's fast gain leads the list.
            if cplinu[blk]:
                cplfgaincod = r.bits(3)
            fgaincod = [r.bits(3) for _ in range(nfchans)] + \
                       ([r.bits(3)] if lfeon else [4])
        if strmtyp == 0:
            if r.bits(1):            # convsnroffste
                r.bits(10)
        if cplinu[blk]:
            # §E2.2.4: firstcplleak makes block 0's cplleake implied, so the
            # leak seeds are mandatory there and the flag costs nothing.
            if firstcplleak:
                cplleake = 1
                firstcplleak = 0
            else:
                cplleake = r.bits(1)
            if cplleake:
                cplfleak = r.bits(3)
                cplsleak = r.bits(3)
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
        # §E2.2.4's got_cplchan: the coupling channel's mantissas sit right
        # after the FIRST coupled channel's, not after all of them. While only
        # the block's bit TOTAL was being checked this made no difference,
        # which is exactly how it went unnoticed - reading AHT regions in
        # place is what makes the order matter.
        regions = []
        got_cplchan = False
        for ch in range(nfchans):
            regions.append((exps[ch], 0, endmant[ch], fgaincod[ch], fsnroffst[ch],
                            False, chahtinu[ch]))
            if cplinu[blk] and chincpl[ch] and not got_cplchan:
                # cplfsnroffst and cplfgaincod follow frmfsnroffst / 0x4 exactly
                # as the fbw channels do under snroffststr 0 and frmfgaincode 0.
                regions.append((cplexps, cplstrtmant, cplendmant, cplfgaincod,
                                cplfsnroffst, True, cplahtinu))
                got_cplchan = True
        if lfeon:
            regions.append((lfeexps, 0, LFE_ENDMANT, fgaincod[nfchans],
                            fsnroffst[nfchans], False, lfeahtinu))
        # The mantissa element is walked strictly in bitstream order, region by
        # region, because AHT regions have to be READ rather than counted:
        # gain-adaptive quantization makes a mantissa's length depend on the
        # mantissa, so the only independent check of the encoder's arithmetic
        # is to follow the tags.
        #
        # Grouped baps (1, 2 and 4) are counted across the whole block rather
        # than per region, and their codeword lands at the position of the
        # group's FIRST member - so a region's own cost is how much the
        # running grouped total moves while it is being walked. That is exact
        # even when a group straddles two channels.
        def grouped_bits():
            return (5 * ((counts[1] + 2) // 3) + 7 * ((counts[2] + 2) // 3)
                    + 7 * ((counts[4] + 1) // 2))

        per_region = []
        mant_start = r.pos
        for e, begin, end, fgain, fsnr, is_cpl, aht in regions:
            bap = bit_alloc(e[:end], fscod_family, codes['sdcycod'], codes['fdcycod'],
                            codes['sgaincod'], codes['dbpbcod'], codes['floorcod'],
                            fgain, csnroffst, fsnr, start=begin, coupling=is_cpl,
                            cplfleak=cplfleak, cplsleak=cplsleak, high_efficiency=aht)
            if aht:
                # §E2.2.4: an AHT region's whole frame of mantissas is read in
                # block 0, and nothing is read for it in blocks 1 to 5.
                if blk != 0:
                    per_region.append(('aht-', 0))
                    continue
                before = r.pos
                read_aht_region(r, bap, begin, end)
                per_region.append((f'aht{end - begin}', r.pos - before))
                continue
            before = grouped_bits()
            direct = 0
            for b in bap[begin:]:
                if b in counts:
                    counts[b] += 1
                elif b:
                    direct += (0, 0, 0, 3, 0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16)[b]
            share = direct + grouped_bits() - before
            if r.pos + share > len(r.data) * 8:
                raise SystemExit(
                    f'  OVERRUN: block {blk} wants {share} more mantissa bits but only '
                    f'{len(r.data) * 8 - r.pos} remain in the frame. The encoder and an '
                    f'independent allocation disagree.')
            r.bits(share)
            per_region.append(('cpl' if is_cpl else f'{end - begin}bin', share))
        total_mant_bits = r.pos - mant_start
        log(f'  blk {blk}: side {side} bits, csnroffst={csnroffst} '
            f'fsnroffst={fsnroffst[0]}, mantissas {total_mant_bits} bits '
            f'{per_region} -> ends at {r.pos}')

    if any(w != 0x00 for w in dynrng_blocks):
        log('  dynrng: ' + ' '.join(f'{dynrng_db(w):+.2f}' for w in dynrng_blocks) + ' dB')

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
                               'fscod': fscod, 'fscod_family': fscod_family,
                               'numblkscod': numblkscod,
                               'acmod': acmod, 'lfeon': lfeon, 'chanmap': chanmap,
                               'dialnorm': dialnorm, 'compr': compr,
                               'mixmdate': mix or None, 'dynrng': dynrng_blocks,
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


def expand_cpl(absexp, groups, grpsize, count):
    """Coupling exponents: absexp is a reference, not a coefficient's own."""
    out = []
    prev = absexp
    for g in groups:
        for d in (g // 25, (g % 25) // 5, (g % 25) % 5):
            prev += d - 2
            out.extend([prev] * grpsize)
    return out[:count] + [24] * max(0, count - len(out))


# Table E3.2: scalar mantissa width per hebap, and the VQ index width below it.
AHT_MANTISSA_BITS = {8: 3, 9: 4, 10: 5, 11: 6, 12: 7, 13: 8,
                     14: 9, 15: 10, 16: 11, 17: 12, 18: 14, 19: 16}
AHT_VQ_BITS = [0, 2, 3, 4, 5, 7, 8, 9]


def read_aht_region(r, bap, begin, end):
    """Consume one AHT region: gaqmod, the gain words, then the mantissas.

    Nothing here is computed from a table of lengths, because under
    gain-adaptive quantization there is no such table - a mantissa that will
    not fit the small quantizer is sent as that quantizer's unused
    full-scale-negative symbol followed by a longer codeword, so the reader
    has to follow the tags exactly as a decoder does.
    """
    gaqmod = r.bits(2)
    endbap = 12 if gaqmod < 2 else 17
    active = [b for b in range(begin, end) if 7 < bap[b] < endbap]

    gains = {}
    if gaqmod == 3:
        # Table E3.4: three three-state gains packed into a 5-bit word.
        mapped = []
        for _ in range((len(active) + 2) // 3):
            word = r.bits(5)
            mapped += [word // 9, (word % 9) // 3, (word % 9) % 3]
        for i, b in enumerate(active):
            gains[b] = (1, 2, 4)[mapped[i]]
    elif gaqmod in (1, 2):
        other = 2 if gaqmod == 1 else 4
        for b in active:
            gains[b] = other if r.bits(1) else 1

    for b in range(begin, end):
        hebap = bap[b]
        if hebap == 0:
            continue
        if hebap <= 7:
            r.bits(AHT_VQ_BITS[hebap])     # one VQ index for all six blocks
            continue
        m = AHT_MANTISSA_BITS[hebap]
        gain = gains.get(b, 1)
        if gain == 1:
            for _ in range(6):
                r.bits(m)
            continue
        small = m - 1 if gain == 2 else m - 2
        large = m - 1 if gain == 2 else m
        tag = 1 << (small - 1)             # the full-scale-negative symbol
        for _ in range(6):
            if r.bits(small) == tag:
                r.bits(large)


def nrematbd(cplinu, cplbegf, spxinu=0, spxbegf=0):
    """§E3.3.2: rematrixing bands stop where the first tool takes over."""
    if cplinu:
        if cplbegf == 0:
            return 2
        return 3 if cplbegf < 3 else 4
    if spxinu:
        return 3 if spxbegf < 2 else 4
    return 4


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
            for field in ('fscod', 'fscod_family', 'numblkscod'):
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
