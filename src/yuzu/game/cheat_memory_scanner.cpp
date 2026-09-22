// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/game/cheat_memory_scanner.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#include <QByteArray>

#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_process_page_table.h"
#include "core/hle/kernel/svc_types.h"
#include "core/hle/result.h"
#include "core/memory.h"
#include "core/memory/dmnt_cheat_types.h"

namespace {

constexpr u64 ScanPageSize = 4 * 1024;
constexpr u64 ProgressInterval = 4 * 1024 * 1024;
constexpr std::size_t MaximumCandidates = 2'000'000;
constexpr u32 SnapshotMagic = 0x504E5345; // "ESNP"
constexpr u32 SnapshotVersion = 2;
constexpr u64 SnapshotEndAddress = std::numeric_limits<u64>::max();
constexpr u64 MaximumSnapshotFileSize = 1ULL * 1024 * 1024 * 1024;

struct ScanRegion {
    u64 address{};
    u64 size{};
};

bool IsScannableRegion(const Kernel::Svc::MemoryInfo& info) {
    if (info.permission != Kernel::Svc::MemoryPermission::ReadWrite) {
        return false;
    }

    // Writable application data and heap regions contain the values a player can change.
    // Excluding stacks, IPC/shared mappings and aliases avoids noisy duplicate candidates.
    return info.state == Kernel::Svc::MemoryState::Normal ||
           info.state == Kernel::Svc::MemoryState::CodeData ||
           info.state == Kernel::Svc::MemoryState::AliasCodeData;
}

bool CollectScanRegions(Kernel::KProcess& process, std::vector<ScanRegion>& regions,
                        u64& total_bytes) {
    auto& page_table = process.GetPageTable();
    u64 current_address{};

    while (true) {
        Kernel::KMemoryInfo memory_info{};
        Kernel::Svc::PageInfo page_info{};
        const Result result =
            page_table.QueryInfo(std::addressof(memory_info), std::addressof(page_info),
                                 current_address);
        if (R_FAILED(result)) {
            return false;
        }

        const Kernel::Svc::MemoryInfo info = memory_info.GetSvcMemoryInfo();
        if (IsScannableRegion(info) && info.size >= sizeof(s32)) {
            regions.push_back({info.base_address, info.size});
            if (std::numeric_limits<u64>::max() - total_bytes < info.size) {
                return false;
            }
            total_bytes += info.size;
        }

        const u64 next_address = info.base_address + info.size;
        if (next_address <= current_address) {
            break;
        }
        current_address = next_address;
    }

    return true;
}

u64 ReadRawBits(const void* source, std::size_t size) {
    u64 bits{};
    std::memcpy(&bits, source, size);
    return bits;
}

const CheatScanValue* FindComparisonValue(std::span<const CheatScanValue> values,
                                          CheatScanDataType type) {
    const auto found = std::find_if(values.begin(), values.end(), [type](const auto& value) {
        return value.type == type;
    });
    return found == values.end() ? nullptr : std::addressof(*found);
}

bool ValuesEqual(const CheatScanValue& left, const CheatScanValue& right) {
    if (left.type != right.type) {
        return false;
    }
    if (CheatScanValueIsFloatingPoint(left.type)) {
        return CheatScanValueAsNumber(left) == CheatScanValueAsNumber(right);
    }
    const std::size_t size = CheatScanDataTypeSize(left.type);
    const u64 mask = size == sizeof(u64) ? std::numeric_limits<u64>::max()
                                         : (u64{1} << (size * 8)) - 1;
    return (left.bits & mask) == (right.bits & mask);
}

bool MatchesComparison(const CheatScanValue& current, const CheatScanValue& previous,
                       CheatScanComparison comparison,
                       std::span<const CheatScanValue> comparison_values) {
    const CheatScanValue* requested = FindComparisonValue(comparison_values, current.type);
    switch (comparison) {
    case CheatScanComparison::Exact:
        return requested != nullptr && ValuesEqual(current, *requested);
    case CheatScanComparison::Changed:
        return !ValuesEqual(current, previous);
    case CheatScanComparison::Unchanged:
        return ValuesEqual(current, previous);
    case CheatScanComparison::Increased:
        return CheatScanValueAsNumber(current) > CheatScanValueAsNumber(previous);
    case CheatScanComparison::Decreased:
        return CheatScanValueAsNumber(current) < CheatScanValueAsNumber(previous);
    case CheatScanComparison::GreaterThan:
        return requested != nullptr &&
               CheatScanValueAsNumber(current) > CheatScanValueAsNumber(*requested);
    case CheatScanComparison::LessThan:
        return requested != nullptr &&
               CheatScanValueAsNumber(current) < CheatScanValueAsNumber(*requested);
    }
    return false;
}

template <typename T>
bool WriteSnapshotField(std::ofstream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(std::addressof(value)), sizeof(value));
    return output.good();
}

template <typename T>
bool ReadSnapshotField(std::ifstream& input, T& value) {
    input.read(reinterpret_cast<char*>(std::addressof(value)), sizeof(value));
    return input.good();
}

} // Anonymous namespace

std::size_t CheatScanDataTypeSize(CheatScanDataType type) {
    switch (type) {
    case CheatScanDataType::Int8:
    case CheatScanDataType::UInt8:
        return 1;
    case CheatScanDataType::Int16:
    case CheatScanDataType::UInt16:
        return 2;
    case CheatScanDataType::Int32:
    case CheatScanDataType::UInt32:
    case CheatScanDataType::Float:
        return 4;
    case CheatScanDataType::Int64:
    case CheatScanDataType::UInt64:
    case CheatScanDataType::Double:
        return 8;
    }
    return 0;
}

std::string_view CheatScanDataTypeName(CheatScanDataType type) {
    switch (type) {
    case CheatScanDataType::Int8:
        return "Int8";
    case CheatScanDataType::UInt8:
        return "UInt8";
    case CheatScanDataType::Int16:
        return "Int16";
    case CheatScanDataType::UInt16:
        return "UInt16";
    case CheatScanDataType::Int32:
        return "Int32";
    case CheatScanDataType::UInt32:
        return "UInt32";
    case CheatScanDataType::Int64:
        return "Int64";
    case CheatScanDataType::UInt64:
        return "UInt64";
    case CheatScanDataType::Float:
        return "Float";
    case CheatScanDataType::Double:
        return "Double";
    }
    return "Unknown";
}

bool CheatScanValueIsFloatingPoint(CheatScanDataType type) {
    return type == CheatScanDataType::Float || type == CheatScanDataType::Double;
}

bool CheatScanValueIsUnsigned(CheatScanDataType type) {
    return type == CheatScanDataType::UInt8 || type == CheatScanDataType::UInt16 ||
           type == CheatScanDataType::UInt32 || type == CheatScanDataType::UInt64;
}

long double CheatScanValueAsNumber(const CheatScanValue& value) {
    switch (value.type) {
    case CheatScanDataType::Int8:
        return static_cast<s8>(value.bits);
    case CheatScanDataType::UInt8:
        return static_cast<u8>(value.bits);
    case CheatScanDataType::Int16:
        return static_cast<s16>(value.bits);
    case CheatScanDataType::UInt16:
        return static_cast<u16>(value.bits);
    case CheatScanDataType::Int32:
        return static_cast<s32>(value.bits);
    case CheatScanDataType::UInt32:
        return static_cast<u32>(value.bits);
    case CheatScanDataType::Int64:
        return static_cast<s64>(value.bits);
    case CheatScanDataType::UInt64:
        return static_cast<long double>(value.bits);
    case CheatScanDataType::Float:
        return std::bit_cast<float>(static_cast<u32>(value.bits));
    case CheatScanDataType::Double:
        return std::bit_cast<double>(value.bits);
    }
    return 0;
}

CheatMemoryScanner::CheatMemoryScanner(Core::System& system_) : system{system_} {}

CheatScanResult CheatMemoryScanner::ScanExactInt32(
    s32 value, const ProgressCallback& progress_callback) const {
    CheatScanResult scan_result{};
    Kernel::KProcess* process = system.ApplicationProcess();
    if (process == nullptr) {
        scan_result.status = CheatScanResult::Status::NoApplicationProcess;
        return scan_result;
    }

    std::vector<ScanRegion> regions;
    if (!CollectScanRegions(*process, regions, scan_result.total_bytes)) {
        scan_result.status = CheatScanResult::Status::MemoryQueryFailed;
        return scan_result;
    }

    auto& memory = system.ApplicationMemory();
    std::vector<u8> buffer(static_cast<std::size_t>(ScanPageSize));
    scan_result.candidates.reserve(64 * 1024);
    u64 next_progress_update = ProgressInterval;

    for (const ScanRegion& region : regions) {
        for (u64 region_offset = 0; region_offset < region.size;) {
            const u64 remaining = region.size - region_offset;
            const std::size_t bytes_to_read =
                static_cast<std::size_t>(std::min<u64>(remaining, ScanPageSize));
            const u64 chunk_address = region.address + region_offset;

            // Never race the renderer and never force GPU downloads. Values owned by GPU-cached
            // pages are not useful for ordinary lives/currency searches, so skip those pages.
            if (memory.ReadBlockCpuOnly(chunk_address, buffer.data(), bytes_to_read)) {
                for (std::size_t offset = 0; offset + sizeof(s32) <= bytes_to_read;
                     offset += sizeof(s32)) {
                    s32 current_value{};
                    std::memcpy(&current_value, buffer.data() + offset, sizeof(current_value));
                    if (current_value != value) {
                        continue;
                    }

                    if (scan_result.candidates.size() >= MaximumCandidates) {
                        scan_result.candidates.clear();
                        scan_result.status = CheatScanResult::Status::TooManyCandidates;
                        return scan_result;
                    }
                    scan_result.candidates.push_back(chunk_address + offset);
                }
            }

            region_offset += bytes_to_read;
            scan_result.scanned_bytes += bytes_to_read;
            if ((scan_result.scanned_bytes >= next_progress_update ||
                 scan_result.scanned_bytes == scan_result.total_bytes) &&
                progress_callback != nullptr &&
                !progress_callback(scan_result.scanned_bytes, scan_result.total_bytes)) {
                scan_result.candidates.clear();
                scan_result.status = CheatScanResult::Status::Cancelled;
                return scan_result;
            }
            if (scan_result.scanned_bytes >= next_progress_update) {
                next_progress_update = scan_result.scanned_bytes + ProgressInterval;
            }
        }
    }

    scan_result.status = CheatScanResult::Status::Success;
    return scan_result;
}

CheatScanResult CheatMemoryScanner::FilterExactInt32(
    std::span<const u64> candidates, s32 value,
    const ProgressCallback& progress_callback) const {
    CheatScanResult scan_result{};
    if (system.ApplicationProcess() == nullptr) {
        scan_result.status = CheatScanResult::Status::NoApplicationProcess;
        return scan_result;
    }

    scan_result.total_bytes = candidates.size();
    scan_result.candidates.reserve(candidates.size());
    auto& memory = system.ApplicationMemory();

    constexpr std::size_t FilterProgressInterval = 4096;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        s32 current_value{};
        if (memory.ReadBlockCpuOnly(candidates[index], &current_value, sizeof(current_value)) &&
            current_value == value) {
            scan_result.candidates.push_back(candidates[index]);
        }

        scan_result.scanned_bytes = index + 1;
        if ((scan_result.scanned_bytes % FilterProgressInterval == 0 ||
             scan_result.scanned_bytes == scan_result.total_bytes) &&
            progress_callback != nullptr &&
            !progress_callback(scan_result.scanned_bytes, scan_result.total_bytes)) {
            scan_result.candidates.clear();
            scan_result.status = CheatScanResult::Status::Cancelled;
            return scan_result;
        }
    }

    scan_result.status = CheatScanResult::Status::Success;
    return scan_result;
}

std::vector<CheatCandidateValue> CheatMemoryScanner::ReadInt32Values(
    std::span<const u64> candidates) const {
    std::vector<CheatCandidateValue> values;
    values.reserve(candidates.size());

    Core::Memory::CheatProcessMetadata metadata{};
    const bool has_metadata = system.GetCheatProcessMetadata(metadata);
    const auto classify_address = [&metadata, has_metadata](CheatCandidateValue& candidate) {
        if (!has_metadata) {
            return;
        }
        const auto classify_region = [&candidate](const Core::Memory::MemoryRegionExtents& region,
                                                  CheatAddressRegion type) {
            if (region.size != 0 && candidate.address >= region.base &&
                candidate.address - region.base < region.size) {
                candidate.region = type;
                candidate.relative_offset = candidate.address - region.base;
                return true;
            }
            return false;
        };
        if (classify_region(metadata.main_nso_extents, CheatAddressRegion::Main) ||
            classify_region(metadata.heap_extents, CheatAddressRegion::Heap) ||
            classify_region(metadata.alias_extents, CheatAddressRegion::Alias)) {
            return;
        }
        classify_region(metadata.aslr_extents, CheatAddressRegion::Aslr);
    };

    Kernel::KProcess* process = system.ApplicationProcess();
    if (process == nullptr) {
        for (const u64 address : candidates) {
            CheatCandidateValue candidate{.address = address};
            classify_address(candidate);
            values.push_back(candidate);
        }
        return values;
    }

    auto& memory = system.ApplicationMemory();
    auto& page_table = process->GetPageTable();
    for (const u64 address : candidates) {
        CheatCandidateValue candidate{.address = address};
        classify_address(candidate);
        Kernel::KMemoryInfo memory_info{};
        Kernel::Svc::PageInfo page_info{};
        if (R_SUCCEEDED(page_table.QueryInfo(std::addressof(memory_info),
                                             std::addressof(page_info), address))) {
            const Kernel::Svc::MemoryInfo info = memory_info.GetSvcMemoryInfo();
            candidate.mapping_base = info.base_address;
            candidate.mapping_size = info.size;
            if (candidate.region == CheatAddressRegion::Other && address >= info.base_address) {
                candidate.relative_offset = address - info.base_address;
                switch (info.state) {
                case Kernel::Svc::MemoryState::Normal:
                    candidate.region = CheatAddressRegion::MappedNormal;
                    break;
                case Kernel::Svc::MemoryState::CodeData:
                    candidate.region = CheatAddressRegion::MappedCodeData;
                    break;
                case Kernel::Svc::MemoryState::AliasCodeData:
                    candidate.region = CheatAddressRegion::MappedAliasCodeData;
                    break;
                default:
                    break;
                }
            }
        }
        candidate.readable =
            memory.ReadBlockCpuOnly(address, &candidate.value, sizeof(candidate.value));
        values.push_back(candidate);
    }
    return values;
}

CheatWriteResult CheatMemoryScanner::WriteInt32Value(u64 address, s32 value) const {
    CheatWriteResult result{.address = address, .requested_value = value};
    if (system.ApplicationProcess() == nullptr) {
        return result;
    }

    auto& memory = system.ApplicationMemory();
    result.previous_readable =
        memory.ReadBlockCpuOnly(address, &result.previous_value, sizeof(result.previous_value));
    if (!result.previous_readable) {
        return result;
    }

    // Candidates originate from CPU-readable pages while the game is paused. Use the emulator's
    // normal write path so any cache bookkeeping remains correct, then verify the exact value.
    result.write_succeeded = memory.WriteBlock(address, &value, sizeof(value));
    if (!result.write_succeeded) {
        return result;
    }

    result.verified =
        memory.ReadBlockCpuOnly(address, &result.verified_value, sizeof(result.verified_value)) &&
        result.verified_value == value;
    return result;
}

CheatTypedScanResult CheatMemoryScanner::ScanExactValues(
    std::span<const CheatScanValue> values, bool byte_aligned,
    const ProgressCallback& progress_callback) const {
    CheatTypedScanResult scan_result{};
    Kernel::KProcess* process = system.ApplicationProcess();
    if (process == nullptr) {
        scan_result.status = CheatScanResult::Status::NoApplicationProcess;
        return scan_result;
    }
    if (values.empty()) {
        return scan_result;
    }

    std::vector<ScanRegion> regions;
    if (!CollectScanRegions(*process, regions, scan_result.total_bytes)) {
        scan_result.status = CheatScanResult::Status::MemoryQueryFailed;
        return scan_result;
    }

    auto& memory = system.ApplicationMemory();
    std::vector<u8> buffer(static_cast<std::size_t>(ScanPageSize));
    scan_result.candidates.reserve(64 * 1024);
    u64 next_progress_update = ProgressInterval;

    for (const ScanRegion& region : regions) {
        for (u64 region_offset = 0; region_offset < region.size;) {
            const u64 remaining = region.size - region_offset;
            const std::size_t bytes_to_read =
                static_cast<std::size_t>(std::min<u64>(remaining, ScanPageSize));
            const u64 chunk_address = region.address + region_offset;

            if (memory.ReadBlockCpuOnly(chunk_address, buffer.data(), bytes_to_read)) {
                for (const CheatScanValue& requested : values) {
                    const std::size_t value_size = CheatScanDataTypeSize(requested.type);
                    const std::size_t alignment = byte_aligned ? 1 : value_size;
                    const std::size_t first_offset = static_cast<std::size_t>(
                        (alignment - (chunk_address % alignment)) % alignment);
                    for (std::size_t offset = first_offset;
                         offset + value_size <= bytes_to_read; offset += alignment) {
                        const CheatScanValue current{
                            .type = requested.type,
                            .bits = ReadRawBits(buffer.data() + offset, value_size),
                        };
                        if (!ValuesEqual(current, requested)) {
                            continue;
                        }
                        if (scan_result.candidates.size() >= MaximumCandidates) {
                            scan_result.candidates.clear();
                            scan_result.status = CheatScanResult::Status::TooManyCandidates;
                            return scan_result;
                        }
                        scan_result.candidates.push_back({
                            .address = chunk_address + offset,
                            .last_value = current,
                        });
                    }
                }
            }

            region_offset += bytes_to_read;
            scan_result.scanned_bytes += bytes_to_read;
            if ((scan_result.scanned_bytes >= next_progress_update ||
                 scan_result.scanned_bytes == scan_result.total_bytes) &&
                progress_callback != nullptr &&
                !progress_callback(scan_result.scanned_bytes, scan_result.total_bytes)) {
                scan_result.candidates.clear();
                scan_result.status = CheatScanResult::Status::Cancelled;
                return scan_result;
            }
            if (scan_result.scanned_bytes >= next_progress_update) {
                next_progress_update = scan_result.scanned_bytes + ProgressInterval;
            }
        }
    }

    std::ranges::sort(scan_result.candidates, {}, [](const CheatScanCandidate& candidate) {
        return std::pair{candidate.address, candidate.last_value.type};
    });
    scan_result.status = CheatScanResult::Status::Success;
    return scan_result;
}

CheatTypedScanResult CheatMemoryScanner::FilterValues(
    std::span<const CheatScanCandidate> candidates, CheatScanComparison comparison,
    std::span<const CheatScanValue> comparison_values,
    const ProgressCallback& progress_callback) const {
    CheatTypedScanResult scan_result{};
    if (system.ApplicationProcess() == nullptr) {
        scan_result.status = CheatScanResult::Status::NoApplicationProcess;
        return scan_result;
    }

    scan_result.total_bytes = candidates.size();
    scan_result.candidates.reserve(candidates.size());
    auto& memory = system.ApplicationMemory();

    constexpr std::size_t FilterProgressInterval = 4096;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const CheatScanCandidate& candidate = candidates[index];
        const std::size_t value_size = CheatScanDataTypeSize(candidate.last_value.type);
        CheatScanValue current{.type = candidate.last_value.type};
        if (memory.ReadBlockCpuOnly(candidate.address, &current.bits, value_size) &&
            MatchesComparison(current, candidate.last_value, comparison, comparison_values)) {
            scan_result.candidates.push_back({
                .address = candidate.address,
                .last_value = current,
            });
        }

        scan_result.scanned_bytes = index + 1;
        if ((scan_result.scanned_bytes % FilterProgressInterval == 0 ||
             scan_result.scanned_bytes == scan_result.total_bytes) &&
            progress_callback != nullptr &&
            !progress_callback(scan_result.scanned_bytes, scan_result.total_bytes)) {
            scan_result.candidates.clear();
            scan_result.status = CheatScanResult::Status::Cancelled;
            return scan_result;
        }
    }

    scan_result.status = CheatScanResult::Status::Success;
    return scan_result;
}

std::vector<CheatTypedCandidateValue> CheatMemoryScanner::ReadValues(
    std::span<const CheatScanCandidate> candidates) const {
    std::vector<CheatTypedCandidateValue> values;
    values.reserve(candidates.size());

    Core::Memory::CheatProcessMetadata metadata{};
    const bool has_metadata = system.GetCheatProcessMetadata(metadata);
    Kernel::KProcess* process = system.ApplicationProcess();
    auto& memory = system.ApplicationMemory();

    for (const CheatScanCandidate& source : candidates) {
        CheatTypedCandidateValue candidate{
            .candidate = source,
            .current_value = {.type = source.last_value.type},
        };
        const auto classify_region = [&candidate](const Core::Memory::MemoryRegionExtents& region,
                                                  CheatAddressRegion type) {
            if (region.size != 0 && candidate.candidate.address >= region.base &&
                candidate.candidate.address - region.base < region.size) {
                candidate.region = type;
                candidate.relative_offset = candidate.candidate.address - region.base;
                return true;
            }
            return false;
        };
        if (has_metadata) {
            classify_region(metadata.main_nso_extents, CheatAddressRegion::Main) ||
                classify_region(metadata.heap_extents, CheatAddressRegion::Heap) ||
                classify_region(metadata.alias_extents, CheatAddressRegion::Alias) ||
                classify_region(metadata.aslr_extents, CheatAddressRegion::Aslr);
        }

        if (process != nullptr) {
            Kernel::KMemoryInfo memory_info{};
            Kernel::Svc::PageInfo page_info{};
            if (R_SUCCEEDED(process->GetPageTable().QueryInfo(
                    std::addressof(memory_info), std::addressof(page_info),
                    source.address))) {
                const Kernel::Svc::MemoryInfo info = memory_info.GetSvcMemoryInfo();
                candidate.mapping_base = info.base_address;
                candidate.mapping_size = info.size;
                if (candidate.region == CheatAddressRegion::Other &&
                    source.address >= info.base_address) {
                    candidate.relative_offset = source.address - info.base_address;
                    switch (info.state) {
                    case Kernel::Svc::MemoryState::Normal:
                        candidate.region = CheatAddressRegion::MappedNormal;
                        break;
                    case Kernel::Svc::MemoryState::CodeData:
                        candidate.region = CheatAddressRegion::MappedCodeData;
                        break;
                    case Kernel::Svc::MemoryState::AliasCodeData:
                        candidate.region = CheatAddressRegion::MappedAliasCodeData;
                        break;
                    default:
                        break;
                    }
                }
            }
            candidate.readable = memory.ReadBlockCpuOnly(
                source.address, &candidate.current_value.bits,
                CheatScanDataTypeSize(source.last_value.type));
        }
        values.push_back(candidate);
    }
    return values;
}

CheatTypedWriteResult CheatMemoryScanner::WriteValue(u64 address,
                                                     const CheatScanValue& value) const {
    CheatTypedWriteResult result{
        .address = address,
        .previous_value = {.type = value.type},
        .requested_value = value,
        .verified_value = {.type = value.type},
    };
    if (system.ApplicationProcess() == nullptr) {
        return result;
    }

    const std::size_t value_size = CheatScanDataTypeSize(value.type);
    auto& memory = system.ApplicationMemory();
    result.previous_readable =
        memory.ReadBlockCpuOnly(address, &result.previous_value.bits, value_size);
    if (!result.previous_readable) {
        return result;
    }
    result.write_succeeded = memory.WriteBlock(address, &value.bits, value_size);
    if (!result.write_succeeded) {
        return result;
    }
    result.verified =
        memory.ReadBlockCpuOnly(address, &result.verified_value.bits, value_size) &&
        ValuesEqual(result.verified_value, value);
    return result;
}

CheatSnapshotResult CheatMemoryScanner::CaptureUnknownSnapshot(
    const std::filesystem::path& snapshot_path,
    const ProgressCallback& progress_callback) const {
    CheatSnapshotResult result{};
    Kernel::KProcess* process = system.ApplicationProcess();
    if (process == nullptr) {
        result.status = CheatScanResult::Status::NoApplicationProcess;
        return result;
    }

    std::vector<ScanRegion> regions;
    if (!CollectScanRegions(*process, regions, result.total_bytes)) {
        result.status = CheatScanResult::Status::MemoryQueryFailed;
        return result;
    }

    std::error_code error;
    std::filesystem::create_directories(snapshot_path.parent_path(), error);
    if (error) {
        result.status = CheatScanResult::Status::SnapshotIoFailed;
        return result;
    }
    std::filesystem::path temporary_path = snapshot_path;
    temporary_path += ".tmp";
    std::ofstream output{temporary_path, std::ios::binary | std::ios::trunc};
    if (!output || !WriteSnapshotField(output, SnapshotMagic) ||
        !WriteSnapshotField(output, SnapshotVersion)) {
        result.status = CheatScanResult::Status::SnapshotIoFailed;
        return result;
    }

    auto& memory = system.ApplicationMemory();
    std::vector<u8> buffer(static_cast<std::size_t>(ScanPageSize));
    u64 next_progress_update = ProgressInterval;
    for (const ScanRegion& region : regions) {
        for (u64 offset = 0; offset < region.size;) {
            const u64 remaining = region.size - offset;
            const u32 bytes_to_read =
                static_cast<u32>(std::min<u64>(remaining, ScanPageSize));
            const u64 address = region.address + offset;
            if (memory.ReadBlockCpuOnly(address, buffer.data(), bytes_to_read)) {
                const QByteArray compressed = qCompress(buffer.data(), bytes_to_read, 1);
                const u32 compressed_size = static_cast<u32>(compressed.size());
                if (!WriteSnapshotField(output, address) ||
                    !WriteSnapshotField(output, bytes_to_read) ||
                    !WriteSnapshotField(output, compressed_size)) {
                    output.close();
                    std::filesystem::remove(temporary_path, error);
                    result.status = CheatScanResult::Status::SnapshotIoFailed;
                    return result;
                }
                output.write(compressed.constData(), compressed.size());
                if (!output.good()) {
                    output.close();
                    std::filesystem::remove(temporary_path, error);
                    result.status = CheatScanResult::Status::SnapshotIoFailed;
                    return result;
                }
                result.saved_bytes += compressed_size;
                if (static_cast<u64>(output.tellp()) > MaximumSnapshotFileSize) {
                    output.close();
                    std::filesystem::remove(temporary_path, error);
                    result.status = CheatScanResult::Status::SnapshotTooLarge;
                    return result;
                }
            }
            offset += bytes_to_read;
            result.scanned_bytes += bytes_to_read;
            if ((result.scanned_bytes >= next_progress_update ||
                 result.scanned_bytes == result.total_bytes) &&
                progress_callback != nullptr &&
                !progress_callback(result.scanned_bytes, result.total_bytes)) {
                output.close();
                std::filesystem::remove(temporary_path, error);
                result.status = CheatScanResult::Status::Cancelled;
                return result;
            }
            if (result.scanned_bytes >= next_progress_update) {
                next_progress_update = result.scanned_bytes + ProgressInterval;
            }
        }
    }

    const u32 end_size{};
    if (!WriteSnapshotField(output, SnapshotEndAddress) ||
        !WriteSnapshotField(output, end_size)) {
        output.close();
        std::filesystem::remove(temporary_path, error);
        result.status = CheatScanResult::Status::SnapshotIoFailed;
        return result;
    }
    output.close();
    if (!output.good()) {
        std::filesystem::remove(temporary_path, error);
        result.status = CheatScanResult::Status::SnapshotIoFailed;
        return result;
    }
    std::filesystem::remove(snapshot_path, error);
    error.clear();
    std::filesystem::rename(temporary_path, snapshot_path, error);
    if (error) {
        std::filesystem::remove(temporary_path, error);
        result.status = CheatScanResult::Status::SnapshotIoFailed;
        return result;
    }
    result.status = CheatScanResult::Status::Success;
    return result;
}

CheatTypedScanResult CheatMemoryScanner::ScanUnknownSnapshot(
    const std::filesystem::path& snapshot_path,
    std::span<const CheatScanDataType> data_types, CheatScanComparison comparison,
    std::span<const CheatScanValue> comparison_values, bool byte_aligned,
    const ProgressCallback& progress_callback) const {
    CheatTypedScanResult result{};
    if (system.ApplicationProcess() == nullptr) {
        result.status = CheatScanResult::Status::NoApplicationProcess;
        return result;
    }
    std::ifstream input{snapshot_path, std::ios::binary};
    std::error_code error;
    result.total_bytes = std::filesystem::file_size(snapshot_path, error);
    if (!input || error) {
        result.status = CheatScanResult::Status::SnapshotIoFailed;
        return result;
    }
    u32 magic{};
    u32 version{};
    if (!ReadSnapshotField(input, magic) || !ReadSnapshotField(input, version) ||
        magic != SnapshotMagic || version != SnapshotVersion) {
        result.status = CheatScanResult::Status::SnapshotIoFailed;
        return result;
    }

    auto& memory = system.ApplicationMemory();
    std::vector<u8> previous;
    std::vector<u8> current;
    while (true) {
        u64 address{};
        u32 size{};
        u32 compressed_size{};
        if (!ReadSnapshotField(input, address) || !ReadSnapshotField(input, size)) {
            result.status = CheatScanResult::Status::SnapshotIoFailed;
            return result;
        }
        if (address == SnapshotEndAddress && size == 0) {
            break;
        }
        if (!ReadSnapshotField(input, compressed_size) || size == 0 || size > ScanPageSize ||
            compressed_size == 0 || compressed_size > ScanPageSize * 2) {
            result.status = CheatScanResult::Status::SnapshotIoFailed;
            return result;
        }
        QByteArray compressed;
        compressed.resize(static_cast<qsizetype>(compressed_size));
        input.read(compressed.data(), compressed.size());
        if (!input.good()) {
            result.status = CheatScanResult::Status::SnapshotIoFailed;
            return result;
        }
        const QByteArray decompressed = qUncompress(compressed);
        if (decompressed.size() != static_cast<qsizetype>(size)) {
            result.status = CheatScanResult::Status::SnapshotIoFailed;
            return result;
        }
        previous.resize(size);
        current.resize(size);
        std::memcpy(previous.data(), decompressed.constData(), size);
        if (memory.ReadBlockCpuOnly(address, current.data(), size)) {
            for (const CheatScanDataType type : data_types) {
                const std::size_t value_size = CheatScanDataTypeSize(type);
                const std::size_t alignment = byte_aligned ? 1 : value_size;
                const std::size_t first_offset = static_cast<std::size_t>(
                    (alignment - (address % alignment)) % alignment);
                for (std::size_t offset = first_offset; offset + value_size <= size;
                     offset += alignment) {
                    const CheatScanValue previous_value{
                        .type = type,
                        .bits = ReadRawBits(previous.data() + offset, value_size),
                    };
                    const CheatScanValue current_value{
                        .type = type,
                        .bits = ReadRawBits(current.data() + offset, value_size),
                    };
                    if (!MatchesComparison(current_value, previous_value, comparison,
                                           comparison_values)) {
                        continue;
                    }
                    if (result.candidates.size() >= MaximumCandidates) {
                        result.candidates.clear();
                        result.status = CheatScanResult::Status::TooManyCandidates;
                        return result;
                    }
                    result.candidates.push_back({
                        .address = address + offset,
                        .last_value = current_value,
                    });
                }
            }
        }
        result.scanned_bytes = static_cast<u64>(input.tellg());
        if (progress_callback != nullptr &&
            !progress_callback(result.scanned_bytes, result.total_bytes)) {
            result.candidates.clear();
            result.status = CheatScanResult::Status::Cancelled;
            return result;
        }
    }

    std::ranges::sort(result.candidates, {}, [](const CheatScanCandidate& candidate) {
        return std::pair{candidate.address, candidate.last_value.type};
    });
    result.status = CheatScanResult::Status::Success;
    return result;
}
