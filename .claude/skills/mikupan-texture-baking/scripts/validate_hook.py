"""Temporary texture-level validation of the 2D texture slots (never commit).

Feeds a GS emulation (mikupan_gs_memory) with every upload the slots see and,
for every TEX0 the game resolves, compares what the slots show with what the
old GS path would have decoded at that moment:

  VALIDATE mismatch <kind> TEX0 <tex0> region WxH: N texels differ ...

kinds: picture (stamped TIM2), derived, unstamped, raw (runtime image), miss
(nothing registered; the GS would show non-zero texels), copy (fonts, map).
Regions: a picture's own width x height, a runtime image's extent, the whole
texture otherwise. Each TEX0 is checked again whenever what shows it changes.
A summary line is printed at shutdown.

    python validate_hook.py <tree>            apply
    python validate_hook.py <tree> --remove   remove
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from edit import patch

SLOTS = "/src/mikupan/rendering/mikupan_texture_slots.cpp"

PAIRS = [
    ('''#include <memory>
#include <vector>
''', '''#include <memory>
#include <vector>
#include "mikupan/gs/mikupan_gs_memory.h" /* VALIDATE (temporary) */
#include <cstdio>                         /* VALIDATE (temporary) */
#include <cstdlib>                        /* VALIDATE (temporary) */
#include <map>                            /* VALIDATE (temporary) */
#include <string>                         /* VALIDATE (temporary) */
'''),
    ('''void ReleaseTexture(unsigned int* texture)
{
    if (*texture != 0)
    {
        MikuPan_GPUReleaseTexture(*texture);
        *texture = 0;
    }
}

}// namespace
''', '''void ReleaseTexture(unsigned int* texture)
{
    if (*texture != 0)
    {
        MikuPan_GPUReleaseTexture(*texture);
        *texture = 0;
    }
}

/* VALIDATE (temporary): a GS emulation fed with every upload, compared with
 * what the slots show for every TEX0 they resolve. */
GS::GSHelper* g_validate_gs = nullptr;
std::map<uint64_t, uint64_t> g_validate_seen;
size_t g_validate_checks = 0;
size_t g_validate_mismatches = 0;
std::map<std::string, size_t> g_validate_kinds;

GS::GSHelper& ValidateGs()
{
    if (g_validate_gs == nullptr)
    {
        g_validate_gs = new GS::GSHelper();
    }

    return *g_validate_gs;
}

void ValidatePicture(const Tim2Picture& p, int block, bool clut)
{
    const Tim2Tex0 t = SplitTex0(p.tex0);

    if (!clut && p.image_size != 0)
    {
        ValidateGs().Upload(t.psm, block, t.tbw, 0, 0, p.width & 0xFFF,
                            p.height & 0xFFF, p.image);
    }

    if (clut && p.clut_size != 0)
    {
        ValidateGs().Upload(0, block, 1, 0, 0, p.clut_colors == 16 ? 8 : 16,
                            p.clut_colors == 16 ? 2 : p.clut_colors / 16,
                            p.clut);
    }
}

void ValidateCompare(const char* kind, uint64_t tex0, const uint8_t* ours,
                     int region_w, int region_h, uint64_t signature)
{
    auto it = g_validate_seen.find(tex0);

    if (it != g_validate_seen.end() && it->second == signature)
    {
        return;
    }

    g_validate_seen[tex0] = signature;

    const Tim2Tex0 t = SplitTex0(tex0);
    const int w = 1 << t.tw;
    const int h = 1 << t.th;
    std::vector<uint8_t> gs((size_t)w * h * 4);
    GS::Tex0 g;
    g.tbp0 = t.tbp0;
    g.tbw = t.tbw;
    g.psm = t.psm;
    g.tw = t.tw;
    g.th = t.th;
    g.cbp = t.cbp;
    g.csa = t.csa;

    if (!ValidateGs().DecodeTexture(g, gs.data()))
    {
        return;
    }

    g_validate_checks++;
    g_validate_kinds[kind]++;

    const int rw = std::min(w, region_w);
    const int rh = std::min(h, region_h);
    size_t differ = 0;
    int max_diff = 0;
    int fx = -1;
    int fy = -1;

    for (int y = 0; y < rh; y++)
    {
        for (int x = 0; x < rw; x++)
        {
            const size_t at = ((size_t)y * w + x) * 4;
            int d = 0;

            for (int c = 0; c < 4; c++)
            {
                const int o = ours != nullptr ? ours[at + c] : 0;
                d = std::max(d, std::abs((int)gs[at + c] - o));
            }

            if (d != 0)
            {
                differ++;
                max_diff = std::max(max_diff, d);

                if (fx < 0)
                {
                    fx = x;
                    fy = y;
                }
            }
        }
    }

    if (differ != 0)
    {
        g_validate_mismatches++;
        std::printf("VALIDATE mismatch %s TEX0 %#018llx region %dx%d: %zu "
                    "texels differ (max %d), first at %d,%d\\n",
                    kind, (unsigned long long)tex0, rw, rh, differ, max_diff,
                    fx, fy);
        std::fflush(stdout);
    }
}

void ValidateResolved(const char* kind_override, uint64_t tex0,
                      const Slot* s, Unstamped* unstamped)
{
    if (s != nullptr && s->kind != Kind::kUnstamped)
    {
        std::vector<uint8_t> px;
        int w = 0;
        int h = 0;
        int rw = 1 << 30;
        int rh = 1 << 30;
        Tim2Picture p;

        if (!IdPixels(*s, &px, &w, &h))
        {
            std::printf("VALIDATE no pixels for 2D id %u (TEX0 %#018llx)\\n",
                        s->id, (unsigned long long)tex0);
            return;
        }

        if (s->kind == Kind::kPicture && s->id < g_sources.size()
            && !g_sources[s->id].image.empty()
            && ReadTim2Picture(g_sources[s->id].image.data(),
                               g_sources[s->id].image.size(), &p))
        {
            rw = p.width;
            rh = p.height;
        }

        ValidateCompare(kind_override != nullptr ? kind_override
                        : s->kind == Kind::kPicture ? "picture" : "derived",
                        tex0, px.data(), rw, rh,
                        ((uint64_t)s->id << 32) ^ g_owner[s->check_block]
                            ^ ((uint64_t)s->kind << 60));
        return;
    }

    if (s != nullptr && unstamped != nullptr)
    {
        ValidateCompare(kind_override != nullptr ? kind_override
                                                 : "unstamped",
                        tex0, unstamped->pixels.data(), 1 << 30, 1 << 30,
                        (uint64_t)(uintptr_t)unstamped->pixels.data()
                            ^ ((uint64_t)unstamped->source.size() << 40)
                            ^ (uint64_t)unstamped->texture);
        return;
    }

    RawImage* r;
    const RawImage* c;

    if (FindRaw(tex0, &r, &c))
    {
        std::vector<uint8_t> px;
        RawPixels(tex0, *r, c, &px);
        ValidateCompare(kind_override != nullptr ? kind_override : "raw",
                        tex0, px.data(), r->width, r->height,
                        ((uint64_t)r->generation << 32)
                            ^ (c != nullptr ? c->generation : 0)
                            ^ (uint64_t)r->owner);
        return;
    }

    if (Tex0Drawable(tex0))
    {
        ValidateCompare("miss", tex0, nullptr, 1 << 30, 1 << 30, 1);
    }
}

}// namespace
'''),
    ('''    Init();
    block &= kBlockMask;

    const Tex2DEntry* e = Stamped(header, p, &stamp);
    const Tim2Tex0 t = SplitTex0(e != nullptr ? e->tex0 : p.tex0);
    const int offset = (block - t.tbp0) & kBlockMask;
''', '''    Init();
    block &= kBlockMask;
    ValidatePicture(p, block, false); /* VALIDATE (temporary) */

    const Tex2DEntry* e = Stamped(header, p, &stamp);
    const Tim2Tex0 t = SplitTex0(e != nullptr ? e->tex0 : p.tex0);
    const int offset = (block - t.tbp0) & kBlockMask;
'''),
    ('''    Init();
    block &= kBlockMask;

    const Tex2DEntry* e = Stamped(header, p, &stamp);
    const Tim2Tex0 t = SplitTex0(e != nullptr ? e->tex0 : p.tex0);
    const int offset = (block - t.cbp) & kBlockMask;
''', '''    Init();
    block &= kBlockMask;
    ValidatePicture(p, block, true); /* VALIDATE (temporary) */

    const Tex2DEntry* e = Stamped(header, p, &stamp);
    const Tim2Tex0 t = SplitTex0(e != nullptr ? e->tex0 : p.tex0);
    const int offset = (block - t.cbp) & kBlockMask;
'''),
    ('''    Init();

    const int block = dbp & kBlockMask;
    const int psm = StoredPsm(dpsm);
''', '''    Init();
    ValidateGs().Upload(dpsm, dbp & 0x3FFF, dbw, x, y, w, h,
                        (const uint8_t*)data); /* VALIDATE (temporary) */

    const int block = dbp & kBlockMask;
    const int psm = StoredPsm(dpsm);
'''),
    ('''    if (texture == 0 && Tex0Drawable(tex0))
    {
        ReportMiss(tex0);
    }

    return texture;
}''', '''    if (texture == 0 && Tex0Drawable(tex0))
    {
        ReportMiss(tex0);
    }

    ValidateResolved(nullptr, tex0, s, unstamped); /* VALIDATE (temporary) */
    return texture;
}'''),
    ('''unsigned int MikuPan_TexSlots_CreateCopy(uint64_t tex0)
{
    Init();

    Unstamped* unstamped = nullptr;
    const Slot* s = FindSlot(tex0, &unstamped);
    int w = 0;
    int h = 0;
''', '''unsigned int MikuPan_TexSlots_CreateCopy(uint64_t tex0)
{
    Init();

    Unstamped* unstamped = nullptr;
    const Slot* s = FindSlot(tex0, &unstamped);
    int w = 0;
    int h = 0;
    ValidateResolved("copy", tex0, s, unstamped); /* VALIDATE (temporary) */
'''),
    ('''void MikuPan_TexSlots_Shutdown(void)
{
    MikuPan_TexSlots_ReleaseTextures();
''', '''void MikuPan_TexSlots_Shutdown(void)
{
    std::printf("VALIDATE summary: %zu checks, %zu mismatches\\n",
                g_validate_checks, g_validate_mismatches); /* VALIDATE */
    for (const auto& kc : g_validate_kinds)                 /* VALIDATE */
    {
        std::printf("VALIDATE kind %s: %zu checks\\n", kc.first.c_str(),
                    kc.second);
    }
    std::fflush(stdout);                                   /* VALIDATE */
    MikuPan_TexSlots_ReleaseTextures();
'''),
]


def main():
    root = sys.argv[1]
    remove = len(sys.argv) > 2 and sys.argv[2] == "--remove"
    for old, new in (reversed(PAIRS) if remove else PAIRS):
        if remove:
            patch(root + SLOTS, new, old)
        else:
            patch(root + SLOTS, old, new)
    print("validation hook", "removed from" if remove else "applied to", root)


if __name__ == "__main__":
    main()
