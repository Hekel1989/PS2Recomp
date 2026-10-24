#include "module_factories.h"
#include "rpc_reply.h"

#include "ps2x/iop/iso9660.h"
#include "ps2x/iop/ps2_path.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        // SID of the IOP FILEIO RPC server (ps2sdk iop/fs/fileio). On retail
        // hardware FILEIO.SYS registers it at boot; the game's EE libfileio
        // client binds it and blocks until the server answers, so an
        // always-active HLE provider is required once the client runs as guest
        // code (see config/recomp_base.toml `unstub`).
        constexpr uint32_t kFileIoSid = 0x80000001u;

        // RPC function numbers from ps2sdk common/include/fileio-common.h.
        constexpr uint32_t kFuncOpen = 0u;
        constexpr uint32_t kFuncClose = 1u;
        constexpr uint32_t kFuncRead = 2u;
        constexpr uint32_t kFuncWrite = 3u;
        constexpr uint32_t kFuncLseek = 4u;
        constexpr uint32_t kFuncIoctl = 5u;
        constexpr uint32_t kFuncDopen = 9u;
        constexpr uint32_t kFuncDclose = 10u;
        constexpr uint32_t kFuncDread = 11u;
        constexpr uint32_t kFuncGetstat = 12u;
        // Sony libfileio extension: the client's init handshake. The client
        // sends the two EE transfer-slot addresses and expects an 8-byte
        // reply whose second word is 2 (protocol ready marker).
        constexpr uint32_t kFuncInit = 0xFFu;

        // Negative IOP errno values, matching what the real server returns.
        constexpr int32_t kEnoent = -2;
        constexpr int32_t kEbadf = -9;
        constexpr int32_t kErofs = -30;

        // rom0:ROMVER contents, identical to ps2xRuntime's PS2RomDevice.
        constexpr char kRomVersion[] = "0200AC20040614";

        constexpr size_t kMaxPathBytes = 1024u;
        constexpr size_t kTransferChunk = 64u * 1024u;
        constexpr uint32_t kMaxFds = 64u;
        constexpr uint32_t kMaxOpenLogs = 64u;

        // One open file, indexed by the client's fd-table slot index
        // (send+0x414): the game's sceOpen returns that slot as the fd and
        // later calls pass it back. rom0: entries carry their bytes in
        // `memory`; everything else is a window into a host file (ISO image
        // or plain).
        struct OpenEntry
        {
            bool used = false;
            uint64_t hostHandle = 0u;
            uint64_t baseOffset = 0u;
            uint64_t size = 0u;
            uint64_t position = 0u;
            std::vector<uint8_t> memory;
        };

        bool readFully(IopHost &host, uint64_t handle, uint64_t offset, uint8_t *destination, size_t size)
        {
            size_t done = 0u;
            while (done < size)
            {
                size_t bytesRead = 0u;
                if (!host.readHostFile(handle, offset + done, destination + done, size - done, bytesRead) ||
                    bytesRead == 0u)
                {
                    return false;
                }
                done += bytesRead;
            }
            return true;
        }

        bool equalsIgnoreCase(std::string_view left, std::string_view right)
        {
            if (left.size() != right.size())
                return false;
            for (size_t i = 0u; i < left.size(); ++i)
            {
                char a = left[i];
                char b = right[i];
                if (a >= 'A' && a <= 'Z')
                    a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z')
                    b = static_cast<char>(b - 'A' + 'a');
                if (a != b)
                    return false;
            }
            return true;
        }

        class FileIoService final : public IopService
        {
        public:
            explicit FileIoService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "fileio";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (OpenEntry &entry : m_files)
                {
                    releaseEntry(entry);
                }
                m_files.clear();
                m_openLogs = 0u;
                m_unhandledLogged.clear();
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != kFileIoSid)
                {
                    return result;
                }

                result.handled = true;
                // Every path below writes its reply straight into the receive
                // buffer, so tell the transport not to copy anything else.
                result.resultAddress = request.receive.address;
                result.signalNowaitCompletion = true;
                result.signalCompletion = true;
                // The libfileio client creates a semaphore per call and stores
                // its id at send+0x00; on hardware the IOP's SIF reply signals
                // it (the client then runs WaitSema after CallRpc returns).
                // Init (0xFF) has a different packet layout and no sema wait.
                if (request.function != kFuncInit)
                {
                    result.completionSemaId = static_cast<int32_t>(requestWord(request, 0u));
                }

                switch (request.function)
                {
                case kFuncInit:
                    handleInit(request);
                    break;
                case kFuncOpen:
                    handleOpen(request);
                    break;
                case kFuncClose:
                    complete(request, handleClose(request));
                    break;
                case kFuncRead:
                    complete(request, handleRead(request));
                    break;
                case kFuncWrite:
                    complete(request, handleWrite(request));
                    break;
                case kFuncLseek:
                    complete(request, handleLseek(request));
                    break;
                case kFuncIoctl:
                    complete(request, handleIoctl(request));
                    break;
                case kFuncGetstat:
                    logUnhandled(request, kEnoent);
                    complete(request, kEnoent);
                    break;
                case kFuncDopen:
                case kFuncDclose:
                case kFuncDread:
                    logUnhandled(request, kEnoent);
                    complete(request, kEnoent);
                    break;
                default:
                    logUnhandled(request, kEnoent);
                    complete(request, kEnoent);
                    break;
                }
                return result;
            }

        private:
            // Read the shared request header: {sema, result_ptr, result_size}.
            // The client stores the RPC result word at `result_ptr` and treats
            // a non-zero receive buffer as the completion marker.
            [[nodiscard]] uint32_t resultPointer(const RpcRequest &request) const
            {
                uint32_t words[3] = {0u, 0u, 0u};
                if (request.send.address == 0u || request.send.size < sizeof(words))
                    return 0u;
                if (!m_host.readGuest(request.send.address, words, sizeof(words)))
                    return 0u;
                return words[1];
            }

            [[nodiscard]] uint32_t requestWord(const RpcRequest &request, uint32_t offset) const
            {
                uint32_t word = 0u;
                if (request.send.address == 0u || request.send.size < offset + 4u)
                    return 0u;
                if (!m_host.readGuest(request.send.address + offset, &word, sizeof(word)))
                    return 0u;
                return word;
            }

            // Deliver a reply. The client watches two separate words:
            //   - result_ptr (send+0x04): the actual return value. sceOpen
            //     stores it at [fd_struct+0] (matched against the fd argument
            //     by get_iob) and returns its own slot index instead, so open
            //     must echo the slot index from send+0x414 back here.
            //   - receive buffer: a completion marker; the client polls it and
            //     only proceeds once it is non-zero (bnez checks in sceOpen /
            //     sceRead), then reads the value from result_ptr.
            void complete(const RpcRequest &request, int32_t result, int32_t marker = 1)
            {
                const uint32_t resultPtr = resultPointer(request);
                if (resultPtr != 0u)
                {
                    const uint32_t word = static_cast<uint32_t>(result);
                    (void)m_host.writeGuest(resultPtr, &word, sizeof(word));
                }
                const uint32_t word = static_cast<uint32_t>(marker);
                (void)writeRpcWords(m_host, request.receive, {&word, 1});
            }

            void handleInit(const RpcRequest &request)
            {
                // The client compares the first reply word against its own
                // version string "3000" (memcmp in the ready-check helper) and
                // requires the second word to be 2 before it issues any file
                // RPC. The client hands over its read/write transfer slots.
                const std::array<uint32_t, 2> reply{0x30303033u /* "3000" */, 2u};
                (void)writeRpcWords(m_host, request.receive, reply);

                std::ostringstream message;
                message << "[FILEIO:HLE] init slots=0x" << std::hex
                        << requestWord(request, 0u) << "/0x" << requestWord(request, 4u)
                        << " recv=0x" << request.receive.address << "/0x" << request.receive.size
                        << std::dec;
                m_host.log(LogLevel::Info, message.str());
            }

            void handleOpen(const RpcRequest &request)
            {
                // sceOpen copies the path to request+0x14 (max 0x400 bytes).
                std::array<uint8_t, kMaxPathBytes> pathBytes{};
                size_t copySize = 0u;
                if (request.send.address != 0u && request.send.size > 0x14u)
                {
                    copySize = std::min<size_t>(kMaxPathBytes - 1u, request.send.size - 0x14u);
                    if (!m_host.readGuest(request.send.address + 0x14u, pathBytes.data(), copySize))
                    {
                        copySize = 0u;
                    }
                }
                while (copySize > 0u && pathBytes[copySize - 1u] == 0u)
                {
                    --copySize;
                }
                const std::string path(reinterpret_cast<const char *>(pathBytes.data()), copySize);

                // The client's fd-table slot index (send+0x414). sceOpen hands
                // this value back to the game as the fd and expects the reply
                // word at result_ptr to equal it (get_iob matches on it).
                const uint32_t slot = requestWord(request, 0x414u);
                const int32_t fd = slot < kMaxFds ? openFile(path, slot) : kEnoent;
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_openLogs < kMaxOpenLogs)
                    {
                        ++m_openLogs;
                        shouldLog = true;
                    }
                }
                if (shouldLog)
                {
                    std::ostringstream message;
                    message << "[FILEIO:HLE] open '" << path << "' slot=" << fd;
                    m_host.log(LogLevel::Info, message.str());
                }
                complete(request, fd);
            }

            int32_t openFile(const std::string &path, uint32_t slot)
            {
                const ParsedPs2Path parsed = parsePs2Path(path);
                if (!parsed)
                    return kEnoent;

                OpenEntry entry;
                if (parsed.device == Ps2PathDevice::Rom0)
                {
                    if (!equalsIgnoreCase(parsed.path, "ROMVER"))
                        return kEnoent;
                    entry.memory.assign(std::begin(kRomVersion), std::end(kRomVersion) - 1u);
                    entry.size = entry.memory.size();
                }
                else if (parsed.device == Ps2PathDevice::Cdrom)
                {
                    if (!openFromIsoImage(parsed.path, entry) && !openFromHost(path, entry))
                        return kEnoent;
                }
                else if (!openFromHost(path, entry))
                {
                    return kEnoent;
                }

                std::lock_guard<std::mutex> lock(m_mutex);
                if (slot >= m_files.size())
                {
                    m_files.resize(static_cast<size_t>(slot) + 1u);
                }
                OpenEntry &slotEntry = m_files[slot];
                if (slotEntry.used)
                {
                    releaseEntry(slotEntry);  // client reused a leaked slot
                }
                slotEntry = std::move(entry);
                slotEntry.used = true;
                return static_cast<int32_t>(slot);
            }

            bool openFromIsoImage(const std::string &isoPath, OpenEntry &entry)
            {
                const std::string imagePath = m_host.hostPath(HostPathKind::CdImage);
                if (imagePath.empty())
                    return false;
                const uint64_t handle = m_host.openHostFile(imagePath);
                if (handle == 0u)
                    return false;

                iso9660::Entry found;
                const bool ok = iso9660::lookup(
                    [&](void *destination, uint64_t offset, size_t count)
                    {
                        size_t bytesRead = 0u;
                        return m_host.readHostFile(handle, offset, destination, count, bytesRead) &&
                               bytesRead == count;
                    },
                    isoPath, found);
                if (!ok || found.isDirectory)
                {
                    m_host.closeHostFile(handle);
                    return false;
                }
                entry.hostHandle = handle;
                entry.baseOffset = found.byteOffset;
                entry.size = found.sizeBytes;
                return true;
            }

            bool openFromHost(const std::string &path, OpenEntry &entry)
            {
                const std::string hostPath = m_host.translateGuestPath(path);
                if (hostPath.empty())
                    return false;
                const uint64_t handle = m_host.openHostFile(hostPath);
                if (handle == 0u)
                    return false;
                uint64_t size = 0u;
                if (!m_host.hostFileSize(handle, size))
                {
                    m_host.closeHostFile(handle);
                    return false;
                }
                entry.hostHandle = handle;
                entry.size = size;
                return true;
            }

            int32_t handleClose(const RpcRequest &request)
            {
                const int32_t fd = static_cast<int32_t>(requestWord(request, 0xCu));
                std::lock_guard<std::mutex> lock(m_mutex);
                if (fd < 0 || static_cast<uint32_t>(fd) >= m_files.size() || !m_files[fd].used)
                    return kEbadf;
                releaseEntry(m_files[fd]);
                m_files[fd].used = false;
                return 0;
            }

            int32_t handleRead(const RpcRequest &request)
            {
                const int32_t fd = static_cast<int32_t>(requestWord(request, 0xCu));
                const uint32_t destination = requestWord(request, 0x10u);
                const int32_t count = static_cast<int32_t>(requestWord(request, 0x14u));
                if (fd < 0 || destination == 0u || count < 0)
                    return kEbadf;

                std::lock_guard<std::mutex> lock(m_mutex);
                if (static_cast<uint32_t>(fd) >= m_files.size() || !m_files[fd].used)
                    return kEbadf;
                OpenEntry &entry = m_files[fd];

                if (!entry.memory.empty())
                {
                    const uint64_t available =
                        entry.size > entry.position ? entry.size - entry.position : 0u;
                    const size_t bytes =
                        static_cast<size_t>(std::min<uint64_t>(available, static_cast<uint32_t>(count)));
                    if (!m_host.writeGuest(destination, entry.memory.data() + entry.position, bytes))
                        return kEbadf;
                    entry.position += bytes;
                    return static_cast<int32_t>(bytes);
                }

                uint32_t done = 0u;
                std::vector<uint8_t> chunk;
                while (done < static_cast<uint32_t>(count))
                {
                    const uint64_t remaining = entry.size - entry.position;
                    if (remaining == 0u)
                        break;
                    const size_t want = static_cast<size_t>(std::min<uint64_t>(
                        std::min<uint64_t>(remaining, static_cast<uint32_t>(count) - done),
                        static_cast<uint64_t>(kTransferChunk)));
                    chunk.resize(want);
                    if (!readFully(m_host, entry.hostHandle, entry.baseOffset + entry.position,
                                   chunk.data(), want))
                    {
                        return done != 0u ? static_cast<int32_t>(done) : -1;
                    }
                    if (!m_host.writeGuest(destination + done, chunk.data(), want))
                        return kEbadf;
                    entry.position += want;
                    done += static_cast<uint32_t>(want);
                }
                return static_cast<int32_t>(done);
            }

            int32_t handleWrite(const RpcRequest &request)
            {
                (void)request;
                // Every backing store this service exposes (ISO image,
                // extracted disc, rom0:) is read-only.
                return kErofs;
            }

            int32_t handleLseek(const RpcRequest &request)
            {
                const int32_t fd = static_cast<int32_t>(requestWord(request, 0xCu));
                const int32_t offset = static_cast<int32_t>(requestWord(request, 0x10u));
                const uint32_t whence = requestWord(request, 0x18u);
                std::lock_guard<std::mutex> lock(m_mutex);
                if (fd < 0 || static_cast<uint32_t>(fd) >= m_files.size() || !m_files[fd].used)
                    return kEbadf;
                OpenEntry &entry = m_files[fd];
                int64_t position = 0;
                switch (whence)
                {
                case 0u:
                    position = offset;
                    break;
                case 1u:
                    position = static_cast<int64_t>(entry.position) + offset;
                    break;
                case 2u:
                    position = static_cast<int64_t>(entry.size) + offset;
                    break;
                default:
                    return -1;
                }
                if (position < 0)
                    return -1;
                entry.position = static_cast<uint64_t>(position);
                return static_cast<int32_t>(position);
            }

            int32_t handleIoctl(const RpcRequest &request)
            {
                // No ioctls are required by the live callers; report success
                // so the client keeps moving instead of erroring out.
                const int32_t fd = static_cast<int32_t>(requestWord(request, 0xCu));
                std::lock_guard<std::mutex> lock(m_mutex);
                if (fd < 0 || static_cast<uint32_t>(fd) >= m_files.size() || !m_files[fd].used)
                    return kEbadf;
                return 0;
            }

            void logUnhandled(const RpcRequest &request, int32_t value)
            {
                uint64_t signature = (static_cast<uint64_t>(request.function) << 32) |
                                     static_cast<uint64_t>(request.send.size);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (!m_unhandledLogged.insert(signature).second)
                        return;
                }
                std::ostringstream message;
                message << "[FILEIO:HLE] unhandled rpc=0x" << std::hex << request.function
                        << " send=0x" << request.send.address << "/0x" << std::dec << request.send.size
                        << " -> " << value;
                m_host.log(LogLevel::Warning, message.str());
            }

            void releaseEntry(OpenEntry &entry)
            {
                if (entry.hostHandle != 0u)
                {
                    m_host.closeHostFile(entry.hostHandle);
                    entry.hostHandle = 0u;
                }
                entry.memory.clear();
                entry.memory.shrink_to_fit();
            }

            inline static constexpr std::array<uint32_t, 1> kSids{kFileIoSid};

            IopHost &m_host;
            mutable std::mutex m_mutex;
            std::vector<OpenEntry> m_files;
            uint32_t m_openLogs = 0u;
            std::unordered_set<uint64_t> m_unhandledLogged;
        };
    }

    std::unique_ptr<IopService> createFileIoService(IopHost &host)
    {
        return std::make_unique<FileIoService>(host);
    }
}
