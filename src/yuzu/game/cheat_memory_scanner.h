// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <compare>
#include <filesystem>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

struct CheatScanResult {
    enum class Status {
        Success,
        Cancelled,
        TooManyCandidates,
        NoApplicationProcess,
        MemoryQueryFailed,
        MemoryReadFailed,
        SnapshotIoFailed,
        SnapshotTooLarge,
    };

    Status status{Status::Success};
    std::vector<u64> candidates;
    u64 scanned_bytes{};
    u64 total_bytes{};
};

enum class CheatScanDataType : u8 {
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
};

enum class CheatScanComparison : u8 {
    Exact,
    Changed,
    Unchanged,
    Increased,
    Decreased,
    GreaterThan,
    LessThan,
};

struct CheatScanValue {
    CheatScanDataType type{CheatScanDataType::Int32};
    u64 bits{};

    auto operator<=>(const CheatScanValue&) const = default;
};

struct CheatScanCandidate {
    u64 address{};
    CheatScanValue last_value{};

    auto operator<=>(const CheatScanCandidate&) const = default;
};

struct CheatTypedScanResult {
    CheatScanResult::Status status{CheatScanResult::Status::Success};
    std::vector<CheatScanCandidate> candidates;
    u64 scanned_bytes{};
    u64 total_bytes{};
};

struct CheatSnapshotResult {
    CheatScanResult::Status status{CheatScanResult::Status::Success};
    u64 scanned_bytes{};
    u64 total_bytes{};
    u64 saved_bytes{};
};

enum class CheatAddressRegion {
    Main,
    Heap,
    Alias,
    Aslr,
    MappedNormal,
    MappedCodeData,
    MappedAliasCodeData,
    Other,
};

struct CheatCandidateValue {
    u64 address{};
    u64 relative_offset{};
    u64 mapping_base{};
    u64 mapping_size{};
    s32 value{};
    CheatAddressRegion region{CheatAddressRegion::Other};
    bool readable{};
};

struct CheatWriteResult {
    u64 address{};
    s32 previous_value{};
    s32 requested_value{};
    s32 verified_value{};
    bool previous_readable{};
    bool write_succeeded{};
    bool verified{};
};

struct CheatTypedCandidateValue {
    CheatScanCandidate candidate{};
    u64 relative_offset{};
    u64 mapping_base{};
    u64 mapping_size{};
    CheatScanValue current_value{};
    CheatAddressRegion region{CheatAddressRegion::Other};
    bool readable{};
};

struct CheatTypedWriteResult {
    u64 address{};
    CheatScanValue previous_value{};
    CheatScanValue requested_value{};
    CheatScanValue verified_value{};
    bool previous_readable{};
    bool write_succeeded{};
    bool verified{};
};

[[nodiscard]] std::size_t CheatScanDataTypeSize(CheatScanDataType type);
[[nodiscard]] std::string_view CheatScanDataTypeName(CheatScanDataType type);
[[nodiscard]] bool CheatScanValueIsFloatingPoint(CheatScanDataType type);
[[nodiscard]] bool CheatScanValueIsUnsigned(CheatScanDataType type);
[[nodiscard]] long double CheatScanValueAsNumber(const CheatScanValue& value);

class CheatMemoryScanner final {
public:
    using ProgressCallback = std::function<bool(u64 scanned_bytes, u64 total_bytes)>;

    explicit CheatMemoryScanner(Core::System& system);

    CheatScanResult ScanExactInt32(s32 value, const ProgressCallback& progress_callback) const;
    CheatScanResult FilterExactInt32(std::span<const u64> candidates, s32 value,
                                     const ProgressCallback& progress_callback) const;
    std::vector<CheatCandidateValue> ReadInt32Values(
        std::span<const u64> candidates) const;
    CheatWriteResult WriteInt32Value(u64 address, s32 value) const;

    CheatTypedScanResult ScanExactValues(
        std::span<const CheatScanValue> values, bool byte_aligned,
        const ProgressCallback& progress_callback) const;
    CheatTypedScanResult FilterValues(
        std::span<const CheatScanCandidate> candidates, CheatScanComparison comparison,
        std::span<const CheatScanValue> comparison_values,
        const ProgressCallback& progress_callback) const;
    std::vector<CheatTypedCandidateValue> ReadValues(
        std::span<const CheatScanCandidate> candidates) const;
    CheatTypedWriteResult WriteValue(u64 address, const CheatScanValue& value) const;
    CheatSnapshotResult CaptureUnknownSnapshot(
        const std::filesystem::path& snapshot_path,
        const ProgressCallback& progress_callback) const;
    CheatTypedScanResult ScanUnknownSnapshot(
        const std::filesystem::path& snapshot_path,
        std::span<const CheatScanDataType> data_types, CheatScanComparison comparison,
        std::span<const CheatScanValue> comparison_values, bool byte_aligned,
        const ProgressCallback& progress_callback) const;

private:
    Core::System& system;
};
