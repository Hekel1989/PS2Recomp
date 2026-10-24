#include "iop_cdvd.h"

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/ps2_path.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kSectorSize = 2048u;
        constexpr uint32_t kPrimaryVolumeDescriptorLsn = 16u;
        constexpr uint32_t kVolumeDescriptorTerminatorLsn = 17u;
        constexpr uint32_t kFirstDirectoryLsn = 20u;
        constexpr uint32_t kCdvdErrorNone = 0u;
        constexpr uint32_t kCdvdErrorRead = 0x30u;
        constexpr uint32_t kCdvdTypePs2Dvd = 0x14u;
        constexpr uint32_t kCdvdReadyComplete = 2u;
        constexpr uint32_t kCdvdStatusPause = 0x0Au;
        constexpr uint32_t kCdvdInitExit = 5u;
        constexpr uint32_t kCdvdCallbackRead = 1u;
        constexpr uint32_t kCdvdCallbackSeek = 4u;
        constexpr uint32_t kCdvdInterruptReadyBits = 0x29u;
        constexpr uint32_t kEventFlagMulti = 2u;
        constexpr uint32_t kCdvdStreamTimeout = 5000u;
        constexpr uint32_t kCdvdSyncTimeout = 15000u;
        constexpr uint32_t kCdvdmanVersion = 0x0226u;

        uint32_t alignSectors(uint64_t bytes)
        {
            return static_cast<uint32_t>((bytes + kSectorSize - 1u) / kSectorSize);
        }

        void writeLe16(uint8_t *destination, uint16_t value)
        {
            destination[0] = static_cast<uint8_t>(value);
            destination[1] = static_cast<uint8_t>(value >> 8u);
        }

        void writeBe16(uint8_t *destination, uint16_t value)
        {
            destination[0] = static_cast<uint8_t>(value >> 8u);
            destination[1] = static_cast<uint8_t>(value);
        }

        void writeLe32(uint8_t *destination, uint32_t value)
        {
            for (uint32_t i = 0u; i < 4u; ++i)
                destination[i] = static_cast<uint8_t>(value >> (i * 8u));
        }

        void writeBe32(uint8_t *destination, uint32_t value)
        {
            for (uint32_t i = 0u; i < 4u; ++i)
                destination[i] = static_cast<uint8_t>(value >> ((3u - i) * 8u));
        }

        void writeBoth16(uint8_t *destination, uint16_t value)
        {
            writeLe16(destination, value);
            writeBe16(destination + 2u, value);
        }

        void writeBoth32(uint8_t *destination, uint32_t value)
        {
            writeLe32(destination, value);
            writeBe32(destination + 4u, value);
        }

        std::string isoName(const std::filesystem::path &path, bool directory)
        {
            std::string name = path.filename().string();
            for (char &character : name)
            {
                const unsigned char byte = static_cast<unsigned char>(character);
                character = byte < 0x80u ? static_cast<char>(std::toupper(byte)) : '_';
            }
            if (name.size() > 200u)
                name.resize(200u);
            if (!directory && name.find(';') == std::string::npos)
                name += ";1";
            return name;
        }

        size_t directoryRecordSize(size_t identifierSize)
        {
            const size_t unpadded = 33u + identifierSize;
            return unpadded + (unpadded & 1u);
        }

        uint32_t directoryBytesFor(const std::vector<size_t> &identifierSizes)
        {
            uint64_t cursor = 0u;
            for (const size_t identifierSize : identifierSizes)
            {
                const uint64_t recordSize = directoryRecordSize(identifierSize);
                const uint64_t sectorOffset = cursor % kSectorSize;
                if (sectorOffset + recordSize > kSectorSize)
                    cursor += kSectorSize - sectorOffset;
                cursor += recordSize;
            }
            return static_cast<uint32_t>(std::max<uint64_t>(kSectorSize, alignSectors(cursor) * kSectorSize));
        }

        std::string normalizedIsoComponent(std::string_view value)
        {
            std::string result(value);
            const size_t semicolon = result.rfind(';');
            if (semicolon != std::string::npos && semicolon + 1u < result.size() &&
                std::all_of(result.begin() + static_cast<std::ptrdiff_t>(semicolon + 1u), result.end(),
                            [](unsigned char ch)
                            { return std::isdigit(ch) != 0; }))
            {
                result.resize(semicolon);
            }
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char ch)
                           { return static_cast<char>(std::toupper(ch)); });
            return result;
        }

        size_t writeDirectoryRecord(uint8_t *destination,
                                    uint32_t lsn,
                                    uint32_t size,
                                    bool directory,
                                    const uint8_t *identifier,
                                    size_t identifierSize)
        {
            const size_t recordSize = directoryRecordSize(identifierSize);
            std::memset(destination, 0, recordSize);
            destination[0] = static_cast<uint8_t>(recordSize);
            writeBoth32(destination + 2u, lsn);
            writeBoth32(destination + 10u, size);
            destination[25] = directory ? 2u : 0u;
            writeBoth16(destination + 28u, 1u);
            destination[32] = static_cast<uint8_t>(identifierSize);
            std::memcpy(destination + 33u, identifier, identifierSize);
            return recordSize;
        }
    }

    class IopCdvd::Impl
    {
    public:
        struct Callback
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
        };

        struct IsoNode
        {
            std::filesystem::path hostPath;
            std::string identifier;
            size_t parent = 0u;
            bool directory = false;
            uint32_t lsn = 0u;
            uint32_t size = 0u;
            uint32_t sectors = 0u;
            uint64_t handle = 0u;
            std::vector<size_t> children;
        };

        Impl(IopHost &hostRef, IopMemory &memoryRef, IopKernel &kernelRef)
            : host(hostRef), memory(memoryRef), kernel(kernelRef)
        {
        }

        ~Impl()
        {
            closeFiles();
        }

        void reset()
        {
            closeFiles();
            callback = {};
            initialized = false;
            mediaMode = 0u;
            currentLsn = 0u;
            lastError = kCdvdErrorNone;
            streamFlag = 0u;
            lastReadTimeout = 0u;
            interruptEventFlagId = 0;
            virtualIsoBuilt = false;
            virtualIsoValid = false;
            completionCallback.reset();
            nodes.clear();
            metadataSectors.clear();
            imageHandle = 0u;
        }

        bool dispatchImport(uint16_t ordinal, IopCpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            const uint32_t a1 = cpu.gpr[5];
            const uint32_t a2 = cpu.gpr[6];

            switch (ordinal)
            {
            case 4: // sceCdInit
                initialized = a0 != kCdvdInitExit;
                if (initialized)
                {
                    callback = {};
                    completionCallback.reset();
                }
                lastError = kCdvdErrorNone;
                cpu.gpr[2] = 1u;
                return true;

            case 5: // sceCdStandby
                cpu.gpr[2] = 1u;
                return true;

            case 6: // sceCdRead
                if (readSectors(a0, a1, a2))
                {
                    signalCommandComplete();
                    if (callback.address != 0u)
                    {
                        completionCallback = CompletionCallback{
                            callback.address,
                            callback.gp,
                            kCdvdCallbackRead,
                        };
                    }
                    cpu.gpr[2] = 1u;
                }
                else
                    cpu.gpr[2] = 0u;
                return true;

            case 7: // sceCdSeek
                currentLsn = a0;
                lastError = kCdvdErrorNone;
                signalCommandComplete();
                if (callback.address != 0u)
                {
                    completionCallback = CompletionCallback{
                        callback.address,
                        callback.gp,
                        kCdvdCallbackSeek,
                    };
                }
                cpu.gpr[2] = 1u;
                return true;

            case 8: // sceCdGetError
                cpu.gpr[2] = lastError;
                return true;

            case 10: // sceCdSearchFile
                cpu.gpr[2] = searchFile(a0, a1) ? 1u : 0u;
                return true;

            case 11: // sceCdSync
                cpu.gpr[2] = 0u;
                return true;

            case 12: // sceCdGetDiskType
                cpu.gpr[2] = kCdvdTypePs2Dvd;
                return true;

            case 13: // sceCdDiskReady
                cpu.gpr[2] = kCdvdReadyComplete;
                return true;

            case 28: // sceCdStatus
                cpu.gpr[2] = kCdvdStatusPause;
                return true;

            case 37: // sceCdCallback
            {
                const uint32_t previous = callback.address;
                callback = {a0, cpu.gpr[28]};
                cpu.gpr[2] = previous;
                return true;
            }

            case 50: // sceCdSC
            {
                const int32_t code = static_cast<int32_t>(a0);
                switch (code)
                {
                case -23: // Translate a logical sector for a dual-layer disc.
                    // The host image exposes one continuous LSN space, so no layer offset is required.
                    cpu.gpr[2] = a1 != 0u ? memory.read32(a1) : 0u;
                    return true;
                case -18:
                    lastReadTimeout = a1 != 0u ? memory.read32(a1) : 0u;
                    cpu.gpr[2] = 0u;
                    return true;
                case -17:
                    cpu.gpr[2] = kCdvdStreamTimeout;
                    return true;
                case -15:
                    cpu.gpr[2] = kCdvdSyncTimeout;
                    return true;
                case -11:
                    cpu.gpr[2] = static_cast<uint32_t>(ensureInterruptEventFlag());
                    return true;
                case -9:
                    cpu.gpr[2] = kCdvdmanVersion;
                    return true;
                case -2:
                    lastError = a1 != 0u ? memory.read8(a1) : kCdvdErrorNone;
                    cpu.gpr[2] = lastError;
                    return true;
                case -1:
                case 0:
                case 1:
                case 2:
                    if (a1 != 0u)
                        memory.write32(a1, lastError & 0xFFu);
                    if (code != -1)
                        streamFlag = static_cast<uint32_t>(code);
                    cpu.gpr[2] = streamFlag;
                    return true;
                default:
                    // sceCdSC is intentionally extensible; unsupported controls are no-ops in cdvdman.
                    cpu.gpr[2] = 0u;
                    return true;
                }
            }

            case 75: // sceCdMmode
                mediaMode = a0;
                cpu.gpr[2] = 1u;
                return true;

            default:
                return false;
            }
        }

        std::optional<CompletionCallback> takeCompletionCallback() noexcept
        {
            std::optional<CompletionCallback> result = completionCallback;
            completionCallback.reset();
            return result;
        }

    private:
        int ensureInterruptEventFlag()
        {
            if (interruptEventFlagId == 0)
            {
                interruptEventFlagId = kernel.createInternalEventFlag(
                    kEventFlagMulti, 0u, kCdvdInterruptReadyBits);
            }
            return interruptEventFlagId;
        }

        void signalCommandComplete()
        {
            if (interruptEventFlagId != 0)
                (void)kernel.setInternalEventFlag(interruptEventFlagId, kCdvdInterruptReadyBits);
        }

        void closeFiles()
        {
            if (imageHandle != 0u)
                host.closeHostFile(imageHandle);
            imageHandle = 0u;
            for (IsoNode &node : nodes)
            {
                if (node.handle != 0u)
                    host.closeHostFile(node.handle);
                node.handle = 0u;
            }
        }

        bool addDirectory(size_t parent, const std::filesystem::path &path)
        {
            std::error_code error;
            std::vector<std::filesystem::directory_entry> entries;
            for (
                std::filesystem::directory_iterator iterator(path, std::filesystem::directory_options::skip_permission_denied, error),
                end;
                !error && iterator != end;
                iterator.increment(error))
            {
                const std::filesystem::directory_entry &entry = *iterator;
                if (entry.is_symlink(error))
                {
                    error.clear();
                    continue;
                }
                error.clear();
                if (entry.is_directory(error) || entry.is_regular_file(error))
                    entries.push_back(entry);
                error.clear();
            }

            std::sort(entries.begin(), entries.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          return isoName(lhs.path(), lhs.is_directory()) < isoName(rhs.path(), rhs.is_directory());
                      });

            for (const auto &entry : entries)
            {
                error.clear();
                const bool directory = entry.is_directory(error);
                if (error)
                    continue;
                IsoNode node;
                node.hostPath = entry.path();
                node.identifier = isoName(entry.path(), directory);
                node.parent = parent;
                node.directory = directory;
                if (!directory)
                {
                    const uint64_t fileSize = entry.file_size(error);
                    if (error)
                        continue;
                    node.size = static_cast<uint32_t>(std::min<uint64_t>(fileSize, std::numeric_limits<uint32_t>::max()));
                }
                const size_t index = nodes.size();
                nodes.push_back(std::move(node));
                nodes[parent].children.push_back(index);
                if (directory && !addDirectory(index, entry.path()))
                    return false;
            }
            return true;
        }

        bool buildVirtualIso()
        {
            if (virtualIsoBuilt)
                return virtualIsoValid;
            virtualIsoBuilt = true;

            const std::string rootValue = host.hostPath(HostPathKind::CdRoot);
            if (rootValue.empty())
                return false;
            const std::filesystem::path rootPath(rootValue);
            std::error_code error;
            if (!std::filesystem::is_directory(rootPath, error) || error)
                return false;

            nodes.clear();
            IsoNode root;
            root.hostPath = rootPath;
            root.identifier.clear();
            root.parent = 0u;
            root.directory = true;
            nodes.push_back(std::move(root));
            if (!addDirectory(0u, rootPath))
                return false;

            for (IsoNode &node : nodes)
            {
                if (!node.directory)
                    continue;
                std::vector<size_t> identifierSizes = {1u, 1u};
                identifierSizes.reserve(node.children.size() + 2u);
                for (const size_t child : node.children)
                    identifierSizes.push_back(nodes[child].identifier.size());
                node.size = directoryBytesFor(identifierSizes);
                node.sectors = node.size / kSectorSize;
            }

            uint32_t cursor = kFirstDirectoryLsn;
            for (IsoNode &node : nodes)
            {
                if (!node.directory)
                    continue;
                node.lsn = cursor;
                cursor += node.sectors;
            }
            for (IsoNode &node : nodes)
            {
                if (node.directory)
                    continue;
                node.lsn = cursor;
                node.sectors = alignSectors(node.size);
                cursor += node.sectors;
            }
            volumeSectors = std::max<uint32_t>(cursor, 32u);

            std::array<uint8_t, kSectorSize> primary{};
            primary[0] = 1u;
            std::memcpy(primary.data() + 1u, "CD001", 5u);
            primary[6] = 1u;
            std::memset(primary.data() + 8u, ' ', 32u);
            std::memcpy(primary.data() + 8u, "PS2XRECOMP", 10u);
            std::memset(primary.data() + 40u, ' ', 32u);
            std::memcpy(primary.data() + 40u, "PS2X VIRTUAL DISC", 17u);
            writeBoth32(primary.data() + 80u, volumeSectors);
            writeBoth16(primary.data() + 120u, 1u);
            writeBoth16(primary.data() + 124u, 1u);
            writeBoth16(primary.data() + 128u, static_cast<uint16_t>(kSectorSize));
            const uint8_t rootIdentifier = 0u;
            (void)writeDirectoryRecord(primary.data() + 156u, nodes[0].lsn, nodes[0].size, true, &rootIdentifier, 1u);
            primary[881] = 1u;
            metadataSectors[kPrimaryVolumeDescriptorLsn] = primary;

            std::array<uint8_t, kSectorSize> terminator{};
            terminator[0] = 255u;
            std::memcpy(terminator.data() + 1u, "CD001", 5u);
            terminator[6] = 1u;
            metadataSectors[kVolumeDescriptorTerminatorLsn] = terminator;

            for (size_t nodeIndex = 0u; nodeIndex < nodes.size(); ++nodeIndex)
            {
                const IsoNode &node = nodes[nodeIndex];
                if (!node.directory)
                    continue;
                std::vector<uint8_t> bytes(node.size, 0u);
                size_t offset = 0u;
                const auto appendRecord = [&](const IsoNode &entry, const uint8_t *identifier, size_t identifierSize)
                {
                    const size_t recordSize = directoryRecordSize(identifierSize);
                    const size_t sectorOffset = offset % kSectorSize;
                    if (sectorOffset + recordSize > kSectorSize)
                        offset += kSectorSize - sectorOffset;
                    offset += writeDirectoryRecord(bytes.data() + offset,
                                                   entry.lsn,
                                                   entry.size,
                                                   entry.directory,
                                                   identifier,
                                                   identifierSize);
                };
                const uint8_t selfIdentifier = 0u;
                const uint8_t parentIdentifier = 1u;
                appendRecord(node, &selfIdentifier, 1u);
                appendRecord(nodes[node.parent], &parentIdentifier, 1u);
                for (const size_t childIndex : node.children)
                {
                    const IsoNode &child = nodes[childIndex];
                    appendRecord(child, reinterpret_cast<const uint8_t *>(child.identifier.data()), child.identifier.size());
                }
                for (uint32_t sector = 0u; sector < node.sectors; ++sector)
                {
                    std::array<uint8_t, kSectorSize> contents{};
                    std::memcpy(contents.data(), bytes.data() + sector * kSectorSize, kSectorSize);
                    metadataSectors[node.lsn + sector] = contents;
                }
            }

            virtualIsoValid = true;
            return true;
        }

        IsoNode *findVirtualIsoNode(std::string_view guestPath)
        {
            if (!buildVirtualIso())
                return nullptr;

            const ParsedPs2Path parsed = parsePs2Path(guestPath);
            if (!parsed || parsed.device != Ps2PathDevice::Cdrom)
                return nullptr;

            size_t current = 0u;
            size_t begin = 0u;
            while (begin <= parsed.path.size())
            {
                const size_t end = parsed.path.find('/', begin);
                const size_t length = (end == std::string::npos) ? parsed.path.size() - begin : end - begin;
                const std::string_view component(parsed.path.data() + begin, length);
                begin = (end == std::string::npos) ? parsed.path.size() + 1u : end + 1u;

                if (component.empty() || component == ".")
                    continue;
                if (component == "..")
                    return nullptr;

                const std::string wanted = normalizedIsoComponent(component);
                const auto child = std::find_if(nodes[current].children.begin(), nodes[current].children.end(),
                                                [&](size_t childIndex)
                                                {
                                                    return normalizedIsoComponent(nodes[childIndex].identifier) == wanted;
                                                });
                if (child == nodes[current].children.end())
                    return nullptr;
                current = *child;
            }
            return &nodes[current];
        }

        // --- ISO9660 search in the raw disc image ------------------------------
        // sceCdSearchFile resolves the disc's own directory records on real
        // hardware. When an archive exists only inside the ISO image (not as an
        // extracted file under the disc root), the host-directory virtual ISO
        // cannot answer the search and callers end up using an uninitialized
        // LSN. Parse the actual ISO9660 tree from the image instead.

        static uint32_t readLe32At(const uint8_t *source)
        {
            return static_cast<uint32_t>(source[0]) |
                   (static_cast<uint32_t>(source[1]) << 8u) |
                   (static_cast<uint32_t>(source[2]) << 16u) |
                   (static_cast<uint32_t>(source[3]) << 24u);
        }

        static std::string iso9660Component(std::string_view raw)
        {
            // ISO9660 names on PS2 discs look like "SD.BIN;1". Drop the version
            // suffix and any trailing dot, then upper-case for comparison.
            const size_t version = raw.find(';');
            std::string name(raw.substr(0, version));
            while (!name.empty() && name.back() == '.')
                name.pop_back();
            for (char &c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return name;
        }

        bool findInImageDirectory(uint64_t handle, uint32_t dirLsn, uint32_t dirBytes,
                                  const std::string &wanted, bool &isDirectory,
                                  uint32_t &lsn, uint32_t &size)
        {
            std::array<uint8_t, kSectorSize> sector;
            const uint32_t sectors = std::min<uint32_t>(alignSectors(dirBytes), 8192u);
            for (uint32_t sectorIndex = 0u; sectorIndex < sectors; ++sectorIndex)
            {
                size_t bytesRead = 0u;
                if (!host.readHostFile(handle, static_cast<uint64_t>(dirLsn + sectorIndex) * kSectorSize,
                                       sector.data(), kSectorSize, bytesRead) ||
                    bytesRead != kSectorSize)
                    return false;
                size_t offset = 0u;
                while (offset + 34u <= kSectorSize)
                {
                    const uint8_t recordLength = sector[offset];
                    if (recordLength == 0u)
                        break;  // zero padding until the next sector
                    if (recordLength < 34u || offset + recordLength > kSectorSize)
                        break;
                    const uint8_t nameLength = sector[offset + 32u];
                    if (nameLength > 1u && offset + 33u + nameLength <= offset + recordLength)
                    {
                        const std::string_view raw(
                            reinterpret_cast<const char *>(sector.data() + offset + 33u), nameLength);
                        if (iso9660Component(raw) == wanted)
                        {
                            lsn = readLe32At(sector.data() + offset + 2u);
                            size = readLe32At(sector.data() + offset + 10u);
                            isDirectory = (sector[offset + 25u] & 0x02u) != 0u;
                            return true;
                        }
                    }
                    offset += recordLength;
                }
            }
            return false;
        }

        bool searchFileInImage(uint32_t resultAddress, std::string_view guestPath)
        {
            const std::string imagePath = host.hostPath(HostPathKind::CdImage);
            if (imagePath.empty())
                return false;
            const uint64_t handle = host.openHostFile(imagePath);
            if (handle == 0u)
                return false;

            // Split the PS2 path into components, dropping the device prefix.
            if (const size_t colon = guestPath.find(':'); colon != std::string_view::npos)
                guestPath.remove_prefix(colon + 1u);
            std::vector<std::string> components;
            size_t begin = 0u;
            while (begin < guestPath.size())
            {
                const size_t end = guestPath.find_first_of("\\/", begin);
                const size_t length = (end == std::string_view::npos) ? guestPath.size() - begin : end - begin;
                const std::string_view component(guestPath.data() + begin, length);
                begin = (end == std::string_view::npos) ? guestPath.size() + 1u : end + 1u;
                if (!component.empty())
                    components.push_back(iso9660Component(component));
            }

            bool found = false;
            uint32_t lsn = 0u;
            uint32_t size = 0u;
            std::string leaf;
            std::array<uint8_t, kSectorSize> sector;
            do
            {
                if (components.empty())
                    break;
                size_t bytesRead = 0u;
                if (!host.readHostFile(handle, static_cast<uint64_t>(kPrimaryVolumeDescriptorLsn) * kSectorSize,
                                       sector.data(), kSectorSize, bytesRead) ||
                    bytesRead != kSectorSize || sector[0u] != 1u ||
                    std::memcmp(sector.data() + 1u, "CD001", 5u) != 0)
                    break;
                uint32_t dirLsn = readLe32At(sector.data() + 158u);  // root directory record
                uint32_t dirSize = readLe32At(sector.data() + 166u);
                for (size_t index = 0u; index < components.size(); ++index)
                {
                    bool isDirectory = false;
                    if (!findInImageDirectory(handle, dirLsn, dirSize, components[index],
                                              isDirectory, lsn, size))
                    {
                        found = false;
                        break;
                    }
                    leaf = components[index];
                    if (index + 1u < components.size())
                    {
                        if (!isDirectory)
                        {
                            found = false;
                            break;
                        }
                        dirLsn = lsn;
                        dirSize = size;
                        found = false;
                    }
                    else
                    {
                        found = true;
                    }
                }
            } while (false);
            host.closeHostFile(handle);

            if (!found)
                return false;

            std::array<uint8_t, 32u> result{};
            writeLe32(result.data(), lsn);
            writeLe32(result.data() + 4u, size);
            std::memcpy(result.data() + 8u, leaf.data(), std::min<size_t>(16u, leaf.size()));
            return memory.writeRam(resultAddress, result.data(), result.size());
        }

        bool searchFile(uint32_t resultAddress, uint32_t nameAddress)
        {
            if (resultAddress == 0u || nameAddress == 0u)
                return false;

            const std::string guestPath = memory.readString(nameAddress, 1024u);
            static const bool s_trace = std::getenv("PS2X_IOP_CDVD_TRACE") != nullptr;
            if (searchFileInImage(resultAddress, guestPath))
            {
                if (s_trace)
                    std::cerr << "[iop-cdvd] searchFile path=\"" << guestPath << "\" -> image" << std::endl;
                return true;
            }
            IsoNode *node = findVirtualIsoNode(guestPath);
            if (s_trace)
            {
                std::cerr << "[iop-cdvd] searchFile path=\"" << guestPath
                          << "\" -> " << (node ? "found" : "MISS") << std::endl;
            }
            if (!node)
                return false;

            // sceCdlFILE: lsn, size, name[16], date/flags[8].
            std::array<uint8_t, 32u> result{};
            writeLe32(result.data(), node->lsn);
            writeLe32(result.data() + 4u, node->size);
            const std::string leaf = normalizedIsoComponent(node->identifier);
            std::memcpy(result.data() + 8u, leaf.data(), std::min<size_t>(16u, leaf.size()));
            result[24u] = node->directory ? 2u : 0u;
            return memory.writeRam(resultAddress, result.data(), result.size());
        }

        IsoNode *fileForSector(uint32_t lsn)
        {
            for (IsoNode &node : nodes)
            {
                if (!node.directory && lsn >= node.lsn && lsn < node.lsn + node.sectors)
                    return &node;
            }
            return nullptr;
        }

        bool readVirtualSector(uint32_t lsn, uint8_t *destination)
        {
            const auto metadata = metadataSectors.find(lsn);
            if (metadata != metadataSectors.end())
            {
                std::memcpy(destination, metadata->second.data(), kSectorSize);
                return true;
            }

            IsoNode *node = fileForSector(lsn);
            if (!node)
            {
                std::memset(destination, 0, kSectorSize);
                return lsn < volumeSectors;
            }
            if (node->handle == 0u)
                node->handle = host.openHostFile(node->hostPath.string());
            if (node->handle == 0u)
                return false;

            std::memset(destination, 0, kSectorSize);
            const uint64_t offset = static_cast<uint64_t>(lsn - node->lsn) * kSectorSize;
            const size_t wanted = static_cast<size_t>(std::min<uint64_t>(kSectorSize, static_cast<uint64_t>(node->size) - offset));
            size_t bytesRead = 0u;
            return host.readHostFile(node->handle, offset, destination, wanted, bytesRead) && bytesRead == wanted;
        }

        bool readSectors(uint32_t lsn, uint32_t sectors, uint32_t destination)
        {
            if (sectors == 0u)
            {
                lastError = kCdvdErrorNone;
                return true;
            }
            const uint64_t byteCount64 = static_cast<uint64_t>(sectors) * kSectorSize;
            if (byteCount64 > IopMemory::RamSize || !memory.ownsRamRange(destination, static_cast<size_t>(byteCount64)))
            {
                lastError = kCdvdErrorRead;
                return false;
            }
            const size_t byteCount = static_cast<size_t>(byteCount64);
            std::vector<uint8_t> bytes(byteCount, 0u);

            bool read = false;
            const std::string imagePath = host.hostPath(HostPathKind::CdImage);
            if (!imagePath.empty())
            {
                if (imageHandle == 0u)
                    imageHandle = host.openHostFile(imagePath);
                if (imageHandle != 0u)
                {
                    size_t bytesRead = 0u;
                    read = host.readHostFile(imageHandle,
                                             static_cast<uint64_t>(lsn) * kSectorSize,
                                             bytes.data(),
                                             byteCount,
                                             bytesRead) &&
                           bytesRead == byteCount;
                }
            }

            if (!read && buildVirtualIso())
            {
                read = true;
                for (uint32_t sector = 0u; sector < sectors; ++sector)
                {
                    if (!readVirtualSector(lsn + sector, bytes.data() + static_cast<size_t>(sector) * kSectorSize))
                    {
                        read = false;
                        break;
                    }
                }
            }

            if (!read || !memory.writeRam(destination, bytes.data(), bytes.size()))
            {
                lastError = kCdvdErrorRead;
                return false;
            }
            lastError = kCdvdErrorNone;
            return true;
        }

        IopHost &host;
        IopMemory &memory;
        IopKernel &kernel;
        Callback callback;
        std::optional<CompletionCallback> completionCallback;
        bool initialized = false;
        uint32_t mediaMode = 0u;
        uint32_t currentLsn = 0u;
        uint32_t lastError = kCdvdErrorNone;
        uint32_t streamFlag = 0u;
        uint32_t lastReadTimeout = 0u;
        int interruptEventFlagId = 0;
        uint64_t imageHandle = 0u;
        bool virtualIsoBuilt = false;
        bool virtualIsoValid = false;
        uint32_t volumeSectors = 0u;
        std::vector<IsoNode> nodes;
        std::unordered_map<uint32_t, std::array<uint8_t, kSectorSize>> metadataSectors;
    };

    IopCdvd::IopCdvd(IopHost &host, IopMemory &memory, IopKernel &kernel)
        : m_impl(std::make_unique<Impl>(host, memory, kernel))
    {
    }

    IopCdvd::~IopCdvd() = default;

    void IopCdvd::reset() noexcept
    {
        m_impl->reset();
    }

    bool IopCdvd::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        static const bool s_trace = std::getenv("PS2X_IOP_CDVD_TRACE") != nullptr;
        if (s_trace)
        {
            static const std::unordered_map<uint16_t, const char *> kNames = {
                {4, "CdInit"}, {5, "CdStandby"}, {6, "CdRead"}, {7, "CdSeek"},
                {8, "CdGetError"}, {10, "CdSearchFile"}, {11, "CdSync"},
                {12, "CdGetDiskType"}, {13, "CdDiskReady"}, {28, "CdStatus"},
                {37, "CdCallback"}, {50, "CdSC"}, {75, "CdMmode"},
            };
            auto it = kNames.find(ordinal);
            std::cerr << "[iop-cdvd] " << (it != kNames.end() ? it->second : "Unknown")
                      << "(" << std::dec << ordinal << ") a0=0x" << std::hex << cpu.gpr[4]
                      << " a1=0x" << cpu.gpr[5] << " a2=0x" << cpu.gpr[6] << std::endl;
        }
        const bool handled = m_impl->dispatchImport(ordinal, cpu);
        if (s_trace)
            std::cerr << "[iop-cdvd]  -> v0=0x" << std::hex << cpu.gpr[2] << std::dec << std::endl;
        return handled;
    }

    std::optional<IopCdvd::CompletionCallback> IopCdvd::takeCompletionCallback() noexcept
    {
        return m_impl->takeCompletionCallback();
    }
}
