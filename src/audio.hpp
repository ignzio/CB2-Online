#pragma once
// Audio output: a lock-free ring buffer of stereo frames drained by a miniaudio playback device.
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace psx {

class AudioOut {
public:
    AudioOut();
    ~AudioOut();
    bool start(uint32_t sampleRate = 44100);
    void push(int16_t l, int16_t r);   // emulation thread; drops frames when the ring is full
    size_t queued() const { return (head.load() - tail.load()) & mask; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    static constexpr size_t kFrames = 1 << 15;
    static constexpr size_t mask = kFrames - 1;
    std::vector<int16_t> ring = std::vector<int16_t>(kFrames * 2, 0);
    std::atomic<size_t> head{0}, tail{0};
    friend struct Impl;
    void pull(int16_t* out, uint32_t frames);
};

}  // namespace psx
