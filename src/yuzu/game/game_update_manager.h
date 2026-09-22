// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>

#include "common/common_types.h"

class GameUpdateManager final : public QObject {
    Q_OBJECT

public:
    enum class State {
        Unknown,
        Checking,
        UpToDate,
        UpdateAvailable,
        LocalNewer,
        NetworkError,
    };

    struct LatestVersion {
        u32 numeric_version{};
        QString release_date;
    };

    explicit GameUpdateManager(QObject* parent = nullptr);

    /// Revalidates the shared catalog when needed and looks up a friendly version label for the
    /// selected game. This method never blocks game launch.
    void RequestRefresh(u64 title_id, const QString& game_name, u32 installed_version,
                        bool installed_version_comparable);

    State GetState(u64 title_id, u32 installed_version,
                   bool installed_version_comparable) const;
    LatestVersion GetLatestVersion(u64 title_id) const;
    QString FriendlyVersion(u64 title_id, u32 numeric_version) const;

signals:
    void VersionsChanged();
    void FriendlyVersionChanged(u64 title_id);

private:
    struct FriendlyVersionEntry {
        u32 numeric_version{};
        QString display_version;
        QString source_url;
        QDateTime fetched_at;
    };

    struct PendingFriendlyRequest {
        QString game_name;
        u32 installed_version{};
        bool installed_version_comparable{};
    };

    struct FriendlyRequest {
        u64 title_id{};
        QString game_name;
        u32 numeric_version{};
    };

    static u64 NormalizeTitleId(u64 title_id);
    static QString TitleIdToString(u64 title_id);

    void LoadCache();
    void SaveCache() const;
    void RefreshCatalog();
    void MaybeRequestFriendlyVersion(u64 title_id, const QString& game_name,
                                     u32 installed_version, bool installed_version_comparable);
    void ProcessFriendlyQueue();

    QHash<u64, LatestVersion> latest_versions;
    QHash<u64, FriendlyVersionEntry> friendly_versions;
    QHash<u64, PendingFriendlyRequest> pending_friendly_requests;
    QQueue<FriendlyRequest> friendly_request_queue;
    QSet<u64> queued_friendly_requests;
    QSet<u64> active_friendly_requests;
    QTimer friendly_request_spacing_timer;
    QDateTime catalog_fetched_at;
    bool catalog_request_active{};
    bool catalog_request_failed{};
};
