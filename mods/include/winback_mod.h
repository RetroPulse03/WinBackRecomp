// What a WinBack mod includes: the runtime's section macros, its config
// functions, and the functions WinBack adds (src/main/mods.cpp).
#ifndef WINBACK_MOD_H
#define WINBACK_MOD_H

#define RECOMP_IMPORT(mod, func) \
    __attribute__((noinline, weak, used, section(".recomp_import." mod))) func {}
#define RECOMP_HOOK(func) __attribute__((section(".recomp_hook." func)))
#define RECOMP_HOOK_RETURN(func) __attribute__((section(".recomp_hook_return." func)))
#define RECOMP_PATCH __attribute__((section(".recomp_patch")))

#pragma GCC diagnostic ignored "-Wreturn-type"
#pragma GCC diagnostic ignored "-Wunused-parameter"

// Runtime.
RECOMP_IMPORT("*", unsigned int recomp_get_config_u32(const char* option_id))
RECOMP_IMPORT("*", double recomp_get_config_double(const char* option_id))
RECOMP_IMPORT("*", void* recomp_alloc(unsigned long size))
RECOMP_IMPORT("*", void recomp_free(void* memory))

// Reads a file from this mod's folder, by a path relative to it. The size is
// -1 if the file doesn't exist. Reading returns the number of bytes copied.
RECOMP_IMPORT("*", int winback_mod_file_size(const char* path))
RECOMP_IMPORT("*", int winback_mod_read_file(const char* path, void* dest, unsigned int max_size))

// Prints a line to the game's log (run with --console to see it).
RECOMP_IMPORT("*", void winback_mod_log(const char* text))

// A steady clock in microseconds, which wraps around about every 71 minutes.
RECOMP_IMPORT("*", unsigned int winback_mod_time_us(void))

// A controller as it is held, whatever the game makes of it. Everything is 0
// while the recomp menu is open. other_buttons is the N64 buttons pressed by the
// keyboard or a scripted test.
typedef struct {
    unsigned int pad_buttons;  // WB_PAD_ bits
    unsigned int other_buttons;
    float left_x, left_y;  // -1 to 1, up and right positive
    float right_x, right_y;
} WinbackPad;

#define WB_PAD_A (1u << 0)
#define WB_PAD_B (1u << 1)
#define WB_PAD_X (1u << 2)
#define WB_PAD_Y (1u << 3)
#define WB_PAD_START (1u << 6)
#define WB_PAD_LSTICK (1u << 7)
#define WB_PAD_RSTICK (1u << 8)
#define WB_PAD_LB (1u << 9)
#define WB_PAD_RB (1u << 10)
#define WB_PAD_DU (1u << 11)
#define WB_PAD_DD (1u << 12)
#define WB_PAD_DL (1u << 13)
#define WB_PAD_DR (1u << 14)
#define WB_PAD_LT (1u << 30)
#define WB_PAD_RT (1u << 31)

RECOMP_IMPORT("*", void winback_mod_pad_read(int controller, WinbackPad* pad))

// Replaces what the game reads from this controller during normal play: N64
// buttons and the left stick. Menus and the pause screen keep the usual layout.
// Once called, the recomp's own right stick click is off for this controller.
RECOMP_IMPORT("*", void winback_mod_pad_set(int controller, unsigned int buttons, float x, float y))

#endif
