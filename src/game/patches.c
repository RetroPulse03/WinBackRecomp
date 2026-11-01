// Hand-written replacements for recompiled functions (listed under
// [patches] ignored in winback.us.toml).

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
