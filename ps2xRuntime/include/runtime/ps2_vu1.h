#ifndef PS2_VU1_H
#define PS2_VU1_H

#include "runtime/ps2_vu1_aot.h"

#if defined(__GNUC__) || defined(__clang__)
#define PS2X_VU1_INLINE inline __attribute__((always_inline))
#else
#define PS2X_VU1_INLINE inline
#endif

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

class GS;
class PS2Memory;
class VU1InventoryRecorder;

struct VU1State
{
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q;
    float p;
    float i;
    uint32_t r;
    uint32_t pc;
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    uint64_t cycles;
    bool ebit;
    bool haltAfterDelaySlot;
    bool dBitEnabled;
    bool tBitEnabled;
    bool stoppedByD;
    bool stoppedByT;
    uint32_t top;  // VIF TOP visible to XTOP
    uint32_t itop; // VIF ITOP visible to XITOP

    bool branchPending;
    uint32_t branchTarget;
    uint32_t branchDelay;
};

class VU1Interpreter
{
public:
    enum class Unit : uint8_t
    {
        VU0,
        VU1
    };

    explicit VU1Interpreter(Unit unit = Unit::VU1);

    void reset();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    VU1State &state() { return m_state; }
    const VU1State &state() const { return m_state; }

    // PS2X(vu1-aot-v2): test seams (tests/vu1/vu1_aot_selftest.cpp).  Both
    // are inert unless called: the game never disables AOT per interpreter and
    // never captures packets outside the differential harness.
    void setAotEnabled(bool enabled) { m_aotDisabled = !enabled; }
    void setXgkickCapture(std::vector<std::vector<uint8_t>> *capture) { m_xgkickCapture = capture; }
    void resetAotProgramCache() { m_aotProgramCacheValid = false; }
    uint64_t lastRunPairs() const { return m_traceInsns; }

    // PS2X(vu1-aot): narrow seam used by generated code.  The decoded pair
    // comes from the program's constexpr table; everything else stays private.
    ps2x::vu1::AotResult aotStepPair(const ps2x::vu1::DecodedPair &decoded);
    uint32_t aotPc() const { return m_state.pc; }

    // PS2X(vu1-aot-v2): one instantiation per microcode pair.  kPc and the
    // decoded pair are compile-time constants so the compiler removes every
    // branch and loop that the generic step routine would evaluate at run time.
    template <uint32_t kPc, const ps2x::vu1::DecodedPair &D>
    ps2x::vu1::AotResult aotStepConstant();
    template <uint32_t kPc, const ps2x::vu1::DecodedPair &D>
    uint64_t pairReadyCycleConstant() const;
    template <const ps2x::vu1::DecodedPair &D>
    void markPairWritesConstant();

private:
    using Pipeline = ps2x::vu1::Pipeline;
    using enum ps2x::vu1::Pipeline;
    using VfAccess = ps2x::vu1::VfAccess;
    using InstructionUsage = ps2x::vu1::InstructionUsage;
    using DecodedInstructionPair = ps2x::vu1::DecodedPair;

    struct FlagPipelineEntry
    {
        uint64_t readyCycle = 0;
        uint64_t issueCycle = 0;
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t extraSticky = 0;
        uint32_t clip = 0;
        bool valid = false;
        bool writesMac = false;
        bool writesStatus = false;
        bool writesSticky = false;
        bool writesClip = false;
    };

    struct ScalarPipelineEntry
    {
        uint64_t readyCycle = 0;
        float value = 0.0f;
        uint32_t statusDi = 0;
        bool valid = false;
    };

    struct PendingStore
    {
        uint64_t readyCycle = 0;
        uint32_t address = 0;
        std::array<uint32_t, 4> words{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingVfWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t reg = 0;
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingViWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        int32_t value = 0;
        uint8_t reg = 0;
        bool valid = false;
    };

    struct PendingAccWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct XgkickPipeline
    {
        static constexpr uint32_t kBufferSize = 0x10000u;
        std::array<uint8_t, kBufferSize> packet{};
        uint32_t sourceAddress = 0;
        uint32_t totalBytes = 0;
        uint32_t copiedBytes = 0;
        uint32_t currentTagEnd = 0;
        uint32_t cycleCredit = 0;
        uint64_t issueCycle = 0;
        bool active = false;
        bool currentTagEop = false;
    };

    static constexpr uint32_t kFmacLatency = 4u;
    static constexpr uint32_t kAccForwardLatency = 1u;
    static constexpr uint32_t kMaxFlagEntries = 8u;
    static constexpr uint32_t kMaxPendingStores = 8u;
    static constexpr uint32_t kMaxPendingVfWrites = 16u;
    static constexpr uint32_t kMaxPendingViWrites = 8u;
    static constexpr uint32_t kMaxPendingAccWrites = 8u;
    static constexpr uint32_t kMaxDecodedPairs = 0x4000u / 8u;

    Unit m_unit;
    VU1State m_state;
    std::array<DecodedInstructionPair, kMaxDecodedPairs> m_decodedCodeCache{};
    const uint8_t *m_cachedVuCode = nullptr;
    const PS2Memory *m_cachedMemory = nullptr;
    uint32_t m_cachedCodeSize = 0;
    uint64_t m_cachedCodeGeneration = 0;
    bool m_decodedCodeCacheValid = false;

    std::array<FlagPipelineEntry, kMaxFlagEntries> m_flagPipeline{};
    ScalarPipelineEntry m_fdiv{};
    std::array<ScalarPipelineEntry, 2> m_efu{};
    std::array<PendingStore, kMaxPendingStores> m_storePipeline{};
    std::array<PendingVfWrite, kMaxPendingVfWrites> m_vfWritePipeline{};
    std::array<PendingViWrite, kMaxPendingViWrites> m_viWritePipeline{};
    std::array<PendingAccWrite, kMaxPendingAccWrites> m_accWritePipeline{};
    // Bit per occupied slot.  Commits and frees walk only live entries instead
    // of scanning every slot on each executed instruction pair.
    uint16_t m_flagValid = 0u;
    uint8_t m_efuValid = 0u;
    uint16_t m_storeValid = 0u;
    uint16_t m_vfValid = 0u;
    uint16_t m_viValid = 0u;
    uint16_t m_accValid = 0u;
    XgkickPipeline m_xgkick{};
    std::array<std::array<uint64_t, 4>, 32> m_vfReady{};
    std::array<uint64_t, 16> m_viReady{};
    std::array<uint64_t, 4> m_accReady{};
    std::array<std::array<uint64_t, 4>, 32> m_vfLatestWrite{};
    std::array<uint64_t, 16> m_viLatestWrite{};
    std::array<uint64_t, 4> m_accLatestWrite{};

    uint64_t m_cycle = 0;
    uint64_t m_nextWriteSequence = 0;
    uint64_t m_efuResourceReady = 0;
    // Earliest readyCycle among pending pipeline entries.  commitReadyPipelines()
    // can then return without scanning every array on the cycles where nothing
    // becomes visible.
    uint64_t m_nextCommitCycle = UINT64_MAX;
    // PS2X(vu1-aot-v2): earliest ready cycle per pipeline type.  commit only
    // walks a type whose entries can be visible this cycle; the others are one
    // compare.  The exhaustive scan used to visit every pending entry just to
    // recompute the global minimum.
    uint64_t m_nextFlagCycle = UINT64_MAX;
    uint64_t m_nextFdivCycle = UINT64_MAX;
    uint64_t m_nextEfuCycle = UINT64_MAX;
    uint64_t m_nextStoreCycle = UINT64_MAX;
    uint64_t m_nextVfCycle = UINT64_MAX;
    uint64_t m_nextViCycle = UINT64_MAX;
    uint64_t m_nextAccCycle = UINT64_MAX;
    uint32_t m_workingClip = 0;
    uint32_t m_currentUpperInstruction = 0;
    int32_t m_viBranchBackupValue = 0;
    uint8_t m_viBranchBackupReg = 0;
    bool m_viBranchBackupValid = false;
    uint8_t *m_activeVuData = nullptr;
    uint32_t m_activeVuDataSize = 0;
    GS *m_activeGs = nullptr;
    PS2Memory *m_activeMemory = nullptr;
    bool m_stopRequested = false;
    bool m_pendingHaltD = false;
    bool m_pendingHaltT = false;
    // PS2X_VU1_DIFF: run the reference scheduler and capture XGKICK packets.
    bool m_referenceScheduler = false;
    std::vector<std::vector<uint8_t>> *m_xgkickCapture = nullptr;

    // PS2X(vu1-aot): compiled-program dispatch.
    const uint8_t *m_activeVuCode = nullptr;
    uint32_t m_activeCodeSize = 0;
    uint64_t m_budgetEnd = 0;
    VU1InventoryRecorder *m_inventory = nullptr;
    bool m_traceActive = false;
    uint32_t m_traceInsns = 0;
    const char *m_stopReason = "budget";
    uint32_t m_traceStartPc = 0;
    const ps2x::vu1::CompiledProgram *m_aotProgram = nullptr;
    const uint8_t *m_aotProgramCode = nullptr;
    uint32_t m_aotProgramCodeSize = 0;
    uint64_t m_aotProgramGeneration = 0;
    bool m_aotProgramCacheValid = false;
    bool m_aotDisabled = false;

    const ps2x::vu1::CompiledProgram *findCompiledProgram(uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory);

    void run(uint8_t *vuCode, uint32_t codeSize,
             uint8_t *vuData, uint32_t dataSize,
             GS &gs, PS2Memory *memory, uint32_t maxCycles);

    InstructionUsage decodeUpperUsage(uint32_t upper) const;
    InstructionUsage decodeLowerUsage(uint32_t lower) const;
    static void addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static void addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static uint8_t vfReadLanes(const InstructionUsage &usage, uint8_t reg);
    DecodedInstructionPair decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const;
    DecodedInstructionPair getDecodedInstructionPairForPc(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc);
    void rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory, uint64_t generation);

    void execUpper(uint32_t instr);
    template <uint32_t U> void execUpperConstant();
    template <uint32_t U, uint32_t Op, bool WriteAcc, bool CrossNoAcc>
    void execUpperArith();
    template <uint32_t U, uint32_t Special> void execUpperSpecial();
    void execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);
    template <uint32_t kPc, uint32_t L>
    void execLowerConstant(uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory,
                           uint32_t upperInstr);

    // PS2X(vu1-aot-v2): kRuntimeUpper selects the interpreter's runtime
    // instruction word; any other value is the generated pair's compile-time
    // upper word, which lets the compiler fold the opcode switches (and the
    // dead branches of the exact-result machinery) out of every pair.
    static constexpr uint32_t kRuntimeUpper = 0xFFFFFFFFu;
    template <uint32_t U>
    uint32_t upperWord() const
    {
        if constexpr (U == kRuntimeUpper)
            return m_currentUpperInstruction;
        else
            return U;
    }

    void applyDest(float *dst, const float *result, uint8_t dest);
    void applyDestAcc(const float *result, uint8_t dest);
    template <uint32_t U = kRuntimeUpper>
    void applyFmacDest(float *dst, float *result, uint8_t dest);
    template <uint32_t U = kRuntimeUpper>
    void applyFmacDestAcc(float *result, uint8_t dest);
    template <uint32_t U = kRuntimeUpper>
    void normalizeFmacResult(float *result, uint8_t dest, uint8_t laneFlags[4]);
    template <uint32_t U = kRuntimeUpper>
    bool calculateFmacExactResult(uint32_t component, long double &result) const;
    uint8_t normalizeFmacExactResult(float &value, long double exactResult) const;
    template <uint32_t U = kRuntimeUpper>
    uint32_t calculateFmacProductSticky(uint8_t dest) const;
    void updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky);
    void queueFsset(uint16_t immediate);
    void queueClip(uint32_t clip);
    void queueFcset(uint32_t clip);
    void queueQ(float value, uint32_t latency, uint32_t statusDi);
    void queueP(float value, uint32_t latency);
    void queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask);
    void queueVfWrite(uint8_t reg, uint8_t laneMask, const float value[4], uint32_t latency);
    void queueViWrite(uint8_t reg, int32_t value, uint32_t latency);
    void queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency);
    void startXgkick(uint32_t qwordAddress);

    void resetScheduler();
    void commitReadyPipelines();
    PS2X_VU1_INLINE void advanceOneCycle()
    {
        ++m_cycle;
        m_state.cycles = m_cycle;
        // LSU commits become visible at the cycle boundary before PATH1
        // consumes its next qword from VU memory.
        if (m_nextCommitCycle <= m_cycle)
            commitReadyPipelines();
        if (m_xgkick.active)
            progressXgkick();
    }
    void commitReadyPipelinesReference();
    void advanceTo(uint64_t targetCycle);
    void runDifferential(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles);
    void runAotDifferential(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory, uint32_t maxCycles);
    void runDifferentialImpl(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory, uint32_t maxCycles,
                             bool candidateAot);
    void flushPipelines();
    void progressXgkick();
    void finishXgkick();
    uint64_t calculatePairReadyCycle(const DecodedInstructionPair &decoded) const;
    void markPairWrites(const DecodedInstructionPair &decoded);
    bool pipelinesPending() const;

    static constexpr uint8_t lane(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }

    // flush-to-zero/one operand conversion; hot enough (every VF read and
    // FMAC exact-result lane) that it lives in the header for inlining.
    float normalizeOperand(float value) const
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0xFFu)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    float normalizeResult(float value, uint32_t &laneFlags) const;
    uint32_t microAddressMask() const { return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu; }
    int32_t readBranchVi(uint8_t reg) const;
    void recordViWriteForBranch(uint8_t reg, int32_t oldValue);
    void reportReservedInstruction(bool upper, uint32_t instruction);
    float broadcast(const float *vf, uint8_t bc);
};

#include "runtime/ps2_vu1_pipe_inl.h"
#include "runtime/ps2_vu1_upper_inl.h"
#include "runtime/ps2_vu1_lower_inl.h"
#include "runtime/ps2_vu1_aot_step.inl"

#endif
