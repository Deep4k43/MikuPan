#ifndef MIKUPAN_BAKED_TEXTURE_H
#define MIKUPAN_BAKED_TEXTURE_H

/// Baked model textures (IMG_TEX.BIN, see src/mikupan/bake/
/// mikupan_bake_format.h). The pack is baked from the user's data on first
/// launch and kept next to the configuration. The archive entries it
/// rewrote are served from the pack, and every mesh names its texture by an
/// id: binding one is an array index. Model textures never touch the
/// emulated GS.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    MIKUPAN_BAKE_IDLE,   ///< not checked yet
    MIKUPAN_BAKE_RUNNING,///< baking on a worker thread
    MIKUPAN_BAKE_READY,  ///< pack loaded
    MIKUPAN_BAKE_FAILED, ///< see MikuPan_BakedTexture_GetError()
} MikuPan_BakeState;

/// Called every frame until it returns 1, before the game starts: loads the
/// pack, or starts the bake on a worker thread when the pack is missing or
/// was baked from other data, then loads its result.
int MikuPan_BakedTexture_Prepare(void);
MikuPan_BakeState MikuPan_BakedTexture_GetState(void);
/// Progress of the current bake stage, 0..1, and the stage name.
float MikuPan_BakedTexture_GetProgress(const char **stage);
const char *MikuPan_BakedTexture_GetError(void);
/// Starts again after a failed bake.
void MikuPan_BakedTexture_Retry(void);

/// CDVD loader: copies `size` bytes of archive entry `file_no`, starting at
/// sector `start_sector` of IMG_BD.BIN, from the pack. Returns 0 when the
/// pack does not hold that entry (read IMG_BD.BIN instead).
int MikuPan_BakedTexture_ReadEntry(int file_no, unsigned int start_sector,
                                   unsigned int size, void *dst);

/// Mesh bind sites: binds the texture a mesh texture slot (the 16 data bytes
/// of its A+D packet) names. kNoTexture keeps the current binding.
void MikuPan_BakedTexture_BindMesh(const void *slot);

/// Texture lists (MikuPanBake::TextureListHeader): creates a model's GPU
/// textures when it loads. A player costume list also selects the costume.
int MikuPan_BakedTexture_IsList(const void *data);
void MikuPan_BakedTexture_LoadList(const void *list);

/// LoadTextureAnimation(): the frame of the model being drawn.
void MikuPan_BakedTexture_SetAnimationFrame(int frame);
/// SetPlyrClut(): 1 while the player shows its black & white CLUTs.
void MikuPan_BakedTexture_SetPlayerBlackWhite(int black_white);

/// Waits for (or cancels) a running bake and releases the textures.
void MikuPan_BakedTexture_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif// MIKUPAN_BAKED_TEXTURE_H
