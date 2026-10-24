// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// PS2X: host interface for the real SPU2 core. ps2xIOP owns the IOP side
// (register window 0x1F900000-0x1F9007FF, SPU2 DMA channels 4 and 7, INTC
// lines); the host implements the SPU2 itself. With no host attached the
// emulator keeps its original register-store / fake-DMA behaviour.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2x::iop
{
	class IopSpu2Host
	{
	public:
		virtual ~IopSpu2Host() = default;

		// Physical IOP RAM, handed over once when the host is attached (and
		// again if the emulator re-attaches). The SPU2 core reads AutoDMA
		// buffers and writes DMA-read results through this pointer.
		virtual void setIopRam(uint8_t* ram, size_t size) = 0;

		// 16-bit SPU2 registers, physical addresses 0x1F900000-0x1F9007FF.
		virtual uint16_t readRegister(uint32_t address) = 0;
		virtual void writeRegister(uint32_t address, uint16_t value) = 0;

		// IOP DMA channel 4/7 registers (MADR, BCR, CHCR, TADR). Writes to
		// CHCR with the start bit perform the transfer synchronously.
		virtual uint32_t readDmaRegister(uint32_t address) = 0;
		virtual void writeDmaRegister(uint32_t address, uint32_t value) = 0;

		// Advance the SPU2 to the given absolute IOP cycle. Returns a bitmask
		// of interrupt lines to deliver: bit 9 is the SPU2 line, bits 36/40
		// are the SPU2 DMA channel lines used by LIBSD.
		virtual uint64_t advance(uint64_t iopCycle) = 0;

		// Keep the SPU2's notion of "now" in step before a register/DMA
		// access, without mixing (the access itself calls TimeUpdate).
		virtual void syncCycles(uint64_t iopCycle) = 0;
	};
} // namespace ps2x::iop
