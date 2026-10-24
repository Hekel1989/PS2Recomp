#pragma once

#include "ps2x/iop/iop_spu2_host.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory
    {
    public:
        static constexpr uint32_t RamSize = 2u * 1024u * 1024u;
        static constexpr uint32_t ScratchBase = 0x1F800000u;
        static constexpr uint32_t ScratchSize = 0x400u;
        static constexpr uint32_t HardwareBase = 0x1F801000u;
        static constexpr uint32_t HardwareEnd = 0x1F900000u;
        static constexpr uint32_t Spu2Base = 0x1F900000u;
        static constexpr uint32_t Spu2End = 0x1FA00000u;
        static constexpr uint32_t SifBase = 0x1D000000u;
        static constexpr uint32_t SifEnd = 0x1D001000u;
        // The sysmem allocation region covers all RAM available to modules and
        // AllocSysMemory: from the module load base up to the emulator's call
        // stack reservation (mirroring how the real IOP kernel reserves the top
        // of RAM for its stacks). Module images are registered as fixed
        // allocations so the free list never hands out their ranges.
        static constexpr uint32_t HeapBase = 0x00010000u;
        static constexpr uint32_t HeapLimit = 0x001F0000u;

        struct Allocation
        {
            uint32_t address = 0;
            uint32_t size = 0;
        };

        struct DmaStart
        {
            int irq = 0;
            uint64_t delayCycles = 0;
        };

        IopMemory();

        void reset();

        [[nodiscard]] uint8_t read8(uint32_t address) const;
        [[nodiscard]] uint16_t read16(uint32_t address) const;
        [[nodiscard]] uint32_t read32(uint32_t address) const;
        void write8(uint32_t address, uint8_t value);
        void write16(uint32_t address, uint16_t value);
        void write32(uint32_t address, uint32_t value);

        [[nodiscard]] bool readRam(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeRam(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroRam(uint32_t address, size_t size);
        [[nodiscard]] bool ownsRamRange(uint32_t address, size_t size) const;
        [[nodiscard]] bool isHardwareAddress(uint32_t address) const;
        [[nodiscard]] std::string readString(uint32_t address, size_t limit = 1024u) const;

        // Free-list allocator over [HeapBase, HeapLimit). `fixed` selects an
        // exact address (AllocSysMemory ADDR mode); `last` allocates the
        // highest fitting free block (LAST mode); the default is the lowest
        // fitting free block (FIRST mode).
        [[nodiscard]] uint32_t allocate(uint32_t size, uint32_t alignment = 16u,
                                        std::optional<uint32_t> fixed = std::nullopt,
                                        bool last = false);
        [[nodiscard]] bool freeAllocation(uint32_t address);
        [[nodiscard]] uint32_t maxFreeMemory() const;
        [[nodiscard]] uint32_t totalFreeMemory() const;
        [[nodiscard]] std::optional<Allocation> allocationContaining(uint32_t address) const;

        [[nodiscard]] uint32_t interruptStatus() const noexcept { return m_interruptStatus; }
        [[nodiscard]] uint32_t interruptMask() const noexcept { return m_interruptMask; }
        [[nodiscard]] uint32_t interruptControl() const noexcept { return m_interruptControl; }
        void setInterruptStatus(uint32_t value) noexcept { m_interruptStatus = value; }
        void setInterruptMask(uint32_t value) noexcept { m_interruptMask = value; }
        void setInterruptControl(uint32_t value) noexcept { m_interruptControl = value & 1u; }

        [[nodiscard]] std::optional<DmaStart> takeDmaStart() noexcept;
        [[nodiscard]] std::span<const uint8_t> ram() const noexcept { return m_ram; }
        [[nodiscard]] uint8_t *ramData() noexcept { return m_ram.data(); }

        // SUIKO: optional real SPU2. When set, the SPU2 register window and
        // DMA channels 4/7 are routed to the host instead of the register
        // store / fake DMA-complete path below.
        void setSpu2Host(IopSpu2Host *host) noexcept { m_spu2Host = host; }
        [[nodiscard]] IopSpu2Host *spu2Host() const noexcept { return m_spu2Host; }

        [[nodiscard]] static bool isSpu2DmaRegister(uint32_t physicalAddress) noexcept
        {
            return (physicalAddress >= 0x1F8010C0u && physicalAddress < 0x1F8010D0u) ||
                   (physicalAddress >= 0x1F801500u && physicalAddress < 0x1F801510u);
        }

        // Diagnostic write watchpoint over physical RAM: invokes the callback with
        // the physical address, stored value, and access size for every write into
        // [low, high).
        void setWriteWatch(uint32_t low, uint32_t high, std::function<void(uint32_t, uint32_t, uint32_t)> callback)
        {
            m_watchLow = low;
            m_watchHigh = high;
            m_watchCallback = std::move(callback);
        }

        void setReadWatch(uint32_t low, uint32_t high, std::function<void(uint32_t, uint32_t)> callback)
        {
            m_readLow = low;
            m_readHigh = high;
            m_readCallback = std::move(callback);
        }

        [[nodiscard]] static uint32_t physicalAddress(uint32_t address) noexcept;

    private:
        [[nodiscard]] uint32_t readHardware32(uint32_t address) const;
        void writeHardware32(uint32_t address, uint32_t value);
        void markOwned(uint32_t address, size_t size);

        std::vector<uint8_t> m_ram;
        std::vector<uint8_t> m_owned;
        std::vector<uint8_t> m_scratch;
        std::unordered_map<uint32_t, uint32_t> m_hardware;
        std::vector<Allocation> m_allocations;
        uint32_t m_interruptStatus = 0;
        uint32_t m_interruptMask = 0;
        uint32_t m_interruptControl = 1;
        std::optional<DmaStart> m_dmaStart;
        std::function<void(uint32_t, uint32_t, uint32_t)> m_watchCallback;
        uint32_t m_watchLow = 0;
        uint32_t m_watchHigh = 0;
        std::function<void(uint32_t, uint32_t)> m_readCallback;
        uint32_t m_readLow = 0;
        uint32_t m_readHigh = 0;
        IopSpu2Host *m_spu2Host = nullptr;
    };
}
