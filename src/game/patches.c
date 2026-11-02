// Hand-written replacements for recompiled functions (listed under
// [patches] ignored in config/winback.us.toml).

#include "recomp.h"

// ultramodern: parks the calling game thread, letting others run on events.
void pause_self(uint8_t* rdram);

// Idle thread loop (entered from func_800E3CF4 after it drops to priority 0):
//     for (;;) { if (D_801BE858 != NULL) D_801BE858(); }
// The callback is only ever cleared, never set. The runtime only switches
// game threads at OS calls, so the busy loop would starve every other
// thread; park the thread instead.
RECOMP_FUNC void func_800E3D88(uint8_t* rdram, recomp_context* ctx) {
    pause_self(rdram);
}

// ultramodern: lets other game threads and pending events run for up to 1 ms.
void yield_self_1ms(uint8_t* rdram);

// Waits for the pending graphics task counter to drop to zero:
//     while (D_801AADFC != 0);
// The counter is decremented by the task-done thread (func_800EC7A0), which
// can only run if this thread yields.
RECOMP_FUNC void func_800ECC20(uint8_t* rdram, recomp_context* ctx) {
    while (MEM_W(0, (gpr)(int32_t)0x801AADFC) != 0) {
        yield_self_1ms(rdram);
    }
}

void func_800CDF50(uint8_t* rdram, recomp_context* ctx);

// Copies the fields selected by a bitmask into a 7-byte record:
//     if (s->count_176B > 0) {
//         for (i = 0; i < 6; i++)
//             if (mask & (1 << i)) switch (i) { ... }
//         func_800CDF50(s);
//     }
// GCC walks the jump table (jtbl_8011B6E0) with a pointer instead of indexing
// it, so N64Recomp can't find the table and the jr falls through to a lookup.
RECOMP_FUNC void func_800CE0D4(uint8_t* rdram, recomp_context* ctx) {
    gpr s = ctx->r4;
    uint32_t mask = (uint32_t)ctx->r5;
    gpr values = ctx->r6;
    gpr dst = s + 0x1958;
    int i;

    if ((int8_t)MEM_B(0x176B, s) <= 0) {
        return;
    }
    for (i = 0; i < 6; i++) {
        if (((int32_t)mask >> i) & 1) {
            int32_t v = MEM_W(i * 4, values);
            if (i == 0) {
                MEM_H(0, dst) = (int16_t)v;
            } else {
                MEM_B(i + 1, dst) = (int8_t)v;
            }
        }
    }
    ctx->r4 = s;
    func_800CDF50(rdram, ctx);
}

float winback_camera_stick(int controller_num);

// Analog camera turning (hook at 0x80035B0C in func_80035970, the player
// camera update). Holding C-Left/C-Right adds a step ($f6, 1 degree) to the
// camera's turn speed (camera+0x110, radians per frame) while it is below
// 6 degrees, and every frame the speed falls 0.5 degrees towards zero. With
// the right stick, the step, the limit and the fall while turning are scaled
// by how far the stick is pushed, so full deflection turns exactly like the
// buttons. Returns 1 when it has done the frame's turning; the hook then
// skips the game's own. Scripted turns (flags 0x8000, 0x4, 0x2000 in
// camera+0x132) are left to the game.
//     $a2 = camera, $t1 = the button that turns the speed down, $t2 = up.
int winback_camera_analog(uint8_t* rdram, recomp_context* ctx) {
    gpr cam = ctx->r6;
    if (MEM_HU(0x132, cam) & 0xA004) {
        return 0;
    }
    float stick = winback_camera_stick(MEM_H(0x18, cam));
    if (stick == 0.0f) {
        return 0;
    }
    // Stick right acts as C-Right (0x0001), whichever way the game turns it.
    float scale = stick > 0.0f ? stick : -stick;
    float dir = (ctx->r9 & 0x0001) ? -1.0f : 1.0f;
    if (stick < 0.0f) {
        dir = -dir;
    }
    union { uint32_t u; float f; } speed = { (uint32_t)MEM_W(0x110, cam) };
    float magnitude = speed.f > 0.0f ? speed.f : -speed.f;
    if (magnitude < 0.10472f * scale) {
        speed.f += dir * ctx->f6.fl * scale;
        MEM_H(0x132, cam) = MEM_HU(0x132, cam) | 0x1001;
    }
    if (speed.f > 0.0f) {
        speed.f -= 0.0087265f * scale;
    }
    else if (speed.f < 0.0f) {
        speed.f += 0.0087265f * scale;
    }
    MEM_W(0x110, cam) = speed.u;
    return 1;
}
