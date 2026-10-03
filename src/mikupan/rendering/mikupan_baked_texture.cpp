#include "mikupan_baked_texture.h"

#include "mikupan/bake/mikupan_bake.h"
#include "mikupan/bake/mikupan_bake_format.h"
#include "mikupan/io/mikupan_file.h"
#include "mikupan/mikupan_config.h"
#include "mikupan_gl_compat.h"
#include "mikupan_gpu.h"
#include "mikupan_pipeline.h"
#include "os/eeiop/cdvd/eecdvd.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern char* file_name[];
extern int loadbw_flg;

namespace
{

using namespace MikuPanBake;
namespace fs = std::filesystem;

constexpr uint64_t kSectorSize = 0x800;

/// The loaded pack. Archive entries are read on the IOP thread and texture
/// pixels on the render thread, so each has its own file handle.
struct Pack
{
    PackHeader header{};
    std::vector<TextureEntry> directory;
    std::vector<EntryOverride> entries;
    std::vector<uint32_t> entry_sectors;///< IMG_HD.BIN start sector per file
    std::mutex entry_mutex;
    std::ifstream entry_file;
    std::ifstream pixel_file;
};

std::unique_ptr<Pack> g_pack;
/// GPU texture per id, 0 until created. Aliases share their owner's texture.
std::vector<unsigned int> g_textures;
std::vector<uint8_t> g_pixels;

std::atomic<int> g_state{MIKUPAN_BAKE_IDLE};
std::thread g_bake_thread;
std::atomic<bool> g_bake_finished{false};
std::atomic<bool> g_bake_cancel{false};
std::atomic<float> g_progress{0.0f};
std::atomic<const char*> g_stage{""};
std::mutex g_error_mutex;
std::string g_error;
bool g_bake_ok = false;

int g_player_costume = 0;
int g_player_black_white = 0;
int g_animation_frame = 0;
bool g_reported_unbaked = false;
bool g_reported_stamp = false;
bool g_reported_read = false;

void SetError(const std::string& error)
{
    std::lock_guard<std::mutex> lock(g_error_mutex);
    g_error = error;
}

std::string PackPath()
{
    char path[1024];

    if (!MikuPan_ResolveUserPath(kPackFileName, path, sizeof(path)))
    {
        return std::string();
    }

    return path;
}

bool ReadAt(std::ifstream& file, uint64_t offset, void* dst, size_t size)
{
    file.clear();
    file.seekg((std::streamoff)offset);
    file.read((char*)dst, (std::streamsize)size);
    return (bool)file;
}

bool ReadDataFile(const char* name, std::vector<uint8_t>* out)
{
    const u_int size = MikuPan_GetDataFileSize(name);
    out->resize(size);
    return size != 0 && MikuPan_ReadDataFileRange(name, 0, out->data(), size);
}

/// Opens the pack and checks that it was baked from the data set the game
/// is about to load. `why` says what is missing or stale.
std::unique_ptr<Pack> OpenPack(std::string* why)
{
    const std::string path = PackPath();
    std::error_code ec;
    const uint64_t file_size = path.empty() ? 0 : fs::file_size(path, ec);

    if (path.empty() || ec || file_size < sizeof(PackHeader))
    {
        *why = "no texture pack yet";
        return nullptr;
    }

    auto pack = std::make_unique<Pack>();
    pack->pixel_file.open(path, std::ios::binary);
    PackHeader& h = pack->header;

    if (!ReadAt(pack->pixel_file, 0, &h, sizeof(h))
        || std::memcmp(h.magic, kPackMagic, 4) != 0
        || h.version != kPackVersion || h.pack_size != file_size)
    {
        *why = "the texture pack is incomplete or from another version";
        return nullptr;
    }

    /// Every table the header describes must lie inside the file before
    /// anything is allocated from its sizes.
    const auto fits = [file_size](uint64_t offset, uint64_t count,
                                  uint64_t item) {
        return count <= file_size / item && offset <= file_size
               && count * item <= file_size - offset;
    };

    if (!fits(h.source_hd_offset, h.source_hd_size, 1)
        || !fits(h.texture_directory_offset, h.texture_count,
                 sizeof(TextureEntry))
        || !fits(h.entry_directory_offset, h.entry_count,
                 sizeof(EntryOverride))
        || !fits(h.probe_offset, h.entry_count, sizeof(EntryProbe)))
    {
        *why = "the texture pack is damaged";
        return nullptr;
    }

    std::vector<uint8_t> hd;
    std::vector<uint8_t> baked_hd(h.source_hd_size);

    if (!ReadDataFile("IMG_HD.BIN", &hd) || hd.size() != h.source_hd_size
        || h.entry_count != hd.size() / 8
        || h.source_hd_offset + h.source_hd_size > file_size
        || !ReadAt(pack->pixel_file, h.source_hd_offset, baked_hd.data(),
                   baked_hd.size())
        || baked_hd != hd
        || h.source_bd_size != MikuPan_GetDataFileSize("IMG_BD.BIN"))
    {
        *why = "the texture pack was baked from other game data";
        return nullptr;
    }

    pack->directory.resize(h.texture_count);
    pack->entries.resize(h.entry_count);

    if (h.texture_directory_offset
                + (uint64_t)h.texture_count * sizeof(TextureEntry)
            > file_size
        || h.entry_directory_offset
                   + (uint64_t)h.entry_count * sizeof(EntryOverride)
               > file_size
        || !ReadAt(pack->pixel_file, h.texture_directory_offset,
                   pack->directory.data(),
                   pack->directory.size() * sizeof(TextureEntry))
        || !ReadAt(pack->pixel_file, h.entry_directory_offset,
                   pack->entries.data(),
                   pack->entries.size() * sizeof(EntryOverride)))
    {
        *why = "the texture pack is damaged";
        return nullptr;
    }

    for (uint32_t id = 0; id < h.texture_count; id++)
    {
        const TextureEntry& e = pack->directory[id];

        if (e.owner >= h.texture_count
            || pack->directory[e.owner].owner != e.owner
            || pack->directory[e.owner].offset != e.offset
            || e.offset + (uint64_t)e.width * e.height * 4 > file_size)
        {
            *why = "the texture pack is damaged";
            return nullptr;
        }
    }

    std::vector<EntryProbe> probes(h.entry_count);

    if (h.probe_offset + (uint64_t)h.entry_count * sizeof(EntryProbe)
            > file_size
        || !ReadAt(pack->pixel_file, h.probe_offset, probes.data(),
                   probes.size() * sizeof(EntryProbe)))
    {
        *why = "the texture pack is damaged";
        return nullptr;
    }

    /// The pack replaces whole archive entries, so it must have been baked
    /// from these very entries: compare a probe of each with IMG_BD.BIN.
    void* bd = MikuPan_OpenDataFile("IMG_BD.BIN");
    bool same_data = bd != nullptr;
    pack->entry_sectors.resize(h.entry_count);

    for (uint32_t i = 0; i < h.entry_count; i++)
    {
        uint32_t sector;
        uint32_t size;
        std::memcpy(&sector, &hd[i * 8], 4);
        std::memcpy(&size, &hd[i * 8 + 4], 4);
        pack->entry_sectors[i] = sector;

        const EntryOverride& o = pack->entries[i];
        if (o.size == 0)
        {
            continue;
        }

        if (o.size != size || o.offset + o.size > file_size)
        {
            MikuPan_CloseDataFile(bd);
            *why = "the texture pack is damaged";
            return nullptr;
        }

        EntryProbe probe = {};
        same_data = same_data
                    && MikuPan_ReadOpenDataFile(
                        bd, (int64_t)(sector * kSectorSize + ProbeOffset(size)),
                        probe.bytes, std::min<size_t>(size, sizeof(probe)))
                    && std::memcmp(probe.bytes, probes[i].bytes,
                                   sizeof(probe))
                           == 0;
    }

    MikuPan_CloseDataFile(bd);

    if (!same_data)
    {
        *why = "the texture pack was baked from another IMG_BD.BIN";
        return nullptr;
    }

    pack->entry_file.open(path, std::ios::binary);
    if (!pack->entry_file)
    {
        *why = "the texture pack cannot be opened";
        return nullptr;
    }

    return pack;
}

/// First-launch bake: reads the user's IMG_HD.BIN / IMG_BD.BIN and writes
/// the pack next to the configuration, through a temporary file so an
/// interrupted bake never leaves a pack that looks valid.
void BakeThread()
{
    const std::string path = PackPath();
    const std::string temp = path + ".part";
    std::string error;

    Source source;
    void* bd = MikuPan_OpenDataFile("IMG_BD.BIN");

    source.bd_size = MikuPan_GetDataFileSize("IMG_BD.BIN");
    source.read_bd = [bd](uint64_t offset, void* buffer, size_t size) {
        return MikuPan_ReadOpenDataFile(bd, (int64_t)offset, buffer, size);
    };

    for (int i = 0; i < GetFileNameCount(); i++)
    {
        source.names.push_back(file_name[i]);
    }

    std::fstream out;

    if (path.empty())
    {
        error = "no user folder to store the texture pack in";
    }
    else if (bd == nullptr || !ReadDataFile("IMG_HD.BIN", &source.hd))
    {
        error = "cannot read IMG_HD.BIN / IMG_BD.BIN";
    }
    else
    {
        out.open(temp, std::ios::binary | std::ios::in | std::ios::out
                           | std::ios::trunc);
        if (!out)
        {
            error = "cannot write " + temp;
        }
    }

    bool ok = false;

    if (error.empty())
    {
        Sink sink;
        sink.write = [&out](uint64_t offset, const void* data, size_t size) {
            out.clear();
            out.seekp((std::streamoff)offset);
            out.write((const char*)data, (std::streamsize)size);
            return (bool)out;
        };
        sink.read = [&out](uint64_t offset, void* buffer, size_t size) {
            out.flush();
            out.clear();
            out.seekg((std::streamoff)offset);
            out.read((char*)buffer, (std::streamsize)size);
            return (bool)out;
        };

        Options options;
        options.build_stamp =
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        options.verify = true;
        options.progress = [](const char* stage, size_t done, size_t total) {
            g_stage = stage;
            g_progress = total == 0 ? 1.0f : (float)done / (float)total;
        };
        options.log = [](const std::string& line) {
            spdlog::info("Texture bake: {}", line);
        };
        options.cancelled = []() { return g_bake_cancel.load(); };

        const auto t0 = std::chrono::steady_clock::now();
        Stats stats;
        ok = BakeTextures(source, options, &sink, &stats, &error);
        out.close();

        if (ok)
        {
            std::error_code ec;
            fs::remove(path, ec);
            fs::rename(temp, path, ec);

            if (ec)
            {
                ok = false;
                error = "cannot store the texture pack: " + ec.message();
            }
            else
            {
                spdlog::info(
                    "Texture bake: {} entries, {} meshes, {} images "
                    "({:.1f} MB), pack {:.1f} MB, {:.1f} s",
                    stats.files_rewritten, stats.slots, stats.images,
                    stats.image_bytes / 1e6, stats.pack_size / 1e6,
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - t0)
                        .count());
            }
        }
    }

    if (!ok)
    {
        std::error_code ec;
        out.close();
        fs::remove(temp, ec);
        SetError(error);
        spdlog::error("Texture bake failed: {}", error);
    }

    MikuPan_CloseDataFile(bd);
    g_bake_ok = ok;
    g_bake_finished = true;
}

void StartBake()
{
    g_bake_finished = false;
    g_bake_cancel = false;
    g_progress = 0.0f;
    g_stage = "bake";
    g_state = MIKUPAN_BAKE_RUNNING;
    g_bake_thread = std::thread(BakeThread);
}

unsigned int CreateTexture(uint32_t id)
{
    Pack& pack = *g_pack;
    const TextureEntry& e = pack.directory[id];
    const uint32_t owner = e.owner;

    if (g_textures[owner] == 0)
    {
        const size_t bytes = (size_t)e.width * e.height * 4;
        g_pixels.resize(bytes);

        if (!ReadAt(pack.pixel_file, e.offset, g_pixels.data(), bytes))
        {
            if (!g_reported_read)
            {
                spdlog::error("Cannot read texture {} from the texture pack",
                              id);
                g_reported_read = true;
            }

            return 0;
        }

        g_textures[owner] = MikuPan_GPUCreateTextureRGBA8(
            e.width, e.height, g_pixels.data(), e.width * 4, 1, 0);
    }

    g_textures[id] = g_textures[owner];
    return g_textures[id];
}

}// namespace

extern "C" {

int MikuPan_BakedTexture_Prepare(void)
{
    if (g_state == MIKUPAN_BAKE_READY)
    {
        return 1;
    }

    if (g_state == MIKUPAN_BAKE_FAILED
        || (g_state == MIKUPAN_BAKE_RUNNING && !g_bake_finished))
    {
        return 0;
    }

    const bool baked = g_state == MIKUPAN_BAKE_RUNNING;

    if (baked)
    {
        g_bake_thread.join();

        if (!g_bake_ok)
        {
            g_state = MIKUPAN_BAKE_FAILED;
            return 0;
        }
    }

    std::string why;
    g_pack = OpenPack(&why);

    if (g_pack != nullptr)
    {
        g_textures.assign(g_pack->header.texture_count, 0);
        g_state = MIKUPAN_BAKE_READY;
        spdlog::info("Texture pack loaded: {} textures", g_textures.size());
        return 1;
    }

    if (baked)
    {
        SetError("the new texture pack does not load: " + why);
        g_state = MIKUPAN_BAKE_FAILED;
        return 0;
    }

    spdlog::info("Baking the model textures ({})", why);
    StartBake();
    return 0;
}

MikuPan_BakeState MikuPan_BakedTexture_GetState(void)
{
    return (MikuPan_BakeState)g_state.load();
}

float MikuPan_BakedTexture_GetProgress(const char** stage)
{
    if (stage != nullptr)
    {
        *stage = g_stage.load();
    }

    return g_progress;
}

const char* MikuPan_BakedTexture_GetError(void)
{
    std::lock_guard<std::mutex> lock(g_error_mutex);
    static std::string copy;
    copy = g_error;
    return copy.c_str();
}

void MikuPan_BakedTexture_Retry(void)
{
    if (g_state == MIKUPAN_BAKE_FAILED)
    {
        SetError("");
        StartBake();
    }
}

int MikuPan_BakedTexture_ReadEntry(int file_no, unsigned int start_sector,
                                   unsigned int size, void* dst)
{
    Pack* pack = g_pack.get();

    if (pack == nullptr || file_no < 0
        || (size_t)file_no >= pack->entries.size())
    {
        return 0;
    }

    const EntryOverride& o = pack->entries[file_no];

    if (o.size == 0)
    {
        return 0;
    }

    const int64_t delta =
        ((int64_t)start_sector - pack->entry_sectors[file_no]) * kSectorSize;

    if (delta < 0 || (uint64_t)delta + size > o.size)
    {
        spdlog::warn("Archive entry {} read outside its baked copy", file_no);
        return 0;
    }

    std::lock_guard<std::mutex> lock(pack->entry_mutex);
    return ReadAt(pack->entry_file, o.offset + (uint64_t)delta, dst, size)
               ? 1
               : 0;
}

void MikuPan_BakedTexture_BindMesh(const void* slot)
{
    uint32_t w[4];
    std::memcpy(w, slot, sizeof(w));

    if (w[2] != kSlotMarker)
    {
        if (!g_reported_unbaked)
        {
            spdlog::error("A mesh with an unbaked texture slot was drawn");
            g_reported_unbaked = true;
        }

        return;
    }

    uint32_t id = w[0];
    const uint32_t info = w[1];

    if (id == kNoTexture || g_pack == nullptr)
    {
        return;
    }

    if (info & kInfoCostume)
    {
        id += (uint32_t)g_player_costume;
    }

    const uint32_t frames = info & kInfoFramesMask;
    if (frames > 1)
    {
        id += (uint32_t)std::min(std::max(g_animation_frame, 0),
                                 (int)frames - 1);
    }

    if (id >= g_textures.size())
    {
        return;
    }

    const unsigned int texture =
        g_textures[id] != 0 ? g_textures[id] : CreateTexture(id);

    if (texture == 0)
    {
        return;
    }

    MikuPan_ActiveTextureCached(GL_TEXTURE0);
    MikuPan_BindTexture2DCached(texture);

    const int black_white =
        (info & kInfoPlayerClut) ? g_player_black_white : loadbw_flg;
    MikuPan_GPUSetBlackWhiteTexel((info & kInfoBlackWhite) != 0
                                  && black_white != 0);
}

int MikuPan_BakedTexture_IsList(const void* data)
{
    return data != nullptr && std::memcmp(data, kPackMagic, 4) == 0;
}

void MikuPan_BakedTexture_LoadList(const void* list)
{
    if (!MikuPan_BakedTexture_IsList(list) || g_pack == nullptr)
    {
        return;
    }

    TextureListHeader h;
    std::memcpy(&h, list, sizeof(h));

    if (h.build_stamp != g_pack->header.build_stamp)
    {
        if (!g_reported_stamp)
        {
            spdlog::error("A model was baked into a different texture pack; "
                          "delete {} to bake again",
                          kPackFileName);
            g_reported_stamp = true;
        }

        return;
    }

    if (h.flags & kListFlagPlayer)
    {
        /// The costume's TIM2 files used to upload colour CLUTs.
        g_player_costume = (int)h.costume;
        g_player_black_white = 0;
    }

    const uint8_t* ids = (const uint8_t*)list + sizeof(h);

    for (uint32_t i = 0; i < h.count; i++)
    {
        uint32_t id;
        std::memcpy(&id, ids + i * 4, 4);

        if (id < g_textures.size() && g_textures[id] == 0)
        {
            CreateTexture(id);
        }
    }
}

void MikuPan_BakedTexture_SetAnimationFrame(int frame)
{
    g_animation_frame = frame;
}

void MikuPan_BakedTexture_SetPlayerBlackWhite(int black_white)
{
    g_player_black_white = black_white != 0;
}

void MikuPan_BakedTexture_Shutdown(void)
{
    if (g_bake_thread.joinable())
    {
        g_bake_cancel = true;
        g_bake_thread.join();
    }

    if (g_pack != nullptr)
    {
        for (size_t id = 0; id < g_textures.size(); id++)
        {
            if (g_textures[id] != 0 && g_pack->directory[id].owner == id)
            {
                MikuPan_GPUReleaseTexture(g_textures[id]);
            }
        }
    }

    g_textures.clear();
    g_pack.reset();
    g_state = MIKUPAN_BAKE_IDLE;
}

}// extern "C"
