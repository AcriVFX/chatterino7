// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/PinnedMessageWidget.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/buttons/DrawnButton.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

using namespace std::chrono_literals;
using namespace Qt::Literals;

#include <chrono>
#include <memory>
#include <optional>

namespace chatterino {

namespace {

constexpr auto MUTED_STYLE = "color: #adadb8;";

}  // namespace

PinnedMessageWidget::PinnedMessageWidget(QWidget *parent)
    : BaseWidget(parent)
    , pinnedByLabel_(new QLabel(this))
    , countdownLabel_(new QLabel(this))
    , menuButton_(new DrawnButton(DrawnButton::Symbol::Kebab, {}, this))
    , messageScrollArea_(new QScrollArea(this))
    , messageLabel_(new QLabel(this))
    , footerLabel_(new QLabel(this))
    , progressTimer_(new QTimer(this))
{
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    auto *outerBox = new QVBoxLayout(this);
    outerBox->setContentsMargins(0, 0, 0, 0);
    outerBox->setSpacing(0);

    auto *contentBox = new QVBoxLayout();
    contentBox->setContentsMargins(8, 6, 8, 6);
    contentBox->setSpacing(3);

    // Header row: "Pinned by <user>"  [⋮]
    auto *headerRow = new QHBoxLayout();
    headerRow->setSpacing(4);

    headerRow->addWidget(this->pinnedByLabel_);
    headerRow->addStretch(1);
    this->menuButton_->setScaleIndependentSize(28, 28);
    this->menuButton_->setToolTip(u"Mod options"_s);
    this->menuButton_->setMenu(this->buildModMenu());
    this->menuButton_->hide();
    headerRow->addWidget(this->menuButton_);

    contentBox->addLayout(headerRow);

    // Message body
    this->messageLabel_->setWordWrap(true);
    this->messageLabel_->setTextFormat(Qt::PlainText);
    this->messageLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    this->messageLabel_->setStyleSheet("background: transparent;");
    this->messageLabel_->setSizePolicy(QSizePolicy::Expanding,
                                       QSizePolicy::Preferred);
    this->messageLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    this->messageScrollArea_->setWidgetResizable(true);
    this->messageScrollArea_->setHorizontalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    this->messageScrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->messageScrollArea_->setFrameShape(QFrame::NoFrame);
    this->messageScrollArea_->setFocusPolicy(Qt::NoFocus);
    this->messageScrollArea_->setStyleSheet(
        "QScrollArea { background: transparent; } "
        "QScrollArea > QWidget > QWidget { background: transparent; }");
    this->messageScrollArea_->viewport()->setAutoFillBackground(false);
    this->messageScrollArea_->setSizePolicy(QSizePolicy::Expanding,
                                            QSizePolicy::Fixed);
    this->messageScrollArea_->setWidget(this->messageLabel_);
    // Chatterino9: the viewport only gets its real width once our layout
    // runs, which can be after our own resize/show (first show, a tab that
    // was never opened). Measuring before that wraps the text at the default
    // ~100px width and leaves the banner at the max height with empty space,
    // so measure again whenever the viewport width changes.
    this->messageScrollArea_->viewport()->installEventFilter(this);
    contentBox->addWidget(this->messageScrollArea_);

    // Footer: [sender · time] ... [countdown]
    auto *footerRow = new QHBoxLayout();
    footerRow->setContentsMargins(0, 2, 0, 0);
    footerRow->setSpacing(4);

    this->footerLabel_->setStyleSheet(MUTED_STYLE);
    footerRow->addWidget(this->footerLabel_);
    footerRow->addStretch(1);

    this->countdownLabel_->setStyleSheet(MUTED_STYLE);
    this->countdownLabel_->hide();
    footerRow->addWidget(this->countdownLabel_);
    contentBox->addLayout(footerRow);

    outerBox->addLayout(contentBox);

    // 1px bottom border - separates pin widget from the chat view below
    auto *bottomBorder = new QWidget(this);
    bottomBorder->setFixedHeight(1);
    bottomBorder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bottomBorder->setAutoFillBackground(true);
    {
        QPalette pal = bottomBorder->palette();
        pal.setColor(QPalette::Window, pal.color(QPalette::Mid));
        bottomBorder->setPalette(pal);
    }
    outerBox->addWidget(bottomBorder);

    // Countdown timer (fires every second)
    this->progressTimer_->setInterval(1s);
    QObject::connect(this->progressTimer_, &QTimer::timeout, this, [this] {
        this->tickProgress();
    });

    this->scaleChangedEvent(this->scale());
    this->hide();
}

void PinnedMessageWidget::tickProgress()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 endsMs = this->pinEndsAt_.toMSecsSinceEpoch();

    if (nowMs >= endsMs)
    {
        this->progressTimer_->stop();
        this->countdownLabel_->hide();
        // Clearing re-enters refresh() and frees the pin, which refresh()
        // may still be reading, so do it once the current call is done.
        QTimer::singleShot(0, this, [this] {
            if (this->channel_ == nullptr)
            {
                return;
            }
            const auto *pin = this->channel_->getPinnedMessage();
            if (pin != nullptr && pin->endsAt && pin->endsAt->isValid() &&
                *pin->endsAt <= QDateTime::currentDateTimeUtc())
            {
                this->channel_->clearPinnedMessage();
            }
        });
        return;
    }

    const qint64 remainingMs = endsMs - nowMs;
    const qint64 totalSecs = (remainingMs + 999) / 1000;  // round up
    const qint64 hours = totalSecs / 3600;
    const qint64 mins = (totalSecs % 3600) / 60;
    const qint64 secs = totalSecs % 60;

    QString timeStr;
    if (hours > 0)
    {
        timeStr = u"Unpins in %1:%2:%3"_s.arg(hours)
                      .arg(mins, 2, 10, QChar(u'0'))
                      .arg(secs, 2, 10, QChar(u'0'));
    }
    else
    {
        timeStr = u"Unpins in %1:%2"_s.arg(mins, 2, 10, QChar(u'0'))
                      .arg(secs, 2, 10, QChar(u'0'));
    }

    this->countdownLabel_->setText(timeStr);
    this->countdownLabel_->show();
}

void PinnedMessageWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    auto *theme = getTheme();

    // Fill background (same color as the split header above)
    painter.fillRect(event->rect(), theme->splits.header.background);

    // Draw 1px top border
    painter.setPen(theme->splits.header.border);
    painter.drawLine(0, 0, this->width() - 1, 0);
}

void PinnedMessageWidget::setChannel(TwitchChannel *channel)
{
    this->signalHolder_.clear();
    if (channel != this->channel_)
    {
        // a pin hidden here stays hidden when the split only changes its
        // filters or lane style
        this->hiddenPinID_.clear();
    }
    this->channel_ = channel;

    if (channel)
    {
        this->signalHolder_.managedConnect(channel->pinnedMessageChanged,
                                           [this] {
                                               this->refresh();
                                           });
        this->signalHolder_.managedConnect(channel->userStateChanged, [this] {
            this->refresh();
        });
    }

    this->refresh();
}

std::unique_ptr<QMenu> PinnedMessageWidget::buildModMenu()
{
    auto menu = std::make_unique<QMenu>(this);

    menu->addAction(u"Unpin this Message"_s, this, [this] {
        if (this->channel_)
        {
            this->channel_->unpinCurrentMessage();
        }
    });

    auto *unpinAfterMenu = menu->addMenu(u"Unpin After"_s);

    const auto addDuration = [&](const QString &label,
                                 std::optional<std::chrono::seconds> duration) {
        unpinAfterMenu->addAction(label, this, [this, duration] {
            if (!this->channel_)
            {
                return;
            }
            const auto *pin = this->channel_->getPinnedMessage();
            if (!pin)
            {
                return;
            }
            auto currentAccount = getApp()->getAccounts()->twitch.getCurrent();
            if (!currentAccount || currentAccount->isAnon())
            {
                return;
            }
            this->channel_->updatePinnedMessageAs(
                pin->messageID, duration, *currentAccount, pin->messageText);
        });
    };

    addDuration(u"1 minute"_s, 1min);
    addDuration(u"5 minutes"_s, 5min);
    addDuration(u"10 minutes"_s, 10min);
    addDuration(u"20 minutes"_s, 20min);
    addDuration(u"30 minutes"_s, 30min);
    unpinAfterMenu->addSeparator();
    addDuration(u"End of stream"_s, std::nullopt);

    menu->addSeparator();

    menu->addAction(u"Hide for Yourself"_s, this, [this] {
        if (!this->isHidden())
        {
            this->toggleUserPinned();
        }
    });

    return menu;
}

void PinnedMessageWidget::refresh()
{
    if (!this->channel_)
    {
        this->progressTimer_->stop();
        this->hide();
        return;
    }

    const auto *pin = this->channel_->getPinnedMessage();
    if (!pin)
    {
        this->progressTimer_->stop();
        this->hide();
        return;
    }

    const auto mode = static_cast<UsernameDisplayMode>(
        getSettings()->usernameDisplayMode.getValue());
    this->pinnedByLabel_->setText(u"Pinned by <b>%1</b>"_s.arg(
        pin->pinnedBy.formatted(mode).toHtmlEscaped()));

    this->messageLabel_->setText(pin->messageText);
    this->updateMessageHeight();

    {
        const QString sentAt = pin->startsAt.toLocalTime().toString(
            getSettings()->timestampFormat);
        this->footerLabel_->setText(u"Sent by %1 \u00B7 %2"_s.arg(
            pin->sender.formatted(mode).toHtmlEscaped(), sentAt));
    }

    this->progressTimer_->stop();
    this->countdownLabel_->hide();
    if (pin->endsAt.has_value() && pin->endsAt->isValid())
    {
        if (*pin->endsAt <= QDateTime::currentDateTimeUtc())
        {
            // already over, tickProgress() clears it right after this
            this->pinEndsAt_ = *pin->endsAt;
            this->tickProgress();
            this->hide();
            return;
        }
        this->pinEndsAt_ = *pin->endsAt;
        this->tickProgress();  // set initial text immediately
        this->progressTimer_->start();
    }

    const bool isMod = this->channel_->hasModRights();
    this->menuButton_->setVisible(isMod);

    // Chatterino9: the pin stays up until the user hides it. Hiding sticks
    // for that pin only, a different pinned message shows again.
    if (this->hiddenPinID_ != pin->messageID)
    {
        this->hiddenPinID_.clear();
        this->show();
        this->updateMessageHeightIfNeeded();
    }
}

void PinnedMessageWidget::toggleUserPinned()
{
    const auto *pin =
        this->channel_ ? this->channel_->getPinnedMessage() : nullptr;
    if (!this->isHidden() || !pin)
    {
        this->hiddenPinID_ = pin ? pin->messageID : QString{};
        this->hide();
    }
    else
    {
        this->hiddenPinID_.clear();
        this->show();
        this->updateMessageHeightIfNeeded();
    }
}

void PinnedMessageWidget::updateMessageHeight()
{
    if (!this->messageLabel_ || !this->messageScrollArea_)
    {
        return;
    }

    // Wrapped height of the label at the current viewport width.
    this->lastViewportWidth_ = this->messageScrollArea_->viewport()->width();
    int contentH =
        this->messageLabel_->heightForWidth(this->lastViewportWidth_);
    if (contentH <= 0)
    {
        contentH = this->messageLabel_->sizeHint().height();
    }

    // Size to content, but never taller than the cap.
    this->messageScrollArea_->setFixedHeight(
        qBound(1, contentH, this->messageMaxHeight_));
}

void PinnedMessageWidget::updateMessageHeightIfNeeded()
{
    if (this->lastViewportWidth_ !=
        this->messageScrollArea_->viewport()->width())
    {
        this->updateMessageHeight();
    }
}

void PinnedMessageWidget::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    this->updateMessageHeight();
}

void PinnedMessageWidget::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    this->visibilityChanged.invoke();
}

void PinnedMessageWidget::hideEvent(QHideEvent *event)
{
    BaseWidget::hideEvent(event);
    this->visibilityChanged.invoke();
}

void PinnedMessageWidget::scaleChangedEvent(float newScale)
{
    QFont headerFont = this->pinnedByLabel_->font();
    headerFont.setPointSizeF(9.5F * newScale);
    this->pinnedByLabel_->setFont(headerFont);
    this->countdownLabel_->setFont(headerFont);

    QFont bodyFont = this->messageLabel_->font();
    bodyFont.setPointSizeF(11.0F * newScale);
    this->messageLabel_->setFont(bodyFont);
    this->messageMaxHeight_ = int(110 * newScale);
    this->updateMessageHeight();

    QFont footerFont = this->footerLabel_->font();
    footerFont.setPointSizeF(9.0F * newScale);
    this->footerLabel_->setFont(footerFont);
}

void PinnedMessageWidget::mousePressEvent(QMouseEvent *event)
{
    // ignore to disable the parent's right click menu
}

bool PinnedMessageWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->messageScrollArea_->viewport() &&
        event->type() == QEvent::Resize && !this->heightUpdateQueued_ &&
        this->messageScrollArea_->viewport()->width() !=
            this->lastViewportWidth_)
    {
        // Resizing the scroll area from inside its own viewport resize
        // re-enters its layout, so do it right after this event instead.
        this->heightUpdateQueued_ = true;
        QTimer::singleShot(0, this, [this] {
            this->heightUpdateQueued_ = false;
            this->updateMessageHeightIfNeeded();
        });
    }
    return BaseWidget::eventFilter(watched, event);
}

}  // namespace chatterino
