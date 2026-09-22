// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <limits>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <QtConcurrentRun>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "web_service/title_versions.h"
#include "yuzu/game/game_update_manager.h"

namespace {

constexpr int CacheVersion = 2;
constexpr int FriendlyRequestSpacingMilliseconds = 5000;
constexpr qint64 CatalogLifetimeSeconds = 24LL * 60LL * 60LL;
constexpr qint64 FriendlyVersionLifetimeSeconds = 30LL * 24LL * 60LL * 60LL;

QString PathToQString(const std::filesystem::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

QString CachePath() {
    return PathToQString(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                         "title_versions.json");
}

} // Anonymous namespace

GameUpdateManager::GameUpdateManager(QObject* parent) : QObject{parent} {
    friendly_request_spacing_timer.setSingleShot(true);
    friendly_request_spacing_timer.setInterval(FriendlyRequestSpacingMilliseconds);
    connect(&friendly_request_spacing_timer, &QTimer::timeout, this,
            &GameUpdateManager::ProcessFriendlyQueue);
    LoadCache();
    QTimer::singleShot(0, this, &GameUpdateManager::RefreshCatalog);
}

u64 GameUpdateManager::NormalizeTitleId(u64 title_id) {
    return title_id & ~u64{0xFFF};
}

QString GameUpdateManager::TitleIdToString(u64 title_id) {
    return QStringLiteral("%1").arg(NormalizeTitleId(title_id), 16, 16, QLatin1Char{'0'}).toUpper();
}

void GameUpdateManager::RequestRefresh(u64 title_id, const QString& game_name,
                                       u32 installed_version,
                                       bool installed_version_comparable) {
    title_id = NormalizeTitleId(title_id);
    if (title_id == 0) {
        return;
    }

    RefreshCatalog();
    if (catalog_request_active) {
        pending_friendly_requests.insert(
            title_id, {game_name, installed_version, installed_version_comparable});
    } else if (latest_versions.contains(title_id)) {
        MaybeRequestFriendlyVersion(title_id, game_name, installed_version,
                                    installed_version_comparable);
    }
}

GameUpdateManager::State GameUpdateManager::GetState(u64 title_id, u32 installed_version,
                                                     bool installed_version_comparable) const {
    title_id = NormalizeTitleId(title_id);
    const auto latest = latest_versions.constFind(title_id);
    if (latest != latest_versions.cend()) {
        if (!installed_version_comparable) {
            return State::Unknown;
        }
        if (latest->numeric_version > installed_version) {
            return State::UpdateAvailable;
        }
        if (latest->numeric_version < installed_version) {
            return State::LocalNewer;
        }
        return State::UpToDate;
    }
    if (catalog_request_active) {
        return State::Checking;
    }
    if (catalog_request_failed) {
        return State::NetworkError;
    }
    return State::Unknown;
}

GameUpdateManager::LatestVersion GameUpdateManager::GetLatestVersion(u64 title_id) const {
    return latest_versions.value(NormalizeTitleId(title_id));
}

QString GameUpdateManager::FriendlyVersion(u64 title_id, u32 numeric_version) const {
    const auto friendly = friendly_versions.constFind(NormalizeTitleId(title_id));
    if (friendly == friendly_versions.cend() || friendly->numeric_version != numeric_version) {
        return {};
    }
    return friendly->display_version;
}

void GameUpdateManager::RefreshCatalog() {
    if (catalog_request_active ||
        (catalog_fetched_at.isValid() &&
         catalog_fetched_at.secsTo(QDateTime::currentDateTimeUtc()) < CatalogLifetimeSeconds)) {
        return;
    }

    catalog_request_active = true;
    catalog_request_failed = false;
    emit VersionsChanged();

    auto* watcher = new QFutureWatcher<WebService::TitleVersionsResult>(this);
    connect(watcher, &QFutureWatcher<WebService::TitleVersionsResult>::finished, this,
            [this, watcher] {
                const WebService::TitleVersionsResult result = watcher->result();
                watcher->deleteLater();
                catalog_request_active = false;

                if (result.code == WebService::TitleVersionsResultCode::Success &&
                    !result.versions.empty()) {
                    QHash<u64, LatestVersion> refreshed;
                    refreshed.reserve(static_cast<qsizetype>(result.versions.size()));
                    for (const auto& version : result.versions) {
                        refreshed.insert(NormalizeTitleId(version.title_id),
                                         {.numeric_version = version.version,
                                          .release_date = QString::fromStdString(
                                              version.release_date)});
                    }
                    latest_versions = std::move(refreshed);
                    catalog_fetched_at = QDateTime::currentDateTimeUtc();
                    catalog_request_failed = false;
                    SaveCache();
                } else {
                    catalog_request_failed = latest_versions.isEmpty();
                }

                emit VersionsChanged();

                const auto pending = std::move(pending_friendly_requests);
                pending_friendly_requests.clear();
                for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
                    MaybeRequestFriendlyVersion(it.key(), it->game_name,
                                                it->installed_version,
                                                it->installed_version_comparable);
                }
            });
    watcher->setFuture(QtConcurrent::run([] { return WebService::FetchLatestTitleVersions(); }));
}

void GameUpdateManager::MaybeRequestFriendlyVersion(u64 title_id, const QString& game_name,
                                                    u32 installed_version,
                                                    bool installed_version_comparable) {
    title_id = NormalizeTitleId(title_id);
    const auto latest = latest_versions.constFind(title_id);
    if (latest == latest_versions.cend() || !installed_version_comparable ||
        latest->numeric_version <= installed_version ||
        game_name.trimmed().isEmpty() || active_friendly_requests.contains(title_id)) {
        return;
    }

    const auto cached = friendly_versions.constFind(title_id);
    if (cached != friendly_versions.cend() &&
        cached->numeric_version == latest->numeric_version && cached->fetched_at.isValid() &&
        cached->fetched_at.secsTo(QDateTime::currentDateTimeUtc()) <
            FriendlyVersionLifetimeSeconds) {
        return;
    }

    const u32 numeric_version = latest->numeric_version;
    if (queued_friendly_requests.contains(title_id)) {
        return;
    }
    queued_friendly_requests.insert(title_id);
    friendly_request_queue.enqueue({title_id, game_name, numeric_version});
    ProcessFriendlyQueue();
}

void GameUpdateManager::ProcessFriendlyQueue() {
    if (friendly_request_spacing_timer.isActive() || !active_friendly_requests.isEmpty() ||
        friendly_request_queue.isEmpty()) {
        return;
    }

    const FriendlyRequest request = friendly_request_queue.dequeue();
    queued_friendly_requests.remove(request.title_id);
    const u64 title_id = request.title_id;
    const u32 numeric_version = request.numeric_version;
    active_friendly_requests.insert(title_id);
    auto* watcher = new QFutureWatcher<WebService::FriendlyTitleVersionResult>(this);
    connect(watcher, &QFutureWatcher<WebService::FriendlyTitleVersionResult>::finished, this,
            [this, watcher, title_id, numeric_version] {
                const WebService::FriendlyTitleVersionResult result = watcher->result();
                watcher->deleteLater();
                active_friendly_requests.remove(title_id);
                if (result.code == WebService::TitleVersionsResultCode::Success ||
                    result.code == WebService::TitleVersionsResultCode::NotFound) {
                    friendly_versions.insert(
                        title_id,
                        {.numeric_version = numeric_version,
                         .display_version = QString::fromStdString(result.display_version),
                         .source_url = QString::fromStdString(result.source_url),
                         .fetched_at = QDateTime::currentDateTimeUtc()});
                    SaveCache();
                    if (!result.display_version.empty()) {
                        emit FriendlyVersionChanged(title_id);
                    }
                }
                friendly_request_spacing_timer.start();
            });

    const std::string name = request.game_name.toUtf8().toStdString();
    watcher->setFuture(QtConcurrent::run([title_id, name, numeric_version] {
        return WebService::FetchFriendlyTitleVersion(title_id, name, numeric_version);
    }));
}

void GameUpdateManager::LoadCache() {
    QFile file{CachePath()};
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        LOG_WARNING(Frontend, "Ignoring invalid title version cache");
        return;
    }

    const QJsonObject root = document.object();
    const int loaded_cache_version = root.value(QStringLiteral("version")).toInt();
    catalog_fetched_at =
        QDateTime::fromString(root.value(QStringLiteral("fetched_at")).toString(), Qt::ISODate);
    const QJsonObject titles = root.value(QStringLiteral("titles")).toObject();
    for (auto it = titles.begin(); it != titles.end(); ++it) {
        bool valid_id{};
        const u64 title_id = it.key().toULongLong(&valid_id, 16);
        if (!valid_id || !it.value().isObject()) {
            continue;
        }
        const QJsonObject title = it.value().toObject();
        const qint64 numeric = title.value(QStringLiteral("version")).toInteger(-1);
        if (numeric < 0 || numeric > std::numeric_limits<u32>::max()) {
            continue;
        }
        latest_versions.insert(NormalizeTitleId(title_id),
                               {.numeric_version = static_cast<u32>(numeric),
                                .release_date =
                                    title.value(QStringLiteral("release_date")).toString()});
    }

    const QJsonObject friendly = root.value(QStringLiteral("friendly_versions")).toObject();
    for (auto it = friendly.begin(); it != friendly.end(); ++it) {
        bool valid_id{};
        const u64 title_id = it.key().toULongLong(&valid_id, 16);
        if (!valid_id || !it.value().isObject()) {
            continue;
        }
        const QJsonObject entry = it.value().toObject();
        const QString display_version = entry.value(QStringLiteral("display")).toString();
        if (loaded_cache_version < CacheVersion && display_version.isEmpty()) {
            // Cache version 1 used WordPress search, which missed many exact game pages.
            continue;
        }
        const qint64 numeric = entry.value(QStringLiteral("version")).toInteger(-1);
        if (numeric < 0 || numeric > std::numeric_limits<u32>::max()) {
            continue;
        }
        friendly_versions.insert(
            NormalizeTitleId(title_id),
            {.numeric_version = static_cast<u32>(numeric),
             .display_version = display_version,
             .source_url = entry.value(QStringLiteral("source_url")).toString(),
             .fetched_at = QDateTime::fromString(
                 entry.value(QStringLiteral("fetched_at")).toString(), Qt::ISODate)});
    }
}

void GameUpdateManager::SaveCache() const {
    QJsonObject titles;
    for (auto it = latest_versions.cbegin(); it != latest_versions.cend(); ++it) {
        titles.insert(TitleIdToString(it.key()),
                      QJsonObject{{QStringLiteral("version"),
                                   static_cast<qint64>(it->numeric_version)},
                                  {QStringLiteral("release_date"), it->release_date}});
    }

    QJsonObject friendly;
    for (auto it = friendly_versions.cbegin(); it != friendly_versions.cend(); ++it) {
        friendly.insert(
            TitleIdToString(it.key()),
            QJsonObject{{QStringLiteral("version"), static_cast<qint64>(it->numeric_version)},
                        {QStringLiteral("display"), it->display_version},
                        {QStringLiteral("source_url"), it->source_url},
                        {QStringLiteral("fetched_at"), it->fetched_at.toString(Qt::ISODate)}});
    }

    const QString path = CachePath();
    QDir{}.mkpath(QFileInfo{path}.absolutePath());
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument{QJsonObject{{QStringLiteral("version"), CacheVersion},
                                             {QStringLiteral("fetched_at"),
                                              catalog_fetched_at.toString(Qt::ISODate)},
                                             {QStringLiteral("titles"), titles},
                                             {QStringLiteral("friendly_versions"), friendly}}}
                       .toJson()) < 0 ||
        !file.commit()) {
        LOG_WARNING(Frontend, "Failed to save title version cache");
    }
}
