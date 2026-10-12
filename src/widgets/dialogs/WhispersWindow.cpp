#include "widgets/dialogs/WhispersWindow.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/LinkParser.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/WhisperHistory.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

namespace {

using namespace chatterino;

// Twitch cuts whispers to new people at 500 characters
constexpr int MAX_WHISPER_LENGTH = 500;

enum Role {
    PartnerRole = Qt::UserRole,
    LastTextRole,
    TimeRole,
    UnreadRole,
};

const QColor ACCENT(0x91, 0x47, 0xff);
const QColor TEXT(0xec, 0xec, 0xec);
const QColor MUTED(0x8b, 0x8b, 0x90);
const QColor SELECTED(0x2b, 0x2b, 0x30);
const QColor HOVER(0x1a, 0x1a, 0x1d);

QPointer<WhispersWindow> &instance()
{
    static QPointer<WhispersWindow> window;
    return window;
}

QString geometryPath()
{
    const auto &misc = getApp()->getPaths().miscDirectory;
    if (misc.isEmpty())
    {
        return {};
    }
    return QDir(misc).filePath(QStringLiteral("Whispers/window-geometry"));
}

/// "18:14" today, "Yesterday", else "10 Oct"
QString shortTime(qint64 msecs)
{
    const auto time = QDateTime::fromMSecsSinceEpoch(msecs);
    const auto today = QDate::currentDate();
    if (time.date() == today)
    {
        return time.toString(QStringLiteral("HH:mm"));
    }
    if (time.date() == today.addDays(-1))
    {
        return QStringLiteral("Yesterday");
    }
    if (time.date().year() == today.year())
    {
        return QLocale::c().toString(time.date(), QStringLiteral("d MMM"));
    }
    return QLocale::c().toString(time.date(), QStringLiteral("d MMM yyyy"));
}

/// "Today", "Yesterday", else "Saturday, 10 October 2026"
QString dayName(const QDate &date)
{
    const auto today = QDate::currentDate();
    if (date == today)
    {
        return QStringLiteral("Today");
    }
    if (date == today.addDays(-1))
    {
        return QStringLiteral("Yesterday");
    }
    return QLocale::c().toString(date, QStringLiteral("dddd, d MMMM yyyy"));
}

class ConversationDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex & /*index*/) const override
    {
        return {option.rect.width(), option.fontMetrics.height() * 2 + 18};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        const auto rect = option.rect;
        if (option.state & QStyle::State_Selected)
        {
            painter->fillRect(rect, SELECTED);
            painter->fillRect(QRect(rect.left(), rect.top(), 2, rect.height()),
                              ACCENT);
        }
        else if (option.state & QStyle::State_MouseOver)
        {
            painter->fillRect(rect, HOVER);
        }

        const auto inner = rect.adjusted(12, 8, -10, -8);
        const int lineHeight = option.fontMetrics.height();

        auto nameFont = option.font;
        nameFont.setBold(true);
        auto smallFont = option.font;
        smallFont.setPointSizeF(option.font.pointSizeF() * 0.88);
        const QFontMetrics smallMetrics(smallFont);

        // first line: name, time
        const auto time = index.data(TimeRole).toString();
        const int timeWidth = smallMetrics.horizontalAdvance(time);
        painter->setFont(smallFont);
        painter->setPen(MUTED);
        painter->drawText(QRect(inner.right() - timeWidth, inner.top(),
                                timeWidth, lineHeight),
                          Qt::AlignRight | Qt::AlignVCenter, time);

        painter->setFont(nameFont);
        painter->setPen(TEXT);
        const QRect nameRect(inner.left(), inner.top(),
                             inner.width() - timeWidth - 8, lineHeight);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(nameFont).elidedText(
                              index.data(Qt::DisplayRole).toString(),
                              Qt::ElideRight, nameRect.width()));

        // second line: last message, unread count
        const int unread = index.data(UnreadRole).toInt();
        int badgeWidth = 0;
        const int lineTop = inner.top() + lineHeight + 2;
        if (unread > 0)
        {
            const auto count = QString::number(unread);
            const int badgeHeight = smallMetrics.height() + 2;
            badgeWidth = std::max(badgeHeight,
                                  smallMetrics.horizontalAdvance(count) + 10);
            const QRect badge(inner.right() - badgeWidth,
                              lineTop + (lineHeight - badgeHeight) / 2,
                              badgeWidth, badgeHeight);
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(ACCENT);
            painter->drawRoundedRect(badge, badgeHeight / 2.0,
                                     badgeHeight / 2.0);
            painter->setPen(Qt::white);
            painter->setFont(smallFont);
            painter->drawText(badge, Qt::AlignCenter, count);
        }

        painter->setFont(option.font);
        painter->setPen(MUTED);
        const QRect lastRect(inner.left(), lineTop,
                             inner.width() - badgeWidth - (unread ? 8 : 0),
                             lineHeight);
        painter->drawText(
            lastRect, Qt::AlignLeft | Qt::AlignVCenter,
            option.fontMetrics.elidedText(index.data(LastTextRole).toString(),
                                          Qt::ElideRight, lastRect.width()));
        painter->restore();
    }
};

/// Adds the words of a saved whisper like chat shows them: emotes the
/// account can use, global BTTV/FFZ/7TV emotes, emojis and links
void addWords(MessageBuilder &b, const QString &text)
{
    auto *app = getApp();
    const auto &acc = app->getAccounts()->twitch.getCurrent();
    const auto &accemotes = *acc->accessEmotes();
    const auto *bttvemotes = app->getBttvEmotes();
    const auto *ffzemotes = app->getFfzEmotes();
    const auto *seventvemotes = app->getSeventvEmotes();

    for (const auto &word : text.split(u' ', Qt::SkipEmptyParts))
    {
        {
            auto it = accemotes->find({word});
            if (it != accemotes->end())
            {
                b.emplace<EmoteElement>(it->second, MessageElementFlag::Emote);
                continue;
            }
        }

        auto emote = bttvemotes->emote({word});
        if (!emote)
        {
            emote = ffzemotes->emote({word});
        }
        if (!emote)
        {
            emote = seventvemotes->globalEmote({word});
        }
        if (emote)
        {
            b.emplace<EmoteElement>(*emote, MessageElementFlag::Emote);
            continue;
        }

        for (auto &variant : app->getEmotes()->getEmojis()->parse(word))
        {
            if (const auto *emoji = std::get_if<EmotePtr>(&variant))
            {
                b.emplace<EmoteElement>(*emoji, MessageElementFlag::EmojiAll);
                continue;
            }
            const auto str = std::get<QStringView>(variant);
            if (auto link = linkparser::parse(str))
            {
                b.addLink(*link, str);
            }
            else
            {
                b.emplace<TextElement>(str.toString(),
                                       MessageElementFlag::Text);
            }
        }
    }
}

MessagePtr buildMessage(const whisperhistory::Entry &entry)
{
    MessageBuilder b;
    const auto time = QDateTime::fromMSecsSinceEpoch(entry.msecs);
    b.emplace<TimestampElement>(time.time());
    b.emplace<TextElement>(
        entry.name + ":", MessageElementFlag::Username,
        entry.color.isValid() ? MessageColor(entry.color) : MessageColor::Text,
        FontStyle::ChatMediumBold);
    addWords(b, entry.text);
    b->messageText = entry.text;
    b->searchText = entry.name + ": " + entry.text;
    b->serverReceivedTime = time;
    return b.release();
}

MessagePtr buildDaySeparator(const QDate &date)
{
    MessageBuilder b;
    b.emplace<TextElement>(dayName(date), MessageElementFlag::Text,
                           MessageColor::System);
    b->flags.set(MessageFlag::System);
    b->messageText = dayName(date);
    b->searchText = b->messageText;
    return b.release();
}

/// A split to open usercards from: the selected one, else any
Split *anySplit()
{
    auto &notebook = getApp()->getWindows()->getMainWindow().getNotebook();
    if (auto *page = notebook.getSelectedPage())
    {
        if (auto *split = page->getSelectedSplit())
        {
            return split;
        }
    }
    for (int i = 0; i < notebook.getPageCount(); i++)
    {
        auto *page = dynamic_cast<SplitContainer *>(notebook.getPageAt(i));
        if (page && !page->getSplits().empty())
        {
            return page->getSplits().front();
        }
    }
    return nullptr;
}

}  // namespace

namespace chatterino {

void WhispersWindow::showConversation(const QString &partner)
{
    auto &window = instance();
    if (!window)
    {
        window = new WhispersWindow();
    }

    window->reloadList();
    if (!partner.isEmpty())
    {
        window->openPartner(partner.toLower());
    }
    else if (window->partner_.isEmpty() && window->list_->count() > 0)
    {
        window->openPartner(
            window->list_->item(0)->data(PartnerRole).toString());
    }

    if (window->isMinimized())
    {
        window->showNormal();
    }
    window->show();
    window->raise();
    window->activateWindow();
    window->input_->setFocus();
}

WhispersWindow::WhispersWindow()
    : BaseWindow({
          BaseWindow::DisableCustomScaling,
          BaseWindow::DisableLayoutSave,
          BaseWindow::BoundsCheckOnShow,
      })
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle("Whispers");
    this->resize(1000, 680);

    const auto path = geometryPath();
    QFile geometry(path);
    if (!path.isEmpty() && geometry.open(QIODevice::ReadOnly))
    {
        this->restoreGeometry(geometry.readAll());
    }

    this->initUi();

    this->signalHolder_.managedConnect(
        whisperhistory::updated(), [this](const QString &partner) {
            this->reloadList();
            if (!partner.isEmpty() && partner == this->partner_)
            {
                // not from inside the signal that markRead fires again
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        this->markOpenRead();
                    },
                    Qt::QueuedConnection);
            }
        });

    // whispers as they arrive, shown as chat shows them
    this->signalHolder_.managedConnect(
        getApp()->getTwitch()->getWhispersChannel()->messageAppended,
        [this](MessagePtr &message,
               const std::optional<MessageFlags> & /*flags*/) {
            if (this->partner_.isEmpty() || !this->viewChannel_)
            {
                return;
            }
            if (whisperhistory::partnerOf(*message) == this->partner_)
            {
                this->viewChannel_->addMessage(message, MessageContext::Repost);
                return;
            }
            // errors about a whisper just sent from here
            const bool justSent =
                QDateTime::currentMSecsSinceEpoch() - this->lastSendMsecs_ <
                15000;
            if (justSent && message->flags.has(MessageFlag::System))
            {
                this->viewChannel_->addMessage(message, MessageContext::Repost);
            }
        });
}

void WhispersWindow::initUi()
{
    this->setStyleSheet(R"(
        * { font-family: "Segoe UI"; font-size: 13px; color: #ececec; }
        QWidget#root { background: #111111; }
        QWidget#side { background: #111111; border-right: 1px solid #2a2a2d; }
        QWidget#head { background: #1d1d1f; border-bottom: 1px solid #2a2a2d; }
        QLabel#name { font-weight: 600; }
        QLabel#dim { color: #8b8b90; }
        QLabel#counter { color: #8b8b90; font-size: 12px; }
        QLineEdit { background: #000000; border: 1px solid #2a2a2d;
            border-radius: 3px; padding: 6px 8px; }
        QLineEdit:focus { border-color: #9147ff; }
        QListWidget { background: #111111; border: none; outline: none; }
        QPushButton { background: transparent; color: #8b8b90;
            border: 1px solid #2a2a2d; border-radius: 3px; padding: 3px 9px; }
        QPushButton:hover { color: #ececec; border-color: #9147ff; }
        QPushButton:disabled { color: #4a4a4e; border-color: #222225; }
    )");

    auto *root = new QHBoxLayout(this->getLayoutContainer());
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    this->getLayoutContainer()->setObjectName("root");

    // conversation list
    auto *side = new QWidget;
    side->setObjectName("side");
    side->setFixedWidth(260);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 8, 0, 0);
    sideLayout->setSpacing(6);

    this->search_ = new QLineEdit;
    this->search_->setPlaceholderText("Search names");
    this->search_->setClearButtonEnabled(true);
    auto *searchBox = new QHBoxLayout;
    searchBox->setContentsMargins(8, 0, 8, 0);
    searchBox->addWidget(this->search_);
    sideLayout->addLayout(searchBox);

    this->list_ = new QListWidget;
    this->list_->setItemDelegate(new ConversationDelegate(this->list_));
    this->list_->setMouseTracking(true);
    this->list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    sideLayout->addWidget(this->list_, 1);

    this->empty_ = new QLabel("No whispers yet.\nThey show up here as they "
                              "arrive or when you send one.");
    this->empty_->setObjectName("dim");
    this->empty_->setWordWrap(true);
    this->empty_->setAlignment(Qt::AlignCenter);
    this->empty_->setContentsMargins(12, 12, 12, 12);
    sideLayout->addWidget(this->empty_);
    sideLayout->addStretch(0);

    root->addWidget(side);

    // open conversation
    auto *conv = new QWidget;
    auto *convLayout = new QVBoxLayout(conv);
    convLayout->setContentsMargins(0, 0, 0, 0);
    convLayout->setSpacing(0);

    auto *head = new QWidget;
    head->setObjectName("head");
    head->setFixedHeight(38);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(10, 0, 10, 0);
    this->usercardButton_ = new QPushButton("Usercard");
    this->nameLabel_ = new QLabel;
    this->nameLabel_->setObjectName("name");
    this->nameLabel_->setAlignment(Qt::AlignCenter);
    this->markReadButton_ = new QPushButton("Mark all read");
    // equal widths keep the name centered
    const int buttonWidth = std::max(this->usercardButton_->sizeHint().width(),
                                     this->markReadButton_->sizeHint().width());
    this->usercardButton_->setFixedWidth(buttonWidth);
    this->markReadButton_->setFixedWidth(buttonWidth);
    headLayout->addWidget(this->usercardButton_);
    headLayout->addWidget(this->nameLabel_, 1);
    headLayout->addWidget(this->markReadButton_);
    convLayout->addWidget(head);

    this->view_ = new ChannelView(this, ChannelView::Context::None, 5000);
    convLayout->addWidget(this->view_, 1);

    auto *inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(8, 8, 8, 8);
    inputRow->setSpacing(8);
    this->input_ = new QLineEdit;
    this->input_->setMaxLength(MAX_WHISPER_LENGTH);
    this->counter_ = new QLabel(QString("0/%1").arg(MAX_WHISPER_LENGTH));
    this->counter_->setObjectName("counter");
    inputRow->addWidget(this->input_, 1);
    inputRow->addWidget(this->counter_);
    convLayout->addLayout(inputRow);

    root->addWidget(conv, 1);

    QObject::connect(this->search_, &QLineEdit::textChanged, this, [this] {
        this->reloadList();
    });
    QObject::connect(this->list_, &QListWidget::itemClicked, this,
                     [this](QListWidgetItem *item) {
                         this->openPartner(item->data(PartnerRole).toString());
                         this->input_->setFocus();
                     });
    QObject::connect(this->usercardButton_, &QPushButton::clicked, this,
                     [this] {
                         this->openUsercard();
                     });
    QObject::connect(this->markReadButton_, &QPushButton::clicked, this, [] {
        whisperhistory::markAllRead();
    });
    QObject::connect(this->input_, &QLineEdit::returnPressed, this, [this] {
        this->sendInput();
    });
    QObject::connect(this->input_, &QLineEdit::textChanged, this,
                     [this](const QString &text) {
                         this->counter_->setText(QString("%1/%2").arg(
                             QString::number(text.size()),
                             QString::number(MAX_WHISPER_LENGTH)));
                     });

    this->openPartner({});
}

void WhispersWindow::reloadList()
{
    const auto filter = this->search_->text().trimmed().toLower();
    auto conversations = whisperhistory::conversations();

    // someone opened from a usercard who has no whispers yet
    if (!this->partner_.isEmpty() &&
        std::ranges::none_of(conversations, [this](const auto &c) {
            return c.partner == this->partner_;
        }))
    {
        whisperhistory::Conversation fresh;
        fresh.partner = this->partner_;
        fresh.displayName = this->partner_;
        conversations.insert(conversations.begin(), fresh);
    }

    const QSignalBlocker blocker(this->list_);
    this->list_->clear();
    for (const auto &c : conversations)
    {
        if (!filter.isEmpty() && !c.partner.contains(filter) &&
            !c.displayName.toLower().contains(filter))
        {
            continue;
        }
        auto *item = new QListWidgetItem(c.displayName);
        item->setData(PartnerRole, c.partner);
        item->setData(LastTextRole, c.lastOutgoing && !c.lastText.isEmpty()
                                        ? "You: " + c.lastText
                                        : c.lastText);
        item->setData(TimeRole,
                      c.lastMsecs > 0 ? shortTime(c.lastMsecs) : QString());
        item->setData(UnreadRole,
                      c.partner == this->partner_ && this->isActiveWindow()
                          ? 0
                          : c.unread);
        this->list_->addItem(item);
        if (c.partner == this->partner_)
        {
            item->setSelected(true);
            this->list_->setCurrentItem(item);
        }
    }
    this->empty_->setVisible(conversations.empty());
    this->markReadButton_->setEnabled(
        std::ranges::any_of(conversations, [](const auto &c) {
            return c.unread > 0;
        }));
}

void WhispersWindow::openPartner(const QString &partner)
{
    const bool changed = partner != this->partner_;
    this->partner_ = partner;

    QString display = partner;
    for (const auto &c : whisperhistory::conversations())
    {
        if (c.partner == partner)
        {
            display = c.displayName;
            break;
        }
    }
    this->nameLabel_->setText(display);
    this->setWindowTitle(partner.isEmpty() ? QString("Whispers")
                                           : "Whispers - " + display);
    this->usercardButton_->setEnabled(!partner.isEmpty());
    this->input_->setEnabled(!partner.isEmpty());
    this->input_->setPlaceholderText(partner.isEmpty() ? QString()
                                                       : "Whisper " + display);

    if (changed || !this->viewChannel_)
    {
        this->loadConversation();
    }
    this->markOpenRead();
    this->reloadList();
}

void WhispersWindow::loadConversation()
{
    this->viewChannel_ =
        std::make_shared<Channel>("Whispers window", Channel::Type::None);

    QDate lastDay;
    for (const auto &entry : whisperhistory::entries(this->partner_))
    {
        const auto day = QDateTime::fromMSecsSinceEpoch(entry.msecs).date();
        if (day != lastDay)
        {
            this->viewChannel_->addMessage(buildDaySeparator(day),
                                           MessageContext::Original);
            lastDay = day;
        }
        this->viewChannel_->addMessage(buildMessage(entry),
                                       MessageContext::Original);
    }
    this->view_->setChannel(this->viewChannel_);
}

void WhispersWindow::sendInput()
{
    const auto text = this->input_->text().trimmed();
    if (text.isEmpty() || this->partner_.isEmpty())
    {
        return;
    }
    this->lastSendMsecs_ = QDateTime::currentMSecsSinceEpoch();
    getApp()->getCommands()->execCommand(
        "/w " + this->partner_ + " " + text,
        getApp()->getTwitch()->getWhispersChannel(), false);
    this->input_->clear();
}

void WhispersWindow::openUsercard()
{
    auto *split = anySplit();
    if (this->partner_.isEmpty() || split == nullptr)
    {
        return;
    }
    auto *popup = new UserInfoPopup(getSettings()->autoCloseUserPopup, split);
    popup->setData(this->partner_, split->getChannel());
    popup->moveTo(QCursor::pos(), widgets::BoundsChecking::CursorPosition);
    popup->show();
}

void WhispersWindow::markOpenRead()
{
    if (!this->partner_.isEmpty() && this->isActiveWindow())
    {
        whisperhistory::markRead(this->partner_);
    }
}

void WhispersWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_F11)
    {
        if (this->isFullScreen())
        {
            this->showNormal();
        }
        else
        {
            this->showFullScreen();
        }
        return;
    }
    if (event->key() == Qt::Key_Escape && this->isFullScreen())
    {
        this->showNormal();
        return;
    }
    BaseWindow::keyPressEvent(event);
}

void WhispersWindow::closeEvent(QCloseEvent *event)
{
    const auto path = geometryPath();
    if (!path.isEmpty() && QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
        {
            file.write(this->saveGeometry());
        }
    }
    BaseWindow::closeEvent(event);
}

void WhispersWindow::changeEvent(QEvent *event)
{
    BaseWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && this->isActiveWindow())
    {
        this->markOpenRead();
        this->reloadList();
    }
}

}  // namespace chatterino
