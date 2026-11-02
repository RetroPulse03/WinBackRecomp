// SDL audio output for the game's AI buffers.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "sdl_include.hpp"
#include "audio.hpp"

namespace {

SDL_AudioDeviceID device = 0;
uint32_t frequency = 0;
std::vector<int16_t> swap_buffer;
size_t last_buffer_frames = 0;

void open_device(uint32_t freq) {
    if (device != 0) {
        SDL_CloseAudioDevice(device);
        device = 0;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = static_cast<int>(freq);
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (device == 0) {
        std::fprintf(stderr, "[audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    frequency = freq;
    SDL_PauseAudioDevice(device, 0);
}

} // namespace

void winback::audio::init() {
    SDL_InitSubSystem(SDL_INIT_AUDIO);
}

// The game's buffers hold interleaved stereo; RDRAM is word-swapped on the
// host, so each left/right pair arrives reversed.
void winback::audio::queue_samples(int16_t* samples, size_t sample_count) {
    if (device == 0) {
        return;
    }
    swap_buffer.resize(sample_count);
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        swap_buffer[i + 0] = samples[i + 1];
        swap_buffer[i + 1] = samples[i + 0];
    }
    SDL_QueueAudio(device, swap_buffer.data(), static_cast<Uint32>(sample_count * sizeof(int16_t)));
    last_buffer_frames = sample_count / 2;

    // WINBACK_DEBUG: report the input rate and queue depth every 2 seconds.
    static const bool debug = std::getenv("WINBACK_DEBUG") != nullptr;
    static auto last = std::chrono::steady_clock::now();
    static size_t queued_since = 0;
    queued_since += sample_count / 2;
    auto now = std::chrono::steady_clock::now();
    if (debug && now - last >= std::chrono::seconds(2)) {
        double secs = std::chrono::duration<double>(now - last).count();
        std::fprintf(stderr, "[audio] freq %u, in %.0f frames/s, queued %zu frames (%.0f ms)\n",
            frequency, queued_since / secs, get_frames_remaining(),
            frequency ? get_frames_remaining() * 1000.0 / frequency : 0.0);
        last = now;
        queued_since = 0;
    }
}

// Stand-in for the AI status register, read by a hook in the libmus audio
// thread. The real AI holds the playing buffer plus one pending; with two
// buffers' worth queued, report FIFO full (bit 31) so the game skips a frame.
extern "C" uint32_t winback_ai_get_status(void) {
    size_t limit = 2 * (last_buffer_frames != 0 ? last_buffer_frames : 552);
    return winback::audio::get_frames_remaining() >= limit ? 0x80000000u : 0;
}

size_t winback::audio::get_frames_remaining() {
    if (device == 0) {
        return 0;
    }
    return SDL_GetQueuedAudioSize(device) / (2 * sizeof(int16_t));
}

void winback::audio::set_frequency(uint32_t freq) {
    if (freq != frequency || device == 0) {
        open_device(freq);
    }
}

void winback::audio::pause() {
    if (device != 0) {
        SDL_PauseAudioDevice(device, 1);
    }
}
