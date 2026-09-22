// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

namespace WebService {

enum class TitleVersionsResultCode {
    Success,
    NotFound,
    NetworkError,
    InvalidResponse,
};

struct LatestTitleVersion {
    u64 title_id{};
    u32 version{};
    std::string release_date;
};

struct TitleVersionsResult {
    TitleVersionsResultCode code{TitleVersionsResultCode::NetworkError};
    std::vector<LatestTitleVersion> versions;
};

struct FriendlyTitleVersionResult {
    TitleVersionsResultCode code{TitleVersionsResultCode::NetworkError};
    std::string display_version;
    std::string source_url;
};

/// Downloads the public TitleDB version catalog and keeps the newest numeric version per base
/// Title ID.
TitleVersionsResult FetchLatestTitleVersions();

/// Looks up the human-readable label for an already verified numeric update version. The numeric
/// version must occur in the matching page, so this result is never used to decide availability.
FriendlyTitleVersionResult FetchFriendlyTitleVersion(u64 title_id, const std::string& game_name,
                                                     u32 numeric_version);

} // namespace WebService
