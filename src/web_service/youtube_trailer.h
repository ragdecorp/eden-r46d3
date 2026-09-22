// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>

namespace WebService {

enum class YouTubeTrailerResultCode {
    Success,
    CredentialsMissing,
    InvalidCredentials,
    NetworkError,
    NoSuitableVideo,
};

struct YouTubeTrailerResult {
    YouTubeTrailerResultCode code{YouTubeTrailerResultCode::NetworkError};
    std::string video_id;
    std::string title;
    int duration_seconds{};
};

/// Finds a short, relevant YouTube trailer using credentials stored in credential_directory.
/// Supported files are youtube_api_key.json and token_youtube.json.
YouTubeTrailerResult FindYouTubeTrailer(const std::filesystem::path& credential_directory,
                                        const std::string& game_name);

} // namespace WebService
