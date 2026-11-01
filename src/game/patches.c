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
