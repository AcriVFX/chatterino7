// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QRect>

class QTimer;

namespace chatterino {

class TwitchChannel;

/**
 * Chatterino9: read-only strip under the split header showing the prediction
 * that is running, as seen from the prediction badges in chat.
 *
 * Moderators can't read or control predictions through the Twitch API, so
 * "Manage" opens the channel's Twitch popout chat in the browser.
 */
class PredictionStripWidget final : public BaseWidget
{
    Q_OBJECT

public:
    explicit PredictionStripWidget(QWidget *parent = nullptr);

    // Pass nullptr to detach from any channel.
    void setChannel(TwitchChannel *channel);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void scaleChangedEvent(float newScale) override;

private:
    void refresh();

    TwitchChannel *channel_ = nullptr;
    pajlada::Signals::SignalHolder signalHolder_;
    QTimer *expireTimer_ = nullptr;
    QRect manageRect_;
    bool manageHovered_ = false;
};

}  // namespace chatterino
