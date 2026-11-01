// SDL audio output for the game's AI buffers.

#include <cstdio>
#include <vector>

#include "sdl_include.hpp"
#include "audio.hpp"

namespace {

SDL_AudioDeviceID device = 0;
uint32_t frequency = 0;
std::vector<int16_t> swap_buffer;

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
