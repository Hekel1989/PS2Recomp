#pragma once

// SUIKO(pad-host-hooks): optional host hook that lets the embedding application
// feed real controller/keyboard state into the PAD HLE (see docs/input.md).
// The suikoden frontend pumps SDL3 events on the main thread and hands the
// resulting DualShock 2 report to the game thread through this provider; the
// header itself stays submodule-clean (no SDL/raylib types).
//
// Threading: the provider is called from the game thread inside scePadRead, once
// per port being read. The host must make it thread-safe. Register it before
// PS2Runtime::run() and clear it (set an empty provider) only after the game
// thread has joined. Returning false means "no report for this port" and the
// HLE falls back to its own backends. PS2X_PAD_SCRIPT keeps overriding port 0
// buttons while it is active, so scripted automated runs stay deterministic.

#include <array>
#include <cstdint>
#include <functional>
#include <utility>

namespace Ps2PadHostHooks
{
    struct Report
    {
        uint16_t buttons = 0xFFFF; // active-low: a cleared bit is pressed
        uint8_t lx = 0x80;         // analog sticks, PS2 range 0-255, centre 0x80
        uint8_t ly = 0x80;
        uint8_t rx = 0x80;
        uint8_t ry = 0x80;
        // Optional per-button pressure for DualShock pressure mode, in report
        // byte order 8..19: right left up down triangle circle cross square
        // l1 l2 r1 r2. Only consulted when hasPressure is set; otherwise the
        // HLE derives digital 0xFF/0x00 values from the button bits.
        std::array<uint8_t, 12> pressure{};
        bool hasPressure = false;
    };

    // Returns true when the host has a report for this port (fills out).
    using Provider = std::function<bool(int port, Report &out)>;

    inline Provider &mutableProvider()
    {
        static Provider provider;
        return provider;
    }

    inline const Provider &provider()
    {
        return mutableProvider();
    }

    inline void setProvider(Provider provider)
    {
        mutableProvider() = std::move(provider);
    }
} // namespace Ps2PadHostHooks
