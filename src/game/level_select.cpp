// Level Select: stage and checkpoint catalogue, jumps, checkpoint snapshots.
//
// The game keeps a checkpoint as two blocks: the mission's event flags
// (D_80137990, 0x89C bytes; checkpoint id at +0x894, its map at +0x895) and
// the progress block (D_80158760, 0x12E0 bytes; stage number at +1).
// func_8009C8E0 copies both to D_8011DAB0 / D_8011E350 when a checkpoint
// triggers, and the continue screen restores them with func_8009C998.
//
// Jumps run on the game thread at the start of a frame callback (hooks in
// config/winback.us.toml): func_80073600 in the front end (title, menus) and
// func_8004BA40 in gameplay.
//   Stage start: the game's own mission start, func_8008EF28 (reset mission
//     state) + func_8008EEAC(stage). From the front end the game then enters
//     gameplay through func_8008F29C(9), as New Game and Continue do; during
//     gameplay the map reload request (D_8016A418 = 1, map in D_8016A41A)
//     makes the frame callback reload, as a door or map-change script does.
//   Recorded checkpoint: the snapshot from saves/checkpoints/ goes into the
//     saved copies, then func_8008EF28 + func_8009C998 as on the continue
//     screen, and the same transition.
//   Basic checkpoint: a stage start; when the stage's own start snapshot is
//     taken (func_8004BD7C), its checkpoint id and map are replaced with the
//     target's and the next frame restores it like a recorded one.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>

#include "recomp.h"
#include "funcs.h"
#include "librecomp/game.hpp"

#include "level_select.hpp"

namespace fs = std::filesystem;

namespace {

// Main segment: ROM offset = vram - 0x80024C00.
constexpr uint32_t rom_to_vram = 0x80024C00;
constexpr uint32_t stage_first_map = 0x8010D5F4;   // u8[31]
constexpr uint32_t map_area_group = 0x8010D5A0;    // u8 per map
constexpr uint32_t area_names = 0x80111EF0;        // char*[4]
constexpr uint32_t script_opcodes = 0x8010DF38;    // {flags, argc, u16 wide-arg mask} per opcode
constexpr uint32_t script_handlers = 0x801194C0;   // jump table of func_8009AA9C
constexpr int script_opcode_count = 0x2C;
constexpr uint32_t checkpoint_handler = 0x8009BADC;
constexpr uint32_t load_events_handler = 0x8009BA34;
constexpr uint32_t event_archive = 0x931470;       // ROM: u32 count, u32 offsets[count + 1]
constexpr int stage_count = 31;
constexpr int map_count = 64;

constexpr uint32_t events_size = 0x89C;
constexpr uint32_t progress_block = 0x80158760;
constexpr uint32_t progress_size = 0x12E0;
constexpr uint32_t saved_events = 0x8011DAB0;
constexpr uint32_t saved_progress = 0x8011E350;
constexpr uint32_t checkpoint_id_offset = 0x894;
constexpr uint32_t checkpoint_map_offset = 0x895;

class Rom {
public:
    explicit Rom(std::span<const uint8_t> data) : data(data) {}
    uint8_t u8(uint32_t off) const { return off < data.size() ? data[off] : 0; }
    uint16_t u16(uint32_t off) const { return uint16_t(u8(off) << 8 | u8(off + 1)); }
    uint32_t u32(uint32_t off) const { return uint32_t(u16(off)) << 16 | u16(off + 2); }
    uint8_t vu8(uint32_t vram) const { return u8(vram - rom_to_vram); }
    uint32_t vu32(uint32_t vram) const { return u32(vram - rom_to_vram); }
    std::string vstring(uint32_t vram) const {
        std::string s;
        for (uint32_t off = vram - rom_to_vram; u8(off) != 0 && s.size() < 32; off++) {
            s += char(u8(off));
        }
        return s;
    }

private:
    std::span<const uint8_t> data;
};

// Finds the checkpoint opcodes in one event file. A file holds a table of
// events, each with a script of fixed-size commands; an opcode can load
// another event file (followed here) while the map stays the same.
class ScriptScanner {
public:
    explicit ScriptScanner(const Rom& rom) : rom(rom) {
        for (int op = 0; op < script_opcode_count; op++) {
            uint32_t handler = rom.vu32(script_handlers + op * 4);
            if (handler == checkpoint_handler) checkpoint_op = op;
            if (handler == load_events_handler) load_events_op = op;
        }
        file_count = rom.u32(event_archive);
    }

    void scan(int file, uint8_t map, std::vector<winback::levels::Checkpoint>& out, std::set<int>& visited) const {
        if (file < 0 || uint32_t(file) >= file_count || !visited.insert(file).second) {
            return;
        }
        uint32_t base = event_archive + rom.u32(event_archive + 4 + file * 4);
        uint16_t events = rom.u16(base);
        uint16_t scripts = rom.u16(base + 4);
        for (uint16_t e = 0; e < events; e++) {
            uint32_t p = base + scripts + rom.u16(base + 6 + e * 8 + 6);
            uint16_t commands = rom.u16(p);
            p += 2;
            for (uint16_t c = 0; c < commands; c++) {
                int op = rom.u8(p);
                if (op >= script_opcode_count) {
                    break;
                }
                // The first argument is a byte for both opcodes used here.
                uint8_t arg0 = rom.u8(p + 1);
                if (op == checkpoint_op) {
                    bool seen = false;
                    for (const auto& cp : out) {
                        seen |= cp.id == arg0 && cp.map == map;
                    }
                    if (!seen) {
                        out.push_back({arg0, map});
                    }
                }
                else if (op == load_events_op && arg0 != 0xFF) {
                    scan(arg0, map, out, visited);
                }
                p += length(op);
            }
        }
    }

private:
    uint32_t length(int op) const {
        uint32_t entry = script_opcodes + op * 4;
        int argc = rom.vu8(entry + 1);
        uint16_t wide = uint16_t(rom.vu8(entry + 2) << 8 | rom.vu8(entry + 3));
        uint32_t len = 1;
        for (int i = 0; i < argc; i++) {
            len += (wide >> i) & 1 ? 2 : 1;
        }
        return len;
    }

    const Rom& rom;
    int checkpoint_op = -1;
    int load_events_op = -1;
    uint32_t file_count = 0;
};

std::vector<winback::levels::Stage> build_catalogue() {
    Rom rom(recomp::get_rom());
    ScriptScanner scanner(rom);
    static std::array<std::string, 4> names;
    for (int i = 0; i < 4; i++) {
        names[i] = rom.vstring(rom.vu32(area_names + i * 4));
    }
    std::vector<winback::levels::Stage> out;
    for (int s = 0; s < stage_count; s++) {
        int first = rom.vu8(stage_first_map + s);
        int end = s + 1 < stage_count ? rom.vu8(stage_first_map + s + 1) : map_count;
        // The map's area group runs 0-8 outside, 9-15 in the office, 16-19
        // in the factory and 20 up in the control center.
        int group = rom.vu8(map_area_group + first);
        int area = group <= 8 ? 0 : group <= 15 ? 1 : group <= 19 ? 2 : 3;
        winback::levels::Stage stage{s, uint8_t(first), names[area].c_str(), {}};
        std::set<int> visited;
        for (int map = first; map < end; map++) {
            // Entering a map loads event file map + 1 (func_8004BD7C).
            scanner.scan(map + 1, uint8_t(map), stage.checkpoints, visited);
        }
        out.push_back(std::move(stage));
    }
    return out;
}

fs::path snapshot_path(int stage, uint8_t map, uint8_t id) {
    char name[64];
    std::snprintf(name, sizeof(name), "stage%02d_map%02d_cp%03d.bin", stage, map, id);
    return recomp::get_config_path() / "saves" / "checkpoints" / name;
}

constexpr char snapshot_magic[4] = {'W', 'B', 'C', 'P'};

// RDRAM is stored with 32-bit words byte-swapped.
uint8_t& ram_u8(uint8_t* rdram, uint32_t vram) {
    return rdram[(vram - 0x80000000) ^ 3];
}

void write_u16(uint8_t* rdram, uint32_t vram, uint16_t v) {
    ram_u8(rdram, vram) = uint8_t(v >> 8);
    ram_u8(rdram, vram + 1) = uint8_t(v);
}

void write_u32(uint8_t* rdram, uint32_t vram, uint32_t v) {
    write_u16(rdram, vram, uint16_t(v >> 16));
    write_u16(rdram, vram + 2, uint16_t(v));
}

uint16_t read_u16(uint8_t* rdram, uint32_t vram) {
    return uint16_t(ram_u8(rdram, vram) << 8 | ram_u8(rdram, vram + 1));
}

uint32_t read_u32(uint8_t* rdram, uint32_t vram) {
    return uint32_t(read_u16(rdram, vram)) << 16 | read_u16(rdram, vram + 2);
}

void copy_out(uint8_t* rdram, uint32_t vram, uint32_t size, std::vector<char>& out) {
    for (uint32_t i = 0; i < size; i++) {
        out.push_back(char(ram_u8(rdram, vram + i)));
    }
}

void copy_in(uint8_t* rdram, uint32_t vram, const char* data, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        ram_u8(rdram, vram + i) = uint8_t(data[i]);
    }
}

struct Jump {
    int stage;
    int checkpoint;  // Index into Stage::checkpoints, -1 for the stage start.
};

std::mutex state_mutex;
std::optional<Jump> pending_jump;
std::set<std::tuple<int, int, int>> recorded;  // (stage, map, id)
bool recorded_loaded = false;

// Game thread only.
std::optional<winback::levels::Checkpoint> basic_target;
bool restore_next_frame = false;
bool record_next_frame = false;
// Set after a basic load: checkpoints reached from it would record the basic
// state, not one from natural play, so they aren't saved until the next stage
// start or recorded load.
bool from_basic = false;

void load_recorded_list() {
    if (recorded_loaded) {
        return;
    }
    recorded_loaded = true;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(recomp::get_config_path() / "saves" / "checkpoints", ec)) {
        int stage, map, id;
        if (std::sscanf(e.path().filename().string().c_str(), "stage%02d_map%02d_cp%03d.bin", &stage, &map, &id) == 3) {
            recorded.emplace(stage, map, id);
        }
    }
}

void record_checkpoint(uint8_t* rdram) {
    int stage = ram_u8(rdram, saved_progress + 1);
    uint8_t id = ram_u8(rdram, saved_events + checkpoint_id_offset);
    uint8_t map = ram_u8(rdram, saved_events + checkpoint_map_offset);
    std::vector<char> data(snapshot_magic, snapshot_magic + 4);
    copy_out(rdram, saved_events, events_size, data);
    copy_out(rdram, saved_progress, progress_size, data);
    fs::path path = snapshot_path(stage, map, id);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    f.write(data.data(), std::streamsize(data.size()));
    if (f) {
        std::printf("[levels] recorded stage %d map %d checkpoint %d\n", stage, map, id);
        std::lock_guard lock{state_mutex};
        load_recorded_list();
        recorded.emplace(stage, map, id);
    }
}

bool load_checkpoint(uint8_t* rdram, int stage, const winback::levels::Checkpoint& cp) {
    std::ifstream f(snapshot_path(stage, cp.map, cp.id), std::ios::binary);
    std::vector<char> data(4 + events_size + progress_size);
    if (!f.read(data.data(), std::streamsize(data.size())) || std::memcmp(data.data(), snapshot_magic, 4) != 0) {
        return false;
    }
    copy_in(rdram, saved_events, data.data() + 4, events_size);
    copy_in(rdram, saved_progress, data.data() + 4 + events_size, progress_size);
    return true;
}

// Calls a recompiled function with a scratch copy of the hook's context, so
// the hooked function continues with its own registers.
void call(uint8_t* rdram, const recomp_context* ctx, void (*fn)(uint8_t*, recomp_context*), uint32_t a0 = 0) {
    recomp_context c = *ctx;
    c.r4 = a0;
    fn(rdram, &c);
}

// Leaves attract mode, which plays stages from the front end.
void stop_demo(uint8_t* rdram) {
    write_u32(rdram, 0x801390D4, 0);
}

// Starts gameplay in the map func_8008EEAC / func_8009C998 selected.
void enter_map(uint8_t* rdram, const recomp_context* ctx, bool gameplay) {
    if (gameplay) {
        // Normal play flags, as the front end sets them when it starts
        // gameplay. The title intro and attract demos run with 0x0808, which
        // among other things skips loading the map's event scripts.
        write_u16(rdram, 0x8016A41C, 0x0101);
        write_u16(rdram, 0x8016A41A, read_u16(rdram, 0x8016A4EA));
        write_u16(rdram, 0x8016A41E, 0);
        write_u16(rdram, 0x8016A418, 1);
    }
    else {
        ram_u8(rdram, 0x8010DBF8) = 8;
        call(rdram, ctx, func_8008F29C, 9);
    }
}

void start_stage(uint8_t* rdram, const recomp_context* ctx, bool gameplay, int stage) {
    stop_demo(rdram);
    uint8_t difficulty = ram_u8(rdram, progress_block);
    call(rdram, ctx, func_8008EF28);
    // func_8008EF28 clears the progress record; keep the difficulty and give
    // the player the full health a new game starts with.
    ram_u8(rdram, progress_block) = difficulty;
    ram_u8(rdram, progress_block + 2) = 100;
    call(rdram, ctx, func_8008EEAC, uint32_t(stage));
    enter_map(rdram, ctx, gameplay);
}

void restore_checkpoint(uint8_t* rdram, const recomp_context* ctx, bool gameplay) {
    stop_demo(rdram);
    call(rdram, ctx, func_8008EF28);
    call(rdram, ctx, func_8009C998);
    enter_map(rdram, ctx, gameplay);
}

// WINBACK_JUMP=stage[.checkpoint]@seconds[,...]: scripted jumps for tests,
// e.g. "5@20,12.1@60". Checkpoints count from 1, as in the menu.
void poll_scripted_jumps() {
    struct Scripted { int stage, checkpoint; double at; bool done; };
    static std::vector<Scripted> list = [] {
        std::vector<Scripted> out;
        if (const char* env = std::getenv("WINBACK_JUMP")) {
            std::string spec = env;
            size_t pos = 0;
            while (pos < spec.size()) {
                size_t comma = spec.find(',', pos);
                std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                int stage = 0, cp = 0;
                double at = 0;
                if (std::sscanf(item.c_str(), "%d.%d@%lf", &stage, &cp, &at) == 3 ||
                    (cp = 0, std::sscanf(item.c_str(), "%d@%lf", &stage, &at) == 2)) {
                    out.push_back({stage, cp - 1, at, false});
                }
                if (comma == std::string::npos) {
                    break;
                }
                pos = comma + 1;
            }
        }
        return out;
    }();
    static const auto start = std::chrono::steady_clock::now();
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // WINBACK_RECORD_AT=seconds: records the game's current checkpoint
    // snapshot as if a checkpoint had just triggered (tests).
    static double record_at = std::getenv("WINBACK_RECORD_AT") ? std::atof(std::getenv("WINBACK_RECORD_AT")) : -1;
    if (record_at >= 0 && t >= record_at) {
        record_at = -1;
        record_next_frame = true;
    }
    for (auto& s : list) {
        if (!s.done && t >= s.at) {
            s.done = true;
            std::printf("[levels] scripted jump to stage %d checkpoint %d at %.1f s\n", s.stage, s.checkpoint + 1, t);
            winback::levels::request_jump(s.stage, s.checkpoint);
        }
    }
}

} // namespace

const std::vector<winback::levels::Stage>& winback::levels::stages() {
    static const std::vector<Stage> catalogue = build_catalogue();
    return catalogue;
}

bool winback::levels::is_recorded(int stage, const Checkpoint& cp) {
    std::lock_guard lock{state_mutex};
    load_recorded_list();
    return recorded.contains({stage, cp.map, cp.id});
}

void winback::levels::request_jump(int stage, int checkpoint) {
    std::lock_guard lock{state_mutex};
    pending_jump = Jump{stage, checkpoint};
}

// Hook: start of the front end (gameplay = 0) or gameplay (1) frame callback.
extern "C" void winback_level_frame(uint8_t* rdram, recomp_context* ctx, int gameplay) {
    poll_scripted_jumps();
    bool demo = read_u32(rdram, 0x801390D4) != 0;
    if (record_next_frame) {
        record_next_frame = false;
        if (!demo && !from_basic) {
            record_checkpoint(rdram);
        }
    }
    if (restore_next_frame && gameplay) {
        restore_next_frame = false;
        from_basic = true;
        restore_checkpoint(rdram, ctx, true);
        return;
    }

    std::optional<Jump> jump;
    {
        std::lock_guard lock{state_mutex};
        jump.swap(pending_jump);
    }
    if (!jump) {
        return;
    }
    const auto& list = winback::levels::stages();
    if (jump->stage < 0 || jump->stage >= int(list.size())) {
        return;
    }
    const winback::levels::Stage& stage = list[jump->stage];
    basic_target.reset();
    restore_next_frame = false;
    from_basic = false;
    if (jump->checkpoint >= 0 && jump->checkpoint < int(stage.checkpoints.size())) {
        const winback::levels::Checkpoint& cp = stage.checkpoints[jump->checkpoint];
        if (load_checkpoint(rdram, stage.number, cp)) {
            std::printf("[levels] restoring recorded stage %d map %d checkpoint %d\n", stage.number, cp.map, cp.id);
            restore_checkpoint(rdram, ctx, gameplay != 0);
            return;
        }
        std::printf("[levels] basic load of stage %d map %d checkpoint %d\n", stage.number, cp.map, cp.id);
        basic_target = cp;
    }
    else {
        std::printf("[levels] starting stage %d\n", stage.number);
    }
    start_stage(rdram, ctx, gameplay != 0, stage.number);
}

// Hook: func_8004BD7C just took the stage start snapshot (only on entry from
// the front end or after func_8008EF28).
extern "C" void winback_stage_snapshot(uint8_t* rdram) {
    if (basic_target) {
        ram_u8(rdram, saved_events + checkpoint_id_offset) = basic_target->id;
        ram_u8(rdram, saved_events + checkpoint_map_offset) = basic_target->map;
        basic_target.reset();
        restore_next_frame = true;
    }
}

// Hook: a script's checkpoint command just saved the snapshot. The command
// still updates the saved copy afterwards, so it is written next frame.
extern "C" void winback_checkpoint_reached(void) {
    record_next_frame = true;
}
