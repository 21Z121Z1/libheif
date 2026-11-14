#!/usr/bin/env python3
# Independent (non-libheif) ISOBMFF structure verifier for a GB/T 48347 HEIF file.
# Usage: python3 eval/verify_gbt_structure.py FILE.heic
# Checks: ftyp 'tmap' compatible brand; base/gain/tmap/it35 items; one 'dimg'
# from the tmap with exactly two ordered references (base first, gain second);
# base is the primary item (pitm); idat present. Exits non-zero on failure.
import struct
import sys


def find_boxes(buf, off, end):
    res = []
    while off + 8 <= end:
        sz, typ = struct.unpack('>I4s', buf[off:off + 8])
        typ = typ.decode('latin1')
        h = 8
        if sz == 1:
            sz = struct.unpack('>Q', buf[off + 8:off + 16])[0]
            h = 16
        elif sz == 0:
            sz = end - off
        res.append((typ, off, sz, h))
        off += sz
    return res


def main(path):
    data = open(path, 'rb').read()
    top = find_boxes(data, 0, len(data))
    ftyp = [b for b in top if b[0] == 'ftyp'][0]
    compat = [data[ftyp[1] + 16 + i:ftyp[1] + 20 + i].decode()
              for i in range(0, ftyp[2] - 16, 4)]
    meta = [b for b in top if b[0] == 'meta'][0]
    mc = find_boxes(data, meta[1] + meta[3] + 4, meta[1] + meta[2])

    iinf = [b for b in mc if b[0] == 'iinf'][0]
    p = iinf[1] + iinf[3]
    ver = data[p]
    p += 4
    p += 2 if ver == 0 else 4  # entry_count
    items = []
    for typ, o, s, h in find_boxes(data, p, iinf[1] + iinf[2]):
        if typ != 'infe':
            continue
        v = data[o + 8]
        q = o + 12
        iid = struct.unpack('>H', data[q:q + 2])[0] if v < 3 else struct.unpack('>I', data[q:q + 4])[0]
        q += 2 if v < 3 else 4
        q += 2
        items.append((iid, data[q:q + 4].decode()))

    pitm = [b for b in mc if b[0] == 'pitm'][0]
    pv = data[pitm[1] + 8]
    primary = struct.unpack('>H', data[pitm[1] + 12:pitm[1] + 14])[0] if pv == 0 \
        else struct.unpack('>I', data[pitm[1] + 12:pitm[1] + 16])[0]

    dimg = []
    iref = [b for b in mc if b[0] == 'iref']
    if iref:
        ir = iref[0]
        for typ, o, s, h in find_boxes(data, ir[1] + ir[3] + 4, ir[1] + ir[2]):
            if typ == 'dimg':
                q = o + 8
                frm, cnt = struct.unpack('>HH', data[q:q + 4])
                tos = list(struct.unpack('>' + 'H' * cnt, data[q + 4:q + 4 + 2 * cnt]))
                dimg.append((frm, cnt, tos))

    tmap = [i for i, t in items if t == 'tmap']
    it35 = [i for i, t in items if t == 'it35']
    images = [i for i, t in items if t in ('unci', 'hvc1', 'grid', 'av01')]
    has_idat = any(b[0] == 'idat' for b in mc)

    print("ftyp compatible:", compat)
    print("items:", items)
    print("pitm primary:", primary)
    print("dimg:", dimg)
    print("idat present:", has_idat)

    ok = True
    ok &= 'tmap' in compat
    ok &= len(tmap) == 1 and len(it35) >= 1 and len(images) >= 2
    ok &= has_idat
    tmap_dimg = [d for d in dimg if d[0] in tmap]
    ok &= len(tmap_dimg) == 1 and tmap_dimg[0][1] == 2
    if tmap_dimg:
        base, gain = tmap_dimg[0][2]
        ok &= base == primary  # GB/T 9.3.2a base is primary
        ok &= base in images and gain in images and base != gain
    print("INDEPENDENT STRUCTURE", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))