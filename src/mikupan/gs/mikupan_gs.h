#ifndef MIKUPAN_GS_SERVER_H
#define MIKUPAN_GS_SERVER_H

#include <cstdint>
#include <vector>

#include "ee/eestruct.h"
#include "mikupan_gs_memory.h"
#include "sce/libgraph.h"

extern GS::GSHelper gsHelper;

extern "C" {
void MikuPan_GsUpload(sceGsLoadImage *image_load, unsigned char *image);
unsigned char *MikuPan_GsDownloadTexture(sceGsTex0 *tex0, uint64_t* hash);
uint64_t MikuPan_GetTextureHash(sceGsTex0 *tex0);
void MikuPan_GsStore(sceGsStoreImage *sp, unsigned char *out);
}

// Texture *DownloadGsTexture(sceGsTex0* pMeshTexReg);

#endif//MIKUPAN_GS_SERVER_H
