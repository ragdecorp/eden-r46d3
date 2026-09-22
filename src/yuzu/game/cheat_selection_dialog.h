// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include <QDialog>
#include <QVector>

#include "web_service/cheatslips.h"

class QDialogButtonBox;
class QListWidget;
class QPushButton;

class CheatSelectionDialog final : public QDialog {
    Q_OBJECT

public:
    CheatSelectionDialog(const QString& game_name, u64 title_id, const QString& build_id,
                         const std::vector<WebService::CheatCode>& cheats,
                         QWidget* parent = nullptr);

    QVector<int> SelectedIndices() const;

private:
    void UpdateInstallButton();

    QListWidget* cheat_list = nullptr;
    QDialogButtonBox* buttons = nullptr;
    QPushButton* install_button = nullptr;
};
