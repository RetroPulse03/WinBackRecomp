// File-backed Controller Pak (osPfs*), replacing librecomp's "no pak" stubs.
//
// WinBack saves to the Controller Pak. Each controller port gets one pak
// image on disk (<config>/saves/controller_pak_<port>.bin) holding up to 16
// notes in 123 pages of 256 bytes, like a real 32 KiB pak. Only the libultra
// file API is emulated; the game never touches the pak's raw blocks.

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"

namespace {

constexpr int num_ports = 4;
constexpr int max_notes = 16;
constexpr int page_size = 256;
constexpr int total_pages = 123;

enum PfsError : int32_t {
    PFS_OK = 0,
    PFS_ERR_NOPACK = 1,
    PFS_ERR_INVALID = 5,
    PFS_DATA_FULL = 7,
    PFS_DIR_FULL = 8,
    PFS_ERR_EXIST = 9,
};

// OSPfs field offsets (libultra layout).
constexpr int pfs_status = 0x00;
constexpr int pfs_queue = 0x04;
constexpr int pfs_channel = 0x08;

struct Note {
    uint32_t used = 0;
    uint32_t game_code = 0;
    uint16_t company_code = 0;
    uint8_t ext_name[4]{};
    uint8_t game_name[16]{};
    std::vector<uint8_t> data;
};

struct Pak {
    bool loaded = false;
    std::array<Note, max_notes> notes{};
};

std::mutex pak_mutex;
std::array<Pak, num_ports> paks{};

std::filesystem::path pak_path(int port) {
    return recomp::get_config_path() / "saves" / ("controller_pak_" + std::to_string(port + 1) + ".bin");
}

// On-disk format: "WBPK", then per note: used, game_code, company_code,
// ext_name, game_name, size, data.
void load(int port) {
    Pak& p = paks[port];
    if (p.loaded) {
        return;
    }
    p.loaded = true;
    std::ifstream f(pak_path(port), std::ios::binary);
    char magic[4];
    if (!f.read(magic, 4) || std::memcmp(magic, "WBPK", 4) != 0) {
        return;
    }
    for (Note& n : p.notes) {
        uint32_t size = 0;
        f.read(reinterpret_cast<char*>(&n.used), 4);
        f.read(reinterpret_cast<char*>(&n.game_code), 4);
        f.read(reinterpret_cast<char*>(&n.company_code), 2);
        f.read(reinterpret_cast<char*>(n.ext_name), 4);
        f.read(reinterpret_cast<char*>(n.game_name), 16);
        f.read(reinterpret_cast<char*>(&size), 4);
        n.data.resize(size);
        f.read(reinterpret_cast<char*>(n.data.data()), size);
    }
}

void save(int port) {
    std::filesystem::path path = pak_path(port);
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write("WBPK", 4);
        for (const Note& n : paks[port].notes) {
            uint32_t size = static_cast<uint32_t>(n.data.size());
            f.write(reinterpret_cast<const char*>(&n.used), 4);
            f.write(reinterpret_cast<const char*>(&n.game_code), 4);
            f.write(reinterpret_cast<const char*>(&n.company_code), 2);
            f.write(reinterpret_cast<const char*>(n.ext_name), 4);
            f.write(reinterpret_cast<const char*>(n.game_name), 16);
            f.write(reinterpret_cast<const char*>(&size), 4);
            f.write(reinterpret_cast<const char*>(n.data.data()), size);
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
}

int used_pages(const Pak& p) {
    int pages = 0;
    for (const Note& n : p.notes) {
        if (n.used) {
            pages += static_cast<int>(n.data.size() / page_size);
        }
    }
    return pages;
}

int port_of(uint8_t* rdram, gpr pfs) {
    int port = MEM_W(pfs_channel, pfs);
    return (port >= 0 && port < num_ports) ? port : 0;
}

void read_bytes(uint8_t* rdram, gpr addr, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out[i] = MEM_BU(i, addr);
    }
}

void write_bytes(uint8_t* rdram, gpr addr, const uint8_t* in, size_t n) {
    for (size_t i = 0; i < n; i++) {
        MEM_B(i, addr) = in[i];
    }
}

gpr stack_arg(uint8_t* rdram, recomp_context* ctx, int index) {
    return (gpr)(int32_t)MEM_W(index * 4, ctx->r29);
}

int find_note(const Pak& p, uint16_t company, uint32_t game, const uint8_t* name, const uint8_t* ext) {
    for (int i = 0; i < max_notes; i++) {
        const Note& n = p.notes[i];
        if (n.used && n.company_code == company && n.game_code == game &&
            std::memcmp(n.game_name, name, 16) == 0 && std::memcmp(n.ext_name, ext, 4) == 0) {
            return i;
        }
    }
    return -1;
}

} // namespace

// s32 osPfsInitPak(OSMesgQueue* mq, OSPfs* pfs, int channel)
extern "C" void osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr pfs = ctx->r5;
    int channel = static_cast<int>(ctx->r6);
    if (channel < 0 || channel >= num_ports) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    MEM_W(pfs_status, pfs) = 1;  // PFS_INITIALIZED
    MEM_W(pfs_queue, pfs) = static_cast<int32_t>(ctx->r4);
    MEM_W(pfs_channel, pfs) = channel;
    std::lock_guard lock{pak_mutex};
    load(channel);
    ctx->r2 = PFS_OK;
}

// s32 osPfsRepairId(OSPfs* pfs)
extern "C" void osPfsRepairId_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = PFS_OK;
}

// s32 osPfsChecker(OSPfs* pfs)
extern "C" void osPfsChecker_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = PFS_OK;
}

// s32 osPfsFindFile(OSPfs* pfs, u16 company, u32 game, u8* name, u8* ext, s32* file_no)
extern "C" void osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint8_t name[16], ext[4];
    read_bytes(rdram, ctx->r7, name, 16);
    read_bytes(rdram, stack_arg(rdram, ctx, 4), ext, 4);
    gpr file_no = stack_arg(rdram, ctx, 5);
    std::lock_guard lock{pak_mutex};
    Pak& p = paks[port_of(rdram, ctx->r4)];
    load(port_of(rdram, ctx->r4));
    int idx = find_note(p, static_cast<uint16_t>(ctx->r5), static_cast<uint32_t>(ctx->r6), name, ext);
    if (idx < 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    MEM_W(0, file_no) = idx;
    ctx->r2 = PFS_OK;
}

// s32 osPfsAllocateFile(OSPfs* pfs, u16 company, u32 game, u8* name, u8* ext, int size, s32* file_no)
extern "C" void osPfsAllocateFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint8_t name[16], ext[4];
    read_bytes(rdram, ctx->r7, name, 16);
    read_bytes(rdram, stack_arg(rdram, ctx, 4), ext, 4);
    int size = static_cast<int32_t>(stack_arg(rdram, ctx, 5));
    gpr file_no = stack_arg(rdram, ctx, 6);
    uint16_t company = static_cast<uint16_t>(ctx->r5);
    uint32_t game = static_cast<uint32_t>(ctx->r6);
    if (size <= 0 || company == 0 || game == 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    int port = port_of(rdram, ctx->r4);
    std::lock_guard lock{pak_mutex};
    load(port);
    Pak& p = paks[port];
    if (find_note(p, company, game, name, ext) >= 0) {
        ctx->r2 = PFS_ERR_EXIST;
        return;
    }
    int pages = (size + page_size - 1) / page_size;
    if (used_pages(p) + pages > total_pages) {
        ctx->r2 = PFS_DATA_FULL;
        return;
    }
    for (int i = 0; i < max_notes; i++) {
        Note& n = p.notes[i];
        if (!n.used) {
            n.used = 1;
            n.company_code = company;
            n.game_code = game;
            std::memcpy(n.game_name, name, 16);
            std::memcpy(n.ext_name, ext, 4);
            n.data.assign(static_cast<size_t>(pages) * page_size, 0);
            save(port);
            MEM_W(0, file_no) = i;
            ctx->r2 = PFS_OK;
            return;
        }
    }
    ctx->r2 = PFS_DIR_FULL;
}

// s32 osPfsDeleteFile(OSPfs* pfs, u16 company, u32 game, u8* name, u8* ext)
extern "C" void osPfsDeleteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint8_t name[16], ext[4];
    read_bytes(rdram, ctx->r7, name, 16);
    read_bytes(rdram, stack_arg(rdram, ctx, 4), ext, 4);
    int port = port_of(rdram, ctx->r4);
    std::lock_guard lock{pak_mutex};
    load(port);
    int idx = find_note(paks[port], static_cast<uint16_t>(ctx->r5), static_cast<uint32_t>(ctx->r6), name, ext);
    if (idx < 0) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    paks[port].notes[idx] = Note{};
    save(port);
    ctx->r2 = PFS_OK;
}

// s32 osPfsReadWriteFile(OSPfs* pfs, s32 file_no, u8 flag, int offset, int size, u8* buffer)
extern "C" void osPfsReadWriteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    int file_no = static_cast<int32_t>(ctx->r5);
    bool write = (ctx->r6 & 0xFF) == 1;  // PFS_WRITE
    int offset = static_cast<int32_t>(ctx->r7);
    int size = static_cast<int32_t>(stack_arg(rdram, ctx, 4));
    gpr buffer = stack_arg(rdram, ctx, 5);
    int port = port_of(rdram, ctx->r4);
    std::lock_guard lock{pak_mutex};
    load(port);
    if (file_no < 0 || file_no >= max_notes || !paks[port].notes[file_no].used) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    Note& n = paks[port].notes[file_no];
    if (offset < 0 || size < 0 || static_cast<size_t>(offset) + size > n.data.size()) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    if (write) {
        read_bytes(rdram, buffer, n.data.data() + offset, size);
        save(port);
    }
    else {
        write_bytes(rdram, buffer, n.data.data() + offset, size);
    }
    ctx->r2 = PFS_OK;
}

// s32 osPfsFileState(OSPfs* pfs, s32 file_no, OSPfsState* state)
extern "C" void osPfsFileState_recomp(uint8_t* rdram, recomp_context* ctx) {
    int file_no = static_cast<int32_t>(ctx->r5);
    gpr state = ctx->r6;
    int port = port_of(rdram, ctx->r4);
    std::lock_guard lock{pak_mutex};
    load(port);
    if (file_no < 0 || file_no >= max_notes || !paks[port].notes[file_no].used) {
        ctx->r2 = PFS_ERR_INVALID;
        return;
    }
    const Note& n = paks[port].notes[file_no];
    // OSPfsState: u32 file_size, u32 game_code, u16 company_code, char ext_name[4], char game_name[16]
    MEM_W(0x00, state) = static_cast<int32_t>(n.data.size());
    MEM_W(0x04, state) = static_cast<int32_t>(n.game_code);
    MEM_H(0x08, state) = static_cast<int16_t>(n.company_code);
    write_bytes(rdram, state + 0x0A, n.ext_name, 4);
    write_bytes(rdram, state + 0x0E, n.game_name, 16);
    ctx->r2 = PFS_OK;
}

// s32 osPfsFreeBlocks(OSPfs* pfs, s32* bytes_free)
extern "C" void osPfsFreeBlocks_recomp(uint8_t* rdram, recomp_context* ctx) {
    int port = port_of(rdram, ctx->r4);
    std::lock_guard lock{pak_mutex};
    load(port);
    MEM_W(0, ctx->r5) = (total_pages - used_pages(paks[port])) * page_size;
    ctx->r2 = PFS_OK;
}

// s32 osPfsNumFiles(OSPfs* pfs, s32* max_files, s32* files_used)
extern "C" void osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx) {
    int port = port_of(rdram, ctx->r4);
    std::lock_guard lock{pak_mutex};
    load(port);
    int used = 0;
    for (const Note& n : paks[port].notes) {
        used += n.used ? 1 : 0;
    }
    MEM_W(0, ctx->r5) = max_notes;
    MEM_W(0, ctx->r6) = used;
    ctx->r2 = PFS_OK;
}
