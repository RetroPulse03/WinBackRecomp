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

#endif
