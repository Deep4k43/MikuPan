"""Checks the 2D texture slots rely on, over every TIM2 picture of the 2D
files (TIM2 files and pk2s of them outside bin/3ddata, each entry once):

  psm   GsTex0 PSM == psmtbl[ImageType - 1], the value Tim2LoadImage2()
        writes in memory; PSMT4 pictures have CSA 0 (Tim2LoadClut2() sets 0).
        The slots key a stamped picture by its disc TEX0.
  clut  (ClutType, ImageType) combinations. Tim2LoadClut2() places the same
        CLUT entries as MakeClutDirect3() (what the bake and the slots model)
        only for (0x03, IDTEX8), (0x03, IDTEX4) and (0x83, IDTEX4).
  tw    TW/TH == GetLog2() (ceil log2) of ImageWidth/ImageHeight, which
        LoadTIM2Sub() uses to rebuild a sprite's TEX0.
  mip   pictures with mipmaps (the Tim2LoadTexture() mip path).

Expected on NTSC-U and PAL (2026-10-03): no PSM or TW/TH difference, no
PSMT4 CSA, no mipmaps, and only the 14 (0x83, IDTEX8) pictures of
bin/tim/test2d.pk2 outside the matching CLUT layouts.

    python tim2_disc_checks.py IMG_HD.BIN IMG_BD.BIN
"""
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sgdlib import Archive
from survey2d import tim2_pics, pk2_offsets, SKIP

NAMES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "..", "..", "..", "src", "os", "eeiop", "cdvd",
                     "fnameee.h")
PSMTBL = {1: 0x02, 2: 0x01, 3: 0x00, 4: 0x14, 5: 0x13}
SAME_CLUT = {(0x03, 5), (0x03, 4), (0x83, 4)}


def get_log2(n):
    """GetLog2() in tim2.cpp."""
    i = 31
    while i > 0 and not ((n >> i) & 1):
        i -= 1
    return i + (1 if (1 << i) < n else 0)


def pictures(arc):
    seen = set()
    for fid, (sector, size) in enumerate(arc.entries):
        name = arc.name(fid)
        if size == 0 or any(name.startswith(s) for s in SKIP) \
                or (sector, size) in seen:
            continue
        seen.add((sector, size))
        b = arc.read(fid)
        pics = tim2_pics(b, 0, len(b))
        files = [pics] if pics is not None else [
            tim2_pics(b, o, len(b) - o) for o in pk2_offsets(b, 0, len(b)) or []]
        for pics in files:
            for p in pics or []:
                yield name, p


def main():
    arc = Archive(sys.argv[1], sys.argv[2], os.path.normpath(NAMES))
    stats = Counter()
    clut = Counter()
    odd = []
    for name, p in pictures(arc):
        stats["pictures"] += 1
        tex0 = p["tex0"]
        psm = (tex0 >> 20) & 0x3F
        tw = (tex0 >> 26) & 0xF
        th = (tex0 >> 30) & 0xF
        csa = (tex0 >> 56) & 0x1F
        where = (name, hex(p["ph"]))
        if p["mip"] > 1:
            stats["mipmapped"] += 1
            odd.append(where + ("mipmaps", p["mip"]))
        if PSMTBL.get(p["itype"]) != psm:
            stats["PSM differs"] += 1
            odd.append(where + ("PSM", hex(psm), p["itype"]))
        if psm == 0x14 and csa != 0:
            stats["PSMT4 with CSA"] += 1
            odd.append(where + ("CSA", csa))
        if tw != get_log2(p["w"]) or th != get_log2(p["h"]):
            stats["TW/TH differ"] += 1
            odd.append(where + ("TW/TH", tw, th, p["w"], p["h"]))
        if p["clut_size"]:
            clut[(p["ctype"], p["itype"], p["colors"])] += 1
            if (p["ctype"], p["itype"]) not in SAME_CLUT:
                stats["other CLUT layout"] += 1
                odd.append(where + ("ClutType", hex(p["ctype"]), p["itype"]))
    for k, v in sorted(stats.items()):
        print(f"{k}: {v}")
    for (ct, it, n), c in sorted(clut.items()):
        print(f"ClutType {ct:#04x} ImageType {it} colours {n}: {c}")
    for row in odd[:40]:
        print(*row)


if __name__ == "__main__":
    main()
