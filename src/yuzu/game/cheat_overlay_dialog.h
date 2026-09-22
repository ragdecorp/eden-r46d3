// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include <QDialog>
#include <QString>

#include "common/common_types.h"
#include "yuzu/game/cheat_memory_scanner.h"

class QComboBox;
class QFrame;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QWidget;

class ControllerNavigation;

namespace Core {
class System;
}

class CheatOverlayDialog final : public QDialog {
    Q_OBJECT

public:
    explicit CheatOverlayDialog(QWidget* parent, Core::System& system,
                                const QString& game_name, const QString& title_id,
                                const QString& build_id, const QString& screenshot_path);
    ~CheatOverlayDialog() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void BuildInterface(const QString& game_name, const QString& title_id,
                        const QString& build_id, const QString& screenshot_path);
    void ChooseKnownValue();
    void ChooseUnknownValue();
    void OpenNumericInput();
    void UpdatePanelSize();
    void UpdateScanMode();
    void UpdateScanControls();
    void ResetScanForm();
    void StartExactScan();
    void CaptureUnknownSnapshot();
    void StartNextScan();
    void ContinuePreviousSearch();
    void OpenCandidateViewer();
    void RunScan(bool filter_existing_candidates);
    bool LoadScanSession();
    bool LoadUnknownSnapshotSession();
    bool SaveScanSession() const;
    bool SaveUnknownSnapshotSession() const;
    bool SaveCandidateDrafts() const;
    void DeleteScanSession();
    void UndoLastScan();
    void StepValue(int direction);

    Core::System& system;
    QString title_id;
    QString build_id;
    QString session_directory;
    QString snapshot_file_path;
    std::vector<CheatScanCandidate> scan_candidates;
    std::vector<CheatScanCandidate> selected_candidates;
    std::vector<CheatScanCandidate> undo_scan_candidates;
    std::vector<CheatScanCandidate> undo_selected_candidates;
    bool has_scan_session{};
    bool has_unknown_snapshot{};
    bool undo_restores_unknown_snapshot{};
    bool filtering_existing_candidates{};
    QFrame* panel{};
    QComboBox* scan_mode_combo{};
    QComboBox* data_type_combo{};
    QComboBox* alignment_combo{};
    QLineEdit* value_edit{};
    QLabel* result_count_label{};
    QLabel* scanner_status_label{};
    QLabel* mode_summary_label{};
    QLabel* screenshot_preview{};
    QPushButton* known_value_button{};
    QPushButton* continue_search_button{};
    QPushButton* first_scan_button{};
    QPushButton* next_scan_button{};
    QPushButton* undo_scan_button{};
    QPushButton* reset_scan_button{};
    QPushButton* candidate_viewer_button{};
    QPushButton* value_keyboard_button{};
    QPushButton* continue_button{};
    QWidget* advanced_options_widget{};
    ControllerNavigation* controller_navigation{};
};
