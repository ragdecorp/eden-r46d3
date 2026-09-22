// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/httplib.h"
#include "common/logging.h"
#include "web_service/game_metadata.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace WebService {
namespace {

using json = nlohmann::json;

constexpr std::size_t TimeoutSeconds = 15;
constexpr std::string_view MetadataHost = "https://api.nlib.cc";

void ConfigureClient(httplib::Client& client) {
    client.set_connection_timeout(TimeoutSeconds);
    client.set_read_timeout(TimeoutSeconds);
    client.set_write_timeout(TimeoutSeconds);
    client.set_follow_location(true);
#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
}

std::string NormalizeLanguage(std::string language) {
    const auto separator = language.find_first_of("_-.");
    if (separator != std::string::npos) {
        language.resize(separator);
    }
    std::ranges::transform(language, language.begin(),
                           [](unsigned char character) { return std::tolower(character); });

    constexpr std::array SupportedLanguages{
        "en", "fr", "ja", "es", "de", "nl", "pt", "it", "zh", "ko", "ru",
    };
    if (std::ranges::find(SupportedLanguages, language) == SupportedLanguages.end()) {
        return "en";
    }
    return language;
}

int ParsePlayerMaximum(const json& value) {
    if (value.is_number_integer()) {
        return std::max(0, value.get<int>());
    }
    if (!value.is_string()) {
        return 0;
    }

    static const std::regex NumberPattern{R"((\d+))"};
    const std::string text = value.get<std::string>();
    int maximum = 0;
    for (std::sregex_iterator it{text.begin(), text.end(), NumberPattern}, end; it != end; ++it) {
        maximum = std::max(maximum, std::stoi((*it)[1].str()));
    }
    return maximum;
}

std::string ReadString(const json& document, std::string_view key) {
    const auto value = document.find(std::string{key});
    return value != document.end() && value->is_string() ? value->get<std::string>() : "";
}

} // Anonymous namespace

GameMetadataResult FetchGameMetadata(u64 title_id, const std::string& language) {
    const std::string title_id_string = fmt::format("{:016X}", title_id);
    const std::string normalized_language = NormalizeLanguage(language);
    const std::string path = fmt::format(
        "/nx/{}?lang={}&fields=name,publisher,category,nsuId,numberOfPlayers,region,releaseDate",
        title_id_string, normalized_language);

    httplib::Client client{std::string{MetadataHost}};
    ConfigureClient(client);
    const httplib::Headers headers{{"User-Agent", "Eden-Emulator-Metadata/0.2.1"},
                                   {"Accept", "application/json"}};
    const auto response = client.Get(path, headers);
    if (!response) {
        LOG_WARNING(WebService, "Metadata request failed for title {}", title_id_string);
        return {.code = GameMetadataResultCode::NetworkError};
    }
    if (response->status == 404) {
        return {.code = GameMetadataResultCode::NotFound};
    }
    if (response->status < 200 || response->status >= 300) {
        LOG_WARNING(WebService, "Metadata request for title {} returned status {}",
                    title_id_string, response->status);
        return {.code = GameMetadataResultCode::NetworkError};
    }

    try {
        const json document = json::parse(response->body);
        const std::string returned_id = ReadString(document, "id");
        if (returned_id != title_id_string) {
            LOG_WARNING(WebService, "Metadata response Title ID mismatch for {}", title_id_string);
            return {.code = GameMetadataResultCode::InvalidResponse};
        }

        GameMetadata metadata{
            .title_id = title_id,
            .name = ReadString(document, "name"),
            .publisher = ReadString(document, "publisher"),
            .region = ReadString(document, "region"),
            .release_date = ReadString(document, "releaseDate"),
            .source_url = std::string{MetadataHost} + "/nx/" + title_id_string,
        };

        if (const auto nsuid = document.find("nsuId"); nsuid != document.end()) {
            if (nsuid->is_string()) {
                metadata.nsuid = nsuid->get<std::string>();
            } else if (nsuid->is_number_integer() || nsuid->is_number_unsigned()) {
                metadata.nsuid = std::to_string(nsuid->get<u64>());
            }
        }

        if (const auto players = document.find("numberOfPlayers"); players != document.end()) {
            metadata.system_players_max = ParsePlayerMaximum(*players);
            metadata.system_players_min = metadata.system_players_max > 0 ? 1 : 0;
        }

        if (const auto categories = document.find("category");
            categories != document.end() && categories->is_array()) {
            for (const auto& category : *categories) {
                if (category.is_string()) {
                    metadata.genres.push_back(category.get<std::string>());
                }
            }
        }

        return {.code = GameMetadataResultCode::Success, .metadata = std::move(metadata)};
    } catch (const json::exception&) {
        LOG_WARNING(WebService, "Metadata service returned invalid JSON for title {}",
                    title_id_string);
        return {.code = GameMetadataResultCode::InvalidResponse};
    }
}

} // namespace WebService
