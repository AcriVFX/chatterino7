// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayout.hpp"

#include "Application.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"

#include <QApplication>
#include <QDebug>
#include <QPainter>
#include <QtGlobal>
#include <QThread>

namespace chatterino {

namespace {

/// A lane tag or moderation chip drawn as a pill: rounded background, small
/// dark or light text, optional icon (the mod-action ban icon) in front
class LanePillLayoutElement : public TextLayoutElement
{
public:
    LanePillLayoutElement(MessageElement &creator, QString &text, QSizeF size,
                          QColor color, QColor background, ImagePtr icon,
                          bool centered, qreal radius, float scale)
        : TextLayoutElement(creator, text, size, color,
                            FontStyle::ChatMediumSmall, MessageColor::Text,
                            scale)
        , background_(std::move(background))
        , icon_(std::move(icon))
        , centered_(centered)
        , radius_(radius)
    {
    }

protected:
    void paint(QPainter &painter, const MessageColors & /*colors*/) override
    {
        const qreal pad = 5 * this->scale_;
        const qreal inset = 2 * this->scale_;
        QRectF rect = QRectF(this->getRect()).adjusted(0, inset, 0, -inset);

        if (this->background_.alpha() == 0)
        {
            return;  // spacer on untagged rows
        }

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(this->background_);
        painter.drawRoundedRect(rect, this->radius_ * this->scale_,
                                this->radius_ * this->scale_);

        qreal textX = rect.x() + pad;
        if (this->icon_)
        {
            auto pixmap = this->icon_->pixmapOrLoad();
            const qreal iconSize = rect.height() - 2 * this->scale_;
            if (pixmap)
            {
                painter.drawPixmap(
                    QRectF(textX, rect.center().y() - iconSize / 2, iconSize,
                           iconSize),
                    *pixmap, QRectF());
            }
            textX += iconSize + 3 * this->scale_;
        }

        painter.setPen(this->color_);
        painter.setFont(
            getApp()->getFonts()->getFont(this->style_, this->scale_));
        painter.drawText(
            QRectF(textX, rect.y(), rect.right() - textX, rect.height()),
            this->getText(),
            QTextOption((this->centered_ ? Qt::AlignHCenter : Qt::AlignLeft) |
                        Qt::AlignVCenter));
        painter.restore();
    }

    bool paintAnimated(QPainter & /*painter*/, qreal /*yOffset*/) override
    {
        // never a 7TV paint, even when the pill links to a user
        return false;
    }

private:
    QColor background_;
    ImagePtr icon_;
    bool centered_;
    qreal radius_;
};

class LanePillElement : public MessageElement
{
public:
    /// fixedWidth: as wide as the longest category tag, so every row of a
    /// lane starts its timestamp and message at the same x
    LanePillElement(QString text, QColor color, QColor background,
                    ImagePtr icon = nullptr, bool fixedWidth = false,
                    qreal radius = 3)
        : MessageElement(MessageElementFlag::HighlightLane)
        , text_(std::move(text))
        , color_(std::move(color))
        , background_(std::move(background))
        , icon_(std::move(icon))
        , fixedWidth_(fixedWidth)
        , radius_(radius)
    {
    }

    void addToContainer(MessageLayoutContainer &container,
                        const MessageLayoutContext &ctx) override
    {
        if (!ctx.flags.has(MessageElementFlag::HighlightLane))
        {
            return;
        }
        const float scale = container.getScale();
        auto *fonts = getApp()->getFonts();
        // as tall as a chat line, so the pill sits centered on the row
        const qreal height =
            fonts->getFontMetrics(FontStyle::ChatMedium, scale).height();
        const auto metrics =
            fonts->getFontMetrics(FontStyle::ChatMediumSmall, scale);
        QString text = this->text_;
        qreal textWidth = metrics.horizontalAdvance(text);
        if (this->fixedWidth_)
        {
            textWidth = metrics.horizontalAdvance(QStringLiteral("POLITICS"));
            text = metrics.elidedText(text, Qt::ElideRight,
                                      static_cast<int>(textWidth));
        }
        qreal width = textWidth + 10 * scale;
        if (this->icon_)
        {
            width += height - 4 * scale - 2 * scale + 3 * scale;
        }
        auto *element = new LanePillLayoutElement(
            *this, text, QSizeF(width, height), this->color_, this->background_,
            this->icon_, this->fixedWidth_, this->radius_, scale);
        element->setTrailingSpace(true);
        container.addElement(element);
    }

    std::unique_ptr<MessageElement> clone() const override
    {
        auto element = std::make_unique<LanePillElement>(
            this->text_, this->color_, this->background_, this->icon_,
            this->fixedWidth_, this->radius_);
        element->setLink(this->getLink());
        return element;
    }

    std::string_view type() const override
    {
        return "LanePillElement";
    }

private:
    QString text_;
    QColor color_;
    QColor background_;
    ImagePtr icon_;
    bool fixedWidth_;
    qreal radius_;
};

/// Repeat counter in lane-style splits, e.g. "8 in 49s ▸" or "17 in 2 min ▸"
QString repeatCounterOf(const Message &message)
{
    QString span =
        message.repeatSeconds < 90
            ? QString::number(message.repeatSeconds) + QStringLiteral("s")
            : QString::number((message.repeatSeconds + 30) / 60) +
                  QStringLiteral(" min");
    return QString::number(message.repeatCount) + QStringLiteral(" in ") +
           span + QStringLiteral(" \u25B8");
}

/// Solid tag background: the category color, lifted a quarter towards white
QColor pillColor(const QColor &highlight)
{
    auto lift = [](int c) {
        return std::min(255, static_cast<int>(c + (255 - c) * 0.25 + 0.5));
    };
    return {lift(highlight.red()), lift(highlight.green()),
            lift(highlight.blue())};
}

QColor blendColors(const QColor &base, const QColor &apply)
{
    const qreal &alpha = apply.alphaF();
    QColor result;
    result.setRgbF(base.redF() * (1 - alpha) + apply.redF() * alpha,
                   base.greenF() * (1 - alpha) + apply.greenF() * alpha,
                   base.blueF() * (1 - alpha) + apply.blueF() * alpha);
    return result;
}
}  // namespace

MessageLayout::MessageLayout(MessagePtr message)
    : message_(std::move(message))
{
    DebugCount::increase(DebugObject::MessageLayout);
}

MessageLayout::~MessageLayout()
{
    DebugCount::decrease(DebugObject::MessageLayout);
}

const Message *MessageLayout::getMessage()
{
    return this->message_.get();
}

const MessagePtr &MessageLayout::getMessagePtr() const
{
    return this->message_;
}

// Height
int MessageLayout::getHeight() const
{
    return static_cast<int>(this->container_.getHeight());
}

int MessageLayout::getWidth() const
{
    return static_cast<int>(this->container_.getWidth());
}

// Layout
// return true if redraw is required
bool MessageLayout::layout(const MessageLayoutContext &ctx,
                           bool shouldInvalidateBuffer)
{
    //    BenchmarkGuard benchmark("MessageLayout::layout()");

    bool layoutRequired = false;

    // check if width changed
    bool widthChanged = ctx.width != this->currentLayoutWidth_;
    layoutRequired |= widthChanged;
    this->currentLayoutWidth_ = ctx.width;

    // check if layout state changed
    const auto layoutGeneration = getApp()->getWindows()->getGeneration();
    if (this->layoutState_ != layoutGeneration)
    {
        layoutRequired = true;
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        this->layoutState_ = layoutGeneration;
    }

    // check if work mask changed
    layoutRequired |= this->currentWordFlags_ != ctx.flags;
    this->currentWordFlags_ = ctx.flags;  // getSettings()->getWordTypeMask();

    // check if a timeout chip was added since the last layout
    layoutRequired |= ctx.flags.has(MessageElementFlag::HighlightLane) &&
                      this->laidOutChip_ != this->message_->moderationChip;

    // check if layout was requested manually
    layoutRequired |= this->flags.has(MessageLayoutFlag::RequiresLayout);
    this->flags.unset(MessageLayoutFlag::RequiresLayout);

    // check if dpi changed
    layoutRequired |= this->scale_ != ctx.scale;
    this->scale_ = ctx.scale;
    layoutRequired |= this->imageScale_ != ctx.imageScale;
    this->imageScale_ = ctx.imageScale;

    if (!layoutRequired)
    {
        if (shouldInvalidateBuffer)
        {
            this->invalidateBuffer();
            return true;
        }
        return false;
    }

    qreal oldHeight = this->container_.getHeight();
    this->actuallyLayout(ctx);
    if (widthChanged || this->container_.getHeight() != oldHeight)
    {
        this->deleteBuffer();
    }
    this->invalidateBuffer();

    return true;
}

void MessageLayout::actuallyLayout(const MessageLayoutContext &ctx)
{
#ifdef FOURTF
    this->layoutCount_++;
#endif

    auto messageFlags = this->message_->flags;

    if (this->flags.has(MessageLayoutFlag::Expanded) ||
        (ctx.flags.has(MessageElementFlag::ModeratorTools) &&
         !this->message_->flags.has(MessageFlag::Disabled)))
    {
        messageFlags.unset(MessageFlag::Collapsed);
    }

    bool hideModerated = getSettings()->hideModerated;
    bool hideModerationActions = getSettings()->hideModerationActions;
    bool hideBlockedTermAutomodMessages =
        getSettings()->showBlockedTermAutomodMessages.getEnum() ==
        ShowModerationState::Never;
    bool hideSimilar = getSettings()->hideSimilar;
    bool hideReplies = !ctx.flags.has(MessageElementFlag::RepliedMessage);

    this->container_.beginLayout(ctx.width, this->scale_, this->imageScale_,
                                 messageFlags);

    const bool lane = ctx.flags.has(MessageElementFlag::HighlightLane);
    this->laneTag_.reset();
    this->laneChip_.reset();
    this->laidOutChip_ = this->message_->moderationChip;
    const bool tagged = lane &&
                        this->message_->flags.has(MessageFlag::Highlighted) &&
                        !this->flags.has(MessageLayoutFlag::IgnoreHighlights) &&
                        !this->message_->highlightTag.isEmpty() &&
                        this->message_->highlightColor;
    if (tagged)
    {
        this->laneTag_ = std::make_unique<LanePillElement>(
            this->message_->highlightTag, QColor(0x11, 0x11, 0x11),
            pillColor(*this->message_->highlightColor), nullptr, true);
    }
    else if (lane)
    {
        // same room on untagged rows, so all rows of a lane line up
        this->laneTag_ = std::make_unique<LanePillElement>(
            QString(), QColor(), QColor(Qt::transparent), nullptr, true);
    }
    this->laneRepeat_.reset();
    if (tagged && this->message_->repeatCount > 0)
    {
        // one row per spammer (see ChannelView), the counter opens the usercard
        // round teal pill from the approved repeat mockup (C+D)
        this->laneRepeat_ = std::make_unique<LanePillElement>(
            repeatCounterOf(*this->message_), QColor(Qt::white),
            QColor(0x28, 0xa0, 0x8c), nullptr, false, 8);
        this->laneRepeat_->setLink({Link::UserInfo, this->message_->loginName});
    }
    if (lane && !this->message_->moderationChip.isEmpty())
    {
        ImagePtr icon;
        if (this->message_->moderationChip.startsWith(u"BAN"))
        {
            icon = Image::fromResourcePixmap(getResources().buttons.ban);
        }
        this->laneChip_ = std::make_unique<LanePillElement>(
            this->message_->moderationChip, QColor(0xdd, 0xdd, 0xdd),
            QColor(0x3a, 0x3a, 0x40), std::move(icon));
    }

    // the tag goes right after the timestamp (Twitch messages start with a
    // hidden channel name, so the timestamp is not always the first element)
    const MessageElement *tagAfter = nullptr;
    for (const auto &element : this->message_->elements)
    {
        if (element->getFlags().has(MessageElementFlag::Timestamp))
        {
            tagAfter = element.get();
            break;
        }
    }
    if (this->laneTag_ && tagAfter == nullptr)
    {
        this->laneTag_->addToContainer(this->container_, ctx);
    }
    for (const auto &element : this->message_->elements)
    {
        if (hideModerated && this->message_->flags.has(MessageFlag::Disabled))
        {
            continue;
        }

        if (hideBlockedTermAutomodMessages &&
            this->message_->flags.has(MessageFlag::AutoModBlockedTerm))
        {
            // NOTE: This hides the message but it will make the message re-appear if moderation message hiding is no longer active, and the layout is re-laid-out.
            // This is only the case for the moderation messages that don't get filtered during creation.
            // We should decide which is the correct method & apply that everywhere
            continue;
        }

        if (this->message_->flags.has(MessageFlag::RestrictedMessage))
        {
            if (getApp()->getStreamerMode()->shouldHideRestrictedUsers())
            {
                // Message is being hidden because the source is a
                // restricted user
                continue;
            }
        }

        if (this->message_->flags.has(MessageFlag::ModerationAction))
        {
            if (hideModerationActions ||
                getApp()->getStreamerMode()->shouldHideModActions())
            {
                // Message is being hidden because we consider the message
                // a moderation action (something a streamer is unlikely to
                // want to share if they briefly show their chat on stream)
                continue;
            }
        }

        if (hideSimilar && this->message_->flags.has(MessageFlag::Similar))
        {
            continue;
        }

        if (hideReplies &&
            element->getFlags().has(MessageElementFlag::RepliedMessage))
        {
            continue;
        }

        element->addToContainer(this->container_, ctx);
        if (this->laneTag_ && element.get() == tagAfter)
        {
            this->laneTag_->addToContainer(this->container_, ctx);
        }
    }

    if (this->laneRepeat_)
    {
        this->laneRepeat_->addToContainer(this->container_, ctx);
    }
    if (this->laneChip_ &&
        !(hideModerated && this->message_->flags.has(MessageFlag::Disabled)))
    {
        this->laneChip_->addToContainer(this->container_, ctx);
    }

    if (this->height_ != this->container_.getHeight())
    {
        this->deleteBuffer();
    }

    this->container_.endLayout();
    this->height_ = this->container_.getHeight();

    // collapsed state
    this->flags.unset(MessageLayoutFlag::Collapsed);
    if (this->container_.isCollapsed())
    {
        this->flags.set(MessageLayoutFlag::Collapsed);
    }
}

// Painting
MessagePaintResult MessageLayout::paint(const MessagePaintContext &ctx)
{
    MessagePaintResult result;

    QPixmap *pixmap = this->ensureBuffer(ctx.painter, ctx.canvasWidth,
                                         ctx.messageColors.hasTransparency);

    if (!this->bufferValid_)
    {
        if (ctx.messageColors.hasTransparency)
        {
            pixmap->fill(Qt::transparent);
        }
        this->updateBuffer(pixmap, ctx);
    }

    // draw on buffer
    ctx.painter.drawPixmap(QPoint{0, ctx.y}, *pixmap);

    // draw gif emotes
    result.hasAnimatedElements =
        this->container_.paintAnimatedElements(ctx.painter, ctx.y);

    // draw disabled
    if (this->message_->flags.has(MessageFlag::Disabled))
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);

        // lane-style splits also cross out a punished message (the chip says
        // why); painted here, not into the buffer, like the overlay above
        if (this->currentWordFlags_.has(MessageElementFlag::HighlightLane))
        {
            ctx.painter.save();
            ctx.painter.translate(0, ctx.y);
            this->container_.paintStrikeout(ctx.painter,
                                            QColor(0x9a, 0x9a, 0xa3));
            ctx.painter.restore();
        }
    }

    if (this->message_->flags.has(MessageFlag::RecentMessage) &&
        ctx.preferences.fadeMessageHistory)
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);
    }

    if (!ctx.isMentions &&
        (this->message_->flags.has(MessageFlag::RedeemedChannelPointReward) ||
         this->message_->flags.has(MessageFlag::RedeemedHighlight)) &&
        ctx.preferences.enableRedeemedHighlight)
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                static_cast<int>(this->scale_ * 4),
                pixmap->height(),
            },
            *ColorProvider::instance().color(ColorType::RedeemedHighlight));
    }

    // draw selection
    if (!ctx.selection.isEmpty())
    {
        this->container_.paintSelection(ctx.painter, ctx.messageIndex,
                                        ctx.selection, ctx.y);
    }

    // draw message seperation line
    if (ctx.preferences.separateMessages)
    {
        ctx.painter.fillRect(
            QRectF{
                0.0,
                static_cast<qreal>(ctx.y),
                this->container_.getWidth() + 64,
                1.0,
            },
            ctx.messageColors.messageSeperator);
    }

    // draw last read message line
    if (ctx.isLastReadMessage)
    {
        QColor color;
        if (ctx.preferences.lastMessageColor.isValid())
        {
            color = ctx.preferences.lastMessageColor;
        }
        else
        {
            color = ctx.isWindowFocused
                        ? ctx.messageColors.focusedLastMessageLine
                        : ctx.messageColors.unfocusedLastMessageLine;
        }

        QBrush brush(color, ctx.preferences.lastMessagePattern);

        ctx.painter.fillRect(
            QRectF{
                0,
                ctx.y + this->container_.getHeight() - 1,
                static_cast<qreal>(pixmap->width()),
                1,
            },
            brush);
    }

    this->bufferValid_ = true;

    return result;
}

QPixmap *MessageLayout::ensureBuffer(QPainter &painter, qreal width, bool clear)
{
    if (this->buffer_ != nullptr)
    {
        return this->buffer_.get();
    }

    // Create new buffer
    this->buffer_ = std::make_unique<QPixmap>(
        static_cast<int>(width * painter.device()->devicePixelRatioF()),
        static_cast<int>(this->container_.getHeight() *
                         painter.device()->devicePixelRatioF()));
    this->buffer_->setDevicePixelRatio(painter.device()->devicePixelRatioF());

    if (clear)
    {
        this->buffer_->fill(Qt::transparent);
    }

    this->bufferValid_ = false;
    DebugCount::increase(DebugObject::MessageDrawingBuffer);
    return this->buffer_.get();
}

void MessageLayout::updateBuffer(QPixmap *buffer,
                                 const MessagePaintContext &ctx)
{
    if (buffer->isNull())
    {
        return;
    }

    QPainter painter(buffer);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // draw background
    QColor backgroundColor = [&] {
        if (ctx.preferences.alternateMessages &&
            this->flags.has(MessageLayoutFlag::AlternateBackground))
        {
            return ctx.messageColors.alternateBg;
        }

        return ctx.messageColors.regularBg;
    }();

    if (this->message_->flags.has(MessageFlag::ElevatedMessage) &&
        ctx.preferences.enableElevatedMessageHighlight)
    {
        backgroundColor = blendColors(
            backgroundColor,
            *ctx.colorProvider.color(ColorType::ElevatedMessageHighlight));
    }

    else if (this->message_->flags.has(MessageFlag::FirstMessage) &&
             ctx.preferences.enableFirstMessageHighlight)
    {
        backgroundColor = blendColors(
            backgroundColor,
            *ctx.colorProvider.color(ColorType::FirstMessageHighlight));
    }
    else if (this->message_->flags.has(MessageFlag::WatchStreak) &&
             ctx.preferences.enableWatchStreakHighlight)
    {
        backgroundColor = blendColors(
            backgroundColor, *ctx.colorProvider.color(ColorType::WatchStreak));
    }
    else if ((this->message_->flags.has(MessageFlag::Highlighted) ||
              this->message_->flags.has(MessageFlag::HighlightedWhisper)) &&
             !this->flags.has(MessageLayoutFlag::IgnoreHighlights))
    {
        assert(this->message_->highlightColor);
        if (this->message_->highlightColor)
        {
            // Blend highlight color with usual background color
            backgroundColor =
                blendColors(backgroundColor, *this->message_->highlightColor);
        }
    }
    else if (this->message_->flags.has(MessageFlag::Subscription) &&
             ctx.preferences.enableSubHighlight)
    {
        // Blend highlight color with usual background color
        backgroundColor = blendColors(
            backgroundColor, *ctx.colorProvider.color(ColorType::Subscription));
    }
    else if ((this->message_->flags.has(MessageFlag::RedeemedHighlight) ||
              this->message_->flags.has(
                  MessageFlag::RedeemedChannelPointReward)) &&
             ctx.preferences.enableRedeemedHighlight)
    {
        // Blend highlight color with usual background color
        backgroundColor =
            blendColors(backgroundColor,
                        *ctx.colorProvider.color(ColorType::RedeemedHighlight));
    }
    else if (this->message_->flags.has(MessageFlag::AutoMod) ||
             this->message_->flags.has(MessageFlag::LowTrustUsers))
    {
        if (ctx.preferences.enableAutomodHighlight &&
            (this->message_->flags.has(MessageFlag::AutoModOffendingMessage) ||
             this->message_->flags.has(
                 MessageFlag::AutoModOffendingMessageHeader)))
        {
            backgroundColor = blendColors(
                backgroundColor,
                *ctx.colorProvider.color(ColorType::AutomodHighlight));
        }
        else
        {
            backgroundColor = QColor("#404040");
        }
    }
    else if (this->message_->flags.has(MessageFlag::Debug))
    {
        backgroundColor = QColor("#4A273D");
    }

    painter.fillRect(buffer->rect(), backgroundColor);

    // lane-style splits: a solid stripe in the highlight color on the left
    if (this->currentWordFlags_.has(MessageElementFlag::HighlightLane) &&
        this->message_->flags.has(MessageFlag::Highlighted) &&
        !this->flags.has(MessageLayoutFlag::IgnoreHighlights) &&
        this->message_->highlightColor)
    {
        painter.fillRect(
            QRectF{0, 0, 4 * this->scale_, this->container_.getHeight()},
            pillColor(*this->message_->highlightColor));
    }

    // lane-style splits mark the words the highlight rule matched
    if (this->currentWordFlags_.has(MessageElementFlag::HighlightLane) &&
        this->message_->flags.has(MessageFlag::Highlighted) &&
        !this->flags.has(MessageLayoutFlag::IgnoreHighlights) &&
        this->message_->highlightColor &&
        !this->message_->highlightMatch.isEmpty())
    {
        QColor mark = *this->message_->highlightColor;
        mark.setAlphaF(std::max<float>(mark.alphaF(), 0.45F));
        this->container_.paintWordMarks(painter, this->message_->highlightMatch,
                                        mark);
    }

    // draw message
    this->container_.paintElements(painter, ctx);

#ifdef FOURTF
    // debug
    painter.setPen(QColor(255, 0, 0));
    painter.drawRect(buffer->rect().x(), buffer->rect().y(),
                     buffer->rect().width() - 1, buffer->rect().height() - 1);

    QTextOption option;
    option.setAlignment(Qt::AlignRight | Qt::AlignTop);

    painter.drawText(QRectF(1, 1, this->container_.getWidth() - 3, 1000),
                     QString::number(this->layoutCount_) + ", " +
                         QString::number(++this->bufferUpdatedCount_),
                     option);
#endif
}

void MessageLayout::invalidateBuffer()
{
    this->bufferValid_ = false;
}

void MessageLayout::deleteBuffer()
{
    if (this->buffer_ != nullptr)
    {
        DebugCount::decrease(DebugObject::MessageDrawingBuffer);

        this->buffer_ = nullptr;
    }
}

void MessageLayout::deleteCache()
{
    this->deleteBuffer();

#ifdef XD
    this->container_.clear();
#endif
}

// Elements
//    assert(QThread::currentThread() == QApplication::instance()->thread());

// returns nullptr if none was found

// fourtf: this should return a MessageLayoutItem
const MessageLayoutElement *MessageLayout::getElementAt(QPointF point) const
{
    // go through all words and return the first one that contains the point.
    return this->container_.getElementAt(point);
}

std::pair<int, int> MessageLayout::getWordBounds(
    const MessageLayoutElement *hoveredElement, QPointF relativePos) const
{
    // An element with wordId != -1 can be multiline, so we need to check all
    // elements in the container
    if (hoveredElement->getWordId() != -1)
    {
        return this->container_.getWordBounds(hoveredElement);
    }

    const auto wordStart = this->getSelectionIndex(relativePos) -
                           hoveredElement->getMouseOverIndex(relativePos);
    const auto selectionLength = hoveredElement->getSelectionIndexCount();
    const auto length = hoveredElement->hasTrailingSpace() ? selectionLength - 1
                                                           : selectionLength;

    return {wordStart, wordStart + length};
}

size_t MessageLayout::getLastCharacterIndex() const
{
    return this->container_.getLastCharacterIndex();
}

size_t MessageLayout::getFirstMessageCharacterIndex() const
{
    return this->container_.getFirstMessageCharacterIndex();
}

size_t MessageLayout::getSelectionIndex(QPointF position) const
{
    return this->container_.getSelectionIndex(position);
}

void MessageLayout::addSelectionText(QString &str, uint32_t from, uint32_t to,
                                     CopyMode copymode)
{
    this->container_.addSelectionText(str, from, to, copymode);
}

}  // namespace chatterino
