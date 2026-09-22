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

class CheatAvailabilityManager final : public QObject {
    Q_OBJECT

public:
    enum class State {
        Unknown,
        Pending,
        Available,
        NotFound,
        NetworkError,
    };

    explicit CheatAvailabilityManager(QObject* parent = nullptr);
    ~CheatAvailabilityManager() override;

    void RequestAvailability(u64 title_id, const QString& build_id);
    State GetState(u64 title_id, const QString& build_id) const;
    int CheatCount(u64 title_id, const QString& build_id) const;

signals:
    void AvailabilityChanged(u64 title_id, const QString& build_id);

private:
    struct Request {
        u64 title_id{};
        QString build_id;
        QString key;
    };

    struct CachedAvailability {
        int cheat_count{};
        QDateTime fetched_at;
    };

    static QString NormalizeBuildId(const QString& build_id);
    static QString MakeKey(u64 title_id, const QString& build_id);

    void LoadCache();
    void ScheduleCacheSave();
    void SaveCache() const;
    void ProcessQueue();

    QHash<QString, CachedAvailability> cache;
    QQueue<Request> request_queue;
    QSet<QString> queued_requests;
    QSet<QString> active_requests;
    QSet<QString> failed_requests;
    QTimer cache_save_timer;
    QTimer request_spacing_timer;
    QDateTime service_paused_until;
};
