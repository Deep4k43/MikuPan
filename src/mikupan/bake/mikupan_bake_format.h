#ifndef MIKUPAN_BAKE_FORMAT_H
#define MIKUPAN_BAKE_FORMAT_H

#include <cstdint>

/// IMG_TEX.BIN, the baked model texture pack, and the baked form of the SGD
/// data that refers to it. Written by the texture baker
/// (mikupan_bake.cpp), read by the game. Everything is little endian.
///
/// The pack holds:
///   - every model texture, decoded once to RGBA8 with the PS2 alpha already
///     expanded (what MikuPan_GPUCreateTextureRGBA8() takes);
///   - a copy of every archive entry the baker rewrote, same size as the
///     original. The game reads those entries from the pack instead of
///     IMG_BD.BIN, so the user's data files are never modified;
///   - the IMG_HD.BIN the pack was baked from, and 16 bytes from the middle
///     of every source entry it rewrote, to detect a changed data set (an
///     original and a stitched IMG_BD.BIN have the same IMG_HD.BIN and
///     size).
///
/// A baked mesh names its texture with an id that indexes the texture
/// directory. Finding a texture is one array index: no hashing, no search.

namespace MikuPanBake
{

constexpr char kPackMagic[4] = {'M', 'P', 'T', 'X'};
constexpr uint32_t kPackVersion = 1;

/// File name of the pack, next to the user's configuration.
constexpr const char* kPackFileName = "IMG_TEX.BIN";

/// --- Mesh texture slot ------------------------------------------------------
///
/// Every textured mesh carries one GIF A+D packet that sets TEX0_1. Its 16
/// data bytes are at mesh unit + 0x28 (mesh types 0x02/0x0A/0x82) or + 0x38
/// (preset 0x12/0x32 units that carry the packet). Baked, they hold
/// { u32 id, u32 info, u32 kSlotMarker, u32 0 } instead of
/// { TEX0 lo, TEX0 hi, 6 (TEX0_1), 0 }.

/// Slot word 2. 6 (the TEX0_1 register) means "not baked"; SgdAddTexOffset()
/// only relocates slots whose word is 6, so baked ids are never touched.
constexpr uint32_t kSlotMarker = 0x7F;

/// Slot id: bind nothing, keep the current binding (what an undecodable TEX0
/// does today).
constexpr uint32_t kNoTexture = 0xFFFFFFFFu;

/// Slot info bits.
/// Animation frames (0 = static). Frame f binds id + min(f, frames - 1).
constexpr uint32_t kInfoFramesMask = 0xFFFFu;
/// Gray the texel in black & white mode (the disc's BW CLUT for this
/// texture is gray(colour)).
constexpr uint32_t kInfoBlackWhite = 1u << 16;
/// Add the loaded player costume (0-3) to the id.
constexpr uint32_t kInfoCostume = 1u << 17;
/// Black & white follows the player CLUT state (SetPlyrClut) instead of the
/// map black & white state (loadbw_flg).
constexpr uint32_t kInfoPlayerClut = 1u << 18;

/// --- Procedure units --------------------------------------------------------

/// SGD block 0: the first TRI2 unit (category 10) of a baked SGD becomes a
/// texture list unit; the TextureListHeader starts at unit + 16.
constexpr uint32_t kUnitTextureList = 0x50;
/// Every other upload unit (TRI2 10, mono CLUT 13) of a baked SGD. No walker
/// handles it; its payload is zeroed.
constexpr uint32_t kUnitRetired = 0x51;

/// --- Pack layout --------------------------------------------------------------

struct PackHeader
{
    char magic[4];                    ///< kPackMagic
    uint32_t version;                 ///< kPackVersion
    uint64_t build_stamp;             ///< also in every TextureListHeader
    uint32_t texture_count;           ///< texture ids are 0..texture_count-1
    uint32_t entry_count;             ///< IMG_HD.BIN entry count
    uint64_t texture_directory_offset;///< TextureEntry[texture_count]
    uint64_t entry_directory_offset;  ///< EntryOverride[entry_count]
    uint64_t source_hd_offset;        ///< copy of the source IMG_HD.BIN
    uint32_t source_hd_size;          ///< its size in bytes
    uint32_t flags;                   ///< 0
    uint64_t source_bd_size;          ///< size of the source IMG_BD.BIN
    uint64_t pack_size;               ///< size of this file
    uint64_t probe_offset;            ///< EntryProbe[entry_count]
    uint64_t reserved[2];
};
static_assert(sizeof(PackHeader) == 96, "PackHeader layout");

/// directory[id]. Several ids can share one image: animation frames and
/// costume variants need consecutive ids even when two of them are equal.
struct TextureEntry
{
    uint64_t offset;///< pixels of directory[owner]: width * height RGBA8,
                    ///< rows top-down
    uint16_t width;
    uint16_t height;
    uint32_t owner;///< id whose image this id shows (== id if unique)
};
static_assert(sizeof(TextureEntry) == 16, "TextureEntry layout");

/// entries[file id] of IMG_HD.BIN.
struct EntryOverride
{
    uint64_t offset;///< rewritten entry inside the pack
    uint32_t size;  ///< 0 = read IMG_BD.BIN; else the IMG_HD.BIN size
    uint32_t reserved;
};
static_assert(sizeof(EntryOverride) == 16, "EntryOverride layout");

/// probes[file id]: for an entry with an EntryOverride, the 16 bytes of the
/// source IMG_BD.BIN entry at ProbeOffset(size); zero otherwise.
struct EntryProbe
{
    uint8_t bytes[16];
};
static_assert(sizeof(EntryProbe) == 16, "EntryProbe layout");

/// Where an entry of `size` bytes is probed: its middle, 16-byte aligned.
inline uint32_t ProbeOffset(uint32_t size)
{
    return size < 32 ? 0 : (size / 2) & ~15u;
}

/// What replaces a texture upload container, at the same position and in
/// the same space: every id the container's meshes can bind, so the game can
/// create the GPU textures when the model loads. 16-byte aligned.
///
///   SGD block 0, first TRI2 unit  -> category kUnitTextureList, at unit + 16
///   texture animation unit (12)   -> parameters kept, at the old frame data
///   character .mdl type-6 entry   -> pak header with count 0, at entry + 16
///   player costume .pk2           -> pak header with count 0, at file + 16
struct TextureListHeader
{
    char magic[4];       ///< kPackMagic
    uint32_t count;      ///< ids that follow
    uint64_t build_stamp;///< PackHeader::build_stamp
    uint32_t costume;    ///< player costume .pk2: its costume index, else 0
    uint32_t flags;      ///< kListFlagPlayer for player costume .pk2 files
    uint32_t reserved[2];
    // uint32_t ids[count];
};
static_assert(sizeof(TextureListHeader) == 32, "TextureListHeader layout");

/// TextureListHeader::flags: the list belongs to a player costume. Loading it
/// selects the costume and resets the player CLUT state to colour, as the
/// colour CLUTs inside the costume's TIM2 files did.
constexpr uint32_t kListFlagPlayer = 1u << 0;

}// namespace MikuPanBake

#endif// MIKUPAN_BAKE_FORMAT_H
