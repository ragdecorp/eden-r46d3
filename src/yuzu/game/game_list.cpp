// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include <QAbstractItemView>
#include <QApplication>
#include <QByteArray>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPersistentModelIndex>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSaveFile>
#include <QScroller>
#include <QScrollerProperties>
#include <QThreadPool>
#include <QToolButton>
#include <QUrl>
#include <QVariantAnimation>
#include <QtConcurrentRun>
#include <fmt/ranges.h>
#include <qnamespace.h>
#include "common/common_types.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/memory/cheat_engine.h"
#include "game/game_card.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/game.h"
#include "yuzu/compatibility_list.h"
#include "yuzu/game/cheat_availability_manager.h"
#include "yuzu/game/cheat_selection_dialog.h"
#include "yuzu/game/game_list.h"
#include "yuzu/game/game_metadata_manager.h"
#include "yuzu/game/game_update_manager.h"
#include "yuzu/game/game_list_p.h"
#include "yuzu/game/game_list_worker.h"
#include "yuzu/game/trailer_player_dialog.h"
#include "yuzu/main_window.h"
#include "yuzu/util/controller_navigation.h"
#include "web_service/youtube_trailer.h"
#include "web_service/cheatslips.h"

namespace {

constexpr int LibraryHistoryVersion = 1;

QStringList SplitMetadataValues(const QString& value) {
    return value.split(QRegularExpression{QStringLiteral("[,;]")}, Qt::SkipEmptyParts);
}

int PlayerSortValue(const QString& value) {
    static const QRegularExpression number_pattern{QStringLiteral("(\\d+)")};
    int maximum = 0;
    auto matches = number_pattern.globalMatch(value);
    while (matches.hasNext()) {
        maximum = std::max(maximum, matches.next().captured(1).toInt());
    }
    return maximum;
}

QString PathToQString(const std::filesystem::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

QString LibraryHistoryPath() {
    return PathToQString(Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) /
                         "game_library_history.json");
}

QString TitleIdToString(u64 title_id) {
    return QStringLiteral("%1").arg(title_id, 16, 16, QLatin1Char{'0'}).toUpper();
}

void SetHistoryDateItem(QStandardItem* item, const QDateTime& utc_date_time) {
    if (item == nullptr) {
        return;
    }
    if (!utc_date_time.isValid()) {
        item->setText(QString{});
        item->setData(QVariant::fromValue<qlonglong>(0), GameListItem::SortRole);
        item->setToolTip(QString{});
        return;
    }

    const QDateTime local_date_time = utc_date_time.toLocalTime();
    item->setText(local_date_time.toString(QStringLiteral("dd/MM/yyyy")));
    item->setData(QVariant::fromValue<qlonglong>(utc_date_time.toMSecsSinceEpoch()),
                  GameListItem::SortRole);
    item->setToolTip(local_date_time.toString(QStringLiteral("dd/MM/yyyy HH:mm:ss")));
}

void SetCreatedDateItem(QStandardItem* item, const QString& iso_date) {
    if (item == nullptr) {
        return;
    }
    const QDate date = QDate::fromString(iso_date.left(10), Qt::ISODate);
    if (!date.isValid()) {
        item->setText(QString{});
        item->setData(0, GameListItem::SortRole);
        item->setToolTip(QString{});
        return;
    }
    item->setText(date.toString(QStringLiteral("dd/MM/yyyy")));
    item->setData(date.toJulianDay(), GameListItem::SortRole);
    item->setToolTip(QObject::tr("Market release date: %1")
                         .arg(date.toString(QStringLiteral("dd/MM/yyyy"))));
}

void MoveColumnAfter(QHeaderView* header, int column, int preceding_column) {
    const int current_index = header->visualIndex(column);
    const int target_index = header->visualIndex(preceding_column) + 1;
    if (current_index >= 0 && target_index >= 0 && current_index != target_index) {
        header->moveSection(current_index, target_index);
    }
}

QString SafeCheatDirectoryName(const WebService::CheatCode& cheat) {
    QString name = QString::fromUtf8(cheat.name.data(), static_cast<qsizetype>(cheat.name.size()))
                       .trimmed();
    name.replace(QRegularExpression{QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])")},
                 QStringLiteral("_"));
    name = name.simplified();
    if (name.isEmpty()) {
        name = QStringLiteral("Cheat");
    }
    name = name.left(70).trimmed();
    return QStringLiteral("CheatSlips - %1 - %2-%3")
        .arg(name)
        .arg(cheat.submission_id)
        .arg(cheat.block_index);
}

bool InstallCheatCodes(u64 title_id, const QString& build_id,
                       const std::vector<WebService::CheatCode>& cheats,
                       const QVector<int>& selected_indices, QStringList& installed_packages,
                       QString& error_message) {
    const QString title_id_string =
        QStringLiteral("%1").arg(title_id, 16, 16, QLatin1Char{'0'}).toUpper();
    const QString load_root =
        PathToQString(Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir));
    const Core::Memory::TextCheatParser parser;

    for (const int selected_index : selected_indices) {
        if (selected_index < 0 || static_cast<std::size_t>(selected_index) >= cheats.size()) {
            error_message = GameList::tr("The selected cheat is invalid.");
            return false;
        }
        const auto& cheat = cheats[static_cast<std::size_t>(selected_index)];
        const auto parsed = parser.Parse(cheat.content);
        const bool has_opcodes = std::ranges::any_of(parsed, [](const auto& entry) {
            return entry.definition.num_opcodes > 0;
        });
        if (parsed.empty() || !has_opcodes) {
            error_message = GameList::tr("Cheat '%1' has an invalid code format.")
                                .arg(QString::fromUtf8(
                                    cheat.name.data(), static_cast<qsizetype>(cheat.name.size())));
            return false;
        }

        const QString package_name = SafeCheatDirectoryName(cheat);
        const QString cheats_directory =
            QDir{load_root}.filePath(title_id_string + QLatin1Char{'/'} + package_name +
                                     QStringLiteral("/cheats"));
        if (!QDir{}.mkpath(cheats_directory)) {
            error_message = GameList::tr("Could not create the cheat directory.");
            return false;
        }

        QSaveFile output{QDir{cheats_directory}.filePath(build_id.toUpper() +
                                                         QStringLiteral(".txt"))};
        const QByteArray content = QByteArray::fromStdString(cheat.content);
        if (!output.open(QIODevice::WriteOnly) || output.write(content) != content.size() ||
            !output.commit()) {
            error_message = GameList::tr("Could not save the selected cheat.");
            return false;
        }
        installed_packages.append(package_name);
    }
    return true;
}

} // Anonymous namespace

GameListSearchField::KeyReleaseEater::KeyReleaseEater(GameList* gamelist_, QObject* parent)
    : QObject(parent), gamelist{gamelist_} {}

// EventFilter in order to process systemkeys while editing the searchfield
bool GameListSearchField::KeyReleaseEater::eventFilter(QObject* obj, QEvent* event) {
    // If it isn't a KeyRelease event then continue with standard event processing
    if (event->type() != QEvent::KeyRelease)
        return QObject::eventFilter(obj, event);

    QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
    QString edit_filter_text = gamelist->search_field->edit_filter->text().toLower();

    // If the searchfield's text hasn't changed special function keys get checked
    // If no function key changes the searchfield's text the filter doesn't need to get reloaded
    if (edit_filter_text == edit_filter_text_old) {
        switch (keyEvent->key()) {
        // Escape: Resets the searchfield
        case Qt::Key_Escape: {
            if (edit_filter_text_old.isEmpty()) {
                return QObject::eventFilter(obj, event);
            } else {
                gamelist->search_field->edit_filter->clear();
                edit_filter_text.clear();
            }
            break;
        }
        // Return and Enter
        // If the enter key gets pressed first checks how many and which entry is visible
        // If there is only one result launch this game
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            if (gamelist->search_field->visible == 1) {
                const QString file_path = gamelist->GetLastFilterResultItem();

                // To avoid loading error dialog loops while confirming them using enter
                // Also users usually want to run a different game after closing one
                gamelist->search_field->edit_filter->clear();
                edit_filter_text.clear();
                emit gamelist->GameChosen(file_path);
            } else {
                return QObject::eventFilter(obj, event);
            }
            break;
        }
        default:
            return QObject::eventFilter(obj, event);
        }
    }
    edit_filter_text_old = edit_filter_text;
    return QObject::eventFilter(obj, event);
}

void GameListSearchField::setFilterResult(int visible_, int total_) {
    visible = visible_;
    total = total_;

    label_filter_result->setText(tr("%1 of %n result(s)", "", total).arg(visible));
}

QString GameListSearchField::filterText() const {
    return edit_filter->text();
}

QString GameList::GetLastFilterResultItem() const {
    QString file_path;

    for (int i = 1; i < item_model->rowCount() - 1; ++i) {
        const QStandardItem* folder = item_model->item(i, 0);
        const QModelIndex folder_index = folder->index();
        const int children_count = folder->rowCount();

        for (int j = 0; j < children_count; ++j) {
            if (tree_view->isRowHidden(j, folder_index)) {
                continue;
            }

            const QStandardItem* child = folder->child(j, 0);
            file_path = child->data(GameListItemPath::FullPathRole).toString();
        }
    }

    return file_path;
}

void GameListSearchField::clear() {
    edit_filter->clear();
}

void GameListSearchField::setFocus() {
    if (edit_filter->isVisible()) {
        edit_filter->setFocus();
    }
}

GameListSearchField::GameListSearchField(GameList* parent) : QWidget{parent} {
    auto* const key_release_eater = new KeyReleaseEater(parent, this);
    layout_filter = new QHBoxLayout;
    layout_filter->setContentsMargins(8, 8, 8, 8);
    label_filter = new QLabel;
    edit_filter = new QLineEdit;
    edit_filter->clear();
    edit_filter->installEventFilter(key_release_eater);
    edit_filter->setClearButtonEnabled(true);
    connect(edit_filter, &QLineEdit::textChanged, parent, &GameList::OnTextChanged);
    label_filter_result = new QLabel;
    button_filter_close = new QToolButton(this);
    button_filter_close->setText(QStringLiteral("X"));
    button_filter_close->setCursor(Qt::ArrowCursor);
    button_filter_close->setStyleSheet(
        QStringLiteral("QToolButton{ border: none; padding: 0px; color: "
                       "#000000; font-weight: bold; background: #F0F0F0; }"
                       "QToolButton:hover{ border: none; padding: 0px; color: "
                       "#EEEEEE; font-weight: bold; background: #E81123}"));
    connect(button_filter_close, &QToolButton::clicked, parent, &GameList::OnFilterCloseClicked);
    layout_filter->setSpacing(10);
    layout_filter->addWidget(label_filter);
    layout_filter->addWidget(edit_filter);
    layout_filter->addWidget(label_filter_result);
    layout_filter->addWidget(button_filter_close);
    setLayout(layout_filter);
    RetranslateUI();
}

/**
 * Checks if all words separated by spaces are contained in another string
 * This offers a word order insensitive search function
 *
 * @param haystack String that gets checked if it contains all words of the userinput string
 * @param userinput String containing all words getting checked
 * @return true if the haystack contains all words of userinput
 */
static bool ContainsAllWords(const QString& haystack, const QString& userinput) {
    const QStringList userinput_split = userinput.split(QLatin1Char{' '}, Qt::SkipEmptyParts);

    return std::all_of(userinput_split.begin(), userinput_split.end(),
                       [&haystack](const QString& s) { return haystack.contains(s); });
}

// Syncs the expanded state of Game Directories with settings to persist across sessions
void GameList::OnItemExpanded(const QModelIndex& item) {
    const auto type = item.data(GameListItem::TypeRole).value<GameListItemType>();
    const bool is_dir = type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
                        type == GameListItemType::UserNandDir ||
                        type == GameListItemType::SysNandDir;
    const bool is_fave = type == GameListItemType::Favorites;
    if (!is_dir && !is_fave) {
        return;
    }
    const bool is_expanded = tree_view->isExpanded(item);
    if (is_fave) {
        UISettings::values.favorites_expanded = is_expanded;
        return;
    }
    const int item_dir_index = item.data(GameListDir::GameDirRole).toInt();
    UISettings::values.game_dirs[item_dir_index].expanded = is_expanded;
}

// Event in order to filter the gamelist after editing the searchfield
void GameList::OnTextChanged(const QString& new_text) {
    QString edit_filter_text = new_text.toLower();
    QStandardItem* folder;
    int children_total = 0;
    int result_count = 0;

    auto hide = [this](int row, bool hidden, QModelIndex index = QModelIndex()) {
        if (m_isTreeMode) {
            tree_view->setRowHidden(row, index, hidden);
        } else {
            list_view->setRowHidden(row, hidden);
        }
    };

    // If the searchfield is empty every item is visible
    // Otherwise the filter gets applied

    // TODO(crueter) dedupe
    if (!m_isTreeMode) {
        int row_count = item_model->rowCount();

        for (int i = 0; i < row_count; ++i) {
            QStandardItem* item = item_model->item(i, 0);
            if (!item)
                continue;

            children_total++;

            const QString file_path =
                item->data(GameListItemPath::FullPathRole).toString().toLower();
            const QString file_title = item->data(GameListItemPath::TitleRole).toString().toLower();
            const QString file_name =
                file_path.mid(file_path.lastIndexOf(QLatin1Char{'/'}) + 1) + QLatin1Char{' '} +
                file_title + QLatin1Char{' '} +
                item_model->item(i, COLUMN_GENRE)->text().toLower() + QLatin1Char{' '} +
                item_model->item(i, COLUMN_TAGS)->text().toLower() + QLatin1Char{' '} +
                item_model->item(i, COLUMN_BUILD_ID)->text().toLower() + QLatin1Char{' '} +
                item_model->item(i, COLUMN_CHEATS)->text().toLower() + QLatin1Char{' '} +
                item_model->item(i, COLUMN_UPDATE_STATUS)->text().toLower() + QLatin1Char{' '} +
                item_model->item(i, COLUMN_LAST_PLAYED)->text().toLower() + QLatin1Char{' '} +
                item_model->item(i, COLUMN_DATE_ADDED)->text().toLower() + QLatin1Char{' '} +
                item_model->item(i, COLUMN_CREATED)->text().toLower();

            if (edit_filter_text.isEmpty() || ContainsAllWords(file_name, edit_filter_text)) {
                hide(i, false);
                result_count++;
            } else {
                hide(i, true);
            }
        }
        search_field->setFilterResult(result_count, children_total);
    } else if (edit_filter_text.isEmpty()) {
        hide(0, UISettings::values.favorited_ids.size() == 0,
             item_model->invisibleRootItem()->index());
        for (int i = 1; i < item_model->rowCount() - 1; ++i) {
            folder = item_model->item(i, 0);
            const QModelIndex folder_index = folder->index();
            const int children_count = folder->rowCount();
            for (int j = 0; j < children_count; ++j) {
                ++children_total;
                hide(j, false, folder_index);
            }
        }
        search_field->setFilterResult(children_total, children_total);
    } else {
        hide(0, true, item_model->invisibleRootItem()->index());
        for (int i = 1; i < item_model->rowCount() - 1; ++i) {
            folder = item_model->item(i, 0);
            const QModelIndex folder_index = folder->index();
            const int children_count = folder->rowCount();
            for (int j = 0; j < children_count; ++j) {
                ++children_total;

                const QStandardItem* child = folder->child(j, 0);

                const auto program_id = child->data(GameListItemPath::ProgramIdRole).toULongLong();

                const QString file_path =
                    child->data(GameListItemPath::FullPathRole).toString().toLower();
                const QString file_title =
                    child->data(GameListItemPath::TitleRole).toString().toLower();
                const QString file_program_id =
                    QStringLiteral("%1").arg(program_id, 16, 16, QLatin1Char{'0'});

                // Only items which filename in combination with its title contains all words
                // that are in the searchfield will be visible in the gamelist
                // The search is case insensitive because of toLower()
                // I decided not to use Qt::CaseInsensitive in containsAllWords to prevent
                // multiple conversions of edit_filter_text for each game in the gamelist
                const QString file_name =
                    file_path.mid(file_path.lastIndexOf(QLatin1Char{'/'}) + 1) + QLatin1Char{' '} +
                    file_title + QLatin1Char{' '} +
                    folder->child(j, COLUMN_GENRE)->text().toLower() + QLatin1Char{' '} +
                    folder->child(j, COLUMN_TAGS)->text().toLower() + QLatin1Char{' '} +
                    folder->child(j, COLUMN_BUILD_ID)->text().toLower() + QLatin1Char{' '} +
                    folder->child(j, COLUMN_CHEATS)->text().toLower() + QLatin1Char{' '} +
                    folder->child(j, COLUMN_UPDATE_STATUS)->text().toLower() + QLatin1Char{' '} +
                    folder->child(j, COLUMN_LAST_PLAYED)->text().toLower() + QLatin1Char{' '} +
                    folder->child(j, COLUMN_DATE_ADDED)->text().toLower() + QLatin1Char{' '} +
                    folder->child(j, COLUMN_CREATED)->text().toLower();
                if (ContainsAllWords(file_name, edit_filter_text) ||
                    (file_program_id.size() == 16 && file_program_id.contains(edit_filter_text))) {
                    hide(j, false, folder_index);
                    ++result_count;
                } else {
                    hide(j, true, folder_index);
                }
            }
        }
        search_field->setFilterResult(result_count, children_total);
    }
}

void GameList::OnUpdateThemedIcons() {
    for (int i = 0; i < item_model->invisibleRootItem()->rowCount(); i++) {
        QStandardItem* child = item_model->invisibleRootItem()->child(i);

        const int icon_size = UISettings::values.folder_icon_size.GetValue();

        switch (child->data(GameListItem::TypeRole).value<GameListItemType>()) {
        case GameListItemType::SdmcDir:
            child->setData(
                QIcon::fromTheme(QStringLiteral("sd_card"))
                    .pixmap(icon_size)
                    .scaled(icon_size, icon_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                Qt::DecorationRole);
            break;
        case GameListItemType::UserNandDir:
            child->setData(
                QIcon::fromTheme(QStringLiteral("chip"))
                    .pixmap(icon_size)
                    .scaled(icon_size, icon_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                Qt::DecorationRole);
            break;
        case GameListItemType::SysNandDir:
            child->setData(
                QIcon::fromTheme(QStringLiteral("chip"))
                    .pixmap(icon_size)
                    .scaled(icon_size, icon_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                Qt::DecorationRole);
            break;
        case GameListItemType::CustomDir: {
            const UISettings::GameDir& game_dir =
                UISettings::values.game_dirs[child->data(GameListDir::GameDirRole).toInt()];
            const QString icon_name = QFileInfo::exists(QString::fromStdString(game_dir.path))
                                          ? QStringLiteral("folder")
                                          : QStringLiteral("bad_folder");
            child->setData(
                QIcon::fromTheme(icon_name).pixmap(icon_size).scaled(
                    icon_size, icon_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                Qt::DecorationRole);
            break;
        }
        case GameListItemType::AddDir:
            child->setData(
                QIcon::fromTheme(QStringLiteral("list-add"))
                    .pixmap(icon_size)
                    .scaled(icon_size, icon_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                Qt::DecorationRole);
            break;
        case GameListItemType::Favorites:
            child->setData(
                QIcon::fromTheme(QStringLiteral("star"))
                    .pixmap(icon_size)
                    .scaled(icon_size, icon_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                Qt::DecorationRole);
            break;
        default:
            break;
        }
    }
}

void GameList::OnFilterCloseClicked() {
    main_window->filterBarSetChecked(false);
}

GameList::GameList(FileSys::VirtualFilesystem vfs_, FileSys::ManualContentProvider* provider_,
                   PlayTime::PlayTimeManager& play_time_manager_, Core::System& system_,
                   MainWindow* parent)
    : QWidget{parent}, vfs{std::move(vfs_)}, provider{provider_},
      play_time_manager{play_time_manager_}, system{system_} {
    watcher = new QFileSystemWatcher(this);
    connect(watcher, &QFileSystemWatcher::directoryChanged, this, &GameList::RefreshGameDirectory);

    external_watcher = new QFileSystemWatcher(this);
    ResetExternalWatcher();
    connect(external_watcher, &QFileSystemWatcher::directoryChanged, this,
            &GameList::RefreshExternalContent);

    this->main_window = parent;
    layout = new QVBoxLayout;
    tree_view = new QTreeView(this);
    list_view = new QListView(this);
    m_gameCard = new GameCard(this);

    list_view->setItemDelegate(m_gameCard);

    controller_navigation = new ControllerNavigation(system.HIDCore(), this);
    controller_navigation->MapButton(Settings::NativeButton::R, Qt::Key_R);
    search_field = new GameListSearchField(this);
    item_model = new QStandardItemModel(tree_view);
    cheat_availability_manager = new CheatAvailabilityManager(this);
    metadata_manager = new GameMetadataManager(this);
    game_update_manager = new GameUpdateManager(this);
    LoadLibraryHistory();
    tree_view->setModel(item_model);
    list_view->setModel(item_model);

    SetupScrollAnimation();

    // tree
    tree_view->setAlternatingRowColors(true);
    tree_view->setSelectionMode(QHeaderView::SingleSelection);
    tree_view->setSelectionBehavior(QHeaderView::SelectRows);
    tree_view->setVerticalScrollMode(QHeaderView::ScrollPerPixel);
    tree_view->setHorizontalScrollMode(QHeaderView::ScrollPerPixel);
    tree_view->setSortingEnabled(true);
    tree_view->setEditTriggers(QHeaderView::NoEditTriggers);
    tree_view->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_view->setAttribute(Qt::WA_AcceptTouchEvents, true);
    tree_view->setStyleSheet(QStringLiteral("QTreeView{ border: none; }"));

    // list view setup
    list_view->setViewMode(QListView::ListMode);
    list_view->setResizeMode(QListView::Fixed);
    list_view->setUniformItemSizes(true);
    list_view->setSelectionMode(QAbstractItemView::SingleSelection);
    list_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    // Forcefully disable scroll bar, prevents thing where game list items
    // will start clamping prematurely.
    list_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    list_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_view->setContextMenuPolicy(Qt::CustomContextMenu);
    list_view->setGridSize(QSize(140, 160));
    m_gameCard->setSize(list_view->gridSize(), 0, 4);

    list_view->setSpacing(10);
    list_view->setWordWrap(true);
    list_view->setTextElideMode(Qt::ElideRight);
    list_view->setFlow(QListView::LeftToRight);
    list_view->setWrapping(true);

    item_model->insertColumns(0, COLUMN_COUNT);
    tree_view->header()->moveSection(tree_view->header()->visualIndex(COLUMN_TRAILER), 1);
    tree_view->header()->moveSection(tree_view->header()->visualIndex(COLUMN_PLAYERS), 2);
    tree_view->header()->moveSection(tree_view->header()->visualIndex(COLUMN_GENRE), 3);
    tree_view->header()->moveSection(tree_view->header()->visualIndex(COLUMN_TAGS), 4);
    tree_view->header()->moveSection(tree_view->header()->visualIndex(COLUMN_BUILD_ID), 5);
    tree_view->header()->moveSection(tree_view->header()->visualIndex(COLUMN_CHEATS), 6);
    MoveColumnAfter(tree_view->header(), COLUMN_LAST_PLAYED, COLUMN_PLAY_TIME);
    MoveColumnAfter(tree_view->header(), COLUMN_DATE_ADDED, COLUMN_LAST_PLAYED);
    MoveColumnAfter(tree_view->header(), COLUMN_CREATED, COLUMN_DATE_ADDED);
    tree_view->header()->moveSection(
        tree_view->header()->visualIndex(COLUMN_UPDATE_STATUS),
        tree_view->header()->visualIndex(COLUMN_ADD_ONS) + 1);
    RetranslateUI();

    tree_view->setColumnHidden(COLUMN_ADD_ONS, !UISettings::values.show_add_ons);
    tree_view->setColumnHidden(COLUMN_COMPATIBILITY, !UISettings::values.show_compat);
    tree_view->setColumnHidden(COLUMN_PLAY_TIME, !UISettings::values.show_play_time);
    item_model->setSortRole(GameListItemPath::SortRole);

    connect(main_window, &MainWindow::UpdateThemedIcons, this, &GameList::OnUpdateThemedIcons);

    connect(tree_view, &QTreeView::clicked, this, &GameList::OnItemClicked);
    connect(tree_view, &QTreeView::doubleClicked, this, &GameList::OnItemDoubleClicked);
    connect(tree_view, &QTreeView::activated, this, &GameList::ValidateEntry);
    connect(tree_view, &QTreeView::customContextMenuRequested, this, &GameList::PopupContextMenu);

    connect(list_view, &QListView::activated, this, &GameList::ValidateEntry);
    connect(list_view, &QListView::customContextMenuRequested, this, &GameList::PopupContextMenu);

    connect(item_model, &QStandardItemModel::itemChanged, this,
            &GameList::OnMetadataItemChanged);
    connect(metadata_manager, &GameMetadataManager::MetadataChanged, this,
            &GameList::UpdateMetadataRows);
    connect(cheat_availability_manager, &CheatAvailabilityManager::AvailabilityChanged, this,
            &GameList::UpdateCheatRows);
    connect(game_update_manager, &GameUpdateManager::VersionsChanged, this,
            [this] { UpdateGameUpdateRows(); });
    connect(game_update_manager, &GameUpdateManager::FriendlyVersionChanged, this,
            &GameList::UpdateGameUpdateRows);

    connect(tree_view, &QTreeView::expanded, this, &GameList::OnItemExpanded);
    connect(tree_view, &QTreeView::collapsed, this, &GameList::OnItemExpanded);
    connect(controller_navigation, &ControllerNavigation::TriggerKeyboardEvent, this,
            [this](Qt::Key key) {
                // Avoid pressing buttons while playing
                if (system.IsPoweredOn()) {
                    return;
                }
                if (!this->isActiveWindow()) {
                    return;
                }
                if (key == Qt::Key_R) {
                    const QModelIndex selected = m_currentView->currentIndex();
                    if (selected.isValid()) {
                        OpenTrailerForItem(selected);
                    }
                    return;
                }
                QKeyEvent* event = new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier);

                QCoreApplication::postEvent(m_currentView, event);
            });

    // We must register all custom types with the Qt Automoc system so that we are able to use
    // it with signals/slots. In this case, QList falls under the umbrella of custom types.
    qRegisterMetaType<QList<QStandardItem*>>("QList<QStandardItem*>");

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addWidget(tree_view);
    layout->addWidget(list_view);
    layout->addWidget(search_field);
    setLayout(layout);

    ResetViewMode();
}

void GameList::UnloadController() {
    controller_navigation->UnloadController();
}

bool GameList::IsTreeMode() {
    return m_isTreeMode;
}

void GameList::ResetViewMode() {
    auto& setting = UISettings::values.game_list_mode;
    bool newTreeMode = false;

    switch (setting.GetValue()) {
    case Settings::GameListMode::TreeView:
        m_currentView = tree_view;
        newTreeMode = true;

        tree_view->setVisible(true);
        list_view->setVisible(false);
        break;
    case Settings::GameListMode::GridView:
        m_currentView = list_view;
        newTreeMode = false;

        list_view->setVisible(true);
        tree_view->setVisible(false);
        break;
    default:
        break;
    }

    auto view = m_currentView->viewport();
    view->installEventFilter(this);

    // touch gestures
    view->grabGesture(Qt::SwipeGesture);
    view->grabGesture(Qt::PanGesture);

    // TODO: touch?
    QScroller::grabGesture(view, QScroller::LeftMouseButtonGesture);

    auto scroller = QScroller::scroller(view);
    QScrollerProperties props;
    props.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy,
                          QScrollerProperties::OvershootAlwaysOff);
    props.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy,
                          QScrollerProperties::OvershootAlwaysOff);
    scroller->setScrollerProperties(props);

    if (m_isTreeMode != newTreeMode) {
        m_isTreeMode = newTreeMode;

        RefreshGameDirectory();
    }
}

GameList::~GameList() {
    if (history_dirty) {
        SaveLibraryHistory();
    }
    UnloadController();
}

void GameList::SetFilterFocus() {
    if (tree_view->model()->rowCount() > 0) {
        search_field->setFocus();
    }
}

void GameList::SetFilterVisible(bool visibility) {
    search_field->setVisible(visibility);
}

void GameList::ClearFilter() {
    search_field->clear();
}

void GameList::WorkerEvent() {
    current_worker->ProcessEvents(this);
}

void GameList::AddDirEntry(GameListDir* entry_items) {
    if (m_isTreeMode) {
        item_model->invisibleRootItem()->appendRow(entry_items);
        tree_view->setExpanded(
            entry_items->index(),
            UISettings::values.game_dirs[entry_items->data(GameListDir::GameDirRole).toInt()]
                .expanded);
    }
}

void GameList::AddEntry(const QList<QStandardItem*>& entry_items, GameListDir* parent) {
    if (!m_isTreeMode) {
        item_model->invisibleRootItem()->appendRow(entry_items);
    } else {
        parent->appendRow(entry_items);
    }

    const auto* name_item = entry_items.value(COLUMN_NAME);
    if (name_item == nullptr) {
        return;
    }
    const u64 title_id = name_item->data(GameListItemPath::ProgramIdRole).toULongLong();
    const QString build_id = name_item->data(GameListItemPath::BuildIdRole).toString();
    PopulateHistoryItems(entry_items, title_id);
    game_update_manager->RequestRefresh(
        title_id, name_item->data(GameListItemPath::TitleRole).toString(),
        static_cast<u32>(
            name_item->data(GameListItemPath::InstalledUpdateVersionRole).toUInt()),
        name_item->data(GameListItemPath::InstalledUpdateComparableRole).toBool());
    metadata_manager->RequestMetadata(title_id);
    UpdateMetadataRows(title_id);
    UpdateCheatRows(title_id, build_id);
    UpdateGameUpdateRows(title_id);
}

void GameList::LoadLibraryHistory() {
    QFile file{LibraryHistoryPath()};
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        LOG_WARNING(Frontend, "Ignoring invalid game library history");
        return;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != LibraryHistoryVersion) {
        LOG_WARNING(Frontend, "Ignoring unsupported game library history version");
        return;
    }

    history_baseline_pending =
        !root.value(QStringLiteral("baseline_initialized")).toBool(false);
    for (const QJsonValue& value : root.value(QStringLiteral("known_titles")).toArray()) {
        bool valid = false;
        const u64 title_id = value.toString().toULongLong(&valid, 16);
        if (valid && title_id != 0) {
            known_library_titles.insert(title_id);
        }
    }

    const QJsonObject games = root.value(QStringLiteral("games")).toObject();
    for (auto it = games.begin(); it != games.end(); ++it) {
        bool valid = false;
        const u64 title_id = it.key().toULongLong(&valid, 16);
        if (!valid || title_id == 0 || !it.value().isObject()) {
            continue;
        }
        const QJsonObject game = it.value().toObject();
        game_history.insert(
            title_id,
            GameHistoryEntry{
                .added_at = QDateTime::fromString(
                    game.value(QStringLiteral("added_at")).toString(), Qt::ISODateWithMs),
                .last_played_at = QDateTime::fromString(
                    game.value(QStringLiteral("last_played_at")).toString(),
                    Qt::ISODateWithMs),
            });
        known_library_titles.insert(title_id);
    }
}

void GameList::SaveLibraryHistory() {
    QList<u64> known_titles = known_library_titles.values();
    std::ranges::sort(known_titles);

    QJsonArray known_titles_json;
    for (const u64 title_id : known_titles) {
        known_titles_json.append(TitleIdToString(title_id));
    }

    QJsonObject games;
    for (auto it = game_history.cbegin(); it != game_history.cend(); ++it) {
        QJsonObject game;
        if (it->added_at.isValid()) {
            game.insert(QStringLiteral("added_at"),
                        it->added_at.toUTC().toString(Qt::ISODateWithMs));
        }
        if (it->last_played_at.isValid()) {
            game.insert(QStringLiteral("last_played_at"),
                        it->last_played_at.toUTC().toString(Qt::ISODateWithMs));
        }
        if (!game.isEmpty()) {
            games.insert(TitleIdToString(it.key()), game);
        }
    }

    const QString path = LibraryHistoryPath();
    QDir{}.mkpath(QFileInfo{path}.absolutePath());
    QSaveFile file{path};
    const QJsonObject root{
        {QStringLiteral("version"), LibraryHistoryVersion},
        {QStringLiteral("baseline_initialized"), !history_baseline_pending},
        {QStringLiteral("known_titles"), known_titles_json},
        {QStringLiteral("games"), games},
    };
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        LOG_WARNING(Frontend, "Failed to save game library history");
        return;
    }
    history_dirty = false;
}

void GameList::PopulateHistoryItems(const QList<QStandardItem*>& entry_items, u64 title_id) {
    if (title_id == 0) {
        return;
    }

    observed_library_titles.insert(title_id);
    if (!known_library_titles.contains(title_id)) {
        known_library_titles.insert(title_id);
        if (!history_baseline_pending) {
            game_history[title_id].added_at = QDateTime::currentDateTimeUtc();
        }
        history_dirty = true;
    }

    const GameHistoryEntry history = game_history.value(title_id);
    SetHistoryDateItem(entry_items.value(COLUMN_LAST_PLAYED), history.last_played_at);
    SetHistoryDateItem(entry_items.value(COLUMN_DATE_ADDED), history.added_at);
}

void GameList::UpdateHistoryRows(u64 title_id) {
    const GameHistoryEntry history = game_history.value(title_id);
    const auto update_children = [&](const auto& self, QStandardItem* parent) -> void {
        for (int row = 0; row < parent->rowCount(); ++row) {
            QStandardItem* const name_item = parent->child(row, COLUMN_NAME);
            if (name_item == nullptr) {
                continue;
            }
            if (name_item->data(GameListItem::TypeRole).value<GameListItemType>() ==
                    GameListItemType::Game &&
                name_item->data(GameListItemPath::ProgramIdRole).toULongLong() == title_id) {
                SetHistoryDateItem(parent->child(row, COLUMN_LAST_PLAYED),
                                   history.last_played_at);
                SetHistoryDateItem(parent->child(row, COLUMN_DATE_ADDED), history.added_at);
            }
            if (name_item->hasChildren()) {
                self(self, name_item);
            }
        }
    };
    update_children(update_children, item_model->invisibleRootItem());

    const int sort_column = tree_view->header()->sortIndicatorSection();
    if (sort_column == COLUMN_LAST_PLAYED || sort_column == COLUMN_DATE_ADDED) {
        item_model->sort(sort_column, tree_view->header()->sortIndicatorOrder());
    }
}

void GameList::RecordGameStarted(u64 title_id) {
    if (title_id == 0) {
        return;
    }

    const bool is_new_title = !known_library_titles.contains(title_id);
    known_library_titles.insert(title_id);
    observed_library_titles.insert(title_id);
    GameHistoryEntry& history = game_history[title_id];
    if (is_new_title && !history_baseline_pending) {
        history.added_at = QDateTime::currentDateTimeUtc();
    }
    history.last_played_at = QDateTime::currentDateTimeUtc();
    history_dirty = true;
    SaveLibraryHistory();
    UpdateHistoryRows(title_id);
}

void GameList::OnItemClicked(const QModelIndex& item) {
    if (!item.isValid()) {
        return;
    }
    if (item.column() == COLUMN_TRAILER) {
        OpenTrailerForItem(item);
    } else if (item.column() == COLUMN_CHEATS) {
        OpenCheatsForItem(item);
    } else if (item.column() == COLUMN_UPDATE_STATUS) {
        const QModelIndex selected = item.sibling(item.row(), COLUMN_NAME);
        if (selected.data(GameListItem::TypeRole).value<GameListItemType>() ==
            GameListItemType::Game) {
            game_update_manager->RequestRefresh(
                selected.data(GameListItemPath::ProgramIdRole).toULongLong(),
                selected.data(GameListItemPath::TitleRole).toString(),
                static_cast<u32>(
                    selected.data(GameListItemPath::InstalledUpdateVersionRole).toUInt()),
                selected.data(GameListItemPath::InstalledUpdateComparableRole).toBool());
        }
    } else if (item.column() == COLUMN_TAGS && m_isTreeMode) {
        tree_view->edit(item);
    }
}

void GameList::OpenCheatsForItem(const QModelIndex& item) {
    if (!item.isValid() || cheat_download_in_progress) {
        return;
    }

    const QModelIndex selected = item.sibling(item.row(), COLUMN_NAME);
    if (selected.data(GameListItem::TypeRole).value<GameListItemType>() !=
        GameListItemType::Game) {
        return;
    }
    const u64 title_id = selected.data(GameListItemPath::ProgramIdRole).toULongLong();
    const QString build_id =
        selected.data(GameListItemPath::BuildIdRole).toString().trimmed().toUpper();
    const auto state = cheat_availability_manager->GetState(title_id, build_id);
    if (build_id.isEmpty()) {
        QMessageBox::warning(this, tr("Cheats"),
                             tr("The Build ID for this game could not be determined."));
        return;
    }
    if (state == CheatAvailabilityManager::State::Unknown ||
        state == CheatAvailabilityManager::State::NetworkError) {
        cheat_availability_manager->RequestAvailability(title_id, build_id);
        UpdateCheatRows(title_id, build_id);
        return;
    }
    if (state == CheatAvailabilityManager::State::Pending) {
        QMessageBox::information(this, tr("Cheats"),
                                 tr("Eden is still checking cheats for this game."));
        return;
    }
    if (state != CheatAvailabilityManager::State::Available) {
        QMessageBox::information(
            this, tr("Cheats"),
            tr("No cheats were found for this exact Title ID and Build ID."));
        return;
    }

    QString game_name = selected.data(GameListItemPath::TitleRole).toString().trimmed();
    if (game_name.isEmpty()) {
        game_name = QFileInfo{selected.data(GameListItemPath::FullPathRole).toString()}
                        .completeBaseName();
    }

    cheat_download_in_progress = true;
    const QPersistentModelIndex cheats_item{item.sibling(item.row(), COLUMN_CHEATS)};
    item_model->setData(cheats_item, tr("Downloading..."));
    const auto credential_directory =
        Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "cheats";
    auto* cheat_watcher = new QFutureWatcher<WebService::CheatCatalogResult>{this};
    connect(cheat_watcher, &QFutureWatcher<WebService::CheatCatalogResult>::finished, this,
            [this, cheat_watcher, title_id, build_id, game_name] {
                const WebService::CheatCatalogResult result = cheat_watcher->result();
                cheat_watcher->deleteLater();
                cheat_download_in_progress = false;
                UpdateCheatRows(title_id, build_id);

                if (result.code != WebService::CheatCatalogResultCode::Success) {
                    QString message;
                    switch (result.code) {
                    case WebService::CheatCatalogResultCode::CredentialsMissing:
                        message = tr("The portable CheatSlips token file was not found.");
                        break;
                    case WebService::CheatCatalogResultCode::InvalidCredentials:
                        message = tr("The CheatSlips API token is invalid.");
                        break;
                    case WebService::CheatCatalogResultCode::QuotaExceeded:
                        message = tr("The CheatSlips download quota has been reached for today.");
                        break;
                    case WebService::CheatCatalogResultCode::NotFound:
                        message = tr("No cheats were found for this exact Title ID and Build ID.");
                        break;
                    case WebService::CheatCatalogResultCode::NetworkError:
                        message = tr("Could not connect to CheatSlips.");
                        break;
                    case WebService::CheatCatalogResultCode::InvalidResponse:
                        message = tr("CheatSlips returned an invalid response.");
                        break;
                    case WebService::CheatCatalogResultCode::Success:
                        break;
                    }
                    QMessageBox::warning(this, tr("Cheats"), message);
                    return;
                }

                CheatSelectionDialog dialog{game_name, title_id, build_id, result.cheats, this};
                if (dialog.exec() != QDialog::Accepted) {
                    return;
                }

                QStringList installed_packages;
                QString error_message;
                if (!InstallCheatCodes(title_id, build_id, result.cheats,
                                       dialog.SelectedIndices(), installed_packages,
                                       error_message)) {
                    QMessageBox::critical(this, tr("Cheats"), error_message);
                    return;
                }

                auto& disabled_addons = Settings::values.disabled_addons[title_id];
                for (const QString& package : installed_packages) {
                    std::erase(disabled_addons, package.toStdString());
                }
                Common::FS::RemoveFile(
                    Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "game_list" /
                    fmt::format("{:016X}.pv.txt", title_id));
                emit SaveConfig();

                QMessageBox::information(
                    this, tr("Cheats"),
                    tr("Installed %n cheat(s) for Build ID %1.", nullptr,
                       installed_packages.size())
                        .arg(build_id));
            });
    cheat_watcher->setFuture(QtConcurrent::run(
        [credential_directory, title_id, build_id_string = build_id.toStdString()] {
            return WebService::FetchCheatCatalog(credential_directory, title_id,
                                                 build_id_string);
        }));
}

void GameList::OnItemDoubleClicked(const QModelIndex& item) {
    if (!item.isValid() || !m_isTreeMode) {
        return;
    }
    if (item.column() == COLUMN_PLAYERS || item.column() == COLUMN_GENRE) {
        tree_view->edit(item);
    }
}

void GameList::OnMetadataItemChanged(QStandardItem* item) {
    if (metadata_update_in_progress || item == nullptr ||
        (item->column() != COLUMN_PLAYERS && item->column() != COLUMN_GENRE &&
         item->column() != COLUMN_TAGS)) {
        return;
    }

    const QModelIndex name_index = item->index().siblingAtColumn(COLUMN_NAME);
    if (name_index.data(GameListItem::TypeRole).value<GameListItemType>() !=
        GameListItemType::Game) {
        return;
    }
    const u64 title_id = name_index.data(GameListItemPath::ProgramIdRole).toULongLong();
    switch (item->column()) {
    case COLUMN_PLAYERS:
        metadata_manager->SetPlayersOverride(title_id, item->text());
        break;
    case COLUMN_GENRE:
        metadata_manager->SetGenresOverride(title_id, SplitMetadataValues(item->text()));
        break;
    case COLUMN_TAGS:
        metadata_manager->SetTags(title_id, SplitMetadataValues(item->text()));
        break;
    default:
        break;
    }
}

void GameList::UpdateMetadataRows(u64 title_id) {
    const QString players = metadata_manager->Players(title_id);
    const QString genres = metadata_manager->Genres(title_id).join(QStringLiteral(", "));
    const QString tags = metadata_manager->Tags(title_id).join(QStringLiteral(", "));
    const QString release_date = metadata_manager->ReleaseDate(title_id);
    const bool pending = metadata_manager->IsPending(title_id);
    const bool has_metadata = metadata_manager->HasMetadata(title_id);
    const QString unavailable = pending ? QStringLiteral("\u2026") : QStringLiteral("?");

    metadata_update_in_progress = true;
    const auto update_children = [&](const auto& self, QStandardItem* parent) -> void {
        for (int row = 0; row < parent->rowCount(); ++row) {
            QStandardItem* const name_item = parent->child(row, COLUMN_NAME);
            if (name_item == nullptr) {
                continue;
            }
            if (name_item->data(GameListItem::TypeRole).value<GameListItemType>() ==
                    GameListItemType::Game &&
                name_item->data(GameListItemPath::ProgramIdRole).toULongLong() == title_id) {
                QStandardItem* const players_item = parent->child(row, COLUMN_PLAYERS);
                QStandardItem* const genre_item = parent->child(row, COLUMN_GENRE);
                QStandardItem* const tags_item = parent->child(row, COLUMN_TAGS);
                QStandardItem* const created_item = parent->child(row, COLUMN_CREATED);
                if (players_item != nullptr) {
                    const QString display = players.isEmpty() && !has_metadata ? unavailable : players;
                    players_item->setText(display);
                    players_item->setData(PlayerSortValue(players), GameListItem::SortRole);
                }
                if (genre_item != nullptr) {
                    const QString display = genres.isEmpty() && !has_metadata ? unavailable : genres;
                    genre_item->setText(display);
                    genre_item->setData(genres.toLower(), GameListItem::SortRole);
                    genre_item->setToolTip(genres);
                }
                if (tags_item != nullptr) {
                    tags_item->setText(tags);
                    tags_item->setData(tags.toLower(), GameListItem::SortRole);
                    tags_item->setToolTip(tags);
                }
                SetCreatedDateItem(created_item, release_date);
            }
            if (name_item->hasChildren()) {
                self(self, name_item);
            }
        }
    };
    update_children(update_children, item_model->invisibleRootItem());
    metadata_update_in_progress = false;

    const int sort_column = tree_view->header()->sortIndicatorSection();
    if (sort_column == COLUMN_PLAYERS || sort_column == COLUMN_GENRE ||
        sort_column == COLUMN_TAGS || sort_column == COLUMN_CREATED) {
        item_model->sort(sort_column, tree_view->header()->sortIndicatorOrder());
    }
    if (!search_field->filterText().isEmpty()) {
        OnTextChanged(search_field->filterText());
    }
}

void GameList::UpdateCheatRows(u64 title_id, const QString& build_id) {
    const QString normalized_build_id = build_id.trimmed().toUpper();
    const auto state = cheat_availability_manager->GetState(title_id, normalized_build_id);
    const int cheat_count = cheat_availability_manager->CheatCount(title_id, normalized_build_id);

    QString display;
    int sort_value = -1;
    if (normalized_build_id.isEmpty()) {
        display = tr("Build ID required");
    } else {
        switch (state) {
        case CheatAvailabilityManager::State::Pending:
            display = tr("Checking...");
            break;
        case CheatAvailabilityManager::State::Available:
            display = tr("Available (%1)").arg(cheat_count);
            sort_value = cheat_count;
            break;
        case CheatAvailabilityManager::State::NotFound:
            display = tr("Not available");
            sort_value = 0;
            break;
        case CheatAvailabilityManager::State::NetworkError:
            display = tr("Connection error");
            break;
        case CheatAvailabilityManager::State::Unknown:
            display = tr("Not checked");
            break;
        }
    }

    const auto update_children = [&](const auto& self, QStandardItem* parent) -> void {
        for (int row = 0; row < parent->rowCount(); ++row) {
            QStandardItem* const name_item = parent->child(row, COLUMN_NAME);
            if (name_item == nullptr) {
                continue;
            }
            if (name_item->data(GameListItem::TypeRole).value<GameListItemType>() ==
                    GameListItemType::Game &&
                name_item->data(GameListItemPath::ProgramIdRole).toULongLong() == title_id &&
                name_item->data(GameListItemPath::BuildIdRole).toString().compare(
                    normalized_build_id, Qt::CaseInsensitive) == 0) {
                if (QStandardItem* const cheats_item = parent->child(row, COLUMN_CHEATS);
                    cheats_item != nullptr) {
                    cheats_item->setText(display);
                    cheats_item->setData(sort_value, GameListItem::SortRole);
                    if (!normalized_build_id.isEmpty()) {
                        cheats_item->setToolTip(
                            tr("Exact match: Title ID %1 / Build ID %2")
                                .arg(QStringLiteral("%1")
                                         .arg(title_id, 16, 16, QLatin1Char{'0'})
                                         .toUpper(),
                                     normalized_build_id));
                    }
                }
            }
            if (name_item->hasChildren()) {
                self(self, name_item);
            }
        }
    };
    update_children(update_children, item_model->invisibleRootItem());

    if (tree_view->header()->sortIndicatorSection() == COLUMN_CHEATS) {
        item_model->sort(COLUMN_CHEATS, tree_view->header()->sortIndicatorOrder());
    }
    if (!search_field->filterText().isEmpty()) {
        OnTextChanged(search_field->filterText());
    }
}

void GameList::UpdateGameUpdateRows(u64 title_id) {
    const auto update_children = [&](const auto& self, QStandardItem* parent) -> void {
        for (int row = 0; row < parent->rowCount(); ++row) {
            QStandardItem* const name_item = parent->child(row, COLUMN_NAME);
            if (name_item == nullptr) {
                continue;
            }

            const bool is_game =
                name_item->data(GameListItem::TypeRole).value<GameListItemType>() ==
                GameListItemType::Game;
            const u64 row_title_id =
                name_item->data(GameListItemPath::ProgramIdRole).toULongLong();
            if (is_game && (title_id == 0 || row_title_id == title_id)) {
                QStandardItem* const status_item = parent->child(row, COLUMN_UPDATE_STATUS);
                if (status_item != nullptr) {
                    const u32 installed_version = static_cast<u32>(
                        name_item->data(GameListItemPath::InstalledUpdateVersionRole).toUInt());
                    const bool installed_comparable =
                        name_item->data(GameListItemPath::InstalledUpdateComparableRole).toBool();
                    const QString installed_display =
                        name_item->data(GameListItemPath::InstalledUpdateDisplayRole).toString();
                    const auto state = game_update_manager->GetState(
                        row_title_id, installed_version, installed_comparable);
                    const auto latest = game_update_manager->GetLatestVersion(row_title_id);
                    const QString friendly = game_update_manager->FriendlyVersion(
                        row_title_id, latest.numeric_version);
                    const QString numeric_display =
                        QStringLiteral("v%1").arg(latest.numeric_version);
                    const QString latest_display =
                        friendly.isEmpty()
                            ? numeric_display
                            : QStringLiteral("%1 | %2").arg(numeric_display, friendly);

                    QString display;
                    int sort_value{};
                    switch (state) {
                    case GameUpdateManager::State::Checking:
                        display = tr("Checking...");
                        sort_value = 1;
                        break;
                    case GameUpdateManager::State::UpToDate:
                        display = tr("Up to date");
                        sort_value = 2;
                        break;
                    case GameUpdateManager::State::UpdateAvailable:
                        display = tr("New: %1").arg(latest_display);
                        sort_value = 4;
                        break;
                    case GameUpdateManager::State::LocalNewer:
                        display = tr("Local version newer");
                        sort_value = 3;
                        break;
                    case GameUpdateManager::State::NetworkError:
                        display = tr("Connection error");
                        break;
                    case GameUpdateManager::State::Unknown:
                        display = latest.numeric_version > 0 && !installed_comparable
                                      ? tr("Installed version unknown")
                                      : tr("No data");
                        break;
                    }

                    status_item->setText(display);
                    status_item->setData(sort_value, GameListItem::SortRole);
                    if (latest.numeric_version > 0) {
                        const QString installed = installed_comparable
                                                      ? tr("Installed: %1 (v%2)")
                                                            .arg(installed_display)
                                                            .arg(installed_version)
                                                      : tr("Installed: %1").arg(installed_display);
                        const QString available =
                            friendly.isEmpty()
                                ? tr("Available: %1").arg(numeric_display)
                                : tr("Available: %1 (v%2)")
                                      .arg(friendly)
                                      .arg(latest.numeric_version);
                        QStringList tooltip{installed, available};
                        if (!latest.release_date.isEmpty()) {
                            tooltip.append(tr("Catalog date: %1").arg(latest.release_date));
                        }
                        tooltip.append(tr("Source: TitleDB"));
                        status_item->setToolTip(tooltip.join(QLatin1Char{'\n'}));
                    } else {
                        status_item->setToolTip(QString{});
                    }
                }
            }
            if (name_item->hasChildren()) {
                self(self, name_item);
            }
        }
    };
    update_children(update_children, item_model->invisibleRootItem());

    if (tree_view->header()->sortIndicatorSection() == COLUMN_UPDATE_STATUS) {
        item_model->sort(COLUMN_UPDATE_STATUS, tree_view->header()->sortIndicatorOrder());
    }
    if (!search_field->filterText().isEmpty()) {
        OnTextChanged(search_field->filterText());
    }
}

void GameList::OpenTrailerForItem(const QModelIndex& item) {
    if (!item.isValid() || trailer_search_in_progress) {
        return;
    }

    const auto selected = item.sibling(item.row(), COLUMN_NAME);
    if (selected.data(GameListItem::TypeRole).value<GameListItemType>() !=
        GameListItemType::Game) {
        return;
    }

    QString game_name = selected.data(GameListItemPath::TitleRole).toString().trimmed();
    if (game_name.isEmpty()) {
        const QFileInfo file_info(selected.data(GameListItemPath::FullPathRole).toString());
        game_name = file_info.completeBaseName().trimmed();
    }
    if (game_name.isEmpty()) {
        return;
    }

    m_currentView->setCurrentIndex(selected);
    trailer_search_in_progress = true;
    const QPersistentModelIndex trailer_item{item.sibling(item.row(), COLUMN_TRAILER)};
    item_model->setData(trailer_item, tr("Searching..."));

    const auto credential_directory =
        Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "youtube";
    auto* trailer_watcher = new QFutureWatcher<WebService::YouTubeTrailerResult>(this);
    connect(trailer_watcher, &QFutureWatcher<WebService::YouTubeTrailerResult>::finished, this,
            [this, trailer_watcher, trailer_item, game_name] {
                const auto result = trailer_watcher->result();
                trailer_watcher->deleteLater();
                trailer_search_in_progress = false;
                if (trailer_item.isValid()) {
                    const QString trailer_text =
                        QStringLiteral("\u25B6 ") + tr("Watch Trailer");
                    item_model->setData(trailer_item, trailer_text);
                }

                const QString search_query =
                    game_name + QStringLiteral(" Nintendo Switch trailer");
                const auto open_search_fallback = [search_query] {
                    QByteArray url =
                        QByteArrayLiteral("https://www.youtube.com/results?search_query=");
                    url.append(QUrl::toPercentEncoding(search_query));
                    if (!QDesktopServices::openUrl(QUrl::fromEncoded(url))) {
                        LOG_WARNING(Frontend, "Failed to open trailer search URL");
                    }
                };

                if (result.code != WebService::YouTubeTrailerResultCode::Success) {
                    LOG_WARNING(Frontend, "Automatic trailer search failed with code {}",
                                static_cast<int>(result.code));
                    open_search_fallback();
                    return;
                }

                const QString video_id = QString::fromStdString(result.video_id);
                const QString trailer_title =
                    QString::fromUtf8(result.title.data(),
                                      static_cast<qsizetype>(result.title.size()));
                if (!trailer_player) {
                    trailer_player = new TrailerPlayerDialog(video_id, trailer_title,
                                                             system.HIDCore(), main_window);
                    connect(trailer_player, &TrailerPlayerDialog::PlaybackFailed, this,
                            [](const QString& failed_video_id) {
                                const QUrl direct_url{
                                    QStringLiteral("https://www.youtube.com/watch?v=%1")
                                        .arg(failed_video_id)};
                                if (!QDesktopServices::openUrl(direct_url)) {
                                    LOG_WARNING(Frontend, "Failed to open direct trailer URL");
                                }
                            });
                } else {
                    trailer_player->Play(video_id, trailer_title);
                }
                trailer_player->showFullScreen();
            });
    trailer_watcher->setFuture(QtConcurrent::run([credential_directory,
                                                  name = game_name.toUtf8().toStdString()] {
        return WebService::FindYouTubeTrailer(credential_directory, name);
    }));
}

void GameList::ValidateEntry(const QModelIndex& item) {
    if (item.column() == COLUMN_CHEATS) {
        OpenCheatsForItem(item);
        return;
    }
    if (item.column() == COLUMN_TRAILER || item.column() == COLUMN_PLAYERS ||
        item.column() == COLUMN_GENRE || item.column() == COLUMN_TAGS) {
        return;
    }

    const auto selected = item.sibling(item.row(), COLUMN_NAME);

    switch (selected.data(GameListItem::TypeRole).value<GameListItemType>()) {
    case GameListItemType::Game: {
        const QString file_path = selected.data(GameListItemPath::FullPathRole).toString();
        if (file_path.isEmpty())
            return;
        const QFileInfo file_info(file_path);
        if (!file_info.exists())
            return;

        const auto title_id = selected.data(GameListItemPath::ProgramIdRole).toULongLong();
        const QString game_name = selected.data(GameListItemPath::TitleRole).toString();
        const u32 installed_version = static_cast<u32>(
            selected.data(GameListItemPath::InstalledUpdateVersionRole).toUInt());
        const bool installed_comparable =
            selected.data(GameListItemPath::InstalledUpdateComparableRole).toBool();
        game_update_manager->RequestRefresh(title_id, game_name, installed_version,
                                            installed_comparable);

        if (file_info.isDir()) {
            const QDir dir{file_path};
            const QStringList matching_main = dir.entryList({QStringLiteral("main")}, QDir::Files);
            if (matching_main.size() == 1) {
                emit GameChosen(dir.path() + QDir::separator() + matching_main[0], title_id);
            }
            return;
        }

        const QString build_id =
            selected.data(GameListItemPath::BuildIdRole).toString().trimmed().toUpper();
        cheat_availability_manager->RequestAvailability(title_id, build_id);

        // Users usually want to run a different game after closing one
        search_field->clear();
        emit GameChosen(file_path, title_id);
        break;
    }
    case GameListItemType::AddDir:
        emit AddDirectory();
        break;
    default:
        break;
    }
}

bool GameList::IsEmpty() const {
    for (int i = 0; i < item_model->rowCount(); i++) {
        const QStandardItem* child = item_model->invisibleRootItem()->child(i);
        const auto type = static_cast<GameListItemType>(child->type());

        if (!child->hasChildren() &&
            (type == GameListItemType::SdmcDir || type == GameListItemType::UserNandDir ||
             type == GameListItemType::SysNandDir)) {
            item_model->invisibleRootItem()->removeRow(child->row());
            i--;
        }
    }

    return !item_model->invisibleRootItem()->hasChildren();
}

void GameList::DonePopulating(const QStringList& watch_list) {
    emit ShowList(!IsEmpty());

    // Add favorites row
    if (m_isTreeMode) {
        item_model->invisibleRootItem()->appendRow(new GameListAddDir());

        item_model->invisibleRootItem()->insertRow(0, new GameListFavorites());
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(),
                                UISettings::values.favorited_ids.size() == 0);
        tree_view->setExpanded(item_model->invisibleRootItem()->child(0)->index(),
                               UISettings::values.favorites_expanded.GetValue());
        for (const auto id : std::as_const(UISettings::values.favorited_ids)) {
            AddFavorite(id);
        }
    }

    // Clear out the old directories to watch for changes and add the new ones
    auto watch_dirs = watcher->directories();
    if (!watch_dirs.isEmpty()) {
        watcher->removePaths(watch_dirs);
    }
    // Workaround: Add the watch paths in chunks to allow the gui to refresh
    // This prevents the UI from stalling when a large number of watch paths are added
    // Also artificially caps the watcher to a certain number of directories
    constexpr int LIMIT_WATCH_DIRECTORIES = 5000;
    constexpr int SLICE_SIZE = 25;
    int len = (std::min)(static_cast<int>(watch_list.size()), LIMIT_WATCH_DIRECTORIES);

    // Block signals to prevent the watcher from triggering a refresh while we are adding paths.
    // This fixes a refresh loop on macOS.
#ifdef __APPLE__
    const bool old_signals_blocked = watcher->blockSignals(true);
#endif

    for (int i = 0; i < len; i += SLICE_SIZE) {
        auto chunk = watch_list.mid(i, SLICE_SIZE);
        if (!chunk.isEmpty()) {
            watcher->addPaths(chunk);
        }
        QCoreApplication::processEvents();
    }

#ifdef __APPLE__
    watcher->blockSignals(old_signals_blocked);
#endif
    m_currentView->setEnabled(true);

    int children_total = 0;
    for (int i = 1; i < item_model->rowCount() - 1; ++i) {
        children_total += item_model->item(i, 0)->rowCount();
    }
    search_field->setFilterResult(children_total, children_total);
    if (children_total > 0) {
        search_field->setFocus();
    }
    item_model->sort(tree_view->header()->sortIndicatorSection(),
                     tree_view->header()->sortIndicatorOrder());

    if (history_baseline_pending) {
        known_library_titles.unite(observed_library_titles);
        history_baseline_pending = false;
        history_dirty = true;
    }
    if (history_dirty) {
        SaveLibraryHistory();
    }

    emit PopulatingCompleted();
}

void GameList::PopupContextMenu(const QPoint& menu_location) {
    QModelIndex item = m_currentView->indexAt(menu_location);
    if (!item.isValid()) {
        if (m_isTreeMode)
            return;

        QMenu blank_menu;
        QAction* addGameDirAction = blank_menu.addAction(tr("&Add New Game Directory"));

        connect(addGameDirAction, &QAction::triggered, this, &GameList::AddDirectory);
        blank_menu.exec(m_currentView->viewport()->mapToGlobal(menu_location));
        return;
    }

    const auto selected = item.sibling(item.row(), 0);
    QMenu context_menu;
    switch (selected.data(GameListItem::TypeRole).value<GameListItemType>()) {
    case GameListItemType::Game:
        AddGamePopup(context_menu, selected.data(GameListItemPath::ProgramIdRole).toULongLong(),
                     selected.data(GameListItemPath::FullPathRole).toString().toStdString());
        break;
    case GameListItemType::CustomDir:
        AddPermDirPopup(context_menu, selected);
        AddCustomDirPopup(context_menu, selected);
        break;
    case GameListItemType::SdmcDir:
    case GameListItemType::UserNandDir:
    case GameListItemType::SysNandDir:
        AddPermDirPopup(context_menu, selected);
        break;
    case GameListItemType::Favorites:
        AddFavoritesPopup(context_menu);
        break;
    default:
        break;
    }
    context_menu.exec(m_currentView->viewport()->mapToGlobal(menu_location));
}

void GameList::AddGamePopup(QMenu& context_menu, u64 program_id, const std::string& path) {
    // TODO(crueter): Refactor this and make it less bad
    QAction* favorite = context_menu.addAction(tr("Favorite"));
    context_menu.addSeparator();
    QAction* start_game = context_menu.addAction(tr("Start Game"));
    QAction* start_game_global =
        context_menu.addAction(tr("Start Game without Custom Configuration"));
    context_menu.addSeparator();
    QAction* open_save_location = context_menu.addAction(tr("Open Save Data Location"));
    QAction* open_mod_location = context_menu.addAction(tr("Open Mod Data Location"));
    QAction* open_transferable_shader_cache =
        context_menu.addAction(tr("Open Transferable Pipeline Cache"));
    QAction* ryujinx = context_menu.addAction(tr("Link to Ryujinx"));
    context_menu.addSeparator();
    QMenu* remove_menu = context_menu.addMenu(tr("Remove"));
    QAction* remove_update = remove_menu->addAction(tr("Remove Installed Update"));
    QAction* remove_dlc = remove_menu->addAction(tr("Remove All Installed DLC"));
    QAction* remove_custom_config = remove_menu->addAction(tr("Remove Custom Configuration"));
    QAction* remove_cache_storage = remove_menu->addAction(tr("Remove Cache Storage"));
    QAction* remove_gl_shader_cache = remove_menu->addAction(tr("Remove OpenGL Pipeline Cache"));
    QAction* remove_vk_shader_cache = remove_menu->addAction(tr("Remove Vulkan Pipeline Cache"));
    remove_menu->addSeparator();
    QAction* remove_shader_cache = remove_menu->addAction(tr("Remove All Pipeline Caches"));
    QAction* remove_all_content = remove_menu->addAction(tr("Remove All Installed Contents"));
    QMenu* play_time_menu = context_menu.addMenu(tr("Manage Play Time"));
    QAction* set_play_time = play_time_menu->addAction(tr("Edit Play Time Data"));
    QAction* remove_play_time_data = play_time_menu->addAction(tr("Remove Play Time Data"));
    QMenu* dump_romfs_menu = context_menu.addMenu(tr("Dump RomFS"));
    QAction* dump_romfs = dump_romfs_menu->addAction(tr("Dump RomFS"));
    QAction* dump_romfs_sdmc = dump_romfs_menu->addAction(tr("Dump RomFS to SDMC"));
    QAction* verify_integrity = context_menu.addAction(tr("Verify Integrity"));
    QAction* copy_tid = context_menu.addAction(tr("Copy Title ID to Clipboard"));
    QAction* navigate_to_gamedb_entry = context_menu.addAction(tr("Navigate to GameDB entry"));
// TODO: Implement shortcut creation for macOS
#if !defined(__APPLE__)
    QMenu* shortcut_menu = context_menu.addMenu(tr("Create Shortcut"));
    QAction* create_desktop_shortcut = shortcut_menu->addAction(tr("Add to Desktop"));
    QAction* create_applications_menu_shortcut =
        shortcut_menu->addAction(tr("Add to Applications Menu"));
#endif
    context_menu.addSeparator();
    QAction* properties = context_menu.addAction(tr("Configure Game"));

    favorite->setVisible(program_id != 0);
    favorite->setCheckable(true);
    favorite->setChecked(UISettings::values.favorited_ids.contains(program_id));
    open_save_location->setVisible(program_id != 0);
    open_mod_location->setVisible(program_id != 0);
    open_transferable_shader_cache->setVisible(program_id != 0);
    remove_update->setVisible(program_id != 0);
    remove_dlc->setVisible(program_id != 0);
    remove_gl_shader_cache->setVisible(program_id != 0);
    remove_vk_shader_cache->setVisible(program_id != 0);
    remove_shader_cache->setVisible(program_id != 0);
    remove_all_content->setVisible(program_id != 0);
    auto it = FindMatchingCompatibilityEntry(compatibility_list, program_id);
    navigate_to_gamedb_entry->setVisible(it != compatibility_list.end() && program_id != 0);

    connect(favorite, &QAction::triggered, this,
            [this, program_id]() { ToggleFavorite(program_id); });
    connect(open_save_location, &QAction::triggered, this, [this, program_id, path]() {
        emit OpenFolderRequested(program_id, GameListOpenTarget::SaveData, path);
    });
    connect(start_game, &QAction::triggered, this,
            [this, path]() { emit BootGame(QString::fromStdString(path), StartGameType::Normal); });
    connect(start_game_global, &QAction::triggered, this,
            [this, path]() { emit BootGame(QString::fromStdString(path), StartGameType::Global); });
    connect(open_mod_location, &QAction::triggered, this, [this, program_id, path]() {
        emit OpenFolderRequested(program_id, GameListOpenTarget::ModData, path);
    });
    connect(open_transferable_shader_cache, &QAction::triggered, this,
            [this, program_id]() { emit OpenTransferableShaderCacheRequested(program_id); });
    connect(remove_all_content, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id, QtCommon::Game::InstalledEntryType::Game);
    });
    connect(remove_update, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id, QtCommon::Game::InstalledEntryType::Update);
    });
    connect(remove_dlc, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id,
                                           QtCommon::Game::InstalledEntryType::AddOnContent);
    });
    connect(remove_gl_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::GlShaderCache,
                                 path);
    });
    connect(remove_vk_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::VkShaderCache,
                                 path);
    });
    connect(remove_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::AllShaderCache,
                                 path);
    });
    connect(remove_custom_config, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id,
                                 QtCommon::Game::GameListRemoveTarget::CustomConfiguration, path);
    });
    connect(set_play_time, &QAction::triggered, this,
            [this, program_id]() { emit SetPlayTimeRequested(program_id); });
    connect(remove_play_time_data, &QAction::triggered, this,
            [this, program_id]() { emit RemovePlayTimeRequested(program_id); });
    connect(remove_cache_storage, &QAction::triggered, this, [this, program_id, path] {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::CacheStorage,
                                 path);
    });
    connect(dump_romfs, &QAction::triggered, this, [this, program_id, path]() {
        emit DumpRomFSRequested(program_id, path, DumpRomFSTarget::Normal);
    });
    connect(dump_romfs_sdmc, &QAction::triggered, this, [this, program_id, path]() {
        emit DumpRomFSRequested(program_id, path, DumpRomFSTarget::SDMC);
    });
    connect(verify_integrity, &QAction::triggered, this,
            [this, path]() { emit VerifyIntegrityRequested(path); });
    connect(copy_tid, &QAction::triggered, this,
            [this, program_id]() { emit CopyTIDRequested(program_id); });
    connect(navigate_to_gamedb_entry, &QAction::triggered, this, [this, program_id]() {
        emit NavigateToGamedbEntryRequested(program_id, compatibility_list);
    });
// TODO: Implement shortcut creation for macOS
#if !defined(__APPLE__)
    connect(create_desktop_shortcut, &QAction::triggered, this, [this, program_id, path]() {
        emit CreateShortcut(program_id, path, QtCommon::Game::ShortcutTarget::Desktop);
    });
    connect(create_applications_menu_shortcut, &QAction::triggered, this,
            [this, program_id, path]() {
                emit CreateShortcut(program_id, path, QtCommon::Game::ShortcutTarget::Applications);
            });
#endif
    connect(properties, &QAction::triggered, this,
            [this, path]() { emit OpenPerGameGeneralRequested(path); });

    connect(ryujinx, &QAction::triggered, this,
            [this, program_id]() { emit LinkToRyujinxRequested(program_id); });
};

void GameList::AddCustomDirPopup(QMenu& context_menu, QModelIndex selected) {
    UISettings::GameDir& game_dir =
        UISettings::values.game_dirs[selected.data(GameListDir::GameDirRole).toInt()];

    QAction* deep_scan = context_menu.addAction(tr("Scan Subfolders"));
    QAction* delete_dir = context_menu.addAction(tr("Remove Game Directory"));

    deep_scan->setCheckable(true);
    deep_scan->setChecked(game_dir.deep_scan);

    connect(deep_scan, &QAction::triggered, this, [this, &game_dir] {
        game_dir.deep_scan = !game_dir.deep_scan;
        PopulateAsync(UISettings::values.game_dirs);
    });
    connect(delete_dir, &QAction::triggered, this, [this, &game_dir, selected] {
        UISettings::values.game_dirs.removeOne(game_dir);
        item_model->invisibleRootItem()->removeRow(selected.row());
        OnTextChanged(search_field->filterText());
    });
}

void GameList::AddPermDirPopup(QMenu& context_menu, QModelIndex selected) {
    const int game_dir_index = selected.data(GameListDir::GameDirRole).toInt();

    QAction* move_up = context_menu.addAction(tr("\u25B2 Move Up"));
    QAction* move_down = context_menu.addAction(tr("\u25bc Move Down"));
    QAction* open_directory_location = context_menu.addAction(tr("Open Directory Location"));

    const int row = selected.row();

    move_up->setEnabled(row > 1);
    move_down->setEnabled(row < item_model->rowCount() - 2);

    connect(move_up, &QAction::triggered, this, [this, selected, row, game_dir_index] {
        const int other_index = selected.sibling(row - 1, 0).data(GameListDir::GameDirRole).toInt();
        // swap the items in the settings
        std::swap(UISettings::values.game_dirs[game_dir_index],
                  UISettings::values.game_dirs[other_index]);
        // swap the indexes held by the QVariants
        item_model->setData(selected, QVariant(other_index), GameListDir::GameDirRole);
        item_model->setData(selected.sibling(row - 1, 0), QVariant(game_dir_index),
                            GameListDir::GameDirRole);
        // move the treeview items
        QList<QStandardItem*> item = item_model->takeRow(row);
        item_model->invisibleRootItem()->insertRow(row - 1, item);
        tree_view->setExpanded(selected.sibling(row - 1, 0),
                               UISettings::values.game_dirs[other_index].expanded);
    });

    connect(move_down, &QAction::triggered, this, [this, selected, row, game_dir_index] {
        const int other_index = selected.sibling(row + 1, 0).data(GameListDir::GameDirRole).toInt();
        // swap the items in the settings
        std::swap(UISettings::values.game_dirs[game_dir_index],
                  UISettings::values.game_dirs[other_index]);
        // swap the indexes held by the QVariants
        item_model->setData(selected, QVariant(other_index), GameListDir::GameDirRole);
        item_model->setData(selected.sibling(row + 1, 0), QVariant(game_dir_index),
                            GameListDir::GameDirRole);
        // move the treeview items
        const QList<QStandardItem*> item = item_model->takeRow(row);
        item_model->invisibleRootItem()->insertRow(row + 1, item);
        tree_view->setExpanded(selected.sibling(row + 1, 0),
                               UISettings::values.game_dirs[other_index].expanded);
    });

    connect(open_directory_location, &QAction::triggered, this, [this, game_dir_index] {
        emit OpenDirectory(
            QString::fromStdString(UISettings::values.game_dirs[game_dir_index].path));
    });
}

void GameList::AddFavoritesPopup(QMenu& context_menu) {
    QAction* clear = context_menu.addAction(tr("Clear"));

    connect(clear, &QAction::triggered, this, [this] {
        for (const auto id : std::as_const(UISettings::values.favorited_ids)) {
            RemoveFavorite(id);
        }
        UISettings::values.favorited_ids.clear();
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(), true);
    });
}

void GameList::LoadCompatibilityList() {
    QFile compat_list{QStringLiteral(":compatibility_list/compatibility_list.json")};

    if (!compat_list.open(QFile::ReadOnly | QFile::Text)) {
        LOG_ERROR(Frontend, "Unable to open game compatibility list");
        return;
    }

    if (compat_list.size() == 0) {
        LOG_WARNING(Frontend, "Game compatibility list is empty");
        return;
    }

    const QByteArray content = compat_list.readAll();
    if (content.isEmpty()) {
        LOG_ERROR(Frontend, "Unable to completely read game compatibility list");
        return;
    }

    const QJsonDocument json = QJsonDocument::fromJson(content);
    const QJsonArray arr = json.array();

    for (const QJsonValue& value : arr) {
        const QJsonObject game = value.toObject();
        const QString compatibility_key = QStringLiteral("compatibility");

        if (!game.contains(compatibility_key) || !game[compatibility_key].isDouble()) {
            continue;
        }

        const int compatibility = game[compatibility_key].toInt();
        const QString directory = game[QStringLiteral("directory")].toString();
        const QJsonArray ids = game[QStringLiteral("releases")].toArray();

        for (const QJsonValue& id_ref : ids) {
            const QJsonObject id_object = id_ref.toObject();
            const QString id = id_object[QStringLiteral("id")].toString();

            compatibility_list.emplace(id.toUpper().toStdString(),
                                       std::make_pair(QString::number(compatibility), directory));
        }
    }
}

void GameList::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void GameList::RetranslateUI() {
    item_model->setHeaderData(COLUMN_NAME, Qt::Horizontal, tr("Name"));
    item_model->setHeaderData(COLUMN_TRAILER, Qt::Horizontal, tr("Trailer"));
    item_model->setHeaderData(COLUMN_PLAYERS, Qt::Horizontal, tr("Players"));
    item_model->setHeaderData(COLUMN_GENRE, Qt::Horizontal, tr("Genre"));
    item_model->setHeaderData(COLUMN_TAGS, Qt::Horizontal, tr("Tags"));
    item_model->setHeaderData(COLUMN_BUILD_ID, Qt::Horizontal, tr("Build ID"));
    item_model->setHeaderData(COLUMN_CHEATS, Qt::Horizontal, tr("Cheats"));
    item_model->setHeaderData(COLUMN_COMPATIBILITY, Qt::Horizontal, tr("Compatibility"));
    item_model->setHeaderData(COLUMN_ADD_ONS, Qt::Horizontal, tr("Add-ons"));
    item_model->setHeaderData(COLUMN_UPDATE_STATUS, Qt::Horizontal, tr("Update status"));
    item_model->setHeaderData(COLUMN_FILE_TYPE, Qt::Horizontal, tr("File type"));
    item_model->setHeaderData(COLUMN_SIZE, Qt::Horizontal, tr("Size"));
    item_model->setHeaderData(COLUMN_PLAY_TIME, Qt::Horizontal, tr("Play time"));
    item_model->setHeaderData(COLUMN_LAST_PLAYED, Qt::Horizontal,
                              QStringLiteral("Última partida"));
    item_model->setHeaderData(COLUMN_DATE_ADDED, Qt::Horizontal,
                              QStringLiteral("Agregado a Eden"));
    item_model->setHeaderData(COLUMN_CREATED, Qt::Horizontal, QStringLiteral("Creado"));
}

void GameListSearchField::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void GameListSearchField::RetranslateUI() {
    label_filter->setText(tr("Filter:"));
    edit_filter->setPlaceholderText(tr("Enter pattern to filter"));
}

QStandardItemModel* GameList::GetModel() const {
    return item_model;
}

void GameList::UpdateIconSize() {
    // Update sizes and stuff for the list view
    const u32 icon_size = UISettings::values.game_icon_size.GetValue();

    int heightMargin = 0;
    int widthMargin = 80;

    if (UISettings::values.show_game_name) {
        // the scaling on the card is kinda abysmal.
        // TODO(crueter): refactor
        switch (icon_size) {
        case 128:
            heightMargin = 65;
            break;
        case 0:
            widthMargin = 120;
            heightMargin = 120;
            break;
        case 64:
            heightMargin = 77;
            break;
        case 32:
        case 256:
            heightMargin = 81;
            break;
        }
    } else {
        widthMargin = 24;
        heightMargin = 24;
    }

    // "auto" resize //
    const int view_width = list_view->viewport()->width();

    // Tiny space padding to prevent the list view from forcing its own resize operation.
    const double spacing = 0.01;
    const int min_item_width = icon_size + widthMargin;

    // And now stretch it a bit to fill out remaining space.
    // Not perfect but works well enough for now
    int columns = std::max(1, (view_width - 16) / min_item_width);
    int stretched_width = ((view_width) - (spacing * (columns - 1))) / columns;

    // only updates things if grid size is changed
    QSize grid_size(stretched_width, icon_size + heightMargin);
    if (list_view->gridSize() != grid_size) {
        list_view->setUpdatesEnabled(false);

        list_view->setGridSize(grid_size);
        m_gameCard->setSize(grid_size, stretched_width - min_item_width, columns);

        list_view->setUpdatesEnabled(true);
    }
}

void GameList::PopulateAsync(QVector<UISettings::GameDir>& game_dirs) {
    m_currentView->setEnabled(false);
    observed_library_titles.clear();

    // Update the columns in case UISettings has changed
    tree_view->setColumnHidden(COLUMN_ADD_ONS, !UISettings::values.show_add_ons);
    tree_view->setColumnHidden(COLUMN_COMPATIBILITY, !UISettings::values.show_compat);
    tree_view->setColumnHidden(COLUMN_FILE_TYPE, !UISettings::values.show_types);
    tree_view->setColumnHidden(COLUMN_SIZE, !UISettings::values.show_size);
    tree_view->setColumnHidden(COLUMN_PLAY_TIME, !UISettings::values.show_play_time);

    if (!m_isTreeMode)
        UpdateIconSize();

    // Cancel any existing worker.
    current_worker.reset();

    // Delete any rows that might already exist if we're repopulating
    item_model->removeRows(0, item_model->rowCount());
    search_field->clear();

    current_worker = std::make_unique<GameListWorker>(vfs, provider, game_dirs, compatibility_list,
                                                      play_time_manager, system);

    // Get events from the worker as data becomes available
    connect(current_worker.get(), &GameListWorker::DataAvailable, this, &GameList::WorkerEvent,
            Qt::QueuedConnection);

    QThreadPool::globalInstance()->start(current_worker.get());
}

void GameList::SaveInterfaceLayout() {
    UISettings::values.gamelist_header_state = tree_view->header()->saveState();
}

void GameList::LoadInterfaceLayout() {
    auto* header = tree_view->header();

    if (!header->restoreState(UISettings::values.gamelist_header_state)) {
        // We are using the name column to display icons and titles
        // so make it as large as possible as default.

        // TODO(crueter): width() is not initialized yet, so use a sane default value
        header->resizeSection(COLUMN_NAME, 840);
    }

    header->setSectionHidden(COLUMN_TRAILER, false);
    header->setSectionHidden(COLUMN_PLAYERS, false);
    header->setSectionHidden(COLUMN_GENRE, false);
    header->setSectionHidden(COLUMN_TAGS, false);
    header->setSectionHidden(COLUMN_BUILD_ID, false);
    header->setSectionHidden(COLUMN_CHEATS, false);
    header->setSectionHidden(COLUMN_UPDATE_STATUS, false);
    header->setSectionHidden(COLUMN_LAST_PLAYED, false);
    header->setSectionHidden(COLUMN_DATE_ADDED, false);
    header->setSectionHidden(COLUMN_CREATED, false);

    const std::array metadata_columns{
        COLUMN_TRAILER,
        COLUMN_PLAYERS,
        COLUMN_GENRE,
        COLUMN_TAGS,
        COLUMN_BUILD_ID,
        COLUMN_CHEATS,
    };
    for (std::size_t index = 0; index < metadata_columns.size(); ++index) {
        const int logical_index = metadata_columns[index];
        const int target_visual_index = static_cast<int>(index) + 1;
        const int current_visual_index = header->visualIndex(logical_index);
        if (current_visual_index != target_visual_index) {
            header->moveSection(current_visual_index, target_visual_index);
        }
    }
    const int update_status_visual_index = header->visualIndex(COLUMN_UPDATE_STATUS);
    const int update_status_target_index = header->visualIndex(COLUMN_ADD_ONS) + 1;
    if (update_status_visual_index != update_status_target_index) {
        header->moveSection(update_status_visual_index, update_status_target_index);
    }
    MoveColumnAfter(header, COLUMN_LAST_PLAYED, COLUMN_PLAY_TIME);
    MoveColumnAfter(header, COLUMN_DATE_ADDED, COLUMN_LAST_PLAYED);
    MoveColumnAfter(header, COLUMN_CREATED, COLUMN_DATE_ADDED);
    header->resizeSection(COLUMN_BUILD_ID,
                          std::max(header->sectionSize(COLUMN_BUILD_ID), 145));
    header->resizeSection(COLUMN_CHEATS, std::max(header->sectionSize(COLUMN_CHEATS), 120));
    header->resizeSection(COLUMN_UPDATE_STATUS,
                          std::max(header->sectionSize(COLUMN_UPDATE_STATUS), 135));
    header->resizeSection(COLUMN_LAST_PLAYED,
                          std::max(header->sectionSize(COLUMN_LAST_PLAYED), 125));
    header->resizeSection(COLUMN_DATE_ADDED,
                          std::max(header->sectionSize(COLUMN_DATE_ADDED), 135));
    header->resizeSection(COLUMN_CREATED, std::max(header->sectionSize(COLUMN_CREATED), 110));
}

const QStringList GameList::supported_file_extensions = {
    QStringLiteral("nso"), QStringLiteral("nro"), QStringLiteral("nca"),
    QStringLiteral("xci"), QStringLiteral("nsp"), QStringLiteral("kip")};

void GameList::RefreshGameDirectory() {
    // Reset the externals watcher whenever the game list is reloaded,
    // primarily ensures that new titles and external dirs are caught.
    ResetExternalWatcher();

    if (!UISettings::values.game_dirs.empty() && current_worker != nullptr) {
        LOG_INFO(Frontend, "Change detected in the games directory. Reloading game list.");
        QtCommon::system->GetFileSystemController().CreateFactories(*QtCommon::vfs);
        PopulateAsync(UISettings::values.game_dirs);
    }
}

void GameList::RefreshExternalContent() {
    // TODO: Explore the possibility of only resetting the metadata cache for that specific game.
    if (!UISettings::values.game_dirs.empty() && current_worker != nullptr) {
        LOG_INFO(Frontend, "External content directory changed. Clearing metadata cache.");
        QtCommon::Game::ResetMetadata(false);
        QtCommon::system->GetFileSystemController().CreateFactories(*QtCommon::vfs);
        PopulateAsync(UISettings::values.game_dirs);
    }
}

void GameList::ResetExternalWatcher() {
    auto watch_dirs = external_watcher->directories();
    if (!watch_dirs.isEmpty()) {
        external_watcher->removePaths(watch_dirs);
    }

    for (const std::string& dir : Settings::values.external_content_dirs) {
        external_watcher->addPath(QString::fromStdString(dir));
    }
}

void GameList::ToggleFavorite(u64 program_id) {
    if (!UISettings::values.favorited_ids.contains(program_id)) {
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(),
                                !search_field->filterText().isEmpty());
        UISettings::values.favorited_ids.append(program_id);
        AddFavorite(program_id);
        item_model->sort(tree_view->header()->sortIndicatorSection(),
                         tree_view->header()->sortIndicatorOrder());
    } else {
        UISettings::values.favorited_ids.removeOne(program_id);
        RemoveFavorite(program_id);
        if (UISettings::values.favorited_ids.size() == 0) {
            tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(), true);
        }
    }
    emit SaveConfig();
}

void GameList::AddFavorite(u64 program_id) {
    auto* favorites_row = item_model->item(0);

    for (int i = 1; i < item_model->rowCount() - 1; i++) {
        const auto* folder = item_model->item(i);
        for (int j = 0; j < folder->rowCount(); j++) {
            if (folder->child(j)->data(GameListItemPath::ProgramIdRole).toULongLong() ==
                program_id) {
                QList<QStandardItem*> list;
                for (int k = 0; k < COLUMN_COUNT; k++) {
                    list.append(folder->child(j, k)->clone());
                }
                list[0]->setData(folder->child(j)->data(GameListItem::SortRole),
                                 GameListItem::SortRole);
                list[0]->setText(folder->child(j)->data(Qt::DisplayRole).toString());

                favorites_row->appendRow(list);
                return;
            }
        }
    }
}

void GameList::RemoveFavorite(u64 program_id) {
    auto* favorites_row = item_model->item(0);

    for (int i = 0; i < favorites_row->rowCount(); i++) {
        const auto* game = favorites_row->child(i);
        if (game->data(GameListItemPath::ProgramIdRole).toULongLong() == program_id) {
            favorites_row->removeRow(i);
            return;
        }
    }
}

GameListPlaceholder::GameListPlaceholder(MainWindow* parent) : QWidget{parent} {
    connect(parent, &MainWindow::UpdateThemedIcons, this,
            &GameListPlaceholder::onUpdateThemedIcons);

    layout = new QVBoxLayout;
    image = new QLabel;
    text = new QLabel;
    layout->setAlignment(Qt::AlignCenter);
    image->setPixmap(QIcon::fromTheme(QStringLiteral("plus_folder")).pixmap(200));

    RetranslateUI();
    QFont font = text->font();
    font.setPointSize(20);
    text->setFont(font);
    text->setAlignment(Qt::AlignHCenter);
    image->setAlignment(Qt::AlignHCenter);

    layout->addWidget(image);
    layout->addWidget(text);
    setLayout(layout);
}

GameListPlaceholder::~GameListPlaceholder() = default;

void GameListPlaceholder::onUpdateThemedIcons() {
    image->setPixmap(QIcon::fromTheme(QStringLiteral("plus_folder")).pixmap(200));
}

void GameListPlaceholder::mouseDoubleClickEvent(QMouseEvent* event) {
    emit GameListPlaceholder::AddDirectory();
}

void GameList::SetupScrollAnimation() {
    auto setup = [this](QVariantAnimation* anim, QScrollBar* bar) {
        // animation handles moving the bar instead of Qt's built in crap
        anim->setEasingCurve(QEasingCurve::OutCubic);
        anim->setDuration(200);
        connect(anim, &QVariantAnimation::valueChanged, this,
                [bar](const QVariant& value) { bar->setValue(value.toInt()); });
    };

    vertical_scroll = new QVariantAnimation(this);
    horizontal_scroll = new QVariantAnimation(this);

    setup(vertical_scroll, tree_view->verticalScrollBar());
    setup(horizontal_scroll, tree_view->horizontalScrollBar());

    setup(vertical_scroll, list_view->verticalScrollBar());
    setup(horizontal_scroll, list_view->horizontalScrollBar());
}

bool GameList::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_currentView->viewport() && event->type() == QEvent::Wheel) {
        QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);

        bool horizontal = wheelEvent->modifiers() & Qt::ShiftModifier;

        int deltaX = wheelEvent->angleDelta().x();
        int deltaY = wheelEvent->angleDelta().y();

        // if shift is held do a horizontal scroll
        if (horizontal && deltaY != 0 && deltaX == 0) {
            deltaX = deltaY;
            deltaY = 0;
        }

        // TODO(crueter): dedup this
        if (deltaY != 0) {
            if (vertical_scroll->state() == QAbstractAnimation::Stopped)
                vertical_scroll_target = m_currentView->verticalScrollBar()->value();

            vertical_scroll_target -= deltaY;
            vertical_scroll_target =
                qBound(0, vertical_scroll_target, m_currentView->verticalScrollBar()->maximum());

            vertical_scroll->stop();
            vertical_scroll->setStartValue(m_currentView->verticalScrollBar()->value());
            vertical_scroll->setEndValue(vertical_scroll_target);
            vertical_scroll->start();
        }

        if (deltaX != 0) {
            if (horizontal_scroll->state() == QAbstractAnimation::Stopped)
                horizontal_scroll_target = m_currentView->horizontalScrollBar()->value();

            horizontal_scroll_target -= deltaX;
            horizontal_scroll_target = qBound(0, horizontal_scroll_target,
                                              m_currentView->horizontalScrollBar()->maximum());

            horizontal_scroll->stop();
            horizontal_scroll->setStartValue(m_currentView->horizontalScrollBar()->value());
            horizontal_scroll->setEndValue(horizontal_scroll_target);
            horizontal_scroll->start();
        }

        return true;
    }

    if (obj == m_currentView->viewport() && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);

        // if the user clicks outside of the list, deselect the current item.
        QModelIndex index = m_currentView->indexAt(mouseEvent->pos());
        if (!index.isValid()) {
            m_currentView->selectionModel()->clearSelection();
            m_currentView->setCurrentIndex(QModelIndex());
        }
    }

    if (obj == list_view->viewport() && event->type() == QEvent::Resize) {
        UpdateIconSize();
        return true;
    }

    return QWidget::eventFilter(obj, event);
}

void GameListPlaceholder::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void GameListPlaceholder::RetranslateUI() {
    text->setText(tr("Double-click to add a new folder to the game list"));
}
