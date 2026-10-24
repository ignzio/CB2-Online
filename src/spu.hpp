#pragma once
// PlayStation SPU: 24 ADPCM voices with ADSR envelopes, key on/off, loop flags (ENDX), SPU RAM transfers and IRQ.
// Reverb, noise and CD audio mixing are not modelled yet.
#include <cstdint>
#include <functional>
#include <vector>

namespace psx {

class Spu {
public:
    std::vector<uint8_t> ram = std::vector<uint8_t>(512 * 1024, 0);
    std::function<void(int16_t, int16_t)> sink;  // receives one stereo frame at 44.1 kHz
    bool irq = false;                            // set when the SPU IRQ address was reached; the platform clears it
    uint64_t framesProduced = 0;

    uint16_t read(uint32_t offset);               // offset from 0x1F801C00, in bytes
    void write(uint32_t offset, uint16_t value);
    void dmaWrite(const uint8_t* data, uint32_t bytes);
    void dmaRead(uint8_t* data, uint32_t bytes);
    void tick(uint32_t frames);

private:
    enum class Phase { Off, Attack, Decay, Sustain, Release };
    struct Voice {
        int16_t volL = 0, volR = 0;
        uint16_t pitch = 0, startAddr = 0, adsr1 = 0, adsr2 = 0, repeatAddr = 0;
        uint32_t cur = 0;                // byte address of the current ADPCM block
        uint32_t counter = 0;            // 4.12 position inside the block
        int16_t samples[28 + 3] = {};    // three samples of history followed by the decoded block
        int16_t old = 0, older = 0;
        int32_t env = 0;
        uint32_t envCounter = 0;
        Phase phase = Phase::Off;
        bool repeatSet = false;
    };
    Voice v[24];
    uint16_t regs[0x200] = {};          // plain storage for registers without behaviour
    uint32_t transferAddr = 0;
    uint32_t irqAddr = 0;
    uint32_t endx = 0;

    void keyOn(uint32_t mask);
    void keyOff(uint32_t mask);
    void decodeBlock(Voice& vc);
    void stepEnvelope(Voice& vc);
    static int volume(int16_t raw);
    bool spuEnabled() const { return (regs[0xD5] & 0x8000) != 0; }
};

}  // namespace psx
