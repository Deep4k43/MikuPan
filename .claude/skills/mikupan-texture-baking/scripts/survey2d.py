"""Survey the 2D texture content of the archive: TIM2 files and 2D PK2 paks
(u32 num; pad[3]; u32 offset[num], offsets relative to the pak start)."""
import struct
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sgdlib import Archive, tex0_fields, u32, u64, pak_entries

SKIP = ("bin/bgm", "bin/se", "bin/movie", "bin/3ddata")


def tim2_pics(b, off, size):
    """Pictures of a TIM2 file at off, or None."""
    if off + 16 > len(b) or b[off:off + 4] != b"TIM2":
        return None
    fmt = b[off + 5]
    npic = struct.unpack_from("<H", b, off + 6)[0]
    ph = off + (16 if fmt == 0 else 0x80)
    out = []
    for i in range(npic):
        if ph + 0x30 > len(b):
            return out
        total, clut_size, img_size, hdr_size, colors = struct.unpack_from("<IIIHH", b, ph)
        pf, mip, ctype, itype = struct.unpack_from("<4B", b, ph + 0x10)
        w, h = struct.unpack_from("<HH", b, ph + 0x14)
        tex0 = u64(b, ph + 0x18)
        tex1 = u64(b, ph + 0x20)
        texa, texclut = struct.unpack_from("<II", b, ph + 0x28)
        out.append(dict(ph=ph, fmt=fmt, total=total, clut_size=clut_size, img_size=img_size,
                        hdr_size=hdr_size, colors=colors, mip=mip, ctype=ctype, itype=itype,
                        w=w, h=h, tex0=tex0, tex1=tex1, texa=texa, texclut=texclut))
        ph += total
    return out


def pk2_offsets(b, base, size):
    if size < 20:
        return None
    n = u32(b, base)
    if n == 0 or n > 512:
        return None
    if u32(b, base + 4) or u32(b, base + 8) or u32(b, base + 12):
        # some paks may have non-zero pads; accept anyway but check offsets
        pass
    offs = [u32(b, base + 16 + 4 * i) for i in range(n)]
    for o in offs:
        if o == 0 or o >= size or b[base + o:base + o + 4] != b"TIM2":
            return None
    return offs


def main():
    hd, bd = sys.argv[1], sys.argv[2]
    names_path = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "src", "os", "eeiop", "cdvd", "fnameee.h"))
    arc = Archive(hd, bd, names_path)
    kinds = Counter()
    per_dir = defaultdict(Counter)
    tim_total = 0
    psm_c = Counter()
    itype_c = Counter()
    ctype_c = Counter()
    fmt_c = Counter()
    mip_c = Counter()
    nonpow = 0
    tw_mismatch = []
    tex1_nonzero = 0
    texa_nonzero = 0
    texclut_nonzero = 0
    tex1_vals = Counter()
    seen = set()
    unknown = []
    rows = []
    for fid, (sector, size) in enumerate(arc.entries):
        name = arc.name(fid)
        if size == 0 or any(name.startswith(s) for s in SKIP):
            continue
        b = arc.read(fid)
        d = "/".join(name.split("/")[:3]) if name.count("/") >= 3 else "/".join(name.split("/")[:-1])
        pics = []
        kind = "other"
        t = tim2_pics(b, 0, size)
        if t is not None:
            kind = "tim2"
            pics = [(0, p) for p in t]
        else:
            offs = pk2_offsets(b, 0, size)
            if offs is not None:
                kind = "pk2"
                for i, o in enumerate(offs):
                    for p in tim2_pics(b, o, size - o) or []:
                        pics.append((i, p))
            else:
                ents = pak_entries(b, 0, size)
                if ents:
                    kind = "pak3d"
        kinds[kind] += 1
        per_dir[d][kind] += 1
        if kind == "other":
            unknown.append((fid, name, size, b[:16].hex()))
        key = (sector, size)
        dup = key in seen
        seen.add(key)
        for idx, p in pics:
            f = tex0_fields(p["tex0"])
            tim_total += 1
            psm_c[f["PSM"]] += 1
            itype_c[p["itype"]] += 1
            ctype_c[p["ctype"]] += 1
            fmt_c[p["fmt"]] += 1
            mip_c[p["mip"]] += 1
            if p["tex1"]:
                tex1_nonzero += 1
                tex1_vals[p["tex1"]] += 1
            if p["texa"]:
                texa_nonzero += 1
            if p["texclut"]:
                texclut_nonzero += 1
            if (1 << f["TW"]) < p["w"] or (1 << f["TH"]) < p["h"] or (1 << f["TW"]) >= 2 * p["w"] or (1 << f["TH"]) >= 2 * p["h"]:
                tw_mismatch.append((name, idx, p["w"], p["h"], f["TW"], f["TH"]))
            if p["w"] & (p["w"] - 1) or p["h"] & (p["h"] - 1):
                nonpow += 1
            rows.append((fid, name, idx, p, f, dup))
    print("kinds", dict(kinds))
    for d, c in sorted(per_dir.items()):
        print(f"  {d:30s} {dict(c)}")
    print("pictures", tim_total)
    print("psm", {hex(k): v for k, v in psm_c.items()})
    print("itype", dict(itype_c), "ctype", {hex(k): v for k, v in ctype_c.items()})
    print("fmt", dict(fmt_c), "mip", dict(mip_c))
    print("non-pow2 dims", nonpow)
    print("TW/TH not log2ceil(w/h)", len(tw_mismatch), tw_mismatch[:20])
    print("tex1 nonzero", tex1_nonzero, "texa nonzero", texa_nonzero, "texclut nonzero", texclut_nonzero)
    print("tex1 values", [(hex(k), v) for k, v in tex1_vals.most_common(10)])
    print("unknown files", len(unknown))
    for u in unknown[:80]:
        print("  ", u)
    if len(sys.argv) > 3:
        with open(sys.argv[3], "w") as out:
            for fid, name, idx, p, f, dup in rows:
                out.write(f"{fid}\t{name}\t{idx}\t{p['w']}x{p['h']}\tpsm={f['PSM']:#x}\ttbp={f['TBP0']:#x}\ttbw={f['TBW']}\ttw={f['TW']}\tth={f['TH']}\tcbp={f['CBP']:#x}\tcolors={p['colors']}\tctype={p['ctype']:#x}\titype={p['itype']}\tfmt={p['fmt']}\thdr={p['hdr_size']}\tmip={p['mip']}\ttcc={f['TCC']}\ttfx={f['TFX']}\tcsa={f['CSA']}\tcld={f['CLD']}\tcpsm={f['CPSM']}\ttex1={p['tex1']:#x}\ttexa={p['texa']:#x}\ttexclut={p['texclut']:#x}\t{'dup' if dup else ''}\n")


if __name__ == "__main__":
    main()
