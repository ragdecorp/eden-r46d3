// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include <QDialog>

class ControllerNavigation;
class QCloseEvent;
class QLabel;
class QHideEvent;
class QKeyEvent;
class QResizeEvent;
class QShowEvent;
class QWidget;

namespace Core::HID {
class HIDCore;
}

class TrailerPlayerDialog final : public QDialog {
    Q_OBJECT

public:
    explicit TrailerPlayerDialog(const QString& video_id, const QString& title,
                                 Core::HID::HIDCore& hid_core, QWidget* parent = nullptr);
    ~TrailerPlayerDialog() override;

    void Play(const QString& video_id, const QString& title);

signals:
    void PlaybackFailed(const QString& video_id);

protected:
    void closeEvent(QCloseEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void InitializePlayer();
    void NavigatePlayer();
    void UpdatePlayerBounds();
    void ClosePlayer();
    void StopPlayback();
    void FailPlayback();

    struct Impl;
    std::unique_ptr<Impl> impl;
    QString video_id;
    QWidget* player_container{};
    QLabel* title_label{};
    QLabel* loading_label{};
    ControllerNavigation* controller_navigation{};
    bool initializing{};
    bool initialized{};
    bool unavailable{};
    bool playback_requested{};
};
