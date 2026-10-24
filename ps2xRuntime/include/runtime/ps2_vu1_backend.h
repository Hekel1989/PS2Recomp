#ifndef PS2_VU1_BACKEND_H
#define PS2_VU1_BACKEND_H

// An alternative VU1 executor the host can install, typically a recompiler
// such as PCSX2's microVU1.
//
// With a backend installed the runtime keeps VU1 micro and data memory in the
// backend's buffers (a JIT encodes their addresses into its code) and calls
// run() at every MSCAL/MSCALF/MSCNT instead of VU1Interpreter.  Each XGKICK
// packet comes back whole through the sink, in program order, on the thread
// that called run(): the VU1 thread when it is on, else the game thread.
// Calls are never concurrent.
//
// A backend is not bit-exact with VU1Interpreter (microVU uses SSE floats with
// PCSX2's clamping), so PS2X_VU1_THREAD=verify, whose shadow runs the
// interpreter, keeps the interpreter.  PS2X_VU1_JIT=0 ignores the backend;
// PS2X_VU1_JIT=diff runs both on every kick and reports differences.
//
// Register before PS2Runtime::initialize().  Plain types only, so the header
// stays free of the backend's own (e.g. GPL PCSX2) types.

#include <cstdint>

class GS;
class PS2Memory;
class VU1Interpreter;

namespace ps2x
{
    struct Vu1BackendRegisters
    {
        float vf[32][4];
        uint16_t vi[16];
        float acc[4];
        float q, p, i;
        uint32_t r;
        uint32_t status, mac, clip;
    };

    struct Vu1Backend
    {
        using Sink = void (*)(void *user, const uint8_t *data, uint32_t bytes);

        struct RunArgs
        {
            uint32_t startPc = 0; // byte address; ignored when resume is set
            bool resume = false;  // MSCNT: continue at TPC
            uint32_t top = 0;
            uint32_t itop = 0;
            uint32_t fbrst = 0;   // VU0 FBRST (DE1 0x400, TE1 0x800)
            uint32_t cycles = 0;
            uint64_t microGeneration = 0; // PS2Memory::getVU1CodeGeneration()
            Sink sink = nullptr;
            void *user = nullptr;
        };

        struct RunResult
        {
            bool finished = false;
            bool stoppedByD = false;
            bool stoppedByT = false;
            uint32_t cycles = 0;
            uint32_t tpc = 0;
        };

        const char *name = "";
        uint8_t *micro = nullptr; // PS2_VU1_CODE_SIZE bytes
        uint8_t *data = nullptr;  // PS2_VU1_DATA_SIZE bytes
        // Drops every compiled program and resets VU1's registers.
        void (*reset)() = nullptr;
        RunResult (*run)(const RunArgs &args) = nullptr;
        // PS2X_VU1_JIT=diff only.
        void (*readRegisters)(Vu1BackendRegisters &out) = nullptr;
        void (*writeRegisters)(const Vu1BackendRegisters &in) = nullptr;
    };

    // nullptr clears it.  The object must outlive the runtime.
    void setVu1Backend(const Vu1Backend *backend);
    const Vu1Backend *vu1Backend();

    // Runtime side: runs each VU1 kick on the backend (PS2X_VU1_JIT unset/1)
    // or on both the backend and the interpreter (PS2X_VU1_JIT=diff), where
    // the interpreter's packets and state stay authoritative and every
    // difference is counted and the first ones are logged.
    class Vu1BackendRunner
    {
    public:
        enum class Mode : uint8_t
        {
            Off,
            On,
            Diff,
        };
        static Mode modeFromEnvironment();

        Vu1BackendRunner(const Vu1Backend &backend, PS2Memory &memory, VU1Interpreter &vu1, GS &gs, Mode mode);
        ~Vu1BackendRunner();

        struct Kick
        {
            bool resume = false;
            uint32_t startPc = 0;
            uint32_t top = 0;
            uint32_t itop = 0;
            uint32_t fbrst = 0;
        };
        struct Stop
        {
            bool byD = false;
            bool byT = false;
        };
        Stop run(const Kick &kick);

    private:
        Stop runDiff(const Kick &kick);
        void report(bool force);

        const Vu1Backend &m_backend;
        PS2Memory &m_memory;
        VU1Interpreter &m_vu1;
        GS &m_gs;
        const Mode m_mode;

        struct DiffStats;
        DiffStats *m_diff = nullptr;
    };
} // namespace ps2x

#endif
