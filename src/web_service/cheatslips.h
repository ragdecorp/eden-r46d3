// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace WebService {

enum class CheatAvailabilityResultCode {
    Success,
    NotFound,
    RateLimited,
    NetworkError,
    InvalidResponse,
};

struct CheatAvailabilityResult {
    CheatAvailabilityResultCode code{CheatAvailabilityResultCode::NetworkError};
    int cheat_count{};
};

enum class CheatCatalogResultCode {
    Success,
    NotFound,
    CredentialsMissing,
    InvalidCredentials,
    QuotaExceeded,
    NetworkError,
    InvalidResponse,
};

struct CheatCode {
    int submission_id{};
    int block_index{};
    std::string name;
    std::string content;
    std::string description;
    std::string credits;
};

struct CheatCatalogResult {
    CheatCatalogResultCode code{CheatCatalogResultCode::NetworkError};
    std::vector<CheatCode> cheats;
};

/// Checks CheatSlips for cheats matching one exact Title ID and 16-character Build ID.
/// This endpoint does not return downloadable cheat contents without a user's API token.
CheatAvailabilityResult FetchCheatAvailability(u64 title_id, const std::string& build_id);

/// Downloads the cheat catalog and contents for one exact Title ID and Build ID using a
/// user-owned token stored in the supplied portable configuration directory.
CheatCatalogResult FetchCheatCatalog(const std::filesystem::path& credential_directory,
                                     u64 title_id, const std::string& build_id);

} // namespace WebService
