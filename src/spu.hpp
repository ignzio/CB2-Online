#pragma once
// PlayStation SPU: 24 ADPCM voices with ADSR envelopes, key on/off, loop flags (ENDX), SPU RAM transfers and IRQ.
// Reverb is modelled (22.05 kHz, see reverbStep); noise, pitch modulation and CD audio mixing are not.
#include <cstdint>
#include <functional>
#include <vector>

namespace psx {

struct StateAccess;

class Spu {
    friend struct StateAccess;
public:
    std::vector<uint8_t> ram = std::vector<uint8_t>(512 * 1024, 0);
    std::function<void(int16_t, int16_t)> sink;  // receives one stereo frame at 44.1 kHz
    uint16_t reg(uint32_t offset) const { return regs[(offset >> 1) & 0x1FF]; }   // a register as last written (tools)
    bool reverb = true;                          // false: the reverb is left out of the mix (the game's own reverb setup is still tracked)
    bool irq = false;                            // set when the SPU IRQ address was reached; the platform clears it
    uint64_t framesProduced = 0;

    uint16_t read(uint32_t offset);               // offset from 0x1F801C00, in bytes
    void write(uint32_t offset, uint16_t value);
    void dmaWrite(const uint8_t* data, uint32_t bytes);
    void dmaRead(uint8_t* data, uint32_t bytes);
    void tick(uint32_t frames);
    // One step of the reverb at 22.05 kHz: "in" is what the voices with their reverb flag set put in (sample scale), the result is the reverb's output before the
    // output volumes vLOUT / vROUT (applied here), in the same scale. Reads and writes the reverb work area in SPU RAM (writes only while bit 7 of SPUCNT is set).
    void reverbStep(int32_t inL, int32_t inR, int32_t& outL, int32_t& outR);

    bool irqEnabled() const { return (regs[0xD5] & 0x40) != 0; }
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
    uint16_t regs[0x200] = {};          // plain storage for registers without behaviour; the top 16 slots (offset 0x3E0 up, never used by the hardware) hold the reverb's own state
                                        // (position in the work area, which half-rate step is next, last output, last input) so that save states need no new fields
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
