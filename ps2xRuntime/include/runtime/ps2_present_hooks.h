#pragma once

// SUIKO(gs-phase6): optional host hooks that let the embedding application take
// over presentation (see docs/gs-port-plan.md phase 6). When the host shows the
// game through its own window (the PCSX2 GS Vulkan swapchain), the runtime must
// not open the raylib window nor run its CPU-framebuffer present loop. The
// runtime only calls these; it never requires them, and they contain no
// raylib/PCSX2 types, so this header stays submodule-clean.
//
// Threading: both hooks are queried on the main thread (the thread that calls
// PS2Runtime::initialize/run). presentationDisabled is read once in
// initialize() and again in run(); mainTick is called once per main-loop
// iteration while presentation is disabled, and is the host's chance to pump
// its window events and request shutdown.

#include <functional>
#include <utility>

namespace Ps2PresentHooks
{
    struct Hooks
    {
        // When this returns true, initialize() must not open the raylib window
        // (no InitWindow/InitAudioDevice/SetTargetFPS/debug UI) and run() must
        // not run the raylib present loop. The host is responsible for pacing
        // and for stopping the runtime (requestStop via mainTick's return).
        std::function<bool()> presentationDisabled;

        // Called once per run() main-loop iteration while presentationDisabled()
        // is true, on the main thread. Returning true requests shutdown: run()
        // breaks out of the loop, stops the game thread and returns.
        std::function<bool()> mainTick;
    };

    inline Hooks &mutableHooks()
    {
        static Hooks hooks;
        return hooks;
    }

    inline const Hooks &hooks()
    {
        return mutableHooks();
    }

    inline void setHooks(Hooks hooks)
    {
        mutableHooks() = std::move(hooks);
    }
} // namespace Ps2PresentHooks
