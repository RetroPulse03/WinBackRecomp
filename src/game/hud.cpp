// Widescreen HUD (hooks in func_800D20CC, see config/winback.us.toml).
//
// The gameplay HUD is drawn by one object whose draw function, func_800D20CC, calls a function for
// each part and draws the camera frame's corner lines itself. All of it is texture rectangles. RT64
// keeps rectangles in the centre 4:3 of a widescreen picture, so these hooks add RT64 extended GBI
// commands to the display list: each part is anchored to the left or right edge, at the same distance
// from the widened 3D picture's edge as it had from the 4:3 picture's edge.
//
// WINBACK_HUDLOG=1 logs, once a second per part, the rectangles each part draws.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "recomp.h"

extern "C" int winback_widescreen_on(void);

namespace {

constexpr uint32_t dl_pointer = 0x801D2C80; // Gfx* the scene task is written to
constexpr uint32_t player_cameras = 0x80179280; // camera object per controller; mode (s16) at +0xEC

// RT64 extended GBI (deps/rt64/include/rt64_extended_gbi.h), on F3DEX2.
constexpr uint32_t ex_opcode = 0x64;
constexpr uint32_t ex_hook = 0xE0525464; // G_SPNOOP with RT64's magic number
constexpr uint32_t ex_setscissor = 0x000005, ex_setrectalign = 0x000006, ex_pushscissor = 0x000017, ex_popscissor = 0x000018;
constexpr uint16_t origin_left = 0x0, origin_right = 0x400, origin_none = 0x800;

// RT64 draws a rectangle anchored to the left edge at x pixels from the widened framebuffer's left
// edge, and one anchored to the right edge at x pixels from its right edge. The 3D picture keeps its
// overscan border of 14 of the game's 320 pixels, which becomes 14 * 4/3 when widened, so a part also
// moves 14/3 pixels away from the edge. Offsets are in quarter pixels.
constexpr int border_growth = 19; // 14 / 3 * 4
constexpr int left_offset = border_growth;
constexpr int right_offset = -320 * 4 - border_growth;

enum class Anchor { Centre, Left, Right };

// The parts func_800D20CC draws, in call order. Parts drawn at a 3D object's projected position stay
// in the centre, where they line up with the widened 3D.
enum Part {
    PartLife,        // func_800CF6A8: LIFE and the player's health bar, bottom right
    PartAmmo,        // func_800CEC1C: rounds in the magazine, bottom left
    PartWeapon,      // func_800CF7E8: weapon icon and ammo count, bottom left
    PartAlert,       // func_800CFA9C: indicator at (276,28), top right, shown when certain objects are in the player's room (not identified)
    PartSecondLife,  // func_800CF76C: a second health bar at (40,22), top left, drawn while D_801D362C is set
    PartTarget1,     // func_800D0B14 .. func_800D0E54: func_800D0088 draws markers at the projected position
    PartTarget2,     //   of an object the camera or player targets (camera +0x164, D_80138EF8)
    PartTarget3,
    PartTarget4,
    PartLine,        // func_8007CBEC: the camera frame's corners and the crosshair
    PartCount
};

const char* const part_names[PartCount] = {
    "life", "ammo", "weapon", "alert", "second life", "target 1", "target 2", "target 3", "target 4", "line"
};

Anchor part_anchor(Part part) {
    switch (part) {
        case PartLife: return Anchor::Right;
        case PartAmmo: return Anchor::Left;
        case PartWeapon: return Anchor::Left;
        case PartAlert: return Anchor::Right;
        case PartSecondLife: return Anchor::Left;
        default: return Anchor::Centre;
    }
}

uint32_t rd_w(const uint8_t* rdram, uint32_t addr) {
    return *reinterpret_cast<const uint32_t*>(rdram + (addr & 0x3FFFFFC));
}

int16_t rd_h(const uint8_t* rdram, uint32_t addr) {
    return *reinterpret_cast<const int16_t*>(rdram + ((addr ^ 2) & 0x3FFFFFE));
}

void wr_w(uint8_t* rdram, uint32_t addr, uint32_t value) {
    *reinterpret_cast<uint32_t*>(rdram + (addr & 0x3FFFFFC)) = value;
}

// Appends a command to the scene display list.
void emit(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    uint32_t dl = rd_w(rdram, dl_pointer);
    wr_w(rdram, dl, w0);
    wr_w(rdram, dl + 4, w1);
    wr_w(rdram, dl_pointer, dl + 8);
}

uint32_t ex(uint32_t command) {
    return (ex_opcode << 24) | command;
}

uint32_t pack(int hi, int lo) {
    return (uint32_t(hi & 0xFFFF) << 16) | uint32_t(lo & 0xFFFF);
}

void emit_rect_align(uint8_t* rdram, Anchor anchor) {
    uint16_t origin = anchor == Anchor::Left ? origin_left : anchor == Anchor::Right ? origin_right : origin_none;
    int offset = anchor == Anchor::Left ? left_offset : anchor == Anchor::Right ? right_offset : 0;
    emit(rdram, ex(ex_setrectalign), uint32_t(origin) | (uint32_t(origin) << 12));
    emit(rdram, pack(offset, 0), pack(offset, 0));
}

bool active = false; // the HUD is being drawn with extended commands
Anchor current_anchor = Anchor::Centre;

void set_anchor(uint8_t* rdram, Anchor anchor) {
    if (active && anchor != current_anchor) {
        emit_rect_align(rdram, anchor);
        current_anchor = anchor;
    }
}

// WINBACK_HUDLOG

bool hudlog_enabled() {
    static const bool enabled = std::getenv("WINBACK_HUDLOG") != nullptr;
    return enabled;
}

struct Box {
    int n = 0;
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
    void add(int ax0, int ay0, int ax1, int ay1) {
        n++;
        x0 = std::min(x0, ax0); y0 = std::min(y0, ay0);
        x1 = std::max(x1, ax1); y1 = std::max(y1, ay1);
    }
};

// Texture and fill rectangles in [start, end) of the main display list. The HUD writes its
// rectangles directly into it; as in the microcode, a texture rectangle is sent by G_RDPHALF_2.
void find_rects(const uint8_t* rdram, uint32_t start, uint32_t end, Box& box) {
    uint32_t texrect0 = 0, texrect1 = 0;
    for (uint32_t dl = start; dl < end; dl += 8) {
        uint32_t w0 = rd_w(rdram, dl), w1 = rd_w(rdram, dl + 4);
        switch (w0 >> 24) {
            case 0xE4: case 0xE5: texrect0 = w0; texrect1 = w1; break;
            case 0xF1:
                if ((texrect0 >> 24) == 0xE4 || (texrect0 >> 24) == 0xE5) {
                    box.add(((texrect1 >> 12) & 0xFFF) / 4, (texrect1 & 0xFFF) / 4, ((texrect0 >> 12) & 0xFFF) / 4, (texrect0 & 0xFFF) / 4);
                    texrect0 = 0;
                }
                break;
            case 0xF6:
                box.add(((w1 >> 12) & 0xFFF) / 4, (w1 & 0xFFF) / 4, ((w0 >> 12) & 0xFFF) / 4, (w0 & 0xFFF) / 4);
                break;
        }
    }
}

int camera_mode(const uint8_t* rdram) {
    uint32_t camera = rd_w(rdram, player_cameras);
    return camera != 0 ? rd_h(rdram, camera + 0xEC) : 0;
}

int log_part = -1;
uint32_t log_start = 0;
std::chrono::steady_clock::time_point last_logged[PartCount];

void log_finish(const uint8_t* rdram) {
    if (log_part < 0) {
        return;
    }
    int part = log_part;
    log_part = -1;
    Box box;
    find_rects(rdram, log_start, rd_w(rdram, dl_pointer), box);
    auto now = std::chrono::steady_clock::now();
    if (box.n == 0 || now - last_logged[part] < std::chrono::seconds(1)) {
        return;
    }
    last_logged[part] = now;
    std::fprintf(stderr, "[hud] %s: %d rects (%d,%d)-(%d,%d)\n", part_names[part], box.n, box.x0, box.y0, box.x1, box.y1);
}

void log_begin(const uint8_t* rdram, Part part) {
    if (!hudlog_enabled()) {
        return;
    }
    log_finish(rdram);
    log_part = part;
    log_start = rd_w(rdram, dl_pointer);
}

// RT64 clips rectangles to the scissor, which it also keeps in the centre 4:3, so the scissor is
// widened to the whole picture while the HUD is drawn. A pop at the end brings back the view's own.
void begin(uint8_t* rdram, bool widened) {
    current_anchor = Anchor::Centre;
    active = widened && winback_widescreen_on() != 0;
    if (!active) {
        return;
    }
    emit(rdram, ex_hook, (1u << 28) | ex_opcode); // gEXEnable
    emit(rdram, ex(ex_pushscissor), 0);
    emit(rdram, ex(ex_setscissor), 0 /* G_SC_NON_INTERLACE */ | (uint32_t(origin_left) << 2) | (uint32_t(origin_right) << 14));
    emit(rdram, pack(0, 0), pack(0, 240 * 4));
}

void end(uint8_t* rdram) {
    if (!active) {
        return;
    }
    set_anchor(rdram, Anchor::Centre);
    emit(rdram, ex(ex_popscissor), 0);
    emit(rdram, ex_hook, 2u << 28); // gEXDisable
    active = false;
}

} // namespace

// Start of func_800D20CC, after its setup display list.
extern "C" void winback_hud_begin(uint8_t* rdram) {
    if (hudlog_enabled()) {
        static int last_mode = -1;
        int mode = camera_mode(rdram);
        if (mode != last_mode) {
            std::fprintf(stderr, "[hud] camera mode %d\n", mode);
            last_mode = mode;
        }
    }
    begin(rdram, true);
}

// Before each part's call.
extern "C" void winback_hud_part(uint8_t* rdram, int part) {
    log_begin(rdram, Part(part));
    set_anchor(rdram, part_anchor(Part(part)));
}

// Before each func_8007CBEC call, which draws a line from (x1, y1) to (x2, y2). In camera modes 6 and 8
// (inferred: security cameras and other fixed views) these are the frame's corners, which go to the
// nearer edge. In mode 9 they are a crosshair, which stays in the centre.
extern "C" void winback_hud_line(uint8_t* rdram, int x1) {
    log_begin(rdram, PartLine);
    int mode = camera_mode(rdram);
    Anchor anchor = Anchor::Centre;
    if (mode == 6 || mode == 8) {
        anchor = x1 < 160 ? Anchor::Left : Anchor::Right;
    }
    set_anchor(rdram, anchor);
}

// End of func_800D20CC, before its closing display list.
extern "C" void winback_hud_end(uint8_t* rdram) {
    if (hudlog_enabled()) {
        log_finish(rdram);
    }
    end(rdram);
}

// Versus HUD, func_800B4FD4(player), called from the versus HUD object's draw function for each view.
// Only the two-view split is widened by RT64, so the four-view layout keeps its parts in place.
// Ammo (func_800CEC1C) is on the left. The markers that follow (func_800D0B14 and its neighbours,
// func_800B4E6C) stay in the centre, and life (func_800CF6A8) and the rows of counters drawn after it
// at x 268-284 go right.
extern "C" void winback_vs_hud_begin(uint8_t* rdram) {
    uint8_t modes = rdram[0x16A40E ^ 3];
    uint16_t state = uint16_t(rd_h(rdram, 0x8016A41C));
    begin(rdram, !(modes & 1) && !(state & 0x600) && (modes & 2));
    set_anchor(rdram, Anchor::Left);
}

extern "C" void winback_vs_hud_anchor(uint8_t* rdram, int anchor) {
    set_anchor(rdram, Anchor(anchor));
}

extern "C" void winback_vs_hud_end(uint8_t* rdram) {
    end(rdram);
}
