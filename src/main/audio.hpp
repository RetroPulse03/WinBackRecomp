#pragma once

#include <cstddef>
#include <cstdint>

namespace winback::audio {
    void init();
    void queue_samples(int16_t* samples, size_t sample_count);
    size_t get_frames_remaining();
    void set_frequency(uint32_t freq);
    void pause();
}
