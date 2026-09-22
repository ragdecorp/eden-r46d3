// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "common/common_types.h"

class GameMetadataManager final : public QObject {
    Q_OBJECT

public:
    explicit GameMetadataManager(QObject* parent = nullptr);
    ~GameMetadataManager() override;

    void RequestMetadata(u64 title_id);

    QString Players(u64 title_id) const;
    QStringList Genres(u64 title_id) const;
    QStringList Tags(u64 title_id) const;
    QString ReleaseDate(u64 title_id) const;
    bool HasMetadata(u64 title_id) const;
    bool IsPending(u64 title_id) const;

    void SetPlayersOverride(u64 title_id, const QString& players);
    void SetGenresOverride(u64 title_id, const QStringList& genres);
    void SetTags(u64 title_id, const QStringList& tags);

signals:
    void MetadataChanged(u64 title_id);

private:
    struct CachedMetadata {
        int system_players_min{};
        int system_players_max{};
        QStringList genres;
        QString release_date;
        QString nsuid;
        QString source_url;
        QString language;
        QDateTime fetched_at;
    };

    struct MetadataOverride {
        bool has_players{};
        QString players;
        bool has_genres{};
        QStringList genres;
        QStringList tags;
    };

    void LoadCache();
    void LoadOverrides();
    void ScheduleCacheSave();
    void SaveCache() const;
    void SaveOverrides() const;
    void ProcessQueue();

    static QString TitleIdToString(u64 title_id);
    static QStringList NormalizeList(const QStringList& values);
    std::string CurrentLanguage() const;

    QHash<u64, CachedMetadata> cache;
    QHash<u64, MetadataOverride> overrides;
    QQueue<u64> request_queue;
    QSet<u64> queued_requests;
    QSet<u64> active_requests;
    QSet<u64> unavailable_titles;
    QTimer cache_save_timer;
};
