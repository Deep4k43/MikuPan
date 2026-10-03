#include "mikupan_gs.h"
#include "SDL3/SDL_timer.h"

#include "mikupan_texture_manager.h"
#include "spdlog/spdlog.h"
#include "xxhash.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "mikupan/ui/mikupan_ui.h"
#include "mikupan/mikupan_utils.h"
#include "mikupan/ui/mikupan_ui_debug.h"

GS::GSHelper gsHelper;

struct GsFrameMetrics
{
    int    upload_count;
    int    upload_bytes;
    double upload_ms;
    int    download_count;
    int    download_bytes;
    double download_ms;
};

static GsFrameMetrics g_gs_frame_metrics = {0, 0, 0.0, 0, 0, 0.0};
static GsFrameMetrics g_gs_last_frame    = {0, 0, 0.0, 0, 0, 0.0};

struct GsUploadRegion { int addr; int size; };
static std::vector<GsUploadRegion> g_pending_uploads;

/// sceGsTex0 is a bitfield view of the raw 64-bit register value.
static GS::Tex0 MikuPan_ToGsTex0(const sceGsTex0 *tex0)
{
    uint64_t value = 0;
    memcpy(&value, tex0, sizeof(value));
    return GS::DecodeTex0(value);
}

static int MikuPan_GetRawTextureRegion(sceGsTex0 *tex0, int *out_addr, int *out_size)
{
    if (tex0 == nullptr || out_addr == nullptr || out_size == nullptr)
    {
        return 0;
    }

    return GS::GetRawTextureRegion(MikuPan_ToGsTex0(tex0), out_addr, out_size)
               ? 1
               : 0;
}

static int MikuPan_GetClutRegion(sceGsTex0 *tex0, int *out_addr, int *out_size)
{
    int min_addr;
    int max_addr;

    if (tex0 == nullptr || out_addr == nullptr || out_size == nullptr)
    {
        return 0;
    }

    if (tex0->PSM != PSMT8 && tex0->PSM != PSMT4)
    {
        return 0;
    }

    min_addr = GS::kVramSize;
    max_addr = 0;

    // MikuPan's indexed texture decoder samples a 16x16 PSMCT32 CLUT page.
    // Include that page in hashes and invalidation so black/white CLUT swaps
    // cannot keep reusing a stale color GL texture.
    for (int cy = 0; cy < 16; cy++)
    {
        for (int cx = 0; cx < 16; cx++)
        {
            const int addr = GS::GetPixelAddressPSMCT32(tex0->CBP, tex0->TBW, cx, cy);

            if (addr < min_addr)
            {
                min_addr = addr;
            }

            if (addr + 4 > max_addr)
            {
                max_addr = addr + 4;
            }
        }
    }

    if (min_addr < 0 || max_addr <= min_addr ||
        max_addr > GS::kVramSize)
    {
        return 0;
    }

    *out_addr = min_addr;
    *out_size = max_addr - min_addr;
    return 1;
}

static int MikuPan_GetTextureInvalidationRegion(sceGsTex0 *tex0,
                                                int *out_addr,
                                                int *out_size)
{
    int tex_addr;
    int tex_size;
    int clut_addr;
    int clut_size;

    if (!MikuPan_GetRawTextureRegion(tex0, &tex_addr, &tex_size))
    {
        return 0;
    }

    if (MikuPan_GetClutRegion(tex0, &clut_addr, &clut_size))
    {
        const int tex_end = tex_addr + tex_size;
        const int clut_end = clut_addr + clut_size;
        const int region_addr = tex_addr < clut_addr ? tex_addr : clut_addr;
        const int region_end = tex_end > clut_end ? tex_end : clut_end;

        *out_addr = region_addr;
        *out_size = region_end - region_addr;
        return 1;
    }

    *out_addr = tex_addr;
    *out_size = tex_size;
    return 1;
}

static int MikuPan_GetUploadRegion(sceGsLoadImage *image_load, int *out_addr, int *out_size)
{
    int width;
    int height;
    int addr;
    int size;

    if (image_load == nullptr || out_addr == nullptr || out_size == nullptr)
    {
        return 0;
    }

    width = static_cast<int>(image_load->trxreg.RRW);
    height = static_cast<int>(image_load->trxreg.RRH);

    switch (static_cast<PixelStorageFormat>(image_load->bitbltbuf.DPSM))
    {
        case PSMZ32:
        case PSMCT32:
            addr = GS::GetPixelAddressPSMCT32(
                image_load->bitbltbuf.DBP,
                image_load->bitbltbuf.DBW,
                image_load->trxpos.DSAX,
                image_load->trxpos.DSAY);
            size = width * height * 4;
            break;

        case PSMCT16:
        case PSMCT16S:
        case PSMZ16:
        case PSMZ16S:
            addr = GS::GetPixelAddressPSMCT16(
                image_load->bitbltbuf.DBP,
                image_load->bitbltbuf.DBW,
                image_load->trxpos.DSAX,
                image_load->trxpos.DSAY);
            size = width * height * 2;
            break;

        case PSMT8:
            addr = GS::GetPixelAddressPSMT8(
                image_load->bitbltbuf.DBP,
                image_load->bitbltbuf.DBW,
                image_load->trxpos.DSAX,
                image_load->trxpos.DSAY);
            size = width * height;
            break;

        case PSMT4:
            addr = GS::GetPixelAddressPSMT4(
                image_load->bitbltbuf.DBP,
                image_load->bitbltbuf.DBW,
                image_load->trxpos.DSAX,
                image_load->trxpos.DSAY) >> 1;
            size = (width * height + 1) >> 1;
            break;

        default:
            return 0;
    }

    if (addr < 0 || size <= 0 || addr + size > GS::kVramSize)
    {
        return 0;
    }

    *out_addr = addr;
    *out_size = size;
    return 1;
}

void MikuPan_GsUpload(sceGsLoadImage *image_load, unsigned char *image)
{
    if (MikuPan_IsGsUploadsDisabled())
    {
        return;
    }

    SPDLOG_DEBUG("GS upload request for DBP {:#x} DPSM {} X: {} Y: {}",
                 static_cast<int>(image_load->bitbltbuf.DBP),
                 static_cast<int>(image_load->bitbltbuf.DPSM),
                 static_cast<int>(image_load->trxreg.RRW),
                 static_cast<int>(image_load->trxreg.RRH));

    MikuPan_FirstUploadDone();

    {
        int up_addr;
        int up_size;

        if (MikuPan_GetUploadRegion(image_load, &up_addr, &up_size))
        {
            MikuPan_TextureCache_InvalidateGsRange(up_addr, up_size);
            g_pending_uploads.push_back({up_addr, up_size});
        }
    }

    Uint64 t0 = SDL_GetPerformanceCounter();
    int upload_w = static_cast<int>(image_load->trxreg.RRW);
    int upload_h = static_cast<int>(image_load->trxreg.RRH);

    if (!gsHelper.Upload(image_load->bitbltbuf.DPSM,
                         image_load->bitbltbuf.DBP, image_load->bitbltbuf.DBW,
                         image_load->trxpos.DSAX, image_load->trxpos.DSAY,
                         image_load->trxreg.RRW, image_load->trxreg.RRH,
                         image))
    {
        spdlog::info("Texture Transfer Upload Info FAILED: DPSM {:#x}", static_cast<int>(image_load->bitbltbuf.DPSM));
    }

    Uint64 t1 = SDL_GetPerformanceCounter();
    g_gs_frame_metrics.upload_count++;
    g_gs_frame_metrics.upload_bytes += upload_w * upload_h * 4; // approximate
    g_gs_frame_metrics.upload_ms += (double)(t1 - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

unsigned char *MikuPan_GsDownloadTexture(sceGsTex0 *tex0, uint64_t* hash)
{
    if (!MikuPan_IsFirstUploadDone())
    {
        return nullptr;
    }

    int width = (1 << tex0->TW);
    int height = (1 << tex0->TH);

    if (width > 4096 || height > 4096)
    {
        return nullptr;
    }

    SPDLOG_DEBUG("GS download request for DBP {:#x} CBP {:#x} DPSM {} ",
                 static_cast<int>(tex0->TBP0),
                 static_cast<int>(tex0->CBP),
                 static_cast<int>(tex0->PSM));

    *hash = MikuPan_GetTextureHash(tex0);

    Uint64 t0 = SDL_GetPerformanceCounter();

    unsigned char *image_data =
        static_cast<unsigned char *>(malloc((size_t)width * height * 4));

    if (image_data == nullptr
        || !gsHelper.DecodeTexture(MikuPan_ToGsTex0(tex0), image_data))
    {
        SPDLOG_DEBUG(
            "Texture Transfer Upload Failed, Unsupported Format: DPSM {:#x}", static_cast<int>(tex0->PSM));
        free(image_data);
        *hash = 0;
        return nullptr;
    }

    Uint64 t1 = SDL_GetPerformanceCounter();
    g_gs_frame_metrics.download_count++;
    g_gs_frame_metrics.download_bytes += width * height * 4;
    g_gs_frame_metrics.download_ms += (double)(t1 - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();

    return image_data;
}

uint64_t MikuPan_GetTextureHash(sceGsTex0 *tex0)
{
    int addr;
    int size;
    int clut_addr;
    int clut_size;

    if (!MikuPan_IsFirstUploadDone())
    {
        return 0;
    }

    if (!MikuPan_GetRawTextureRegion(tex0, &addr, &size))
    {
        return 0;
    }

    XXH64_hash_t hash = XXH3_64bits(&gsHelper.mem_[addr], size);

    if (MikuPan_GetClutRegion(tex0, &clut_addr, &clut_size))
    {
        uint64_t tex0_value = 0;
        const XXH64_hash_t clut_hash =
            XXH3_64bits(&gsHelper.mem_[clut_addr], clut_size);
        memcpy(&tex0_value, tex0, sizeof(tex0_value));
        const uint64_t parts[3] = {
            tex0_value,
            static_cast<uint64_t>(hash),
            static_cast<uint64_t>(clut_hash),
        };
        hash = XXH3_64bits(parts, sizeof(parts));
    }

    return hash;
}

extern "C"
{

void MikuPan_GsResetFrameMetrics(void)
{
    g_gs_last_frame = g_gs_frame_metrics;
    g_gs_frame_metrics.upload_count   = 0;
    g_gs_frame_metrics.upload_bytes   = 0;
    g_gs_frame_metrics.upload_ms      = 0.0;
    g_gs_frame_metrics.download_count = 0;
    g_gs_frame_metrics.download_bytes = 0;
    g_gs_frame_metrics.download_ms    = 0.0;
}

int    MikuPan_GsGetUploadCount(void)    { return g_gs_last_frame.upload_count; }
int    MikuPan_GsGetUploadBytes(void)    { return g_gs_last_frame.upload_bytes; }
float  MikuPan_GsGetUploadMs(void)       { return (float)g_gs_last_frame.upload_ms; }
int    MikuPan_GsGetDownloadCount(void)  { return g_gs_last_frame.download_count; }
int    MikuPan_GsGetDownloadBytes(void)  { return g_gs_last_frame.download_bytes; }
float  MikuPan_GsGetDownloadMs(void)     { return (float)g_gs_last_frame.download_ms; }

int MikuPan_GsHasPendingUploads(void)
{
    return g_pending_uploads.empty() ? 0 : 1;
}

void MikuPan_GsConsumePendingUploads(
    void (*cb)(int addr, int size, void *ud), void *ud)
{
    if (cb != NULL)
    {
        for (const auto &r : g_pending_uploads)
        {
            cb(r.addr, r.size, ud);
        }
    }

    g_pending_uploads.clear();
}

void MikuPan_GetTextureGsRegion(sceGsTex0 *tex0, int *out_addr, int *out_size)
{
    if (!MikuPan_GetTextureInvalidationRegion(tex0, out_addr, out_size))
    {
        *out_addr = 0;
        *out_size = 0;
    }
}

void MikuPan_GsStore(sceGsStoreImage *sp, unsigned char *out)
{
    const int sbp  = static_cast<int>(sp->bitbltbuf.SBP);
    const int sbw  = static_cast<int>(sp->bitbltbuf.SBW);
    const int spsm = static_cast<int>(sp->bitbltbuf.SPSM);

    const int x = static_cast<int>(sp->trxpos.SSAX);
    const int y = static_cast<int>(sp->trxpos.SSAY);
    const int w = static_cast<int>(sp->trxreg.RRW);
    const int h = static_cast<int>(sp->trxreg.RRH);

    switch (static_cast<PixelStorageFormat>(spsm))
    {
        case PSMZ32:
        case PSMCT32:
            gsHelper.DownloadPSMCT32(out, sbp, sbw, x, y, w, h);
            break;

        case PSMZ16:
        case PSMZ16S:
        case PSMCT16:
        case PSMCT16S:
            gsHelper.StorePSMCT16(out, sbp, sbw, x, y, w, h);
            break;

        default:
            spdlog::warn("Unsupported sceGsExecStoreImage SPSM: {:#x}", spsm);
            memset(out, 0, w * h * 4);
            break;
    }
}

}
