// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "yuzu/game/cheat_selection_dialog.h"

CheatSelectionDialog::CheatSelectionDialog(
    const QString& game_name, u64 title_id, const QString& build_id,
    const std::vector<WebService::CheatCode>& cheats, QWidget* parent)
    : QDialog{parent} {
    setWindowTitle(tr("Cheats for %1").arg(game_name));
    resize(720, 500);

    auto* layout = new QVBoxLayout{this};
    auto* identity = new QLabel{
        tr("Select cheats for the exact version below.\nTitle ID: %1\nBuild ID: %2")
            .arg(QStringLiteral("%1").arg(title_id, 16, 16, QLatin1Char{'0'}).toUpper(),
                 build_id.toUpper()),
        this};
    identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(identity);

    cheat_list = new QListWidget{this};
    cheat_list->setAlternatingRowColors(true);
    for (qsizetype index = 0; index < static_cast<qsizetype>(cheats.size()); ++index) {
        const auto& cheat = cheats[static_cast<std::size_t>(index)];
        auto* item = new QListWidgetItem{
            QString::fromUtf8(cheat.name.data(), static_cast<qsizetype>(cheat.name.size())),
            cheat_list};
        item->setData(Qt::UserRole, index);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);

        QStringList details;
        if (!cheat.credits.empty()) {
            details.append(tr("Credits: %1").arg(QString::fromUtf8(
                cheat.credits.data(), static_cast<qsizetype>(cheat.credits.size()))));
        }
        if (!cheat.description.empty()) {
            details.append(QString::fromUtf8(cheat.description.data(),
                                             static_cast<qsizetype>(cheat.description.size())));
        }
        item->setToolTip(details.join(QStringLiteral("\n\n")));
    }
    layout->addWidget(cheat_list);

    auto* selection_buttons = new QHBoxLayout;
    auto* select_all = new QPushButton{tr("Select all"), this};
    auto* select_none = new QPushButton{tr("Select none"), this};
    selection_buttons->addWidget(select_all);
    selection_buttons->addWidget(select_none);
    selection_buttons->addStretch();
    layout->addLayout(selection_buttons);

    buttons = new QDialogButtonBox{QDialogButtonBox::Cancel, this};
    install_button = buttons->addButton(tr("Install selected"), QDialogButtonBox::AcceptRole);
    install_button->setEnabled(false);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(cheat_list, &QListWidget::itemChanged, this,
            [this] { UpdateInstallButton(); });
    connect(select_all, &QPushButton::clicked, this, [this] {
        for (int index = 0; index < cheat_list->count(); ++index) {
            cheat_list->item(index)->setCheckState(Qt::Checked);
        }
    });
    connect(select_none, &QPushButton::clicked, this, [this] {
        for (int index = 0; index < cheat_list->count(); ++index) {
            cheat_list->item(index)->setCheckState(Qt::Unchecked);
        }
    });
}

QVector<int> CheatSelectionDialog::SelectedIndices() const {
    QVector<int> selected;
    for (int index = 0; index < cheat_list->count(); ++index) {
        const QListWidgetItem* item = cheat_list->item(index);
        if (item->checkState() == Qt::Checked) {
            selected.append(item->data(Qt::UserRole).toInt());
        }
    }
    return selected;
}

void CheatSelectionDialog::UpdateInstallButton() {
    install_button->setEnabled(!SelectedIndices().isEmpty());
}
