#include "audio.hpp"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace psx {

struct AudioOut::Impl {
    ma_device device{};
    bool started = false;
};

AudioOut::AudioOut() : impl(new Impl) {}

AudioOut::~AudioOut() {
    if (impl->started) ma_device_uninit(&impl->device);
}

void AudioOut::push(int16_t l, int16_t r) {
    const size_t h = head.load(std::memory_order_relaxed);
    if (((h + 1) & mask) == tail.load(std::memory_order_acquire)) return;
    ring[h * 2] = l;
    ring[h * 2 + 1] = r;
    head.store((h + 1) & mask, std::memory_order_release);
}

void AudioOut::pull(int16_t* out, uint32_t frames) {
    size_t t = tail.load(std::memory_order_relaxed);
    const size_t h = head.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < frames; ++i) {
        if (t == h) { out[i * 2] = out[i * 2 + 1] = 0; continue; }
        out[i * 2] = ring[t * 2];
        out[i * 2 + 1] = ring[t * 2 + 1];
        t = (t + 1) & mask;
    }
    tail.store(t, std::memory_order_release);
}

bool AudioOut::start(uint32_t sampleRate) {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = 2;
    cfg.sampleRate = sampleRate;
    cfg.pUserData = this;
    cfg.dataCallback = [](ma_device* d, void* out, const void*, ma_uint32 frames) {
        static_cast<AudioOut*>(d->pUserData)->pull(static_cast<int16_t*>(out), frames);
    };
    if (ma_device_init(nullptr, &cfg, &impl->device) != MA_SUCCESS) return false;
    if (ma_device_start(&impl->device) != MA_SUCCESS) { ma_device_uninit(&impl->device); return false; }
    impl->started = true;
    return true;
}

}  // namespace psx
