#include "module_factories.h"
#include "rpc_reply.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        // CDVD_RPC_ID from ps2sdk. On retail hardware this endpoint is served
        // by cdvdstub.irx, which the IOP ROM loads at boot; games never ship
        // that module, so an always-active HLE provider mirrors the ROM.
        constexpr uint32_t kCdvdStubSid = 0x80000597u;
        // EE libcd cdCmd layout: [0x00, 0x24) response area, 0x24 command
        // byte, 0x25.. command parameters.
        constexpr uint32_t kCommandByteOffset = 0x24u;
        constexpr size_t kCommandLogBytes = 16u;
        constexpr uint32_t kMaxCommandLogs = 64u;
        constexpr uint32_t kMaxCommandBytes = 0x100u;

        constexpr uint64_t kSectorSize = 2048u;
        constexpr uint64_t kPrimaryVolumeOffset = 16u * kSectorSize;

        struct IsoFileLocation
        {
            bool found = false;
            uint32_t lba = 0u;
            uint32_t size = 0u;
        };

        uint32_t readLe32(const uint8_t *data)
        {
            return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                   (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
        }

        char upperAscii(char value)
        {
            return (value >= 'a' && value <= 'z') ? static_cast<char>(value - 'a' + 'A') : value;
        }

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

        // ECMA-119 directory record header: extent LBA at +2, size at +10,
        // flags at +25 (0x02 = directory), FID length at +32, FID at +33.
        bool recordNameMatches(const uint8_t *record, size_t recordSize, const std::string &component)
        {
            const size_t nameLength = record[32];
            if (nameLength == 0u || 33u + nameLength > recordSize)
                return false;

            std::string name;
            name.reserve(nameLength);
            for (size_t i = 0; i < nameLength; ++i)
                name.push_back(upperAscii(static_cast<char>(record[33 + i])));

            // "." and ".." are stored as single bytes 0x28 / 0x29.
            if (nameLength == 1u && name[0] == '(')
                return component == ".";
            if (nameLength == 1u && name[0] == ')')
                return component == "..";

            // A request without an explicit version matches any version.
            const size_t versionSeparator = name.find(';');
            const size_t requestSeparator = component.find(';');
            if (requestSeparator != std::string::npos)
                return component == name;
            return component == name.substr(0, versionSeparator);
        }

        // Resolves an ISO 9660 path such as "\\FILEDATA.BIN;1" against the
        // disc image, mirroring the file lookup the IOP CDVDSERV performs.
        IsoFileLocation lookupIsoPath(IopHost &host, std::string_view path)
        {
            IsoFileLocation location;
            std::string normalized;
            normalized.reserve(path.size());
            for (char value : path)
            {
                if (value != '\0')
                    normalized.push_back(upperAscii(value));
            }

            std::vector<std::string> components;
            size_t start = 0u;
            while (start < normalized.size())
            {
                size_t end = normalized.find_first_of("\\/", start);
                if (end == std::string::npos)
                    end = normalized.size();
                if (end != start)
                    components.emplace_back(normalized.substr(start, end - start));
                start = end + 1u;
            }
            if (components.empty())
                return location;

            const std::string imagePath = host.hostPath(HostPathKind::CdImage);
            if (imagePath.empty())
                return location;
            const uint64_t handle = host.openHostFile(imagePath);
            if (handle == 0u)
                return location;

            std::array<uint8_t, kSectorSize> sector{};
            uint32_t directoryLba = 0u;
            uint32_t directorySize = 0u;
            do
            {
                if (!readFully(host, handle, kPrimaryVolumeOffset, sector.data(), sector.size()))
                    break;
                if (sector[0] != 1u || std::memcmp(sector.data() + 1, "CD001", 5) != 0)
                    break;
                directoryLba = readLe32(sector.data() + 158);
                directorySize = readLe32(sector.data() + 162);
            } while (false);

            for (size_t index = 0; index < components.size() && directorySize != 0u; ++index)
            {
                const bool finalEntry = index + 1u == components.size();
                bool matched = false;
                uint32_t remaining = directorySize;
                uint32_t cursor = directoryLba;
                while (remaining != 0u && !matched)
                {
                    if (!readFully(host, handle, static_cast<uint64_t>(cursor) * kSectorSize,
                                   sector.data(), sector.size()))
                    {
                        remaining = 0u;
                        break;
                    }
                    size_t offset = 0u;
                    while (offset < sector.size())
                    {
                        const size_t recordLength = sector[offset];
                        if (recordLength == 0u)
                        {
                            offset = kSectorSize; // record padding to next sector
                            break;
                        }
                        const size_t available = std::min<size_t>(recordLength, sector.size() - offset);
                        if (recordNameMatches(sector.data() + offset, available, components[index]))
                        {
                            matched = true;
                            directoryLba = readLe32(sector.data() + offset + 2);
                            directorySize = readLe32(sector.data() + offset + 10);
                            if (finalEntry)
                            {
                                location.found = true;
                                location.lba = directoryLba;
                                location.size = directorySize;
                            }
                            else if ((sector[offset + 25] & 0x02u) == 0u)
                            {
                                matched = false; // path component must be a directory
                            }
                            break;
                        }
                        offset += recordLength;
                    }
                    ++cursor;
                    remaining = remaining > kSectorSize ? remaining - static_cast<uint32_t>(kSectorSize) : 0u;
                }
                if (!matched)
                {
                    location = {};
                    break;
                }
            }

            host.closeHostFile(handle);
            return location;
        }

        class CdvdstubService final : public IopService
        {
        public:
            explicit CdvdstubService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "cdvdstub";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_commandLogs = 0u;
                m_callCount = 0u;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != kCdvdStubSid)
                {
                    return result;
                }

                result.handled = true;
                result.resultAddress = request.receive.address;
                result.signalNowaitCompletion = true;
                result.signalCompletion = true;

                logCommand(request);

                // The command is an ISO 9660 path string; resolve it against
                // the disc image and report {lba, size} in the response area,
                // then a non-zero success word in the receive buffer, exactly
                // like the IOP CDVDSERV file lookup.
                uint32_t status = 0u;
                if (request.send.address != 0u && request.send.size > kCommandByteOffset)
                {
                    std::array<uint8_t, kMaxCommandBytes> command{};
                    const size_t count =
                        std::min<size_t>(kMaxCommandBytes, request.send.size - kCommandByteOffset);
                    if (m_host.readGuest(request.send.address + kCommandByteOffset, command.data(), count))
                    {
                        const IsoFileLocation file = lookupIsoPath(
                            m_host, std::string_view(reinterpret_cast<const char *>(command.data())));
                        if (file.found)
                        {
                            const std::array<uint32_t, 2> location{file.lba, file.size};
                            if (m_host.writeGuest(request.send.address, location.data(),
                                                  location.size() * sizeof(uint32_t)))
                            {
                                status = 1u;
                            }
                        }
                    }
                }

                const std::array<uint32_t, 1> reply{status};
                writeRpcWords(m_host, request.receive, reply);
                return result;
            }

            void appendDebugMetrics(std::vector<DebugMetric> &metrics) const override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                metrics.push_back({"rpc_calls", m_callCount, false});
                metrics.push_back({"command_logs", m_commandLogs, false});
            }

        private:
            void logCommand(const RpcRequest &request)
            {
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    ++m_callCount;
                    if (m_commandLogs < kMaxCommandLogs)
                    {
                        ++m_commandLogs;
                        shouldLog = true;
                    }
                }
                if (!shouldLog)
                    return;

                std::array<uint8_t, kCommandLogBytes> bytes{};
                if (request.send.address != 0u && request.send.size > kCommandByteOffset)
                {
                    const size_t count =
                        std::min<size_t>(kCommandLogBytes, request.send.size - kCommandByteOffset);
                    (void)m_host.readGuest(request.send.address + kCommandByteOffset, bytes.data(), count);
                }

                std::ostringstream data;
                for (uint8_t byte : bytes)
                {
                    data << std::hex << std::setw(2) << std::setfill('0')
                         << static_cast<unsigned>(byte) << ' ';
                }

                std::ostringstream message;
                message << "[CDVD:HLE] rpc=0x" << std::hex << request.function
                        << " send=0x" << request.send.address << "/0x" << request.send.size
                        << " recv=0x" << request.receive.address << "/0x" << request.receive.size
                        << " cmd=[" << data.str() << "]";
                m_host.log(LogLevel::Info, message.str());
            }

            inline static constexpr std::array<uint32_t, 1> kSids{kCdvdStubSid};

            IopHost &m_host;
            mutable std::mutex m_mutex;
            uint32_t m_commandLogs = 0u;
            uint64_t m_callCount = 0u;
        };
    }

    std::unique_ptr<IopService> createCdvdstubService(IopHost &host)
    {
        return std::make_unique<CdvdstubService>(host);
    }
}
