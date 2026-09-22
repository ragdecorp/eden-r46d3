// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/game/cheat_overlay_dialog.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include <QAbstractButton>
#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QPixmap>
#include <QResizeEvent>
#include <QSaveFile>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUuid>
#include <QVBoxLayout>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory/dmnt_cheat_types.h"
#include "yuzu/game/cheat_memory_scanner.h"
#include "yuzu/game/cheat_numeric_input_dialog.h"
#include "yuzu/util/controller_navigation.h"

namespace {

constexpr quint32 ScanSessionMagic = 0x45435331; // "ECS1"
constexpr quint32 ScanSessionVersion = 2;
constexpr quint64 MaximumSavedCandidates = 2'000'000;
constexpr std::size_t MaximumViewableCandidates = 500;

const QString& RuntimeScanSessionId() {
    static const QString session_id =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    return session_id;
}

QLabel* CreateCaption(const QString& text, QWidget* parent) {
    auto* label = new QLabel{text, parent};
    label->setObjectName(QStringLiteral("caption"));
    return label;
}

QLabel* CreateValue(const QString& text, QWidget* parent) {
    auto* label = new QLabel{text, parent};
    label->setObjectName(QStringLiteral("metadataValue"));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QString CandidateTr(const char* source_text) {
    return QCoreApplication::translate("CheatCandidateDialog", source_text);
}

QString FormatAddress(u64 address) {
    return QStringLiteral("0x") +
           QString::number(address, 16).rightJustified(16, QLatin1Char{'0'}).toUpper();
}

QString FormatOffset(u64 offset) {
    return QStringLiteral("+0x") + QString::number(offset, 16).toUpper();
}

QString DataTypeName(CheatScanDataType type) {
    const std::string_view name = CheatScanDataTypeName(type);
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

QString FormatScanValue(const CheatScanValue& value) {
    switch (value.type) {
    case CheatScanDataType::Int8:
        return QString::number(static_cast<s8>(value.bits));
    case CheatScanDataType::UInt8:
        return QString::number(static_cast<u8>(value.bits));
    case CheatScanDataType::Int16:
        return QString::number(static_cast<s16>(value.bits));
    case CheatScanDataType::UInt16:
        return QString::number(static_cast<u16>(value.bits));
    case CheatScanDataType::Int32:
        return QString::number(static_cast<s32>(value.bits));
    case CheatScanDataType::UInt32:
        return QString::number(static_cast<u32>(value.bits));
    case CheatScanDataType::Int64:
        return QString::number(static_cast<qint64>(value.bits));
    case CheatScanDataType::UInt64:
        return QString::number(static_cast<quint64>(value.bits));
    case CheatScanDataType::Float:
        return QString::number(std::bit_cast<float>(static_cast<u32>(value.bits)), 'g', 9);
    case CheatScanDataType::Double:
        return QString::number(std::bit_cast<double>(value.bits), 'g', 17);
    }
    return {};
}

std::optional<CheatScanValue> ParseScanValue(const QString& text,
                                             CheatScanDataType type) {
    const QString value_text = text.trimmed();
    bool valid{};
    CheatScanValue value{.type = type};
    switch (type) {
    case CheatScanDataType::Int8:
    case CheatScanDataType::Int16:
    case CheatScanDataType::Int32:
    case CheatScanDataType::Int64: {
        const qlonglong parsed = value_text.toLongLong(&valid);
        if (!valid) {
            return std::nullopt;
        }
        const qlonglong minimum = type == CheatScanDataType::Int8
                                      ? std::numeric_limits<s8>::min()
                                  : type == CheatScanDataType::Int16
                                      ? std::numeric_limits<s16>::min()
                                  : type == CheatScanDataType::Int32
                                      ? std::numeric_limits<s32>::min()
                                      : std::numeric_limits<s64>::min();
        const qlonglong maximum = type == CheatScanDataType::Int8
                                      ? std::numeric_limits<s8>::max()
                                  : type == CheatScanDataType::Int16
                                      ? std::numeric_limits<s16>::max()
                                  : type == CheatScanDataType::Int32
                                      ? std::numeric_limits<s32>::max()
                                      : std::numeric_limits<s64>::max();
        if (parsed < minimum || parsed > maximum) {
            return std::nullopt;
        }
        value.bits = static_cast<u64>(parsed);
        return value;
    }
    case CheatScanDataType::UInt8:
    case CheatScanDataType::UInt16:
    case CheatScanDataType::UInt32:
    case CheatScanDataType::UInt64: {
        const qulonglong parsed = value_text.toULongLong(&valid);
        if (!valid) {
            return std::nullopt;
        }
        const qulonglong maximum = type == CheatScanDataType::UInt8
                                       ? std::numeric_limits<u8>::max()
                                   : type == CheatScanDataType::UInt16
                                       ? std::numeric_limits<u16>::max()
                                   : type == CheatScanDataType::UInt32
                                       ? std::numeric_limits<u32>::max()
                                       : std::numeric_limits<u64>::max();
        if (parsed > maximum) {
            return std::nullopt;
        }
        value.bits = static_cast<u64>(parsed);
        return value;
    }
    case CheatScanDataType::Float: {
        const double parsed = value_text.toDouble(&valid);
        if (!valid || !std::isfinite(parsed) ||
            std::abs(parsed) > std::numeric_limits<float>::max()) {
            return std::nullopt;
        }
        value.bits = std::bit_cast<u32>(static_cast<float>(parsed));
        return value;
    }
    case CheatScanDataType::Double: {
        const double parsed = value_text.toDouble(&valid);
        if (!valid || !std::isfinite(parsed)) {
            return std::nullopt;
        }
        value.bits = std::bit_cast<u64>(parsed);
        return value;
    }
    }
    return std::nullopt;
}

std::vector<CheatScanValue> ParseAutomaticValues(const QString& text) {
    constexpr std::array types{
        CheatScanDataType::Int32,
        CheatScanDataType::Int64,
        CheatScanDataType::Float,
        CheatScanDataType::Double,
    };
    std::vector<CheatScanValue> values;
    for (const CheatScanDataType type : types) {
        if (const auto value = ParseScanValue(text, type)) {
            values.push_back(*value);
        }
    }
    return values;
}

bool SameCandidate(const CheatScanCandidate& left, const CheatScanCandidate& right) {
    return left.address == right.address && left.last_value.type == right.last_value.type;
}

std::filesystem::path PathFromQString(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path{path.toStdWString()};
#else
    return std::filesystem::path{path.toStdString()};
#endif
}

QString CandidateRegionLabel(CheatAddressRegion region) {
    switch (region) {
    case CheatAddressRegion::Main:
        return CandidateTr("Main (potentially stable)");
    case CheatAddressRegion::Heap:
        return CandidateTr("Heap (dynamic)");
    case CheatAddressRegion::Alias:
        return CandidateTr("Alias (dynamic)");
    case CheatAddressRegion::Aslr:
        return CandidateTr("ASLR (dynamic)");
    case CheatAddressRegion::MappedNormal:
        return CandidateTr("Normal mapping (dynamic)");
    case CheatAddressRegion::MappedCodeData:
        return CandidateTr("Code data mapping");
    case CheatAddressRegion::MappedAliasCodeData:
        return CandidateTr("Alias code data mapping");
    case CheatAddressRegion::Other:
        return CandidateTr("Other");
    }
    return CandidateTr("Other");
}

QString CandidateRegionKey(CheatAddressRegion region) {
    switch (region) {
    case CheatAddressRegion::Main:
        return QStringLiteral("main");
    case CheatAddressRegion::Heap:
        return QStringLiteral("heap");
    case CheatAddressRegion::Alias:
        return QStringLiteral("alias");
    case CheatAddressRegion::Aslr:
        return QStringLiteral("aslr");
    case CheatAddressRegion::MappedNormal:
        return QStringLiteral("mapped_normal");
    case CheatAddressRegion::MappedCodeData:
        return QStringLiteral("mapped_code_data");
    case CheatAddressRegion::MappedAliasCodeData:
        return QStringLiteral("mapped_alias_code_data");
    case CheatAddressRegion::Other:
        return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

class CheatConfirmationDialog final : public QMessageBox {
public:
    CheatConfirmationDialog(QWidget* parent, const QString& title, const QString& text)
        : QMessageBox{QMessageBox::Question, title, text,
                      QMessageBox::Yes | QMessageBox::Cancel, parent} {
        setDefaultButton(QMessageBox::Cancel);
        setEscapeButton(QMessageBox::Cancel);
        if (QAbstractButton* yes_button = button(QMessageBox::Yes); yes_button != nullptr) {
            yes_button->setText(CandidateTr("Yes"));
        }
        if (QAbstractButton* cancel_button = button(QMessageBox::Cancel);
            cancel_button != nullptr) {
            cancel_button->setText(CandidateTr("Cancel"));
            cancel_button->setFocus();
        }
    }

    QMessageBox::StandardButton Execute() {
        return static_cast<QMessageBox::StandardButton>(exec());
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Up:
            focusPreviousChild();
            return;
        case Qt::Key_Right:
        case Qt::Key_Down:
            focusNextChild();
            return;
        case Qt::Key_Enter:
        case Qt::Key_Return:
            if (auto* focused_button = qobject_cast<QAbstractButton*>(focusWidget());
                focused_button != nullptr) {
                focused_button->click();
                return;
            }
            break;
        case Qt::Key_Escape:
            done(QMessageBox::Cancel);
            return;
        default:
            break;
        }
        QMessageBox::keyPressEvent(event);
    }
};

class CheatCandidateDialog final : public QDialog {
public:
    explicit CheatCandidateDialog(QWidget* parent, Core::System& system_,
                                  std::span<const CheatScanCandidate> candidates_,
                                  std::vector<CheatScanCandidate>& selected_candidates_,
                                  const QString& recovery_file_path_)
        : QDialog{parent}, system{system_}, candidates{candidates_},
          selected_candidates{selected_candidates_}, recovery_file_path{recovery_file_path_} {
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowTitleHint |
                       Qt::WindowSystemMenuHint | Qt::CustomizeWindowHint);
        setWindowModality(Qt::WindowModal);
        setWindowTitle(CandidateTr("Candidate viewer"));
        setMinimumSize(940, 540);
        setStyleSheet(QStringLiteral(R"(
            CheatCandidateDialog {
                background-color: #343438;
                border: 1px solid #626269;
            }
            QLabel {
                color: #f3f3f3;
                font-size: 14px;
            }
            QLabel#candidateTitle {
                color: #ffffff;
                font-size: 24px;
                font-weight: 600;
            }
            QLabel#candidateStatus {
                color: #8fe3c5;
                background-color: #273f39;
                border: 1px solid #3a6c5e;
                border-radius: 5px;
                padding: 8px;
            }
            QTableWidget {
                color: #f3f3f3;
                background-color: #28282c;
                alternate-background-color: #303035;
                border: 1px solid #686871;
                gridline-color: #4c4c52;
                selection-background-color: #17627a;
                selection-color: #ffffff;
                font-size: 14px;
            }
            QHeaderView::section {
                color: #ffffff;
                background-color: #3d3d43;
                border: 0;
                border-right: 1px solid #56565e;
                padding: 7px;
            }
            QPushButton {
                color: #ffffff;
                background-color: #45454b;
                border: 1px solid #707078;
                border-radius: 5px;
                min-height: 38px;
                padding: 3px 16px;
                font-size: 15px;
            }
            QPushButton:focus, QTableWidget:focus {
                border: 2px solid #00e8bd;
            }
        )"));

        auto* layout = new QVBoxLayout{this};
        layout->setContentsMargins(22, 18, 22, 18);
        layout->setSpacing(10);

        auto* title = new QLabel{CandidateTr("Candidate viewer"), this};
        title->setObjectName(QStringLiteral("candidateTitle"));
        layout->addWidget(title);

        status_label = new QLabel{
            CandidateTr("Manual mode. Values update only when you select Refresh values."),
            this};
        status_label->setObjectName(QStringLiteral("candidateStatus"));
        status_label->setWordWrap(true);
        layout->addWidget(status_label);

        table = new QTableWidget{this};
        table->setColumnCount(8);
        table->setHorizontalHeaderLabels(
            {CandidateTr("Selected"), CandidateTr("Address"), CandidateTr("Region"),
             CandidateTr("Relative offset"), CandidateTr("Data type"), CandidateTr("Last scan"),
             CandidateTr("Current value"), CandidateTr("State")});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
        table->verticalHeader()->setVisible(false);
        table->setAlternatingRowColors(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        layout->addWidget(table, 1);

        auto* action_layout = new QHBoxLayout{};
        select_button = new QPushButton{CandidateTr("Select candidate"), this};
        select_all_button = new QPushButton{CandidateTr("Select all"), this};
        write_button = new QPushButton{CandidateTr("Change selected"), this};
        undo_button = new QPushButton{CandidateTr("Undo last change"), this};
        undo_button->setEnabled(false);
        action_layout->addWidget(select_button);
        action_layout->addWidget(select_all_button);
        action_layout->addWidget(write_button);
        action_layout->addWidget(undo_button);
        layout->addLayout(action_layout);

        auto* freeze_layout = new QHBoxLayout{};
        auto* freeze_help = new QLabel{
            CandidateTr("Temporary freeze runs at 12 Hz and stops when the game closes."), this};
        freeze_layout->addWidget(freeze_help, 1);
        freeze_button = new QPushButton{CandidateTr("Freeze selected"), this};
        disable_freeze_button = new QPushButton{CandidateTr("Disable freeze"), this};
        restore_session_button =
            new QPushButton{CandidateTr("Restore session changes"), this};
        freeze_layout->addWidget(freeze_button);
        freeze_layout->addWidget(disable_freeze_button);
        freeze_layout->addWidget(restore_session_button);
        layout->addLayout(freeze_layout);

        auto* button_layout = new QHBoxLayout{};
        auto* help = new QLabel{
            CandidateTr("D-Pad/Stick: navigate   A: select   B: close"), this};
        button_layout->addWidget(help, 1);
        refresh_button = new QPushButton{CandidateTr("Refresh values"), this};
        auto* close_button = new QPushButton{CandidateTr("Close"), this};
        button_layout->addWidget(refresh_button);
        button_layout->addWidget(close_button);
        layout->addLayout(button_layout);

        connect(refresh_button, &QPushButton::clicked, this,
                [this] { RefreshValues(true); });
        connect(select_button, &QPushButton::clicked, this,
                [this] { ToggleSelectedCandidate(); });
        connect(select_all_button, &QPushButton::clicked, this,
                [this] { ToggleAllCandidates(); });
        connect(write_button, &QPushButton::clicked, this,
                [this] { ChangeSelectedCandidates(); });
        connect(undo_button, &QPushButton::clicked, this,
                [this] { UndoLastChange(); });
        connect(freeze_button, &QPushButton::clicked, this,
                [this] { FreezeSelectedCandidates(); });
        connect(disable_freeze_button, &QPushButton::clicked, this,
                [this] { DisableRuntimeFreeze(); });
        connect(restore_session_button, &QPushButton::clicked, this,
                [this] { RestoreSessionChanges(); });
        connect(close_button, &QPushButton::clicked, this, &QDialog::accept);
        connect(table, &QTableWidget::currentCellChanged, this,
                [this] { UpdateSelectionButton(); });

        RefreshValues(false);
        table->setFocus();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            reject();
            return;
        }

        QWidget* focused = focusWidget();
        if ((event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) &&
            focused == table && table->rowCount() > 0) {
            const int direction = event->key() == Qt::Key_Down ? 1 : -1;
            const int next_row = std::clamp(table->currentRow() + direction, 0,
                                            table->rowCount() - 1);
            table->setCurrentCell(next_row, 0);
            return;
        }
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            event->key() == Qt::Key_Right ? focusNextChild() : focusPreviousChild();
            return;
        }
        if (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return) {
            if (focused == table) {
                ToggleSelectedCandidate();
                return;
            }
            if (auto* button = qobject_cast<QPushButton*>(focused); button != nullptr &&
                                                               button->isEnabled()) {
                button->click();
                return;
            }
        }
        QDialog::keyPressEvent(event);
    }

private:
    bool IsCandidateSelected(const CheatScanCandidate& candidate) const {
        return std::find_if(selected_candidates.begin(), selected_candidates.end(),
                            [&candidate](const CheatScanCandidate& selected) {
                                return SameCandidate(selected, candidate);
                            }) != selected_candidates.end();
    }

    void RefreshSelectionMarkers() {
        for (int table_row = 0; table_row < table->rowCount(); ++table_row) {
            QTableWidgetItem* marker = table->item(table_row, 0);
            if (marker == nullptr) {
                continue;
            }
            const CheatScanCandidate& candidate =
                candidates[static_cast<std::size_t>(table_row)];
            marker->setText(IsCandidateSelected(candidate) ? QStringLiteral("★") : QString{});
        }
    }

    void RefreshValues(bool user_requested) {
        const int previous_row = std::max(0, table->currentRow());
        CheatMemoryScanner scanner{system};
        const std::vector<CheatTypedCandidateValue> values = scanner.ReadValues(candidates);
        table->setRowCount(static_cast<int>(values.size()));

        std::size_t changed_count{};
        std::size_t unavailable_count{};
        for (int row = 0; row < static_cast<int>(values.size()); ++row) {
            const CheatTypedCandidateValue& candidate = values[static_cast<std::size_t>(row)];
            const bool is_selected = IsCandidateSelected(candidate.candidate);
            const bool changed = candidate.readable &&
                                 candidate.current_value.bits != candidate.candidate.last_value.bits;
            changed_count += changed ? 1 : 0;
            unavailable_count += candidate.readable ? 0 : 1;

            const QString current_value = candidate.readable
                                              ? FormatScanValue(candidate.current_value)
                                              : CandidateTr("Unavailable");
            const QString state = !candidate.readable
                                      ? CandidateTr("Unreadable")
                                      : changed ? CandidateTr("Changed")
                                                : CandidateTr("Unchanged");
            const QStringList columns{
                is_selected ? QStringLiteral("★") : QString{},
                FormatAddress(candidate.candidate.address),
                CandidateRegionLabel(candidate.region), FormatOffset(candidate.relative_offset),
                DataTypeName(candidate.candidate.last_value.type),
                FormatScanValue(candidate.candidate.last_value), current_value, state};
            for (int column = 0; column < columns.size(); ++column) {
                auto* item = new QTableWidgetItem{columns[column]};
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                item->setData(Qt::UserRole,
                              QVariant::fromValue<qulonglong>(candidate.candidate.address));
                item->setData(Qt::UserRole + 1,
                              static_cast<int>(candidate.candidate.last_value.type));
                if (column != 1) {
                    item->setTextAlignment(Qt::AlignCenter);
                }
                table->setItem(row, column, item);
            }
        }

        if (!values.empty()) {
            table->setCurrentCell(std::min(previous_row, table->rowCount() - 1), 0);
        }
        status_label->setText(
            CandidateTr("Read-only snapshot: %1 changed, %2 unreadable. No memory was modified.")
                .arg(QLocale{}.toString(static_cast<qulonglong>(changed_count)),
                     QLocale{}.toString(static_cast<qulonglong>(unavailable_count))));
        if (user_requested) {
            table->setFocus();
        }
        UpdateSelectionButton();
    }

    void ToggleSelectedCandidate() {
        const int row = table->currentRow();
        QTableWidgetItem* item = row >= 0 ? table->item(row, 0) : nullptr;
        if (item == nullptr) {
            return;
        }
        const CheatScanCandidate& candidate = candidates[static_cast<std::size_t>(row)];
        const auto selected = std::find_if(
            selected_candidates.begin(), selected_candidates.end(),
            [&candidate](const CheatScanCandidate& value) { return SameCandidate(value, candidate); });
        if (selected != selected_candidates.end()) {
            selected_candidates.erase(selected);
        } else {
            selected_candidates.push_back(candidate);
        }
        RefreshSelectionMarkers();
        UpdateSelectionButton();
    }

    void ToggleAllCandidates() {
        if (selected_candidates.size() == candidates.size()) {
            selected_candidates.clear();
        } else {
            selected_candidates.assign(candidates.begin(), candidates.end());
        }
        RefreshSelectionMarkers();
        UpdateSelectionButton();
        table->setFocus();
    }

    void ChangeSelectedCandidates() {
        if (selected_candidates.empty()) {
            return;
        }

        const bool allow_negative = std::ranges::any_of(
            selected_candidates, [](const CheatScanCandidate& candidate) {
                return !CheatScanValueIsUnsigned(candidate.last_value.type);
            });
        const bool allow_decimal = std::ranges::any_of(
            selected_candidates, [](const CheatScanCandidate& candidate) {
                return CheatScanValueIsFloatingPoint(candidate.last_value.type);
            });
        CheatNumericInputDialog input_dialog{this, QString{}, allow_negative, allow_decimal};
        if (input_dialog.exec() != QDialog::Accepted) {
            return;
        }
        const QString requested_text = input_dialog.Value();
        if (!std::ranges::all_of(selected_candidates, [&requested_text](const auto& candidate) {
                return ParseScanValue(requested_text, candidate.last_value.type).has_value();
            })) {
            status_label->setText(
                CandidateTr("Enter a value valid for the selected data type."));
            return;
        }

        CheatConfirmationDialog confirmation_dialog{
            this, CandidateTr("Confirm memory change"),
            CandidateTr("Write value %1 once to %2 selected candidates? The game may overwrite "
                        "it later.")
                .arg(requested_text,
                     QLocale{}.toString(
                         static_cast<qulonglong>(selected_candidates.size())))};
        if (confirmation_dialog.Execute() != QMessageBox::Yes) {
            table->setFocus();
            return;
        }

        CheatMemoryScanner scanner{system};
        if (!CaptureRecoveryBackup(scanner)) {
            status_label->setText(CandidateTr(
                "The original value could not be saved. No memory was modified."));
            table->setFocus();
            return;
        }
        last_write_backup.clear();
        std::size_t verified_count{};
        for (const CheatScanCandidate& candidate : selected_candidates) {
            const auto requested_value = ParseScanValue(requested_text, candidate.last_value.type);
            if (!requested_value) {
                continue;
            }
            const CheatTypedWriteResult result =
                scanner.WriteValue(candidate.address, *requested_value);
            if (result.previous_readable && result.write_succeeded) {
                last_write_backup.push_back({
                    .address = candidate.address,
                    .last_value = result.previous_value,
                });
            }
            verified_count += result.verified ? 1 : 0;
        }
        LOG_INFO(Frontend,
                 "Cheat candidate write completed: requested_value={}, selected={}, verified={}",
                 requested_text.toStdString(), selected_candidates.size(), verified_count);

        RefreshValues(false);
        status_label->setText(
            CandidateTr("Memory change verified at %1 of %2 selected addresses. Use Undo last "
                        "change before continuing if the result is not correct.")
                .arg(QLocale{}.toString(static_cast<qulonglong>(verified_count)),
                     QLocale{}.toString(
                         static_cast<qulonglong>(selected_candidates.size()))));
        UpdateSelectionButton();
        write_button->setFocus();
    }

    void FreezeSelectedCandidates() {
        if (selected_candidates.empty()) {
            return;
        }
        if (selected_candidates.size() != 1) {
            status_label->setText(CandidateTr(
                "Safe mode: freeze exactly one validated candidate. Freezing several matching "
                "addresses can disable controls or corrupt game state."));
            table->setFocus();
            return;
        }

        const bool allow_negative = std::ranges::any_of(
            selected_candidates, [](const CheatScanCandidate& candidate) {
                return !CheatScanValueIsUnsigned(candidate.last_value.type);
            });
        const bool allow_decimal = std::ranges::any_of(
            selected_candidates, [](const CheatScanCandidate& candidate) {
                return CheatScanValueIsFloatingPoint(candidate.last_value.type);
            });
        CheatNumericInputDialog input_dialog{this, QString{}, allow_negative, allow_decimal};
        if (input_dialog.exec() != QDialog::Accepted) {
            return;
        }
        const QString requested_text = input_dialog.Value();
        if (std::ranges::none_of(selected_candidates, [&requested_text](const auto& candidate) {
                return ParseScanValue(requested_text, candidate.last_value.type).has_value();
            })) {
            status_label->setText(CandidateTr("Enter a value valid for the selected data type."));
            return;
        }

        CheatConfirmationDialog confirmation_dialog{
            this, CandidateTr("Confirm temporary freeze"),
            CandidateTr("Keep value %1 at %2 selected addresses while this game is running? This "
                        "replaces the current temporary freeze.")
                .arg(requested_text,
                     QLocale{}.toString(
                         static_cast<qulonglong>(selected_candidates.size())))};
        if (confirmation_dialog.Execute() != QMessageBox::Yes) {
            table->setFocus();
            return;
        }

        CheatMemoryScanner scanner{system};
        if (!CaptureRecoveryBackup(scanner)) {
            status_label->setText(CandidateTr(
                "The original value could not be saved. No memory was modified."));
            table->setFocus();
            return;
        }
        last_write_backup.clear();
        std::vector<Core::Memory::RuntimeMemoryFreeze> freezes;
        freezes.reserve(selected_candidates.size());
        std::size_t verified_count{};
        for (const CheatScanCandidate& candidate : selected_candidates) {
            const auto requested_value = ParseScanValue(requested_text, candidate.last_value.type);
            if (!requested_value) {
                continue;
            }
            const CheatTypedWriteResult result =
                scanner.WriteValue(candidate.address, *requested_value);
            if (result.previous_readable && result.write_succeeded) {
                last_write_backup.push_back({
                    .address = candidate.address,
                    .last_value = result.previous_value,
                });
            }
            if (result.verified) {
                freezes.push_back({
                    .address = candidate.address,
                    .value = requested_value->bits,
                    .size = static_cast<u8>(CheatScanDataTypeSize(requested_value->type)),
                });
                ++verified_count;
            }
        }
        system.SetRuntimeCheatFreezes(std::move(freezes));
        LOG_INFO(Frontend,
                 "Runtime memory freeze configured: value={}, selected={}, active={}",
                 requested_text.toStdString(), selected_candidates.size(), verified_count);

        RefreshValues(false);
        status_label->setText(
            CandidateTr("Temporary freeze enabled at %1 of %2 addresses. It will stop "
                        "automatically when the game closes.")
                .arg(QLocale{}.toString(static_cast<qulonglong>(verified_count)),
                     QLocale{}.toString(
                         static_cast<qulonglong>(selected_candidates.size()))));
        UpdateSelectionButton();
        freeze_button->setFocus();
    }

    void DisableRuntimeFreeze() {
        const std::size_t disabled_count = system.GetRuntimeCheatFreezes().size();
        system.SetRuntimeCheatFreezes({});
        LOG_INFO(Frontend, "Runtime memory freeze disabled: addresses={}", disabled_count);
        status_label->setText(
            CandidateTr("Temporary freeze disabled for %1 addresses.")
                .arg(QLocale{}.toString(static_cast<qulonglong>(disabled_count))));
        UpdateSelectionButton();
        disable_freeze_button->setFocus();
    }

    void UndoLastChange() {
        if (last_write_backup.empty()) {
            return;
        }

        if (!system.GetRuntimeCheatFreezes().empty()) {
            system.SetRuntimeCheatFreezes({});
        }
        CheatMemoryScanner scanner{system};
        const std::size_t attempted_count = last_write_backup.size();
        std::size_t restored_count{};
        std::vector<CheatScanCandidate> failed_restores;
        for (const CheatScanCandidate& previous : last_write_backup) {
            const CheatTypedWriteResult result =
                scanner.WriteValue(previous.address, previous.last_value);
            if (result.verified) {
                ++restored_count;
            } else {
                failed_restores.push_back(previous);
            }
        }
        last_write_backup = std::move(failed_restores);
        LOG_INFO(Frontend, "Cheat candidate undo completed: attempted={}, restored={}",
                 attempted_count, restored_count);

        RefreshValues(false);
        status_label->setText(
            CandidateTr("Undo restored %1 of %2 addresses.")
                .arg(QLocale{}.toString(static_cast<qulonglong>(restored_count)),
                     QLocale{}.toString(static_cast<qulonglong>(attempted_count))));
        UpdateSelectionButton();
        undo_button->setFocus();
    }

    std::vector<CheatScanCandidate> LoadRecoveryBackup() const {
        QFile recovery_file{recovery_file_path};
        if (!recovery_file.open(QIODevice::ReadOnly)) {
            return {};
        }
        const QJsonDocument document = QJsonDocument::fromJson(recovery_file.readAll());
        const Kernel::KProcess* process = system.ApplicationProcess();
        if (!document.isObject() || process == nullptr) {
            return {};
        }
        const QJsonObject root = document.object();
        if (root.value(QStringLiteral("schema_version")).toInt() != 1 ||
            root.value(QStringLiteral("runtime_session")).toString() != RuntimeScanSessionId() ||
            root.value(QStringLiteral("process_id")).toString() !=
                QString::number(process->GetProcessId())) {
            return {};
        }

        std::vector<CheatScanCandidate> backup;
        for (const QJsonValue& entry_value : root.value(QStringLiteral("original_values")).toArray()) {
            const QJsonObject entry = entry_value.toObject();
            bool address_valid{};
            bool bits_valid{};
            const u64 address = entry.value(QStringLiteral("address"))
                                    .toString()
                                    .toULongLong(&address_valid, 16);
            const u64 bits = entry.value(QStringLiteral("bits"))
                                 .toString()
                                 .toULongLong(&bits_valid, 16);
            const int type_value = entry.value(QStringLiteral("data_type")).toInt(-1);
            if (!address_valid || !bits_valid || type_value < static_cast<int>(CheatScanDataType::Int8) ||
                type_value > static_cast<int>(CheatScanDataType::Double)) {
                continue;
            }
            backup.push_back({
                .address = address,
                .last_value = {
                    .type = static_cast<CheatScanDataType>(type_value),
                    .bits = bits,
                },
            });
        }
        return backup;
    }

    bool SaveRecoveryBackup(const std::vector<CheatScanCandidate>& backup) const {
        if (backup.empty()) {
            return !QFile::exists(recovery_file_path) || QFile::remove(recovery_file_path);
        }
        const Kernel::KProcess* process = system.ApplicationProcess();
        const QFileInfo recovery_info{recovery_file_path};
        if (process == nullptr || !QDir{}.mkpath(recovery_info.absolutePath())) {
            return false;
        }

        QJsonArray entries;
        for (const CheatScanCandidate& candidate : backup) {
            QJsonObject entry;
            entry.insert(QStringLiteral("address"), QString::number(candidate.address, 16));
            entry.insert(QStringLiteral("data_type"),
                         static_cast<int>(candidate.last_value.type));
            entry.insert(QStringLiteral("bits"),
                         QString::number(candidate.last_value.bits, 16));
            entry.insert(QStringLiteral("display_value"),
                         FormatScanValue(candidate.last_value));
            entries.append(entry);
        }

        QJsonObject root;
        root.insert(QStringLiteral("schema_version"), 1);
        root.insert(QStringLiteral("runtime_session"), RuntimeScanSessionId());
        root.insert(QStringLiteral("process_id"), QString::number(process->GetProcessId()));
        root.insert(QStringLiteral("updated_at"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        root.insert(QStringLiteral("original_values"), entries);
        QSaveFile recovery_file{recovery_file_path};
        return recovery_file.open(QIODevice::WriteOnly) &&
               recovery_file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented)) >= 0 &&
               recovery_file.commit();
    }

    bool CaptureRecoveryBackup(CheatMemoryScanner& scanner) {
        std::vector<CheatScanCandidate> backup = LoadRecoveryBackup();
        const std::vector<CheatTypedCandidateValue> current_values =
            scanner.ReadValues(selected_candidates);
        for (const CheatTypedCandidateValue& current : current_values) {
            if (!current.readable ||
                std::ranges::any_of(backup, [&current](const CheatScanCandidate& saved) {
                    return SameCandidate(saved, current.candidate);
                })) {
                continue;
            }
            backup.push_back({
                .address = current.candidate.address,
                .last_value = current.current_value,
            });
        }
        const bool all_selected_backed_up =
            std::ranges::all_of(selected_candidates, [&backup](const CheatScanCandidate& selected) {
                return std::ranges::any_of(backup, [&selected](const CheatScanCandidate& saved) {
                    return SameCandidate(saved, selected);
                });
            });
        return all_selected_backed_up && SaveRecoveryBackup(backup);
    }

    void RestoreSessionChanges() {
        std::vector<CheatScanCandidate> backup = LoadRecoveryBackup();
        if (backup.empty()) {
            status_label->setText(
                CandidateTr("There are no restorable changes in this game session."));
            UpdateSelectionButton();
            return;
        }

        system.SetRuntimeCheatFreezes({});
        CheatMemoryScanner scanner{system};
        const std::size_t attempted_count = backup.size();
        std::size_t restored_count{};
        std::vector<CheatScanCandidate> failed;
        for (const CheatScanCandidate& original : backup) {
            const CheatTypedWriteResult result =
                scanner.WriteValue(original.address, original.last_value);
            if (result.verified) {
                ++restored_count;
            } else {
                failed.push_back(original);
            }
        }
        SaveRecoveryBackup(failed);
        last_write_backup.clear();
        RefreshValues(false);
        status_label->setText(
            CandidateTr("Session recovery restored %1 of %2 original values and disabled freezing.")
                .arg(QLocale{}.toString(static_cast<qulonglong>(restored_count)),
                     QLocale{}.toString(static_cast<qulonglong>(attempted_count))));
        UpdateSelectionButton();
        restore_session_button->setFocus();
        LOG_INFO(Frontend, "Cheat session recovery completed: attempted={}, restored={}",
                 attempted_count, restored_count);
    }

    void UpdateSelectionButton() {
        const int row = table->currentRow();
        QTableWidgetItem* item = row >= 0 ? table->item(row, 0) : nullptr;
        if (item == nullptr) {
            select_button->setEnabled(false);
        } else {
            select_button->setEnabled(true);
            const CheatScanCandidate& candidate = candidates[static_cast<std::size_t>(row)];
            select_button->setText(IsCandidateSelected(candidate)
                                       ? CandidateTr("Unselect candidate")
                                       : CandidateTr("Select candidate"));
        }
        const bool all_selected = !candidates.empty() &&
                                  selected_candidates.size() == candidates.size();
        select_all_button->setEnabled(!candidates.empty());
        select_all_button->setText(all_selected ? CandidateTr("Clear selection")
                                                : CandidateTr("Select all"));
        write_button->setEnabled(!selected_candidates.empty());
        write_button->setText(
            selected_candidates.empty()
                ? CandidateTr("Change selected")
                : CandidateTr("Change selected (%1)")
                      .arg(QLocale{}.toString(
                          static_cast<qulonglong>(selected_candidates.size()))));
        undo_button->setEnabled(!last_write_backup.empty());
        const std::size_t active_freeze_count = system.GetRuntimeCheatFreezes().size();
        freeze_button->setEnabled(!selected_candidates.empty());
        freeze_button->setText(
            selected_candidates.empty()
                ? CandidateTr("Freeze selected")
                : CandidateTr("Freeze selected (%1)")
                      .arg(QLocale{}.toString(
                          static_cast<qulonglong>(selected_candidates.size()))));
        disable_freeze_button->setEnabled(active_freeze_count != 0);
        disable_freeze_button->setText(
            active_freeze_count == 0
                ? CandidateTr("Disable freeze")
                : CandidateTr("Disable freeze (%1)")
                      .arg(QLocale{}.toString(
                          static_cast<qulonglong>(active_freeze_count))));
        restore_session_button->setEnabled(!LoadRecoveryBackup().empty());
    }

    Core::System& system;
    std::span<const CheatScanCandidate> candidates;
    std::vector<CheatScanCandidate>& selected_candidates;
    std::vector<CheatScanCandidate> last_write_backup;
    QString recovery_file_path;
    QLabel* status_label{};
    QTableWidget* table{};
    QPushButton* refresh_button{};
    QPushButton* select_button{};
    QPushButton* select_all_button{};
    QPushButton* write_button{};
    QPushButton* undo_button{};
    QPushButton* freeze_button{};
    QPushButton* disable_freeze_button{};
    QPushButton* restore_session_button{};
};

} // Anonymous namespace

CheatOverlayDialog::CheatOverlayDialog(QWidget* parent, Core::System& system_,
                                       const QString& game_name, const QString& title_id_,
                                       const QString& build_id_, const QString& screenshot_path)
    : QDialog{parent}, system{system_}, title_id{title_id_}, build_id{build_id_} {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowTitleHint |
                   Qt::WindowSystemMenuHint | Qt::CustomizeWindowHint);
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(tr("Cheat Mode"));

    const std::filesystem::path session_path =
        Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "cheats_studio" /
        title_id.toStdString() / build_id.toStdString();
#ifdef _WIN32
    session_directory = QString::fromStdWString(session_path.wstring());
#else
    session_directory = QString::fromStdString(session_path.string());
#endif
    snapshot_file_path =
        QDir{session_directory}.filePath(QStringLiteral("scan_snapshot.bin"));

    BuildInterface(game_name, title_id, build_id, screenshot_path);
    LoadScanSession();
    UpdateScanControls();

    controller_navigation = new ControllerNavigation{system.HIDCore(), this, true};
    controller_navigation->MapButton(Settings::NativeButton::X, Qt::Key_Backspace);
    controller_navigation->MapButton(Settings::NativeButton::Y, Qt::Key_Delete);
    connect(controller_navigation, &ControllerNavigation::TriggerKeyboardEvent, this,
            [this](Qt::Key key) {
                if (!isVisible()) {
                    return;
                }
                // Nested dialogs (such as the numeric keypad) are top-level Qt windows even
                // though they have this overlay as their logical parent. Route controller input
                // to the window containing the focused widget instead of relying on
                // isAncestorOf(), which rejects those nested dialog windows.
                QWidget* target = QApplication::focusWidget();
                target = target != nullptr ? target->window() : nullptr;
                if (target == nullptr) {
                    target = this;
                }
                QCoreApplication::postEvent(target,
                                            new QKeyEvent{QEvent::KeyPress, key, Qt::NoModifier});
            });
}

CheatOverlayDialog::~CheatOverlayDialog() {
    LOG_INFO(Frontend, "Cheat overlay unregistering controller navigation");
    if (controller_navigation != nullptr) {
        controller_navigation->UnloadController();
    }
    LOG_INFO(Frontend, "Cheat overlay controller navigation unregistered");
}

void CheatOverlayDialog::BuildInterface(const QString& game_name, const QString& game_title_id,
                                        const QString& game_build_id,
                                        const QString& screenshot_path) {
    setStyleSheet(QStringLiteral(R"(
        CheatOverlayDialog {
            background-color: rgba(0, 0, 0, 150);
        }
        QFrame#cheatPanel {
            background-color: #343438;
            border: 1px solid #626269;
            border-radius: 10px;
        }
        QLabel {
            color: #f3f3f3;
            font-size: 16px;
        }
        QLabel#title {
            color: #ffffff;
            font-size: 26px;
            font-weight: 600;
        }
        QLabel#sectionTitle {
            color: #ffffff;
            font-size: 19px;
            font-weight: 600;
        }
        QLabel#caption {
            color: #b9b9bf;
            font-size: 13px;
        }
        QLabel#metadataValue {
            color: #ffffff;
            font-family: Consolas, monospace;
            font-size: 16px;
        }
        QLabel#safeStatus {
            color: #8fe3c5;
            background-color: #273f39;
            border: 1px solid #3a6c5e;
            border-radius: 5px;
            padding: 8px;
        }
        QLabel#scannerStatus {
            color: #d7c98c;
            background-color: #45402c;
            border-radius: 5px;
            padding: 8px;
        }
        QLabel#controllerHelp {
            color: #bfc0c6;
            font-size: 13px;
        }
        QLabel#modeSummary {
            color: #ffffff;
            background-color: #29292e;
            border-left: 4px solid #00e8bd;
            padding: 8px;
            font-size: 14px;
        }
        QLabel#screenshotPreview {
            color: #bfc0c6;
            background-color: #151518;
            border: 1px solid #5a5a62;
            border-radius: 5px;
            font-size: 12px;
        }
        QComboBox, QLineEdit {
            color: #ffffff;
            background-color: #28282c;
            border: 1px solid #686871;
            border-radius: 5px;
            min-height: 34px;
            padding: 2px 9px;
            font-size: 15px;
        }
        QComboBox:focus, QLineEdit:focus, QPushButton:focus {
            border: 2px solid #00e8bd;
        }
        QPushButton {
            color: #ffffff;
            background-color: #45454b;
            border: 1px solid #707078;
            border-radius: 5px;
            min-height: 38px;
            padding: 3px 16px;
            font-size: 15px;
        }
        QPushButton:hover {
            background-color: #53535a;
        }
        QPushButton:disabled {
            color: #85858b;
            background-color: #3b3b40;
            border-color: #4c4c52;
        }
        QPushButton#continueButton {
            color: #002f27;
            background-color: #00e8bd;
            border-color: #00e8bd;
            font-weight: 600;
        }
    )"));

    auto* root_layout = new QVBoxLayout{this};
    root_layout->setContentsMargins(24, 16, 24, 16);
    root_layout->addStretch(1);

    panel = new QFrame{this};
    panel->setObjectName(QStringLiteral("cheatPanel"));
    panel->setMinimumWidth(760);
    auto* panel_layout = new QVBoxLayout{panel};
    panel_layout->setContentsMargins(26, 16, 26, 16);
    panel_layout->setSpacing(8);

    auto* title = new QLabel{tr("Cheat Mode"), panel};
    title->setObjectName(QStringLiteral("title"));
    panel_layout->addWidget(title);

    auto* safe_status = new QLabel{
        tr("Manual mode. Memory changes require selected candidates and explicit confirmation."),
        panel};
    safe_status->setObjectName(QStringLiteral("safeStatus"));
    panel_layout->addWidget(safe_status);

    auto* game_context_layout = new QHBoxLayout{};
    auto* metadata_layout = new QGridLayout{};
    metadata_layout->setHorizontalSpacing(18);
    metadata_layout->setVerticalSpacing(5);
    metadata_layout->addWidget(CreateCaption(tr("Game"), panel), 0, 0);
    metadata_layout->addWidget(CreateValue(game_name, panel), 0, 1);
    metadata_layout->addWidget(CreateCaption(tr("Title ID"), panel), 1, 0);
    metadata_layout->addWidget(CreateValue(game_title_id, panel), 1, 1);
    metadata_layout->addWidget(CreateCaption(tr("Build ID"), panel), 2, 0);
    metadata_layout->addWidget(CreateValue(game_build_id, panel), 2, 1);
    metadata_layout->setColumnStretch(1, 1);
    game_context_layout->addLayout(metadata_layout, 1);

    screenshot_preview = new QLabel{panel};
    screenshot_preview->setObjectName(QStringLiteral("screenshotPreview"));
    screenshot_preview->setFixedSize(240, 135);
    screenshot_preview->setAlignment(Qt::AlignCenter);
    const QPixmap screenshot{screenshot_path};
    if (!screenshot.isNull()) {
        screenshot_preview->setPixmap(
            screenshot.scaled(screenshot_preview->size(), Qt::KeepAspectRatio,
                              Qt::SmoothTransformation));
        screenshot_preview->setToolTip(tr("Screenshot captured when Cheat Mode was opened"));
    } else {
        screenshot_preview->setText(tr("Screenshot preview unavailable"));
    }
    game_context_layout->addWidget(screenshot_preview);
    panel_layout->addLayout(game_context_layout);

    auto* scanner_title = new QLabel{tr("Memory Scanner"), panel};
    scanner_title->setObjectName(QStringLiteral("sectionTitle"));
    panel_layout->addWidget(scanner_title);

    auto* quick_question = new QLabel{tr("What do you want to find?"), panel};
    quick_question->setObjectName(QStringLiteral("caption"));
    panel_layout->addWidget(quick_question);

    auto* quick_buttons = new QHBoxLayout{};
    known_value_button = new QPushButton{tr("I know the current value"), panel};
    auto* unknown_value_button = new QPushButton{tr("I don't know the value"), panel};
    continue_search_button = new QPushButton{tr("Continue previous search"), panel};
    auto* advanced_button = new QPushButton{tr("Advanced options"), panel};
    advanced_button->setCheckable(true);
    continue_search_button->setEnabled(false);
    continue_search_button->setToolTip(tr("Available after the first memory scan"));
    quick_buttons->addWidget(known_value_button);
    quick_buttons->addWidget(unknown_value_button);
    quick_buttons->addWidget(continue_search_button);
    quick_buttons->addWidget(advanced_button);
    panel_layout->addLayout(quick_buttons);

    mode_summary_label = new QLabel{panel};
    mode_summary_label->setObjectName(QStringLiteral("modeSummary"));
    panel_layout->addWidget(mode_summary_label);

    advanced_options_widget = new QWidget{panel};
    auto* scanner_layout = new QGridLayout{};
    scanner_layout->setContentsMargins(0, 0, 0, 0);
    scanner_layout->setHorizontalSpacing(18);
    scanner_layout->setVerticalSpacing(9);

    scanner_layout->addWidget(CreateCaption(tr("Scan type"), panel), 0, 0);
    scan_mode_combo = new QComboBox{panel};
    scan_mode_combo->addItem(tr("Exact value"), static_cast<int>(CheatScanComparison::Exact));
    scan_mode_combo->addItem(tr("Unknown initial value"), -1);
    scan_mode_combo->addItem(tr("Changed"), static_cast<int>(CheatScanComparison::Changed));
    scan_mode_combo->addItem(tr("Unchanged"), static_cast<int>(CheatScanComparison::Unchanged));
    scan_mode_combo->addItem(tr("Increased"), static_cast<int>(CheatScanComparison::Increased));
    scan_mode_combo->addItem(tr("Decreased"), static_cast<int>(CheatScanComparison::Decreased));
    scan_mode_combo->addItem(tr("Greater than"),
                             static_cast<int>(CheatScanComparison::GreaterThan));
    scan_mode_combo->addItem(tr("Less than"), static_cast<int>(CheatScanComparison::LessThan));
    scanner_layout->addWidget(scan_mode_combo, 0, 1);

    scanner_layout->addWidget(CreateCaption(tr("Data type"), panel), 1, 0);
    data_type_combo = new QComboBox{panel};
    data_type_combo->addItem(tr("Automatic (32/64-bit)"), -1);
    constexpr std::array data_types{
        CheatScanDataType::Int8,   CheatScanDataType::UInt8,
        CheatScanDataType::Int16,  CheatScanDataType::UInt16,
        CheatScanDataType::Int32,  CheatScanDataType::UInt32,
        CheatScanDataType::Int64,  CheatScanDataType::UInt64,
        CheatScanDataType::Float,  CheatScanDataType::Double,
    };
    for (const CheatScanDataType type : data_types) {
        data_type_combo->addItem(DataTypeName(type), static_cast<int>(type));
    }
    data_type_combo->setCurrentIndex(data_type_combo->findData(
        static_cast<int>(CheatScanDataType::Int32)));
    scanner_layout->addWidget(data_type_combo, 1, 1);

    scanner_layout->addWidget(CreateCaption(tr("Alignment"), panel), 2, 0);
    alignment_combo = new QComboBox{panel};
    alignment_combo->addItem(tr("Natural (recommended)"), false);
    alignment_combo->addItem(tr("Byte by byte (slower)"), true);
    scanner_layout->addWidget(alignment_combo, 2, 1);

    scanner_layout->setColumnStretch(1, 1);
    advanced_options_widget->setLayout(scanner_layout);
    advanced_options_widget->hide();
    panel_layout->addWidget(advanced_options_widget);

    auto* value_layout = new QHBoxLayout{};
    value_layout->addWidget(CreateCaption(tr("Search value"), panel));
    value_edit = new QLineEdit{panel};
    value_edit->setReadOnly(true);
    value_edit->setFocusPolicy(Qt::NoFocus);
    value_edit->setPlaceholderText(tr("Select 'I know the current value'"));
    value_edit->setInputMethodHints(Qt::ImhFormattedNumbersOnly);
    value_layout->addWidget(value_edit, 1);
    value_keyboard_button = new QPushButton{tr("Open numeric keypad"), panel};
    value_layout->addWidget(value_keyboard_button);
    panel_layout->addLayout(value_layout);

    auto* candidate_summary_layout = new QHBoxLayout{};
    result_count_label = new QLabel{tr("Candidates: —"), panel};
    result_count_label->setObjectName(QStringLiteral("metadataValue"));
    candidate_summary_layout->addWidget(result_count_label, 1);
    candidate_viewer_button = new QPushButton{tr("View candidates"), panel};
    candidate_viewer_button->setEnabled(false);
    candidate_viewer_button->setToolTip(
        tr("Narrow the search to 500 candidates or fewer"));
    candidate_summary_layout->addWidget(candidate_viewer_button);
    panel_layout->addLayout(candidate_summary_layout);

    scanner_status_label = new QLabel{
        tr("Typed read-only scanning is ready. Automatic mode checks Int32, Int64, Float, and "
           "Double."),
        panel};
    scanner_status_label->setObjectName(QStringLiteral("scannerStatus"));
    scanner_status_label->setWordWrap(true);
    panel_layout->addWidget(scanner_status_label);

    auto* scan_buttons = new QHBoxLayout{};
    first_scan_button = new QPushButton{tr("New scan"), panel};
    next_scan_button = new QPushButton{tr("Next scan"), panel};
    undo_scan_button = new QPushButton{tr("Undo scan"), panel};
    reset_scan_button = new QPushButton{tr("Reset"), panel};
    first_scan_button->setEnabled(false);
    next_scan_button->setEnabled(false);
    undo_scan_button->setEnabled(false);
    first_scan_button->setToolTip(tr("Enter a numeric value to start a read-only scan"));
    next_scan_button->setToolTip(tr("Run a new scan first"));
    scan_buttons->addWidget(first_scan_button);
    scan_buttons->addWidget(next_scan_button);
    scan_buttons->addWidget(undo_scan_button);
    scan_buttons->addWidget(reset_scan_button);
    panel_layout->addLayout(scan_buttons);

    auto* bottom_layout = new QHBoxLayout{};
    auto* controller_help =
        new QLabel{tr("D-Pad/Stick: navigate   A: confirm   B: close"), panel};
    controller_help->setObjectName(QStringLiteral("controllerHelp"));
    bottom_layout->addWidget(controller_help, 1);
    continue_button = new QPushButton{tr("Continue game"), panel};
    continue_button->setObjectName(QStringLiteral("continueButton"));
    bottom_layout->addWidget(continue_button);
    panel_layout->addLayout(bottom_layout);

    root_layout->addWidget(panel, 0, Qt::AlignHCenter);
    root_layout->addStretch(1);

    connect(known_value_button, &QPushButton::clicked, this,
            &CheatOverlayDialog::ChooseKnownValue);
    connect(unknown_value_button, &QPushButton::clicked, this,
            &CheatOverlayDialog::ChooseUnknownValue);
    connect(advanced_button, &QPushButton::toggled, advanced_options_widget,
            &QWidget::setVisible);
    connect(advanced_button, &QPushButton::toggled, screenshot_preview,
            &QWidget::setHidden);
    connect(scan_mode_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { UpdateScanMode(); });
    connect(data_type_combo, &QComboBox::currentTextChanged, this,
            [this] { UpdateScanControls(); });
    connect(alignment_combo, &QComboBox::currentTextChanged, this,
            [this] { UpdateScanControls(); });
    connect(value_edit, &QLineEdit::textChanged, this,
            [this] { UpdateScanControls(); });
    connect(value_keyboard_button, &QPushButton::clicked, this,
            &CheatOverlayDialog::OpenNumericInput);
    connect(first_scan_button, &QPushButton::clicked, this,
            &CheatOverlayDialog::StartExactScan);
    connect(next_scan_button, &QPushButton::clicked, this,
            &CheatOverlayDialog::StartNextScan);
    connect(undo_scan_button, &QPushButton::clicked, this,
            &CheatOverlayDialog::UndoLastScan);
    connect(continue_search_button, &QPushButton::clicked, this,
            &CheatOverlayDialog::ContinuePreviousSearch);
    connect(candidate_viewer_button, &QPushButton::clicked, this,
            &CheatOverlayDialog::OpenCandidateViewer);
    connect(reset_scan_button, &QPushButton::clicked, this,
            &CheatOverlayDialog::ResetScanForm);
    connect(continue_button, &QPushButton::clicked, this, &QDialog::accept);

    UpdateScanMode();
    UpdateScanControls();
    known_value_button->setFocus();
}

void CheatOverlayDialog::UpdatePanelSize() {
    if (panel == nullptr) {
        return;
    }

    if (parentWidget() != nullptr) {
        const QPoint parent_position = parentWidget()->mapToGlobal(QPoint{0, 0});
        move(parent_position);
        resize(parentWidget()->size());
    }

    const int available_width = std::max(720, width() - 72);
    panel->setMaximumWidth(std::min(1080, available_width));
    panel->setMinimumWidth(std::min(720, available_width));
}

void CheatOverlayDialog::UpdateScanMode() {
    const int mode = scan_mode_combo->currentData().toInt();
    const bool needs_value = mode == static_cast<int>(CheatScanComparison::Exact) ||
                             mode == static_cast<int>(CheatScanComparison::GreaterThan) ||
                             mode == static_cast<int>(CheatScanComparison::LessThan);
    value_edit->setEnabled(needs_value);
    value_keyboard_button->setEnabled(needs_value);
    if (!needs_value) {
        value_edit->clear();
    }
    mode_summary_label->setText(
        mode == -1 ? tr("Guided search: I don't know the initial value")
                   : needs_value ? tr("Guided search: compare with a numeric value")
                                 : tr("Guided search: compare with the previous scan"));
    UpdateScanControls();
}

void CheatOverlayDialog::UpdateScanControls() {
    if (scan_mode_combo == nullptr || data_type_combo == nullptr || value_edit == nullptr) {
        return;
    }
    const int mode = scan_mode_combo->currentData().toInt();
    const bool exact_mode = mode == static_cast<int>(CheatScanComparison::Exact);
    const bool needs_value = exact_mode ||
                             mode == static_cast<int>(CheatScanComparison::GreaterThan) ||
                             mode == static_cast<int>(CheatScanComparison::LessThan);
    const bool automatic = data_type_combo->currentData().toInt() == -1;
    bool valid_value = !needs_value;
    if (needs_value) {
        valid_value = automatic
                          ? !ParseAutomaticValues(value_edit->text()).empty()
                          : ParseScanValue(
                                value_edit->text(),
                                static_cast<CheatScanDataType>(
                                    data_type_combo->currentData().toInt()))
                                .has_value();
    }
    const bool no_active_scan = !has_scan_session && !has_unknown_snapshot;
    first_scan_button->setEnabled(no_active_scan &&
                                  ((exact_mode && valid_value) || mode == -1));
    continue_search_button->setEnabled(!scan_candidates.empty() || has_unknown_snapshot);
    next_scan_button->setEnabled(
        ((has_scan_session && !scan_candidates.empty()) || has_unknown_snapshot) && mode != -1 &&
        valid_value);
    undo_scan_button->setEnabled(undo_restores_unknown_snapshot ||
                                 !undo_scan_candidates.empty());
    candidate_viewer_button->setText(
        scan_candidates.empty()
            ? tr("View candidates")
            : tr("View candidates (%1)")
                  .arg(QLocale{}.toString(
                      static_cast<qulonglong>(scan_candidates.size()))));
    candidate_viewer_button->setEnabled(
        has_scan_session && !scan_candidates.empty() &&
        scan_candidates.size() <= MaximumViewableCandidates);
    candidate_viewer_button->setToolTip(
        scan_candidates.size() > MaximumViewableCandidates
            ? tr("Narrow the search to 500 candidates or fewer")
            : tr("Read the current values without modifying game memory"));

    first_scan_button->setToolTip(tr("Start a typed read-only exact-value scan"));
    next_scan_button->setToolTip(
        scan_candidates.empty()
            ? tr("Run a new scan first")
            : needs_value ? tr("Enter a value to filter the saved candidates")
                          : tr("Compare current memory with the previous scan"));
}

void CheatOverlayDialog::ChooseKnownValue() {
    // If a session already exists, the value the user enters is naturally the next observed
    // value. Do not force them to discover a separate "Continue previous search" workflow.
    filtering_existing_candidates = !scan_candidates.empty() || has_unknown_snapshot;
    scan_mode_combo->setCurrentIndex(
        scan_mode_combo->findData(static_cast<int>(CheatScanComparison::Exact)));
    scanner_status_label->setText(
        filtering_existing_candidates
            ? tr("Enter the new value currently shown in the game. It will filter the saved "
                 "candidates.")
            : tr("Enter the number currently shown in the game. Automatic mode checks the most "
                 "common 32/64-bit representations."));
    OpenNumericInput();
}

void CheatOverlayDialog::ChooseUnknownValue() {
    filtering_existing_candidates = false;
    scan_mode_combo->setCurrentIndex(scan_mode_combo->findData(-1));
    scanner_status_label->setText(
        tr("Unknown-initial snapshots are the next scanner checkpoint. For now, start with a "
           "known value or Automatic data type."));
    known_value_button->setFocus();
}

void CheatOverlayDialog::OpenNumericInput() {
    if (!value_keyboard_button->isEnabled()) {
        return;
    }

    const int selected_type = data_type_combo->currentData().toInt();
    const bool allow_negative = selected_type == -1 ||
                                !CheatScanValueIsUnsigned(
                                    static_cast<CheatScanDataType>(selected_type));
    const bool allow_decimal = selected_type == -1 ||
                               CheatScanValueIsFloatingPoint(
                                   static_cast<CheatScanDataType>(selected_type));
    CheatNumericInputDialog input_dialog{this, value_edit->text(), allow_negative, allow_decimal};
    if (input_dialog.exec() == QDialog::Accepted) {
        value_edit->setText(input_dialog.Value());
        if (filtering_existing_candidates && !scan_candidates.empty()) {
            scanner_status_label->setText(
                tr("New value ready. Select Next scan to filter the saved candidates."));
            next_scan_button->setFocus();
        } else {
            scanner_status_label->setText(
                tr("Value ready. Select New scan to search memory without modifying it."));
            first_scan_button->setFocus();
        }
    }
}

void CheatOverlayDialog::StartExactScan() {
    filtering_existing_candidates = false;
    if (scan_mode_combo->currentData().toInt() == -1) {
        CaptureUnknownSnapshot();
    } else {
        RunScan(false);
    }
}

void CheatOverlayDialog::CaptureUnknownSnapshot() {
    QProgressDialog progress_dialog{tr("Saving a read-only memory snapshot..."), tr("Cancel"),
                                    0, 1000, this};
    progress_dialog.setWindowTitle(tr("Unknown initial value"));
    progress_dialog.setWindowModality(Qt::WindowModal);
    progress_dialog.setMinimumDuration(0);
    progress_dialog.setAutoClose(false);
    progress_dialog.setValue(0);
    first_scan_button->setEnabled(false);
    reset_scan_button->setEnabled(false);
    scanner_status_label->setText(
        tr("Capturing readable game memory. The snapshot will be stored in the portable user "
           "folder."));

    CheatMemoryScanner scanner{system};
    const auto progress_callback = [&progress_dialog](u64 scanned_bytes, u64 total_bytes) {
        const int progress = total_bytes == 0
                                 ? 1000
                                 : static_cast<int>(std::min<u64>(
                                       1000, (scanned_bytes * 1000) / total_bytes));
        progress_dialog.setValue(progress);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        return !progress_dialog.wasCanceled();
    };
    const CheatSnapshotResult result = scanner.CaptureUnknownSnapshot(
        PathFromQString(snapshot_file_path), progress_callback);
    progress_dialog.setValue(1000);
    progress_dialog.close();
    reset_scan_button->setEnabled(true);

    switch (result.status) {
    case CheatScanResult::Status::Success:
        has_unknown_snapshot = true;
        has_scan_session = false;
        filtering_existing_candidates = true;
        scan_candidates.clear();
        selected_candidates.clear();
        undo_scan_candidates.clear();
        undo_selected_candidates.clear();
        undo_restores_unknown_snapshot = false;
        SaveUnknownSnapshotSession();
        scan_mode_combo->setCurrentIndex(
            scan_mode_combo->findData(static_cast<int>(CheatScanComparison::Changed)));
        result_count_label->setText(tr("Candidates: snapshot ready"));
        scanner_status_label->setText(
            tr("Initial snapshot saved. Continue the game, change the value, reopen Cheat Mode "
               "and choose Increased, Decreased, Changed, Unchanged, or an exact value."));
        break;
    case CheatScanResult::Status::Cancelled:
        scanner_status_label->setText(tr("Snapshot cancelled. No memory was modified."));
        break;
    case CheatScanResult::Status::NoApplicationProcess:
        scanner_status_label->setText(tr("The running game process is no longer available."));
        break;
    case CheatScanResult::Status::MemoryQueryFailed:
        scanner_status_label->setText(tr("Could not enumerate the game's memory regions."));
        break;
    case CheatScanResult::Status::MemoryReadFailed:
    case CheatScanResult::Status::SnapshotIoFailed:
        scanner_status_label->setText(
            tr("The portable memory snapshot could not be saved safely."));
        break;
    case CheatScanResult::Status::SnapshotTooLarge:
        scanner_status_label->setText(
            tr("The compressed snapshot exceeded the 1 GB safety limit. Choose a known value or "
               "a narrower scan method."));
        break;
    case CheatScanResult::Status::TooManyCandidates:
        break;
    }
    UpdateScanControls();
    continue_button->setFocus();
}

void CheatOverlayDialog::StartNextScan() {
    if (scan_candidates.empty() && !has_unknown_snapshot) {
        scanner_status_label->setText(tr("There is no saved search to continue."));
        return;
    }
    filtering_existing_candidates = true;
    RunScan(true);
}

void CheatOverlayDialog::ContinuePreviousSearch() {
    if (scan_candidates.empty() && !has_unknown_snapshot) {
        scanner_status_label->setText(tr("There is no saved search to continue."));
        return;
    }

    filtering_existing_candidates = true;
    scan_mode_combo->setCurrentIndex(scan_mode_combo->findData(
        has_unknown_snapshot ? static_cast<int>(CheatScanComparison::Changed)
                             : static_cast<int>(CheatScanComparison::Exact)));
    if (!scan_candidates.empty()) {
        const CheatScanDataType first_type = scan_candidates.front().last_value.type;
        const bool mixed_types = std::ranges::any_of(
            scan_candidates, [first_type](const CheatScanCandidate& candidate) {
                return candidate.last_value.type != first_type;
            });
        data_type_combo->setCurrentIndex(
            data_type_combo->findData(mixed_types ? -1 : static_cast<int>(first_type)));
    }
    value_edit->clear();
    if (has_unknown_snapshot) {
        scanner_status_label->setText(
            tr("Choose how the unknown value changed, then select Next scan."));
        scan_mode_combo->setFocus();
    } else {
        scanner_status_label->setText(
            tr("Enter the value currently shown in the game to reduce the saved candidates."));
        OpenNumericInput();
    }
}

void CheatOverlayDialog::OpenCandidateViewer() {
    if (!has_scan_session || scan_candidates.empty()) {
        scanner_status_label->setText(tr("Run a scan before opening the candidate viewer."));
        return;
    }
    if (scan_candidates.size() > MaximumViewableCandidates) {
        scanner_status_label->setText(
            tr("Reduce the search to 500 candidates or fewer before viewing results."));
        return;
    }

    CheatCandidateDialog candidate_dialog{
        this, system, scan_candidates, selected_candidates,
        QDir{session_directory}.filePath(QStringLiteral("runtime_recovery.json"))};
    candidate_dialog.exec();
    if (!SaveCandidateDrafts()) {
        LOG_WARNING(Frontend, "Could not save portable cheat candidate drafts");
    }
    if (!SaveScanSession()) {
        scanner_status_label->setText(
            tr("The selected candidates could not be saved in the portable user folder."));
    } else if (!selected_candidates.empty()) {
        scanner_status_label->setText(
            tr("%1 candidates are selected for manual memory changes.")
                .arg(QLocale{}.toString(
                    static_cast<qulonglong>(selected_candidates.size()))));
    }
    candidate_viewer_button->setFocus();
}

void CheatOverlayDialog::RunScan(bool filter_existing_candidates) {
    const int mode_value = scan_mode_combo->currentData().toInt();
    if (mode_value == -1 || (!filter_existing_candidates &&
                             mode_value != static_cast<int>(CheatScanComparison::Exact))) {
        scanner_status_label->setText(tr("Choose a supported scan comparison."));
        return;
    }

    const CheatScanComparison comparison = static_cast<CheatScanComparison>(mode_value);
    const bool needs_value = comparison == CheatScanComparison::Exact ||
                             comparison == CheatScanComparison::GreaterThan ||
                             comparison == CheatScanComparison::LessThan;
    std::vector<CheatScanValue> requested_values;
    if (needs_value) {
        if (filter_existing_candidates && !scan_candidates.empty()) {
            for (const CheatScanCandidate& candidate : scan_candidates) {
                if (std::ranges::none_of(requested_values, [&candidate](const auto& value) {
                        return value.type == candidate.last_value.type;
                    })) {
                    if (const auto value =
                            ParseScanValue(value_edit->text(), candidate.last_value.type)) {
                        requested_values.push_back(*value);
                    }
                }
            }
        } else if (data_type_combo->currentData().toInt() == -1) {
            requested_values = ParseAutomaticValues(value_edit->text());
        } else if (const auto value = ParseScanValue(
                       value_edit->text(), static_cast<CheatScanDataType>(
                                                    data_type_combo->currentData().toInt()))) {
            requested_values.push_back(*value);
        }
        if (requested_values.empty()) {
            scanner_status_label->setText(
                tr("Enter a value valid for the selected numeric data type."));
            return;
        }
    }

    std::vector<CheatScanDataType> selected_types;
    if (data_type_combo->currentData().toInt() == -1) {
        selected_types = {
            CheatScanDataType::Int32,
            CheatScanDataType::Int64,
            CheatScanDataType::Float,
            CheatScanDataType::Double,
        };
    } else {
        selected_types.push_back(static_cast<CheatScanDataType>(
            data_type_combo->currentData().toInt()));
    }

    QProgressDialog progress_dialog{filter_existing_candidates
                                        ? tr("Filtering saved candidates (read only)...")
                                        : tr("Scanning writable game memory (read only)..."),
                                    tr("Cancel"), 0, 1000, this};
    progress_dialog.setWindowTitle(tr("Memory scan"));
    progress_dialog.setWindowModality(Qt::WindowModal);
    progress_dialog.setMinimumDuration(0);
    progress_dialog.setAutoClose(false);
    progress_dialog.setAutoReset(false);
    progress_dialog.setValue(0);

    first_scan_button->setEnabled(false);
    next_scan_button->setEnabled(false);
    reset_scan_button->setEnabled(false);
    scanner_status_label->setText(tr("Scanning paused game memory. No values will be changed."));
    LOG_INFO(Frontend, "Cheat typed scan starting: mode={}, comparison={}, value={}, "
                       "previous_candidates={}",
             filter_existing_candidates ? "filter" : "initial", mode_value,
             value_edit->text().toStdString(), scan_candidates.size());

    CheatMemoryScanner scanner{system};
    const auto progress_callback = [&progress_dialog](u64 scanned_bytes, u64 total_bytes) {
            const int progress = total_bytes == 0
                                     ? 1000
                                     : static_cast<int>(std::min<u64>(
                                           1000, (scanned_bytes * 1000) / total_bytes));
            progress_dialog.setValue(progress);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            return !progress_dialog.wasCanceled();
        };
    const bool filtering_snapshot = has_unknown_snapshot;
    CheatTypedScanResult result = filtering_snapshot
                                      ? scanner.ScanUnknownSnapshot(
                                            PathFromQString(snapshot_file_path), selected_types,
                                            comparison, requested_values,
                                            alignment_combo->currentData().toBool(),
                                            progress_callback)
                                  : filter_existing_candidates
                                      ? scanner.FilterValues(scan_candidates, comparison,
                                                             requested_values, progress_callback)
                                      : scanner.ScanExactValues(
                                            requested_values,
                                            alignment_combo->currentData().toBool(),
                                            progress_callback);

    progress_dialog.setValue(1000);
    progress_dialog.close();
    reset_scan_button->setEnabled(true);
    LOG_INFO(Frontend, "Cheat scan finished: mode={}, status={}, candidates={}",
             filter_existing_candidates ? "filter" : "initial",
             static_cast<int>(result.status), result.candidates.size());

    switch (result.status) {
    case CheatScanResult::Status::Success:
        if (filtering_snapshot) {
            undo_scan_candidates.clear();
            undo_selected_candidates.clear();
            undo_restores_unknown_snapshot = !result.candidates.empty();
        } else if (filter_existing_candidates) {
            undo_scan_candidates = scan_candidates;
            undo_selected_candidates = selected_candidates;
            if (undo_restores_unknown_snapshot) {
                QFile::remove(snapshot_file_path);
                QFile::remove(QDir{session_directory}.filePath(
                    QStringLiteral("scan_snapshot.json")));
                undo_restores_unknown_snapshot = false;
            }
        } else {
            undo_scan_candidates.clear();
            undo_selected_candidates.clear();
        }
        scan_candidates = std::move(result.candidates);
        has_scan_session = !scan_candidates.empty();
        if (filtering_snapshot && has_scan_session) {
            has_unknown_snapshot = false;
        }
        std::erase_if(selected_candidates, [this](const CheatScanCandidate& selected) {
            return std::ranges::none_of(scan_candidates, [&selected](const auto& candidate) {
                return SameCandidate(selected, candidate);
            });
        });
        result_count_label->setText(
            tr("Candidates: %1").arg(QLocale{}.toString(
                static_cast<qulonglong>(scan_candidates.size()))));
        if (scan_candidates.empty()) {
            selected_candidates.clear();
            scanner_status_label->setText(
                filter_existing_candidates
                    ? tr("No candidates remain. Use Undo scan to restore the previous results.")
                    : tr("Scan complete: no matching values were found."));
        } else if (!SaveScanSession()) {
            scanner_status_label->setText(
                tr("The scan completed, but its session could not be saved in the portable user "
                   "folder."));
        } else if (filter_existing_candidates) {
            scanner_status_label->setText(
                tr("Next scan complete: %1 candidates remain. Repeat after the value changes "
                   "again.")
                    .arg(QLocale{}.toString(
                        static_cast<qulonglong>(scan_candidates.size()))));
        } else {
            scanner_status_label->setText(
                tr("First scan saved. Continue the game, change the value, reopen Cheat Mode and "
                   "select Continue previous search."));
        }
        break;
    case CheatScanResult::Status::Cancelled:
        result_count_label->setText(tr("Candidates: —"));
        scanner_status_label->setText(tr("Scan cancelled. Game memory was not modified."));
        break;
    case CheatScanResult::Status::TooManyCandidates:
        result_count_label->setText(tr("Candidates: more than 2,000,000"));
        scanner_status_label->setText(
            tr("Too many matches. Use a more distinctive current value and start again."));
        break;
    case CheatScanResult::Status::NoApplicationProcess:
        scanner_status_label->setText(tr("The running game process is no longer available."));
        break;
    case CheatScanResult::Status::MemoryQueryFailed:
        scanner_status_label->setText(tr("Could not enumerate the game's memory regions."));
        break;
    case CheatScanResult::Status::MemoryReadFailed:
        scanner_status_label->setText(
            tr("A readable game-memory region could not be scanned safely."));
        break;
    case CheatScanResult::Status::SnapshotIoFailed:
        scanner_status_label->setText(
            tr("The saved memory snapshot is missing or could not be read."));
        break;
    case CheatScanResult::Status::SnapshotTooLarge:
        scanner_status_label->setText(
            tr("The memory snapshot exceeds the configured safety limit."));
        break;
    }

    UpdateScanControls();
    continue_button->setFocus();
}

void CheatOverlayDialog::UndoLastScan() {
    if (undo_restores_unknown_snapshot) {
        scan_candidates.clear();
        selected_candidates.clear();
        has_scan_session = false;
        has_unknown_snapshot = QFile::exists(snapshot_file_path);
        undo_restores_unknown_snapshot = false;
        filtering_existing_candidates = has_unknown_snapshot;
        result_count_label->setText(tr("Candidates: snapshot ready"));
        scanner_status_label->setText(
            tr("The unknown-value snapshot was restored. Choose another comparison and run Next "
               "scan."));
        UpdateScanControls();
        scan_mode_combo->setFocus();
        return;
    }
    if (undo_scan_candidates.empty()) {
        return;
    }
    scan_candidates = std::move(undo_scan_candidates);
    selected_candidates = std::move(undo_selected_candidates);
    undo_scan_candidates.clear();
    undo_selected_candidates.clear();
    has_scan_session = !scan_candidates.empty();
    filtering_existing_candidates = has_scan_session;
    result_count_label->setText(
        tr("Candidates: %1")
            .arg(QLocale{}.toString(static_cast<qulonglong>(scan_candidates.size()))));
    scanner_status_label->setText(
        tr("The previous candidate set was restored. You can enter a corrected value or choose "
           "another comparison."));
    SaveScanSession();
    UpdateScanControls();
    next_scan_button->setFocus();
}

bool CheatOverlayDialog::LoadScanSession() {
    const QDir directory{session_directory};
    const QString metadata_path = directory.filePath(QStringLiteral("scan_session.json"));
    const QString candidates_path = directory.filePath(QStringLiteral("scan_candidates.bin"));
    if (!QFile::exists(metadata_path) || !QFile::exists(candidates_path)) {
        return LoadUnknownSnapshotSession();
    }

    QFile metadata_file{metadata_path};
    if (!metadata_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(metadata_file.readAll());
    if (!document.isObject()) {
        return false;
    }

    const QJsonObject metadata = document.object();
    const Kernel::KProcess* process = system.ApplicationProcess();
    if (process == nullptr) {
        return false;
    }
    const quint64 expected_count =
        static_cast<quint64>(metadata.value(QStringLiteral("candidate_count")).toDouble());
    if (metadata.value(QStringLiteral("schema_version")).toInt() != ScanSessionVersion ||
        metadata.value(QStringLiteral("title_id")).toString() != title_id ||
        metadata.value(QStringLiteral("build_id")).toString() != build_id ||
        metadata.value(QStringLiteral("runtime_session")).toString() !=
            RuntimeScanSessionId() ||
        metadata.value(QStringLiteral("process_id")).toString() !=
            QString::number(process->GetProcessId()) || expected_count == 0 ||
        expected_count > MaximumSavedCandidates) {
        return false;
    }

    QFile candidates_file{candidates_path};
    if (!candidates_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QDataStream input{&candidates_file};
    input.setByteOrder(QDataStream::LittleEndian);
    quint32 magic{};
    quint32 version{};
    quint64 stored_count{};
    input >> magic >> version >> stored_count;
    if (magic != ScanSessionMagic || version != ScanSessionVersion ||
        stored_count != expected_count || stored_count > MaximumSavedCandidates) {
        return false;
    }

    std::vector<CheatScanCandidate> loaded_candidates;
    loaded_candidates.resize(static_cast<std::size_t>(stored_count));
    for (CheatScanCandidate& candidate : loaded_candidates) {
        quint64 stored_address{};
        quint8 stored_type{};
        quint64 stored_bits{};
        input >> stored_address >> stored_type >> stored_bits;
        if (stored_type > static_cast<quint8>(CheatScanDataType::Double)) {
            return false;
        }
        candidate = {
            .address = stored_address,
            .last_value = {
                .type = static_cast<CheatScanDataType>(stored_type),
                .bits = stored_bits,
            },
        };
    }
    if (input.status() != QDataStream::Ok) {
        return false;
    }

    scan_candidates = std::move(loaded_candidates);
    has_scan_session = true;
    const auto restore_selected_candidate = [this](const QJsonObject& stored) {
        bool address_valid{};
        const u64 address =
            stored.value(QStringLiteral("address")).toString().toULongLong(&address_valid, 16);
        const int stored_type = stored.value(QStringLiteral("data_type")).toInt(-1);
        if (!address_valid || stored_type < 0 ||
            stored_type > static_cast<int>(CheatScanDataType::Double)) {
            return;
        }
        const CheatScanDataType type = static_cast<CheatScanDataType>(stored_type);
        const auto found = std::find_if(scan_candidates.begin(), scan_candidates.end(),
                                        [address, type](const CheatScanCandidate& candidate) {
                                            return candidate.address == address &&
                                                   candidate.last_value.type == type;
                                        });
        if (found != scan_candidates.end() &&
            std::ranges::none_of(selected_candidates,
                                 [&found](const CheatScanCandidate& selected) {
                                     return SameCandidate(selected, *found);
                                 })) {
            selected_candidates.push_back(*found);
        }
    };
    for (const QJsonValue& selected_value :
         metadata.value(QStringLiteral("selected_candidates")).toArray()) {
        restore_selected_candidate(selected_value.toObject());
    }
    filtering_existing_candidates = true;
    result_count_label->setText(
        tr("Candidates: %1").arg(
            QLocale{}.toString(static_cast<qulonglong>(scan_candidates.size()))));
    scanner_status_label->setText(
        tr("A saved read-only search was found. Continue the game until the value changes, then "
           "select Continue previous search."));
    return true;
}

bool CheatOverlayDialog::LoadUnknownSnapshotSession() {
    const QDir directory{session_directory};
    const QString metadata_path =
        directory.filePath(QStringLiteral("scan_snapshot.json"));
    if (!QFile::exists(metadata_path) || !QFile::exists(snapshot_file_path)) {
        return false;
    }
    QFile metadata_file{metadata_path};
    if (!metadata_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(metadata_file.readAll());
    const Kernel::KProcess* process = system.ApplicationProcess();
    if (!document.isObject() || process == nullptr) {
        return false;
    }
    const QJsonObject metadata = document.object();
    if (metadata.value(QStringLiteral("schema_version")).toInt() != 1 ||
        metadata.value(QStringLiteral("title_id")).toString() != title_id ||
        metadata.value(QStringLiteral("build_id")).toString() != build_id ||
        metadata.value(QStringLiteral("runtime_session")).toString() != RuntimeScanSessionId() ||
        metadata.value(QStringLiteral("process_id")).toString() !=
            QString::number(process->GetProcessId())) {
        return false;
    }
    has_unknown_snapshot = true;
    filtering_existing_candidates = true;
    scan_mode_combo->setCurrentIndex(
        scan_mode_combo->findData(static_cast<int>(CheatScanComparison::Changed)));
    result_count_label->setText(tr("Candidates: snapshot ready"));
    scanner_status_label->setText(
        tr("A saved unknown-value snapshot was found. Choose how the value changed and select "
           "Next scan."));
    return true;
}

bool CheatOverlayDialog::SaveUnknownSnapshotSession() const {
    const Kernel::KProcess* process = system.ApplicationProcess();
    if (process == nullptr || !QFile::exists(snapshot_file_path) ||
        !QDir{}.mkpath(session_directory)) {
        return false;
    }
    QJsonObject metadata;
    metadata.insert(QStringLiteral("schema_version"), 1);
    metadata.insert(QStringLiteral("title_id"), title_id);
    metadata.insert(QStringLiteral("build_id"), build_id);
    metadata.insert(QStringLiteral("runtime_session"), RuntimeScanSessionId());
    metadata.insert(QStringLiteral("process_id"), QString::number(process->GetProcessId()));
    metadata.insert(QStringLiteral("updated_at"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    QSaveFile metadata_file{
        QDir{session_directory}.filePath(QStringLiteral("scan_snapshot.json"))};
    return metadata_file.open(QIODevice::WriteOnly) &&
           metadata_file.write(QJsonDocument{metadata}.toJson(QJsonDocument::Indented)) >= 0 &&
           metadata_file.commit();
}

bool CheatOverlayDialog::SaveScanSession() const {
    const Kernel::KProcess* process = system.ApplicationProcess();
    if (scan_candidates.empty() || scan_candidates.size() > MaximumSavedCandidates ||
        process == nullptr || !QDir{}.mkpath(session_directory)) {
        return false;
    }

    const QDir directory{session_directory};
    QSaveFile candidates_file{
        directory.filePath(QStringLiteral("scan_candidates.bin"))};
    if (!candidates_file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream output{&candidates_file};
    output.setByteOrder(QDataStream::LittleEndian);
    output << ScanSessionMagic << ScanSessionVersion
           << static_cast<quint64>(scan_candidates.size());
    for (const CheatScanCandidate& candidate : scan_candidates) {
        output << static_cast<quint64>(candidate.address)
               << static_cast<quint8>(candidate.last_value.type)
               << static_cast<quint64>(candidate.last_value.bits);
    }
    if (output.status() != QDataStream::Ok || !candidates_file.commit()) {
        return false;
    }

    QJsonObject metadata;
    metadata.insert(QStringLiteral("schema_version"), static_cast<int>(ScanSessionVersion));
    metadata.insert(QStringLiteral("title_id"), title_id);
    metadata.insert(QStringLiteral("build_id"), build_id);
    metadata.insert(QStringLiteral("data_type"), QStringLiteral("typed"));
    metadata.insert(QStringLiteral("runtime_session"), RuntimeScanSessionId());
    metadata.insert(QStringLiteral("process_id"), QString::number(process->GetProcessId()));
    QJsonArray selected_addresses;
    for (const CheatScanCandidate& candidate : selected_candidates) {
        QJsonObject selected;
        selected.insert(QStringLiteral("address"), QString::number(candidate.address, 16));
        selected.insert(QStringLiteral("data_type"),
                        static_cast<int>(candidate.last_value.type));
        selected_addresses.append(selected);
    }
    metadata.insert(QStringLiteral("selected_candidates"), selected_addresses);
    metadata.insert(QStringLiteral("candidate_count"),
                    static_cast<double>(scan_candidates.size()));
    metadata.insert(QStringLiteral("updated_at"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

    QSaveFile metadata_file{directory.filePath(QStringLiteral("scan_session.json"))};
    if (!metadata_file.open(QIODevice::WriteOnly) ||
        metadata_file.write(QJsonDocument{metadata}.toJson(QJsonDocument::Indented)) < 0 ||
        !metadata_file.commit()) {
        return false;
    }
    LOG_INFO(Frontend, "Cheat typed scan session saved: title_id={}, build_id={}, candidates={}",
             title_id.toStdString(), build_id.toStdString(), scan_candidates.size());
    return true;
}

bool CheatOverlayDialog::SaveCandidateDrafts() const {
    if (!QDir{}.mkpath(session_directory)) {
        return false;
    }

    CheatMemoryScanner scanner{system};
    const std::vector<CheatTypedCandidateValue> candidates =
        scanner.ReadValues(selected_candidates);
    const std::vector<Core::Memory::RuntimeMemoryFreeze> active_freezes =
        system.GetRuntimeCheatFreezes();

    QJsonArray candidate_entries;
    for (const CheatTypedCandidateValue& candidate : candidates) {
        const auto active_freeze =
            std::find_if(active_freezes.begin(), active_freezes.end(),
                         [&candidate](const Core::Memory::RuntimeMemoryFreeze& freeze) {
                             return freeze.address == candidate.candidate.address &&
                                    freeze.size == CheatScanDataTypeSize(
                                                       candidate.candidate.last_value.type);
                         });
        QJsonObject entry;
        entry.insert(QStringLiteral("address"),
                     QString::number(candidate.candidate.address, 16));
        entry.insert(QStringLiteral("data_type"),
                     DataTypeName(candidate.candidate.last_value.type));
        entry.insert(QStringLiteral("last_scan_value"),
                     FormatScanValue(candidate.candidate.last_value));
        entry.insert(QStringLiteral("region"), CandidateRegionKey(candidate.region));
        entry.insert(QStringLiteral("relative_offset"),
                     QString::number(candidate.relative_offset, 16));
        entry.insert(QStringLiteral("mapping_base"),
                     QString::number(candidate.mapping_base, 16));
        entry.insert(QStringLiteral("mapping_size"),
                     QString::number(candidate.mapping_size, 16));
        QString stability = QStringLiteral("dynamic_requires_pointer_scan");
        if (candidate.region == CheatAddressRegion::Main) {
            stability = QStringLiteral("potentially_stable_for_build_id");
        } else if (candidate.region == CheatAddressRegion::MappedCodeData ||
                   candidate.region == CheatAddressRegion::MappedAliasCodeData) {
            stability = QStringLiteral("module_mapping_requires_restart_validation");
        }
        entry.insert(QStringLiteral("stability"), stability);
        entry.insert(QStringLiteral("readable"), candidate.readable);
        if (candidate.readable) {
            entry.insert(QStringLiteral("current_value"),
                         FormatScanValue(candidate.current_value));
        }
        entry.insert(QStringLiteral("runtime_frozen"), active_freeze != active_freezes.end());
        if (active_freeze != active_freezes.end()) {
            entry.insert(
                QStringLiteral("freeze_value"),
                FormatScanValue({.type = candidate.candidate.last_value.type,
                                 .bits = active_freeze->value}));
        }
        candidate_entries.append(entry);
    }

    QJsonObject document_root;
    document_root.insert(QStringLiteral("schema_version"), 1);
    document_root.insert(QStringLiteral("title_id"), title_id);
    document_root.insert(QStringLiteral("build_id"), build_id);
    document_root.insert(QStringLiteral("runtime_session"), RuntimeScanSessionId());
    document_root.insert(QStringLiteral("auto_apply"), false);
    document_root.insert(
        QStringLiteral("warning"),
        QStringLiteral("Draft only. Absolute addresses are never applied after a process restart."));
    document_root.insert(QStringLiteral("updated_at"),
                         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    document_root.insert(QStringLiteral("candidates"), candidate_entries);
    Core::Memory::CheatProcessMetadata metadata{};
    if (system.GetCheatProcessMetadata(metadata)) {
        QJsonObject regions;
        regions.insert(QStringLiteral("main_base"),
                       QString::number(metadata.main_nso_extents.base, 16));
        regions.insert(QStringLiteral("main_size"),
                       QString::number(metadata.main_nso_extents.size, 16));
        regions.insert(QStringLiteral("heap_base"),
                       QString::number(metadata.heap_extents.base, 16));
        regions.insert(QStringLiteral("heap_size"),
                       QString::number(metadata.heap_extents.size, 16));
        regions.insert(QStringLiteral("alias_base"),
                       QString::number(metadata.alias_extents.base, 16));
        regions.insert(QStringLiteral("alias_size"),
                       QString::number(metadata.alias_extents.size, 16));
        regions.insert(QStringLiteral("aslr_base"),
                       QString::number(metadata.aslr_extents.base, 16));
        regions.insert(QStringLiteral("aslr_size"),
                       QString::number(metadata.aslr_extents.size, 16));
        document_root.insert(QStringLiteral("regions"), regions);
    }

    const QDir directory{session_directory};
    QSaveFile draft_file{directory.filePath(QStringLiteral("cheat_drafts.json"))};
    return draft_file.open(QIODevice::WriteOnly) &&
           draft_file.write(QJsonDocument{document_root}.toJson(QJsonDocument::Indented)) >= 0 &&
           draft_file.commit();
}

void CheatOverlayDialog::DeleteScanSession() {
    const QDir directory{session_directory};
    QFile::remove(directory.filePath(QStringLiteral("scan_session.json")));
    QFile::remove(directory.filePath(QStringLiteral("scan_candidates.bin")));
    QFile::remove(directory.filePath(QStringLiteral("scan_snapshot.json")));
    QFile::remove(snapshot_file_path);
}

void CheatOverlayDialog::ResetScanForm() {
    scan_mode_combo->setCurrentIndex(
        scan_mode_combo->findData(static_cast<int>(CheatScanComparison::Exact)));
    data_type_combo->setCurrentIndex(
        data_type_combo->findData(static_cast<int>(CheatScanDataType::Int32)));
    alignment_combo->setCurrentIndex(0);
    value_edit->clear();
    scan_candidates.clear();
    selected_candidates.clear();
    undo_scan_candidates.clear();
    undo_selected_candidates.clear();
    has_scan_session = false;
    has_unknown_snapshot = false;
    undo_restores_unknown_snapshot = false;
    filtering_existing_candidates = false;
    DeleteScanSession();
    result_count_label->setText(tr("Candidates: —"));
    scanner_status_label->setText(
        tr("Typed read-only scanning is ready. Automatic mode checks Int32, Int64, Float, and "
           "Double."));
    UpdateScanControls();
    known_value_button->setFocus();
}

void CheatOverlayDialog::StepValue(int direction) {
    bool valid{};
    const double current_value = value_edit->text().toDouble(&valid);
    value_edit->setText(QString::number((valid ? current_value : 0.0) + direction, 'g', 15));
}

void CheatOverlayDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }

    QWidget* focused = focusWidget();
    if (event->key() == Qt::Key_Up) {
        focusPreviousChild();
        return;
    }
    if (event->key() == Qt::Key_Down) {
        focusNextChild();
        return;
    }
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        const int direction = event->key() == Qt::Key_Right ? 1 : -1;
        if (auto* combo = qobject_cast<QComboBox*>(focused)) {
            combo->setCurrentIndex(
                std::clamp(combo->currentIndex() + direction, 0, combo->count() - 1));
            return;
        }
        if (focused == value_edit && value_edit->isEnabled()) {
            StepValue(direction);
            return;
        }
        direction > 0 ? focusNextChild() : focusPreviousChild();
        return;
    }
    if ((event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return) && focused != nullptr) {
        if (auto* button = qobject_cast<QPushButton*>(focused); button != nullptr &&
                                                           button->isEnabled()) {
            button->click();
            return;
        }
    }

    QDialog::keyPressEvent(event);
}

void CheatOverlayDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    UpdatePanelSize();
}

void CheatOverlayDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    UpdatePanelSize();
    known_value_button->setFocus();
}
