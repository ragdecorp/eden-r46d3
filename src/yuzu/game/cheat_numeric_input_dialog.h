// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include <QDialog>

class QKeyEvent;
class QLineEdit;
class QPushButton;
class QShowEvent;

class CheatNumericInputDialog final : public QDialog {
    Q_OBJECT

public:
    explicit CheatNumericInputDialog(QWidget* parent, const QString& initial_value,
                                     bool allow_negative, bool allow_decimal);

    [[nodiscard]] QString Value() const;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void AppendCharacter(QChar character);
    void Backspace();
    void ClearValue();
    void MoveFocus(int row_delta, int column_delta);
    void UpdateAcceptButton();

    QLineEdit* value_display{};
    QPushButton* accept_button{};
    std::array<std::array<QPushButton*, 3>, 5> button_grid{};
    bool negative_allowed{};
    bool decimal_allowed{};
};
