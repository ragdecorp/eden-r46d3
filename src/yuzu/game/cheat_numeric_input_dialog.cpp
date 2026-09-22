// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/game/cheat_numeric_input_dialog.h"

#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

CheatNumericInputDialog::CheatNumericInputDialog(QWidget* parent, const QString& initial_value,
                                                 bool allow_negative, bool allow_decimal)
    : QDialog{parent}, negative_allowed{allow_negative}, decimal_allowed{allow_decimal} {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setWindowModality(Qt::WindowModal);
    setWindowTitle(tr("Enter value"));
    setFixedSize(560, 610);
    setStyleSheet(QStringLiteral(R"(
        CheatNumericInputDialog {
            color: #ffffff;
            background-color: #303034;
            border: 2px solid #6a6a72;
            border-radius: 10px;
        }
        QLabel#title {
            color: #ffffff;
            font-size: 24px;
            font-weight: 600;
        }
        QLabel#help {
            color: #bfc0c6;
            font-size: 13px;
        }
        QLineEdit {
            color: #ffffff;
            background-color: #202024;
            border: 2px solid #00e8bd;
            border-radius: 6px;
            min-height: 54px;
            padding: 4px 12px;
            font-family: Consolas, monospace;
            font-size: 25px;
        }
        QPushButton {
            color: #ffffff;
            background-color: #45454b;
            border: 1px solid #71717a;
            border-radius: 6px;
            min-width: 130px;
            min-height: 62px;
            font-size: 20px;
        }
        QPushButton:focus {
            color: #002f27;
            background-color: #00e8bd;
            border: 3px solid #bafff1;
        }
        QPushButton:disabled {
            color: #77777d;
            background-color: #39393e;
            border-color: #48484e;
        }
    )"));

    auto* root_layout = new QVBoxLayout{this};
    root_layout->setContentsMargins(28, 24, 28, 22);
    root_layout->setSpacing(14);

    auto* title = new QLabel{tr("Enter search value"), this};
    title->setObjectName(QStringLiteral("title"));
    root_layout->addWidget(title);

    value_display = new QLineEdit{initial_value, this};
    value_display->setReadOnly(true);
    value_display->setAlignment(Qt::AlignRight);
    value_display->setMaxLength(24);
    value_display->setFocusPolicy(Qt::NoFocus);
    root_layout->addWidget(value_display);

    auto* keypad_layout = new QGridLayout{};
    keypad_layout->setSpacing(10);

    const auto add_character_button = [this, keypad_layout](const QString& text, int row,
                                                             int column, QChar character) {
        auto* button = new QPushButton{text, this};
        button_grid[row][column] = button;
        keypad_layout->addWidget(button, row, column);
        connect(button, &QPushButton::clicked, this,
                [this, character] { AppendCharacter(character); });
        return button;
    };

    int number = 1;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const QString digit = QString::number(number++);
            add_character_button(digit, row, column, digit.front());
        }
    }

    auto* negative_button = add_character_button(QStringLiteral("−"), 3, 0, QLatin1Char{'-'});
    add_character_button(QStringLiteral("0"), 3, 1, QLatin1Char{'0'});
    auto* decimal_button = add_character_button(QStringLiteral("."), 3, 2, QLatin1Char{'.'});
    negative_button->setEnabled(negative_allowed);
    decimal_button->setEnabled(decimal_allowed);

    auto* clear_button = new QPushButton{tr("Clear"), this};
    auto* cancel_button = new QPushButton{tr("Cancel"), this};
    accept_button = new QPushButton{tr("Accept"), this};
    button_grid[4] = {clear_button, cancel_button, accept_button};
    keypad_layout->addWidget(clear_button, 4, 0);
    keypad_layout->addWidget(cancel_button, 4, 1);
    keypad_layout->addWidget(accept_button, 4, 2);
    root_layout->addLayout(keypad_layout, 1);

    auto* help = new QLabel{tr("D-Pad/Stick: navigate   A: select   B: cancel\n"
                               "X: erase one digit   Y: clear all"),
                            this};
    help->setObjectName(QStringLiteral("help"));
    help->setAlignment(Qt::AlignCenter);
    root_layout->addWidget(help);

    connect(clear_button, &QPushButton::clicked, this, &CheatNumericInputDialog::ClearValue);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
    connect(accept_button, &QPushButton::clicked, this, &QDialog::accept);

    UpdateAcceptButton();
    button_grid[0][0]->setFocus();
}

QString CheatNumericInputDialog::Value() const {
    return value_display->text();
}

void CheatNumericInputDialog::AppendCharacter(QChar character) {
    QString value = value_display->text();
    if (character == QLatin1Char{'-'}) {
        if (!negative_allowed) {
            return;
        }
        value.startsWith(QLatin1Char{'-'}) ? value.removeFirst() : value.prepend(QLatin1Char{'-'});
    } else if (character == QLatin1Char{'.'}) {
        if (!decimal_allowed || value.contains(QLatin1Char{'.'})) {
            return;
        }
        value += value.isEmpty() || value == QStringLiteral("-") ? QStringLiteral("0.")
                                                                  : QStringLiteral(".");
    } else if (value.size() < value_display->maxLength()) {
        value += character;
    }
    value_display->setText(value);
    UpdateAcceptButton();
}

void CheatNumericInputDialog::Backspace() {
    QString value = value_display->text();
    value.chop(1);
    value_display->setText(value);
    UpdateAcceptButton();
}

void CheatNumericInputDialog::ClearValue() {
    value_display->clear();
    UpdateAcceptButton();
}

void CheatNumericInputDialog::MoveFocus(int row_delta, int column_delta) {
    int current_row{};
    int current_column{};
    bool found{};
    for (int row = 0; row < static_cast<int>(button_grid.size()) && !found; ++row) {
        for (int column = 0; column < static_cast<int>(button_grid[row].size()); ++column) {
            if (button_grid[row][column]->hasFocus()) {
                current_row = row;
                current_column = column;
                found = true;
                break;
            }
        }
    }

    for (int attempt = 0; attempt < 15; ++attempt) {
        current_row = (current_row + row_delta + 5) % 5;
        current_column = (current_column + column_delta + 3) % 3;
        QPushButton* candidate = button_grid[current_row][current_column];
        if (candidate->isEnabled()) {
            candidate->setFocus();
            return;
        }
    }
}

void CheatNumericInputDialog::UpdateAcceptButton() {
    const QString value = value_display->text();
    bool valid{};
    value.toDouble(&valid);
    accept_button->setEnabled(valid && value != QStringLiteral("-") &&
                              value != QStringLiteral(".") && value != QStringLiteral("-."));
}

void CheatNumericInputDialog::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Escape:
        reject();
        return;
    case Qt::Key_Left:
        MoveFocus(0, -1);
        return;
    case Qt::Key_Right:
        MoveFocus(0, 1);
        return;
    case Qt::Key_Up:
        MoveFocus(-1, 0);
        return;
    case Qt::Key_Down:
        MoveFocus(1, 0);
        return;
    case Qt::Key_Enter:
    case Qt::Key_Return:
        if (auto* button = qobject_cast<QPushButton*>(focusWidget()); button != nullptr &&
                                                              button->isEnabled()) {
            button->click();
        }
        return;
    case Qt::Key_Backspace:
        Backspace();
        return;
    case Qt::Key_Delete:
        ClearValue();
        return;
    case Qt::Key_Minus:
        AppendCharacter(QLatin1Char{'-'});
        return;
    case Qt::Key_Period:
    case Qt::Key_Comma:
        AppendCharacter(QLatin1Char{'.'});
        return;
    default:
        break;
    }

    if (event->key() >= Qt::Key_0 && event->key() <= Qt::Key_9) {
        AppendCharacter(QChar{static_cast<char16_t>(u'0' + event->key() - Qt::Key_0)});
        return;
    }
    QDialog::keyPressEvent(event);
}

void CheatNumericInputDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (parentWidget() != nullptr) {
        move(parentWidget()->mapToGlobal(parentWidget()->rect().center()) - rect().center());
    }
    button_grid[0][0]->setFocus();
}
