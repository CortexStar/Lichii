// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QColor>
#include <QPalette>
#include <QScroller>
#include <QScrollerProperties>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/model.h"
#include "yuzu/game/common.h"
#include "yuzu/game/game_card.h"
#include "yuzu/game/game_grid.h"

GameGrid::GameGrid(QWidget* parent) : QListView{parent} {
    m_gameCard = new GameCard(this);
    setItemDelegate(m_gameCard);
    setObjectName(QStringLiteral("gameGridView"));

    setViewMode(QListView::ListMode);
    setResizeMode(QListView::Fixed);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setMouseTracking(true);

    QPalette grid_palette = palette();
    grid_palette.setColor(QPalette::Highlight, QColor(102, 104, 108));
    grid_palette.setColor(QPalette::HighlightedText, Qt::white);
    setPalette(grid_palette);

    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setContextMenuPolicy(Qt::CustomContextMenu);

    setSpacing(12);
    setWordWrap(true);
    setTextElideMode(Qt::ElideRight);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setStyleSheet(QStringLiteral(
        "QListView {"
        "  border: none;"
        "  outline: none;"
        "  padding: 6px 8px 12px 8px;"
        "}"));
}

void GameGrid::SetModel(GameListModel* model) {
    QListView::setModel(model);
    UpdateIconSize();
}

void GameGrid::ApplyFilter(const QString& edit_filter_text, GameListModel* model) {
    const QModelIndex root = rootIndex();
    const int row_count = model->rowCount(root);

    for (int i = 0; i < row_count; ++i) {
        QStandardItem* item = root.isValid() ? model->itemFromIndex(model->index(i, 0, root))
                                             : model->item(i, 0);
        if (!item)
            continue;

        const auto type = item->data(GameListItem::TypeRole).value<GameListItemType>();
        if (!root.isValid() && type == GameListItemType::Favorites) {
            setRowHidden(i, true);
            continue;
        }
        if (!root.isValid() && type == GameListItemType::AddDir) {
            setRowHidden(i, false);
            continue;
        }

        bool matches = Yuzu::FilterMatches(edit_filter_text, item);
        if (!root.isValid() &&
            (type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
             type == GameListItemType::UserNandDir || type == GameListItemType::SysNandDir)) {
            for (int child = 0; !matches && child < item->rowCount(); ++child) {
                matches = Yuzu::FilterMatches(edit_filter_text, item->child(child, 0));
            }
        }
        setRowHidden(i, !matches);
    }
}

void GameGrid::UpdateIconSize() {
    const bool show_game_name =
        UISettings::values.show_game_name.GetValue() || !rootIndex().isValid();
    const u32 display_icon_size = Yuzu::CardIconDisplaySize(show_game_name);

    int heightMargin = 0;
    int widthMargin = 72;

    if (show_game_name) {
        heightMargin = 92;
    } else {
        widthMargin = 24;
        heightMargin = 24;
    }

    const int view_width = viewport()->width();

    const double spacing = 0.01;
    const int min_item_width = display_icon_size + widthMargin;

    int columns = std::max(1, (view_width - 16) / min_item_width);
    int stretched_width = ((view_width) - (spacing * (columns - 1))) / columns;

    QSize grid_size(stretched_width, display_icon_size + heightMargin);
    if (gridSize() != grid_size) {
        setUpdatesEnabled(false);

        setGridSize(grid_size);
        m_gameCard->setSize(grid_size, grid_size, stretched_width - min_item_width, columns);

        setUpdatesEnabled(true);
    }
}

QModelIndex GameGrid::indexAt(const QPoint& point) const {
    QModelIndex index = QListView::indexAt(point);
    if (!index.isValid())
        return {};
    if (m_gameCard && !m_gameCard->hitTest(point, index, this, visualRect(index)))
        return {};
    return index;
}
