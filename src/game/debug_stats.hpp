#pragma once

#include <atomic>
#include <cstdint>

// Per-second activity counters, printed when WINBACK_DEBUG is set.
namespace winback::stats {
    extern std::atomic<uint32_t> gfx_tasks;
    extern std::atomic<uint32_t> audio_tasks;
    extern std::atomic<uint32_t> screen_updates;
    extern std::atomic<uint32_t> threads_created;
    void start();
    // Remembers a game thread (OSThread vram address) to report its state.
    void register_thread(uint8_t* rdram, int32_t thread);
}
