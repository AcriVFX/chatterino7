// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/ignores/HiddenUserModel.hpp"

#include "util/StandardItemHelper.hpp"

#include <QDate>

namespace chatterino {

HiddenUserModel::HiddenUserModel(QObject *parent)
    : SignalVectorModel<HiddenUser>(2, parent)
{
}

HiddenUser HiddenUserModel::getItemFromRow(std::vector<QStandardItem *> &row,
                                           const HiddenUser &original)
{
    return HiddenUser{
        row[Column::Name]->data(Qt::DisplayRole).toString(),
        original.getSince(),
    };
}

void HiddenUserModel::getRowFromItem(const HiddenUser &item,
                                     std::vector<QStandardItem *> &row)
{
    setStringItem(row[Column::Name], item.getName());

    // shown like 13.10.2026, stored as 2026-10-13
    auto since = QDate::fromString(item.getSince(), Qt::ISODate);
    setStringItem(row[Column::Since],
                  since.isValid() ? since.toString("dd.MM.yyyy") : QString(),
                  false);
}

}  // namespace chatterino
