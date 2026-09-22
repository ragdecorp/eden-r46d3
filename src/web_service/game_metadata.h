// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

namespace WebService {

enum class GameMetadataResultCode {
    Success,
    NotFound,
    NetworkError,
    InvalidResponse,
};

struct GameMetadata {
    u64 title_id{};
    std::string name;
    std::string publisher;
    std::string nsuid;
    std::string region;
    int system_players_min{};
    int system_players_max{};
    std::vector<std::string> genres;
    std::string release_date;
    std::string source_url;
};

struct GameMetadataResult {
    GameMetadataResultCode code{GameMetadataResultCode::NetworkError};
    GameMetadata metadata;
};

/// Fetches metadata for one base game using its 16-character Nintendo Switch Title ID.
GameMetadataResult FetchGameMetadata(u64 title_id, const std::string& language);

} // namespace WebService
