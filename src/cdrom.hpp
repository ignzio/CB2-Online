#pragma once
// CD-ROM controller (register interface used by libcd): command/parameter/response FIFOs, interrupt flags,
// sector reads from a VirtualDisc at double speed, and the data FIFO read through register 1802 or DMA 3.
#include <cstdint>
#include <deque>
#include <vector>

#include "disc.hpp"

namespace psx {

struct StateAccess;

class CdRom {
    friend struct StateAccess;
public:
    VirtualDisc* disc = nullptr;
    bool irqLine = false;          // asserted while (flags & enable) != 0; the platform mirrors it into I_STAT bit 2
    bool trace = false;                // print every register access and interrupt
    uint64_t commandsSeen[0x20] = {};

    uint8_t read(uint32_t reg);
    void write(uint32_t reg, uint8_t v);
    void tick(uint64_t cycles);
    uint32_t readDataWord();       // for DMA 3
    bool dataAvailable() const { return dataPos < data.size(); }
    size_t dataLeft() const { return data.size() - std::min(dataPos, data.size()); }

private:
    int index = 0;
    uint8_t enable = 0x1F, flags = 0, mode = 0, status = 0x02, request = 0;  // the BIOS leaves all CD interrupts enabled
    std::deque<uint8_t> params, response;
    std::vector<uint8_t> data;
    size_t dataPos = 0;
    uint32_t lba = 0, seekLba = 0;
    bool reading = false;
    uint8_t filterFile = 0, filterChannel = 0;

    struct Pending { uint64_t due; uint8_t irq; std::vector<uint8_t> resp; int64_t sectorLba = -1; };
    std::deque<Pending> pending;
    std::vector<uint8_t> sectorBuf;
    uint64_t now = 0, nextSector = 0;

    void command(uint8_t cmd);
    void queue(uint64_t delay, uint8_t irq, std::vector<uint8_t> resp, int64_t sectorLba = -1);
    void deliver();
    void loadSector(uint32_t at);
    void updateIrq() { irqLine = (flags & enable & 0x1F) != 0; }
};

}  // namespace psx
