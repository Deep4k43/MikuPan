# Baked textures: format and behaviour (implemented 2026-10-03)

The authoritative definitions are in code:
`src/mikupan/bake/mikupan_bake_format.h` (format), `mikupan_bake.cpp`
(baker), `src/mikupan/rendering/mikupan_baked_texture.cpp` (runtime). This
file explains them and records why they look the way they do. It covers the
model textures; the 2D textures that share the pack are in
`2d-textures.md`.

## Requirements (from the user)

1. **No GS for model textures.** Rendering rooms, furniture, doors, items,
   characters or the player uploads nothing to the emulated GS VRAM and
   decodes nothing from it.
2. **No hashing of any form at runtime.** A mesh names its texture with an
   integer id that indexes an array. No content hash, hash map or search.
   Offline tools may hash.
3. **The SGD format stays intact.** Rewritten entries keep their size, and
   nothing inside them moves. Rooms, furniture and doors share a fixed
   0x2F0000-byte block (`RoomMdlLoadReq`).
4. **Same pixels as today**, except black & white mode, which may differ by
   1 in 255 (shader gray vs the disc's gray CLUT).

User decision (2026-10-02): **auto-bake on first launch, then require baked
data.** There is no unbaked fallback at the mesh bind sites.

## 1. Mesh texture slot

The 16 data bytes of each textured mesh's A+D packet (unit + 0x28 for
0x02/0x0A/0x82; unit + 0x38 for 0x12/0x32 units with the packet) become:

```
+0x00 u32 id      directory index; 0xFFFFFFFF = bind nothing (keep binding)
+0x04 u32 info    bits 0-15 animation frames (0 = static)
                  bit 16 BW          gray the texel in black & white mode
                  bit 17 COSTUME     add the loaded player costume
                  bit 18 PLAYER_CLUT BW follows the player CLUT state
+0x08 u32 0x7F    baked marker (was 6 = TEX0_1; SgdAddTexOffset skips it)
+0x0C u32 0
```

Bind rule (`MikuPan_BakedTexture_BindMesh`), the whole runtime lookup:

```
if (id == NO_TEXTURE) return
if (info & COSTUME) id += player_costume
if (frames > 1)     id += clamp(animation_frame, 0, frames - 1)
bind(textures[id] or create it from directory[owner])
bw_texel = (info & BW) && ((info & PLAYER_CLUT) ? player_bw : loadbw_flg)
```

**PLAYER_CLUT is an addition to the original design.** The player's BW state
is what `SetPlyrClut` last did, not `loadbw_flg`. A player loaded while BW
mode is already on keeps colour today, because the costume TIM2s upload
colour CLUTs and `RequestBlackWhiteMode` returns early. The bit lets the
runtime mirror that exactly.

The renderer's preset condition (slot read when unit + 0x38 lies before the
colour data) and "unit carries the packet" agree on every mesh of both
regions (checked). Preset units without the packet keep inheriting the
previous texture and BW flag, as before.

## 2. Pack: IMG_TEX.BIN

Location: the user folder (`MikuPan_ResolveUserPath`, i.e.
`SDL_GetPrefPath("Mikompilation", "MikuPan")`, `%APPDATA%` on Windows).
An Android data folder is a read-only SAF tree, so the pack cannot live
next to `IMG_BD.BIN`. About 200 MB.

```
PackHeader (128 bytes)
  magic "MPTX", version 4, build_stamp (u64)
  texture_count, entry_count (= IMG_HD.BIN entries)
  texture_directory_offset   -> TextureEntry[texture_count]  {u64 offset; u16 w, h; u32 owner}
  entry_directory_offset     -> EntryOverride[entry_count]   {u64 offset; u32 size; u32 0}
  source_hd_offset/size      -> copy of the IMG_HD.BIN baked from
  source_bd_size
  pack_size                  -> detects truncation
  probe_offset               -> EntryProbe[entry_count]      16 bytes each
  2D (version 4, 0x50-0x7F; see 2d-textures.md):
  tex2d_directory_offset     -> Tex2DEntry[tex2d_count]      {u64 offset; u64 tex0; u16 w, h; u32 derived_first, derived_count, 0}
  tex2d_count, derived_count
  derived_offset             -> Tex2DDerived[derived_count]  {u64 tex0; u32 id, check_block, check_owner, 0}
  patch_offset               -> PatchRecord[patch_count]     {u32 offset, 0; u8 before[16], after[16]}
  patch_range_offset         -> PatchRange[entry_count]      {u32 first, count}
  patch_count
data: RGBA8 images (alpha already expanded), rewritten archive entries,
      2D pixels (pictures stored as pixels, derived textures)
```

Version 1 held the model textures only. Versions 2 and 3 were development
builds of the 2D format and were never released; a pack of any other
version is baked again.

- **Entry overlay (an addition).** The pack carries every archive entry the
  baker rewrote, at its original size. `ICdvdTryWholeFileLoad` asks
  `MikuPan_BakedTexture_ReadEntry(file_no, start_sector, size, dst)` first,
  which indexes `entries[file_no]` directly. The user's `IMG_BD.BIN` is never
  modified. Requirement 3 still holds: the bytes the game loads are the
  in-place rewrite. `sgdstitch bake --archive` can also write a patched
  `IMG_BD.BIN` for offline tools.
- **Probes (an addition).** For each overridden entry, the 16 source bytes
  at `ProbeOffset(size)` (middle, 16-aligned). An original and a stitched
  `IMG_BD.BIN` have the same `IMG_HD.BIN` and size. At startup, comparing the
  probes with `IMG_BD.BIN` catches a swapped archive (a byte comparison, not
  a hash).
- `owner` lets consecutive ids (frames, costumes) alias one image.
  `textures[id]` shares its owner's GPU texture.

Startup validity (`OpenPack`):

- magic, version, `pack_size == file size`;
- every table (offset, count) is bounds-checked against the file size
  before anything is allocated from header values;
- the stored `IMG_HD.BIN` equals the current one and `bd_size` matches;
- every probe matches and every directory entry is in range;
- 2D: every `Tex2DEntry` has a size of 1-4096 and its pixels (if stored) and
  derived records in range, every derived texture names an entry with
  stored pixels, and every patch lies inside its entry.

Anything else means bake again. Delete `IMG_TEX.BIN` to force a re-bake.

## 3. Texture lists (what replaced the uploads)

`TextureListHeader` (32 bytes): `"MPTX"`, count, build_stamp, costume,
flags (bit 0 = player costume list), 2 reserved words, then `u32 ids[]`.
Runtime use of reserved[0]: `LoadTextureAnimation` stores the current frame
there.

| Container | Baked |
|-----------|-------|
| SGD block 0, first TRI2 unit (10) | category 0x50, list at unit + 16, rest zeroed |
| other TRI2 units, mono CLUT units (13) | category 0x51, payload zeroed |
| texture animation unit (12) | parameters kept, list at the old frame data |
| `.mdl` type-6 TIM2 pak (incl. nested m055) | pak count 0, list at + 16 |
| player costume `.pk2` | pak count 0, list at + 16, costume set, flags = player |
| `.clt`/`.bwc` | unchanged; `SetPlyrClut` now only records the BW state |

Count-0 TIM2 paks already exist in the original data (the shadow-only
models `m030_sakkage`, `m060_tenakage`). They are filled with 0xFF, not a
list, so the runtime's magic check ignores them.

## 4. Runtime (`mikupan_baked_texture.cpp`)

- `MikuPan_BakedTexture_Prepare()` is called from
  `MikuPan_TryInitializeGame` until it returns 1. It loads the pack, or
  starts `BakeTextures` on a `std::thread` and shows
  `MikuPan_DrawTextureBakeUi()` (progress, or the error with Retry). The
  bake writes `IMG_TEX.BIN.part` and renames it on success. Quitting during
  a bake cancels it.
- Lists: `case 0x50` in `SgSortPreProcess`/`SgSortPreProcessP`,
  `LoadTextureAnimation` and `SetManmdlTm2`. Each creates the GPU textures
  it names. A stamp mismatch logs once and creates nothing.
- Texture animation: `LoadTextureAnimation` keeps the state machine and sets
  the frame. The non-P walker (shadow passes) gets `case 12 ->
  SelectTextureAnimationFrame`, which reads the stored frame without
  advancing it.
- Player: a costume list sets the costume and resets player BW to colour.
  `SetPlyrClut(bw)` sets player BW.
- BW: `uFlags3.x` in `MikuPanUniforms` (HLSL and `MikuPan_GPUUniformBlock`),
  set by `MikuPan_GPUSetBlackWhiteTexel`, which marks the fragment uniforms
  dirty only on change. `textured_mesh_lighted.frag.hlsl` applies
  `ToBlackWhite` right after sampling.
- Lifetime: GPU textures live for the session. The worst case is 2,073
  images (117.6 MB), well under `MIKUPAN_GPU_MAX_TEXTURES` (8192).
- Texture creation goes through the current frame's command buffer. Lists
  are loaded on the main thread during `GameMain`/drawing, where
  `MikuPan_SetTexture` already created textures.

## 5. Baker (`src/mikupan/bake/`, also `sgdstitch bake`)

- Contexts (one VRAM state per selector value):
  - generic files (rooms, furniture, doors, items, `.acs`,
    `m900_onnryou.mpk`): one context per SGD with block-0 upload units;
    SGDs without any join the previous context (a room's far and shadow
    models);
  - `.mdl`: per type-5/type-6 pair, TIM2 at offset 0;
  - player `.mpk` (by name): one state per costume `.pk2`; BW = pk2 plus its
    `.bwc`; also checks that pk2 plus `.clt` decodes identically (it does).
- Animation: state k = TRI2 plus frames 0..k (cumulative, like today).
  Mono CLUTs go after the frames (the P-walker order); no file has both.
- Exact coverage: VRAM tracks which nibbles uploads wrote. A decode counts
  texels whose index or CLUT entry nobody wrote. The only cases are 7
  non-power-of-two images inside power-of-two TEX0s (e.g. 63x32 in 64x32);
  the missing columns or rows decode as zero VRAM (CLUT entry 0). Today
  they show stale VRAM (zero after boot).
- Bind-nothing rule: `!GS::GetRawTextureRegion || !GS::IsDecodableTexture`
  gives id 0xFFFFFFFF (6 slots, all in `m050_lte.mdl`).
- BW: per state, gray within ±1 with equal alpha sets the flag; equal to
  colour leaves it clear; anything else fails the bake. With one flag for
  all variants, a variant whose BW equals its colour must already be gray.
  No file failed.
- Ids: images deduped by content (64-bit hash, confirmed by byte compare
  against the pack); variant groups deduped by owner tuple; stable in
  archive order. Player `.pk2`s are written after their `.mpk` (pass 2).
- Verify pass (default on, also in game): re-plans and re-decodes every
  rewritten entry. It reads the entry and images back from the pack, checks
  each slot for every state under the runtime bind rule, checks the lists,
  and checks that no byte outside slots and containers changed.
- Speed: about 1.2 s bake plus 1.6 s verify (Release, MinGW); 17 s in an
  MSVC Debug build (before the per-file image cache in verify).
  Deterministic: the same stamp gives byte-identical packs.

## 6. Settled questions

- Fallback: none; auto-bake plus required baked data (user).
- Baker location: library `src/mikupan/bake/`, linked by the game and by
  `tools/sgdstitch` (`bake` mode).
- Draw ordering vs the animation frame: binds happen while the model's own
  units are walked, right after its pre-process; other passes re-select
  the stored frame.
- Other readers of model VRAM: none found. Effects sample framebuffer
  copies (TBP 0x1A40/0x8C0/0), not model textures. Measured: zero model
  uploads in a 3D frame (see `ab-validation.md`).
- Remaining GS users: none at run time since the 2D change
  (`2d-textures.md`); `mikupan_gs_memory.*` serves the baker only.
- Enemy texture animation (`.mdl` type-4, only `m031_hairei.mdl`) never ran
  before baking: `motEneTexAnm` returns at `i == 3` before
  `SetManmdlTm2`, a decompilation artefact. The bake keeps the static
  textures.
