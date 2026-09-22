// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtConcurrentRun>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "web_service/cheatslips.h"
#include "yuzu/game/cheat_availability_manager.h"

namespace {

constexpr int CacheVersion = 1;
constexpr int MinimumRequestSpacingMilliseconds = 5000;
constexpr int NetworkFailurePauseSeconds = 10 * 60;
constexpr int RateLimitPauseSeconds = 60 * 60;
constexpr qint64 CacheLifetimeSeconds = 7LL * 24LL * 60LL * 60LL;

QString PathToQString(const std::filesystem::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

QString CachePath() {
    return PathToQString(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                         "cheatslips_availability.json");
}

} // Anonymous namespace

CheatAvailabilityManager::CheatAvailabilityManager(QObject* parent) : QObject{parent} {
    cache_save_timer.setSingleShot(true);
    cache_save_timer.setInterval(std::chrono::milliseconds{500});
    request_spacing_timer.setSingleShot(true);
    request_spacing_timer.setInterval(MinimumRequestSpacingMilliseconds);
    connect(&cache_save_timer, &QTimer::timeout, this, &CheatAvailabilityManager::SaveCache);
    connect(&request_spacing_timer, &QTimer::timeout, this,
            &CheatAvailabilityManager::ProcessQueue);
    LoadCache();
}

CheatAvailabilityManager::~CheatAvailabilityManager() {
    if (cache_save_timer.isActive()) {
        SaveCache();
    }
}

QString CheatAvailabilityManager::NormalizeBuildId(const QString& build_id) {
    const QString normalized = build_id.trimmed().toUpper();
    static const QRegularExpression BuildIdPattern{QStringLiteral("^[0-9A-F]{16}$")};
    return BuildIdPattern.match(normalized).hasMatch() ? normalized : QString{};
}

QString CheatAvailabilityManager::MakeKey(u64 title_id, const QString& build_id) {
    return QStringLiteral("%1/%2")
        .arg(title_id, 16, 16, QLatin1Char{'0'})
        .arg(NormalizeBuildId(build_id))
        .toUpper();
}

void CheatAvailabilityManager::RequestAvailability(u64 title_id, const QString& build_id) {
    const QString normalized_build_id = NormalizeBuildId(build_id);
    if (title_id == 0 || normalized_build_id.isEmpty()) {
        return;
    }

    const QString key = MakeKey(title_id, normalized_build_id);
    if (queued_requests.contains(key) || active_requests.contains(key)) {
        return;
    }

    const auto cached = cache.constFind(key);
    if (cached != cache.cend() && cached->fetched_at.isValid() &&
        cached->fetched_at.secsTo(QDateTime::currentDateTimeUtc()) < CacheLifetimeSeconds) {
        return;
    }

    if (service_paused_until.isValid() &&
        QDateTime::currentDateTimeUtc() < service_paused_until) {
        failed_requests.insert(key);
        emit AvailabilityChanged(title_id, normalized_build_id);
        return;
    }

    failed_requests.remove(key);
    queued_requests.insert(key);
    request_queue.enqueue({title_id, normalized_build_id, key});
    emit AvailabilityChanged(title_id, normalized_build_id);
    ProcessQueue();
}

CheatAvailabilityManager::State CheatAvailabilityManager::GetState(
    u64 title_id, const QString& build_id) const {
    const QString normalized_build_id = NormalizeBuildId(build_id);
    if (title_id == 0 || normalized_build_id.isEmpty()) {
        return State::Unknown;
    }
    const QString key = MakeKey(title_id, normalized_build_id);
    if (const auto cached = cache.constFind(key); cached != cache.cend()) {
        return cached->cheat_count > 0 ? State::Available : State::NotFound;
    }
    if (queued_requests.contains(key) || active_requests.contains(key)) {
        return State::Pending;
    }
    if (failed_requests.contains(key)) {
        return State::NetworkError;
    }
    return State::Unknown;
}

int CheatAvailabilityManager::CheatCount(u64 title_id, const QString& build_id) const {
    const auto cached = cache.constFind(MakeKey(title_id, build_id));
    return cached != cache.cend() ? cached->cheat_count : 0;
}

void CheatAvailabilityManager::ProcessQueue() {
    if (request_spacing_timer.isActive() || !active_requests.isEmpty() ||
        request_queue.isEmpty()) {
        return;
    }

    {
        const Request request = request_queue.dequeue();
        queued_requests.remove(request.key);
        active_requests.insert(request.key);

        auto* watcher = new QFutureWatcher<WebService::CheatAvailabilityResult>(this);
        connect(watcher, &QFutureWatcher<WebService::CheatAvailabilityResult>::finished, this,
                [this, watcher, request] {
                    const WebService::CheatAvailabilityResult result = watcher->result();
                    watcher->deleteLater();
                    active_requests.remove(request.key);

                    if (result.code == WebService::CheatAvailabilityResultCode::Success ||
                        result.code == WebService::CheatAvailabilityResultCode::NotFound) {
                        cache.insert(request.key,
                                     {.cheat_count = result.cheat_count,
                                      .fetched_at = QDateTime::currentDateTimeUtc()});
                        failed_requests.remove(request.key);
                        ScheduleCacheSave();
                    } else if (!cache.contains(request.key)) {
                        failed_requests.insert(request.key);
                        const int pause_seconds =
                            result.code == WebService::CheatAvailabilityResultCode::RateLimited
                                ? RateLimitPauseSeconds
                                : NetworkFailurePauseSeconds;
                        service_paused_until =
                            QDateTime::currentDateTimeUtc().addSecs(pause_seconds);

                        while (!request_queue.isEmpty()) {
                            const Request cancelled = request_queue.dequeue();
                            queued_requests.remove(cancelled.key);
                            failed_requests.insert(cancelled.key);
                            emit AvailabilityChanged(cancelled.title_id, cancelled.build_id);
                        }
                    }

                    emit AvailabilityChanged(request.title_id, request.build_id);
                    request_spacing_timer.start();
                });

        const std::string build_id_string = request.build_id.toStdString();
        watcher->setFuture(QtConcurrent::run([title_id = request.title_id, build_id_string] {
            return WebService::FetchCheatAvailability(title_id, build_id_string);
        }));
    }
}

void CheatAvailabilityManager::ScheduleCacheSave() {
    cache_save_timer.start();
}

void CheatAvailabilityManager::LoadCache() {
    QFile file{CachePath()};
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        LOG_WARNING(Frontend, "Ignoring invalid CheatSlips availability cache");
        return;
    }

    const QJsonObject entries = document.object().value(QStringLiteral("entries")).toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        const QJsonObject entry = it.value().toObject();
        cache.insert(it.key().toUpper(),
                     {.cheat_count = std::max(0, entry.value(QStringLiteral("count")).toInt()),
                      .fetched_at = QDateTime::fromString(
                          entry.value(QStringLiteral("fetched_at")).toString(), Qt::ISODate)});
    }
}

void CheatAvailabilityManager::SaveCache() const {
    QJsonObject entries;
    for (auto it = cache.cbegin(); it != cache.cend(); ++it) {
        entries.insert(it.key(),
                       QJsonObject{{QStringLiteral("count"), it->cheat_count},
                                   {QStringLiteral("fetched_at"),
                                    it->fetched_at.toString(Qt::ISODate)}});
    }

    const QString path = CachePath();
    QDir{}.mkpath(QFileInfo{path}.absolutePath());
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument{QJsonObject{{QStringLiteral("version"), CacheVersion},
                                             {QStringLiteral("entries"), entries}}}
                       .toJson()) < 0 ||
        !file.commit()) {
        LOG_WARNING(Frontend, "Failed to save CheatSlips availability cache");
    }
}
