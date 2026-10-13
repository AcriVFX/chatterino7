// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVectorModel.hpp"
#include "controllers/ignores/HiddenUser.hpp"

#include <QObject>

namespace chatterino {

class HiddenUserModel : public SignalVectorModel<HiddenUser>
{
public:
    explicit HiddenUserModel(QObject *parent);

    enum Column {
        Name = 0,
        Since = 1,
    };

protected:
    // turn a vector item into a model row
    HiddenUser getItemFromRow(std::vector<QStandardItem *> &row,
                              const HiddenUser &original) override;

    // turns a row in the model into a vector item
    void getRowFromItem(const HiddenUser &item,
                        std::vector<QStandardItem *> &row) override;
};

}  // namespace chatterino
