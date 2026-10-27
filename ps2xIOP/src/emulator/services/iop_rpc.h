#pragma once

#include "ps2x/iop/iop_types.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopGuestExecutor
    {
    public:
        virtual ~IopGuestExecutor() = default;

        [[nodiscard]] virtual uint32_t executeGuestFunction(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp) = 0;
        [[nodiscard]] virtual uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                                      uint32_t a0,
                                                                      uint32_t a1,
                                                                      uint32_t a2,
                                                                      uint32_t a3,
                                                                      uint32_t gp,
                                                                      uint32_t instructionBudget)
        {
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
    };

    class IopRpcBridge
    {
    public:
        IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request, IopGuestExecutor &executor);
        void onSifTransfer(const SifTransfer &transfer);
        void removeServersInRange(uint32_t base, uint32_t size);

        // PS2X(iop-rpc-thread): sifcmd RPC as on the IOP (docs/iop-kernel-rules.md,
        // R1).  A call to a server whose queue has a thread (SetRpcQueue) is
        // queued and that thread is woken; its RpcLoop runs the handler on the
        // thread, where it may block, and the reply goes to the EE when the
        // handler returns.  Returns the request's token, or 0 when the server
        // has no queue thread and handleRpc serves it directly.
        [[nodiscard]] uint64_t enqueueRequest(const RpcRequest &request);
        void bindLoopThread(uint32_t queue, int threadId);
        struct Dispatch
        {
            uint64_t token = 0;
            uint32_t function = 0;
            uint32_t rpcNumber = 0;
            uint32_t buffer = 0;
            uint32_t size = 0;
            uint32_t gp = 0;
        };
        // The oldest request waiting on `queue`, now in flight.
        [[nodiscard]] std::optional<Dispatch> takeNextRequest(uint32_t queue);
        // The handler returned `returnPointer`: copy the reply to the EE.
        void finishRequest(uint64_t token, uint32_t returnPointer);
        // True once, when the request's reply has been sent.
        [[nodiscard]] bool takeDone(uint64_t token);

        [[nodiscard]] bool hasServer(uint32_t sid) const noexcept;
        [[nodiscard]] size_t serverCount() const noexcept { return m_servers.size(); }

    private:
        struct RpcServer
        {
            uint32_t sid = 0;
            uint32_t serverData = 0;
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t buffer = 0;
            uint32_t callback = 0;
            uint32_t callbackBuffer = 0;
            uint32_t queue = 0;
        };

        struct PendingRequest
        {
            uint64_t token = 0;
            uint32_t sid = 0;
            uint32_t rpcNumber = 0;
            uint32_t sendSize = 0;
            GuestBuffer receive;
        };
        struct RpcQueue
        {
            int threadId = 0;
            std::deque<PendingRequest> requests;
        };

        IopHost &m_host;
        IopMemory &m_memory;
        IopKernel &m_kernel;
        std::unordered_map<uint32_t, RpcServer> m_servers;
        std::unordered_map<uint32_t, RpcQueue> m_queues;
        std::unordered_map<uint64_t, PendingRequest> m_inFlight;
        std::unordered_set<uint64_t> m_done;
        uint64_t m_nextToken = 0;
        uint32_t m_nextDmaId = 1u;
        bool m_sifInitialized = false;
    };
}
