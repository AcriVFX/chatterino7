// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/PredictionStripWidget.hpp"

#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Theme.hpp"

#include <QDesktopServices>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QUrl>

using namespace Qt::Literals;
using namespace std::chrono_literals;

namespace chatterino {

namespace {

const QColor BLUE(0x38, 0x7a, 0xff);
const QColor PINK(0xf5, 0x00, 0x9b);
const QColor MUTED(0xad, 0xad, 0xb8);

QColor outcomeColor(const QString &badge)
{
    if (badge.startsWith(u"pink"))
    {
        return PINK;
    }
    // predictions with more than two outcomes use blue-1 to blue-10:
    // alternate the shade so the segments stay apart
    auto index = badge.section('-', 1).toInt();
    return index % 2 == 0 ? BLUE.darker(135) : BLUE;
}

}  // namespace

PredictionStripWidget::PredictionStripWidget(QWidget *parent)
    : BaseWidget(parent)
    , expireTimer_(new QTimer(this))
{
    this->setMouseTracking(true);
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // the strip disappears a few minutes after the last badge
    this->expireTimer_->setInterval(15s);
    QObject::connect(this->expireTimer_, &QTimer::timeout, this, [this] {
        this->refresh();
    });

    this->scaleChangedEvent(this->scale());
    this->hide();
}

void PredictionStripWidget::setChannel(TwitchChannel *channel)
{
    this->signalHolder_.clear();
    this->channel_ = channel;

    if (channel)
    {
        this->signalHolder_.managedConnect(channel->seenPredictionChanged,
                                           [this] {
                                               this->refresh();
                                           });
    }

    this->refresh();
}

void PredictionStripWidget::refresh()
{
    const bool wasHidden = this->isHidden();
    if (this->channel_ == nullptr || this->channel_->seenPrediction().empty())
    {
        this->expireTimer_->stop();
        this->hide();
    }
    else
    {
        if (!this->expireTimer_->isActive())
        {
            this->expireTimer_->start();
        }
        this->show();
        this->update();
    }

    if (this->isHidden() != wasHidden)
    {
        this->visibilityChanged.invoke();
    }
}

void PredictionStripWidget::openPredictions(const QString &channelName)
{
    QDesktopServices::openUrl(
        QUrl(u"https://www.twitch.tv/popout/%1/chat"_s.arg(channelName)));
}

QSize PredictionStripWidget::sizeHint() const
{
    QFontMetrics fm(this->font());
    auto s = this->scale();
    return {100, int(6 * s) + fm.height() + int(5 * s) + int(18 * s) +
                     int(7 * s) + 1};
}

void PredictionStripWidget::scaleChangedEvent(float newScale)
{
    QFont f = this->font();
    f.setPointSizeF(9.0F * newScale);
    this->setFont(f);
    this->setFixedHeight(this->sizeHint().height());
}

void PredictionStripWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    auto *theme = getTheme();
    auto s = this->scale();

    painter.fillRect(this->rect(), theme->splits.header.background);
    painter.setPen(theme->splits.header.border);
    painter.drawLine(0, this->height() - 1, this->width() - 1,
                     this->height() - 1);

    if (!this->channel_)
    {
        return;
    }
    auto outcomes = this->channel_->seenPrediction();
    if (outcomes.empty())
    {
        return;
    }

    int total = 0;
    for (const auto &o : outcomes)
    {
        total += o.chatters;
    }

    const int pad = int(8 * s);
    QFont font = this->font();
    QFont bold = font;
    bold.setBold(true);
    QFontMetrics fm(font);
    QFontMetrics fmBold(bold);
    const int top = int(6 * s);
    const int lineH = fm.height();

    // header row: "Prediction · seen in chat"   N chatters  [Manage]
    int x = pad;
    painter.setFont(bold);
    painter.setPen(theme->splits.header.text);
    const QString label = u"Prediction"_s;
    painter.drawText(QRect(x, top, fmBold.horizontalAdvance(label), lineH),
                     Qt::AlignLeft | Qt::AlignVCenter, label);
    x += fmBold.horizontalAdvance(label);

    painter.setFont(font);
    painter.setPen(MUTED);
    const QString seen = u" · seen in chat"_s;
    painter.drawText(QRect(x, top, fm.horizontalAdvance(seen), lineH),
                     Qt::AlignLeft | Qt::AlignVCenter, seen);

    const QString manage = u"Manage"_s;
    const int manageW = fm.horizontalAdvance(manage) + int(16 * s);
    this->manageRect_ = QRect(this->width() - pad - manageW, top - int(2 * s),
                              manageW, lineH + int(4 * s));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(this->manageHovered_ ? QColor(0x4a, 0x4a, 0x53)
                                          : QColor(0x34, 0x34, 0x3b));
    painter.drawRoundedRect(this->manageRect_, 3 * s, 3 * s);
    painter.setPen(theme->splits.header.text);
    painter.drawText(this->manageRect_, Qt::AlignCenter, manage);

    const QString count =
        total == 1 ? u"1 chatter"_s : u"%1 chatters"_s.arg(total);
    const int countW = fm.horizontalAdvance(count);
    painter.setPen(MUTED);
    painter.drawText(QRect(this->manageRect_.left() - int(8 * s) - countW, top,
                           countW, lineH),
                     Qt::AlignRight | Qt::AlignVCenter, count);

    // bar: one segment per outcome, as wide as its share of chatters
    const QRect bar(pad, top + lineH + int(5 * s), this->width() - (2 * pad),
                    int(18 * s));
    QPainterPath clip;
    clip.addRoundedRect(bar, 3 * s, 3 * s);
    painter.setClipPath(clip);

    QFont barFont = bold;
    painter.setFont(barFont);
    QFontMetrics fmBar(barFont);
    int segX = bar.left();
    for (size_t i = 0; i < outcomes.size(); i++)
    {
        const auto &o = outcomes[i];
        int w = total > 0 ? int(double(bar.width()) * o.chatters / total) : 0;
        if (i + 1 == outcomes.size())
        {
            w = bar.right() + 1 - segX;  // no rounding gap at the end
        }
        QRect seg(segX, bar.top(), w, bar.height());
        painter.fillRect(seg, outcomeColor(o.badge));

        auto title = o.title.isEmpty() ? o.badge : o.title;
        auto text = u"%1 · %2"_s.arg(title).arg(o.chatters);
        auto inner = seg.adjusted(int(6 * s), 0, -int(6 * s), 0);
        text = fmBar.elidedText(text, Qt::ElideRight, inner.width());
        painter.setPen(Qt::white);
        // two outcomes: second label sits on the right like on Twitch
        auto align =
            (outcomes.size() == 2 && i == 1) ? Qt::AlignRight : Qt::AlignLeft;
        painter.drawText(inner, static_cast<int>(align | Qt::AlignVCenter),
                         text);
        segX += w;
    }
    painter.setClipping(false);
}

void PredictionStripWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && this->channel_ != nullptr &&
        this->manageRect_.contains(event->pos()))
    {
        PredictionStripWidget::openPredictions(this->channel_->getName());
    }
    // don't pass clicks on to the split
}

void PredictionStripWidget::mouseMoveEvent(QMouseEvent *event)
{
    bool hovered = this->manageRect_.contains(event->pos());
    if (hovered != this->manageHovered_)
    {
        this->manageHovered_ = hovered;
        this->setCursor(hovered ? Qt::PointingHandCursor : Qt::ArrowCursor);
        this->update();
    }
}

void PredictionStripWidget::leaveEvent(QEvent * /*event*/)
{
    if (this->manageHovered_)
    {
        this->manageHovered_ = false;
        this->setCursor(Qt::ArrowCursor);
        this->update();
    }
}

}  // namespace chatterino
