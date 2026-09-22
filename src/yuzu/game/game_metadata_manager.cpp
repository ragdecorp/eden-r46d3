// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtConcurrentRun>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "qt_common/config/uisettings.h"
#include "web_service/game_metadata.h"
#include "yuzu/game/game_metadata_manager.h"

namespace {

constexpr int CacheVersion = 2;
constexpr int MaximumConcurrentRequests = 2;
constexpr qint64 CacheLifetimeSeconds = 30LL * 24LL * 60LL * 60LL;

QString PathToQString(const std::filesystem::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

QString CachePath() {
    return PathToQString(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                         "game_metadata.json");
}

QString OverridesPath() {
    return PathToQString(Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) /
                         "game_metadata_overrides.json");
}

QJsonArray StringListToJson(const QStringList& values) {
    QJsonArray array;
    for (const QString& value : values) {
        array.append(value);
    }
    return array;
}

QStringList JsonToStringList(const QJsonValue& value) {
    QStringList values;
    for (const QJsonValue& entry : value.toArray()) {
        if (entry.isString()) {
            values.append(entry.toString());
        }
    }
    return values;
}

} // Anonymous namespace

GameMetadataManager::GameMetadataManager(QObject* parent) : QObject{parent} {
    cache_save_timer.setSingleShot(true);
    cache_save_timer.setInterval(std::chrono::milliseconds{500});
    connect(&cache_save_timer, &QTimer::timeout, this, &GameMetadataManager::SaveCache);
    LoadCache();
    LoadOverrides();
}

GameMetadataManager::~GameMetadataManager() {
    if (cache_save_timer.isActive()) {
        SaveCache();
    }
}

QString GameMetadataManager::TitleIdToString(u64 title_id) {
    return QStringLiteral("%1").arg(title_id, 16, 16, QLatin1Char{'0'}).toUpper();
}

QStringList GameMetadataManager::NormalizeList(const QStringList& values) {
    QStringList normalized;
    for (QString value : values) {
        value = value.trimmed();
        if (!value.isEmpty() && !normalized.contains(value, Qt::CaseInsensitive)) {
            normalized.append(value);
        }
    }
    return normalized;
}

std::string GameMetadataManager::CurrentLanguage() const {
    std::string language = UISettings::values.language.GetValue();
    if (language.empty()) {
        language = QLocale::system().name().toStdString();
    }
    return language;
}

void GameMetadataManager::RequestMetadata(u64 title_id) {
    if (title_id == 0 || queued_requests.contains(title_id) || active_requests.contains(title_id)) {
        return;
    }

    const auto cached = cache.constFind(title_id);
    const QString language = QString::fromStdString(CurrentLanguage()).section(
        QRegularExpression{QStringLiteral("[_\\-.]")}, 0, 0).toLower();
    if (cached != cache.cend() && cached->language == language && cached->fetched_at.isValid() &&
        cached->fetched_at.secsTo(QDateTime::currentDateTimeUtc()) < CacheLifetimeSeconds) {
        return;
    }

    unavailable_titles.remove(title_id);
    queued_requests.insert(title_id);
    request_queue.enqueue(title_id);
    emit MetadataChanged(title_id);
    ProcessQueue();
}

QString GameMetadataManager::Players(u64 title_id) const {
    if (const auto custom = overrides.constFind(title_id);
        custom != overrides.cend() && custom->has_players) {
        return custom->players;
    }

    const auto metadata = cache.constFind(title_id);
    if (metadata == cache.cend() || metadata->system_players_max <= 0) {
        return {};
    }
    if (metadata->system_players_min > 0 &&
        metadata->system_players_min != metadata->system_players_max) {
        return QStringLiteral("%1\u2013%2")
            .arg(metadata->system_players_min)
            .arg(metadata->system_players_max);
    }
    return QString::number(metadata->system_players_max);
}

QStringList GameMetadataManager::Genres(u64 title_id) const {
    if (const auto custom = overrides.constFind(title_id);
        custom != overrides.cend() && custom->has_genres) {
        return custom->genres;
    }
    if (const auto metadata = cache.constFind(title_id); metadata != cache.cend()) {
        return metadata->genres;
    }
    return {};
}

QStringList GameMetadataManager::Tags(u64 title_id) const {
    if (const auto custom = overrides.constFind(title_id); custom != overrides.cend()) {
        return custom->tags;
    }
    return {};
}

QString GameMetadataManager::ReleaseDate(u64 title_id) const {
    if (const auto metadata = cache.constFind(title_id); metadata != cache.cend()) {
        return metadata->release_date;
    }
    return {};
}

bool GameMetadataManager::HasMetadata(u64 title_id) const {
    return cache.contains(title_id);
}

bool GameMetadataManager::IsPending(u64 title_id) const {
    return queued_requests.contains(title_id) || active_requests.contains(title_id);
}

void GameMetadataManager::SetPlayersOverride(u64 title_id, const QString& players) {
    auto& custom = overrides[title_id];
    custom.players = players.trimmed();
    custom.has_players = !custom.players.isEmpty();
    SaveOverrides();
    emit MetadataChanged(title_id);
}

void GameMetadataManager::SetGenresOverride(u64 title_id, const QStringList& genres) {
    auto& custom = overrides[title_id];
    custom.genres = NormalizeList(genres);
    custom.has_genres = !custom.genres.isEmpty();
    SaveOverrides();
    emit MetadataChanged(title_id);
}

void GameMetadataManager::SetTags(u64 title_id, const QStringList& tags) {
    overrides[title_id].tags = NormalizeList(tags);
    SaveOverrides();
    emit MetadataChanged(title_id);
}

void GameMetadataManager::ProcessQueue() {
    while (active_requests.size() < MaximumConcurrentRequests && !request_queue.isEmpty()) {
        const u64 title_id = request_queue.dequeue();
        queued_requests.remove(title_id);
        active_requests.insert(title_id);

        auto* watcher = new QFutureWatcher<WebService::GameMetadataResult>(this);
        connect(watcher, &QFutureWatcher<WebService::GameMetadataResult>::finished, this,
                [this, watcher, title_id] {
                    const WebService::GameMetadataResult result = watcher->result();
                    watcher->deleteLater();
                    active_requests.remove(title_id);

                    if (result.code == WebService::GameMetadataResultCode::Success) {
                        CachedMetadata metadata{
                            .system_players_min = result.metadata.system_players_min,
                            .system_players_max = result.metadata.system_players_max,
                            .release_date = QString::fromStdString(result.metadata.release_date),
                            .nsuid = QString::fromStdString(result.metadata.nsuid),
                            .source_url = QString::fromStdString(result.metadata.source_url),
                            .language = QString::fromStdString(CurrentLanguage())
                                            .section(QRegularExpression{QStringLiteral("[_\\-.]")},
                                                     0, 0)
                                            .toLower(),
                            .fetched_at = QDateTime::currentDateTimeUtc(),
                        };
                        for (const std::string& genre : result.metadata.genres) {
                            metadata.genres.append(QString::fromUtf8(
                                genre.data(), static_cast<qsizetype>(genre.size())));
                        }
                        metadata.genres = NormalizeList(metadata.genres);
                        cache.insert(title_id, std::move(metadata));
                        unavailable_titles.remove(title_id);
                        ScheduleCacheSave();
                    } else if (!cache.contains(title_id)) {
                        unavailable_titles.insert(title_id);
                    }

                    emit MetadataChanged(title_id);
                    ProcessQueue();
                });

        const std::string language = CurrentLanguage();
        watcher->setFuture(QtConcurrent::run(
            [title_id, language] { return WebService::FetchGameMetadata(title_id, language); }));
    }
}

void GameMetadataManager::ScheduleCacheSave() {
    cache_save_timer.start();
}

void GameMetadataManager::LoadCache() {
    QFile file{CachePath()};
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        LOG_WARNING(Frontend, "Ignoring invalid game metadata cache");
        return;
    }

    if (document.object().value(QStringLiteral("version")).toInt() != CacheVersion) {
        LOG_INFO(Frontend, "Refreshing game metadata cache after a schema update");
        return;
    }

    const QJsonObject games = document.object().value(QStringLiteral("games")).toObject();
    for (auto it = games.begin(); it != games.end(); ++it) {
        bool valid_id = false;
        const u64 title_id = it.key().toULongLong(&valid_id, 16);
        if (!valid_id || !it.value().isObject()) {
            continue;
        }
        const QJsonObject game = it.value().toObject();
        cache.insert(title_id,
                     CachedMetadata{
                         .system_players_min = game.value(QStringLiteral("players_min")).toInt(),
                         .system_players_max = game.value(QStringLiteral("players_max")).toInt(),
                         .genres = NormalizeList(
                             JsonToStringList(game.value(QStringLiteral("genres")))),
                         .release_date = game.value(QStringLiteral("release_date")).toString(),
                         .nsuid = game.value(QStringLiteral("nsuid")).toString(),
                         .source_url = game.value(QStringLiteral("source_url")).toString(),
                         .language = game.value(QStringLiteral("language")).toString(),
                         .fetched_at = QDateTime::fromString(
                             game.value(QStringLiteral("fetched_at")).toString(), Qt::ISODate),
                     });
    }
}

void GameMetadataManager::LoadOverrides() {
    QFile file{OverridesPath()};
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        LOG_WARNING(Frontend, "Ignoring invalid game metadata overrides");
        return;
    }

    const QJsonObject games = document.object().value(QStringLiteral("games")).toObject();
    for (auto it = games.begin(); it != games.end(); ++it) {
        bool valid_id = false;
        const u64 title_id = it.key().toULongLong(&valid_id, 16);
        if (!valid_id || !it.value().isObject()) {
            continue;
        }
        const QJsonObject game = it.value().toObject();
        overrides.insert(title_id,
                         MetadataOverride{
                             .has_players = game.contains(QStringLiteral("players")),
                             .players = game.value(QStringLiteral("players")).toString(),
                             .has_genres = game.contains(QStringLiteral("genres")),
                             .genres = NormalizeList(
                                 JsonToStringList(game.value(QStringLiteral("genres")))),
                             .tags = NormalizeList(
                                 JsonToStringList(game.value(QStringLiteral("tags")))),
                         });
    }
}

void GameMetadataManager::SaveCache() const {
    QJsonObject games;
    for (auto it = cache.cbegin(); it != cache.cend(); ++it) {
        games.insert(TitleIdToString(it.key()),
                     QJsonObject{
                         {QStringLiteral("players_min"), it->system_players_min},
                         {QStringLiteral("players_max"), it->system_players_max},
                         {QStringLiteral("genres"), StringListToJson(it->genres)},
                         {QStringLiteral("release_date"), it->release_date},
                         {QStringLiteral("nsuid"), it->nsuid},
                         {QStringLiteral("source_url"), it->source_url},
                         {QStringLiteral("language"), it->language},
                         {QStringLiteral("fetched_at"),
                          it->fetched_at.toString(Qt::ISODate)},
                     });
    }

    const QString path = CachePath();
    QDir{}.mkpath(QFileInfo{path}.absolutePath());
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument{QJsonObject{{QStringLiteral("version"), CacheVersion},
                                             {QStringLiteral("games"), games}}}
                       .toJson()) < 0 ||
        !file.commit()) {
        LOG_WARNING(Frontend, "Failed to save game metadata cache");
    }
}

void GameMetadataManager::SaveOverrides() const {
    QJsonObject games;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        QJsonObject game;
        if (it->has_players) {
            game.insert(QStringLiteral("players"), it->players);
        }
        if (it->has_genres) {
            game.insert(QStringLiteral("genres"), StringListToJson(it->genres));
        }
        if (!it->tags.isEmpty()) {
            game.insert(QStringLiteral("tags"), StringListToJson(it->tags));
        }
        if (!game.isEmpty()) {
            games.insert(TitleIdToString(it.key()), game);
        }
    }

    const QString path = OverridesPath();
    QDir{}.mkpath(QFileInfo{path}.absolutePath());
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument{QJsonObject{{QStringLiteral("version"), CacheVersion},
                                             {QStringLiteral("games"), games}}}
                       .toJson()) < 0 ||
        !file.commit()) {
        LOG_WARNING(Frontend, "Failed to save game metadata overrides");
    }
}
