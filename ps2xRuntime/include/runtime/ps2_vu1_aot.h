#ifndef PS2_VU1_AOT_H
#define PS2_VU1_AOT_H

#include <array>
#include <cstdint>

class VU1Interpreter;

// SUIKO(vu1-aot): shared types for the build-time compiled VU1 microprograms.
//
// The generator (tools/vu1_aot.py) emits a constexpr DecodedPair table per
// program.  The interpreter uses the same types for its decoded-pair cache, so
// generated and interpreted execution feed identical data to the step routine.
namespace suiko::vu1
{
    enum class Pipeline : uint8_t
    {
        PipelineNone = 0,
        PipelineFmac,
        PipelineLsu,
        PipelineFdiv,
        PipelineEfu,
        PipelineIalu,
        PipelineBranch,
        PipelineXgkick
    };

    struct VfAccess
    {
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };

    struct InstructionUsage
    {
        std::array<VfAccess, 2> vfRead{};
        VfAccess vfWrite{};
        uint8_t vfReadCount = 0;
        uint16_t viRead = 0;
        uint16_t viWrite = 0;
        uint8_t accRead = 0;
        uint8_t accWrite = 0;
        uint8_t latency = 0;
        uint8_t vfLatency = 0;
        uint8_t viLatency = 0;
        Pipeline pipeline = Pipeline::PipelineNone;
        bool waitQ = false;
        bool waitP = false;
        bool readsClip = false;
        bool writesClip = false;
        bool delaysNextBranchRead = false;
        bool reserved = false;
    };

    struct DecodedPair
    {
        uint32_t lower = 0;
        uint32_t upper = 0;
        InstructionUsage lowerUsage{};
        InstructionUsage upperUsage{};
        bool iBit = false;
        bool eBit = false;
        bool mBit = false;
        bool dBit = false;
        bool tBit = false;
        uint8_t upperVfShadowReg = 0;
        uint8_t suppressedLowerVf = 0;
    };

    enum class AotResult : uint8_t
    {
        Continue = 0,
        ProgramEnded,
        BudgetEnded,
        StopRequested,
        Fallback
    };

    struct CompiledProgram
    {
        uint32_t programHash = 0;
        uint32_t extentOffset = 0;
        uint32_t extentSize = 0;
        const uint32_t *entries = nullptr;
        uint32_t entryCount = 0;
        AotResult (*run)(VU1Interpreter &) = nullptr;
    };

    // FNV-1a 32-bit, the hash convention used by docs/vu1-inventory.md.
    uint32_t hash(const uint8_t *data, uint32_t size);
    void registerCompiledProgram(const CompiledProgram *program);
}

// The generated registry defines this strongly; the runtime provides a weak
// no-op so a build without generated VU1 code still links.
extern "C" void suikoVu1RegisterGeneratedPrograms();

#endif
