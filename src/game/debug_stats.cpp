#include <chrono>
#include <csignal>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "trace.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "debug_stats.hpp"

std::atomic<uint32_t> winback::stats::gfx_tasks{0};
std::atomic<uint32_t> winback::stats::audio_tasks{0};
std::atomic<uint32_t> winback::stats::screen_updates{0};
std::atomic<uint32_t> winback::stats::threads_created{0};

namespace {

constexpr int trace_depth = 12;

struct ThreadTrace {
    const char* funcs[trace_depth]{};
    unsigned count = 0;
};

std::mutex trace_mutex;
std::vector<ThreadTrace*> traces;

uint8_t* thread_rdram = nullptr;
std::vector<int32_t> game_threads;

// OSThread (ultramodern layout): next, priority, queue, pad, flags/state
// (halfword-swapped), id. RDRAM holds host-endian 32-bit words.
uint32_t read32(int32_t addr) {
    return *reinterpret_cast<uint32_t*>(thread_rdram + (static_cast<uint32_t>(addr) - 0x80000000u));
}

void print_threads() {
    static const char* states[] = {"stopped", "queued", "running", "blocked"};
    std::lock_guard lock{trace_mutex};
    for (int32_t t : game_threads) {
        uint32_t flags_state = read32(t + 0x10);
        uint32_t state = flags_state >> 16;
        std::printf("  OSThread %08X id %u pri %d %s queue %08X\n", static_cast<uint32_t>(t),
            read32(t + 0x14), static_cast<int32_t>(read32(t + 0x4)), state < 4 ? states[state] : "?", read32(t + 0x8));
    }
}

// WINBACK_WATCH=addr[,addr...]: words to print with each thread dump.
void print_watches() {
    const char* env = std::getenv("WINBACK_WATCH");
    if (env == nullptr || thread_rdram == nullptr) {
        return;
    }
    std::printf("  watch:");
    for (const char* p = env; *p;) {
        char* end;
        uint32_t addr = static_cast<uint32_t>(std::strtoul(p, &end, 16));
        if (end == p) {
            break;
        }
        std::printf(" %08X=%08X", addr, read32(static_cast<int32_t>(addr)));
        p = (*end == ',') ? end + 1 : end;
    }
    std::printf("\n");
}

void print_traces() {
    std::lock_guard lock{trace_mutex};
    for (size_t t = 0; t < traces.size(); t++) {
        const ThreadTrace* tr = traces[t];
        unsigned n = tr->count;
        std::printf("  thread %zu (%u calls):", t, n);
        for (int i = trace_depth; i > 0; i--) {
            if (n >= static_cast<unsigned>(i)) {
                std::printf(" %s", tr->funcs[(n - i) % trace_depth]);
            }
        }
        std::printf("\n");
    }
}

} // namespace

extern "C" void winback_trace_enter(const char* func) {
    thread_local ThreadTrace* tr = [] {
        auto* t = new ThreadTrace();
        std::lock_guard lock{trace_mutex};
        traces.push_back(t);
        return t;
    }();
    tr->funcs[tr->count % trace_depth] = func;
    tr->count++;
}

void winback::stats::register_thread(uint8_t* rdram, int32_t thread) {
    std::lock_guard lock{trace_mutex};
    thread_rdram = rdram;
    game_threads.push_back(thread);
}

#ifdef _WIN32
// Prints the exception and every game thread's recent calls instead of the
// Windows "has stopped working" dialog.
LONG WINAPI crash_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    std::printf("[crash] exception %08lX at %p", rec->ExceptionCode, rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        std::printf(" (%s %p)", rec->ExceptionInformation[0] ? "write" : "read",
            reinterpret_cast<void*>(rec->ExceptionInformation[1]));
    }
    std::printf("\n");
    print_traces();
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void abort_handler(int) {
    std::printf("[crash] abort\n");
    print_traces();
    std::fflush(stdout);
    std::_Exit(3);
}

void winback::stats::start() {
    if (std::getenv("WINBACK_DEBUG") == nullptr) {
        return;
    }
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crash_handler);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::signal(SIGABRT, abort_handler);
    std::set_terminate([] {
        std::printf("[crash] std::terminate\n");
        abort_handler(0);
    });
    std::thread([] {
        for (int sec = 1;; sec++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::printf("[stats %3ds] gfx %u  audio %u  vi %u  threads %u\n", sec,
                gfx_tasks.exchange(0), audio_tasks.exchange(0), screen_updates.exchange(0),
                threads_created.load());
            if (sec % 5 == 0) {
                print_traces();
                print_threads();
                print_watches();
            }
            std::fflush(stdout);
        }
    }).detach();
}
