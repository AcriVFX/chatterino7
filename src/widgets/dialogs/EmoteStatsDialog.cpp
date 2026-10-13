// Chatterino9: see EmoteStatsDialog.hpp

#include "widgets/dialogs/EmoteStatsDialog.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/ModActivity.hpp"
#include "util/PostToThread.hpp"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>

namespace chatterino {

namespace {

using emotestats::Source;

constexpr qint64 MINUTE_MS = 60LL * 1000;
constexpr qint64 HOUR_MS = 60 * MINUTE_MS;
constexpr qint64 DAY_MS = 24 * HOUR_MS;
/// A stream seen live this recently counts as still running
constexpr qint64 LIVE_WINDOW_MS = 12 * MINUTE_MS;
/// Messages shortly after the last "seen live" still belong to the stream
constexpr qint64 STREAM_TAIL_MS = 5 * MINUTE_MS;
/// The "Not used" box names at most this many emotes
constexpr int MAX_UNUSED_NAMES = 60;

constexpr int SORT_ROLE = Qt::UserRole;
constexpr int KEY_ROLE = Qt::UserRole + 1;
constexpr int SHARE_ROLE = Qt::UserRole + 2;

// colors of the approved mockup
constexpr QColor WINDOW_COLOR(0x16, 0x16, 0x16);
constexpr QColor ROW_COLOR(0x1b, 0x1b, 0x1b);
constexpr QColor ROW_ALT_COLOR(0x1f, 0x1f, 0x1f);
constexpr QColor HOVER_COLOR(0x26, 0x2a, 0x30);
constexpr QColor TEXT_COLOR(0xe4, 0xe4, 0xe4);
constexpr QColor DIM_COLOR(0x8f, 0x8f, 0x8f);
constexpr QColor TRACK_COLOR(0x26, 0x26, 0x26);
constexpr QColor BAR_COLOR(0x3b, 0x6f, 0xa8);
constexpr QColor TILE_COLOR(0x2b, 0x2b, 0x2b);
constexpr QColor TILE_TEXT_COLOR(0xcf, 0xcf, 0xcf);

constexpr std::array<const char *, 4> RANGE_NAMES{
    "This stream",
    "7 days",
    "30 days",
    "All time",
};

/// Source buttons, "All" last
constexpr std::array<const char *, 5> SOURCE_NAMES{
    "7TV", "Twitch", "BTTV", "FFZ", "All",
};
constexpr std::array<Source, 4> SOURCE_ORDER{
    Source::SevenTv,
    Source::Twitch,
    Source::Bttv,
    Source::Ffz,
};

const QLocale &numberLocale()
{
    static const QLocale locale(QLocale::English, QLocale::UnitedStates);
    return locale;
}

QString formatCount(qint64 n)
{
    return numberLocale().toString(n);
}

QString formatShare(qint64 part, qint64 total)
{
    if (part <= 0 || total <= 0)
    {
        return QStringLiteral("0%");
    }
    const double pct =
        100.0 * static_cast<double>(part) / static_cast<double>(total);
    if (pct < 0.1)
    {
        return QStringLiteral("<0.1%");
    }
    return numberLocale().toString(pct, 'f', 1) + u'%';
}

QString formatDay(qint64 msecs)
{
    return numberLocale().toString(
        QDateTime::fromMSecsSinceEpoch(msecs).toLocalTime().date(),
        QStringLiteral("MMM d"));
}

QString formatTime(qint64 msecs)
{
    return QDateTime::fromMSecsSinceEpoch(msecs).toLocalTime().toString(
        QStringLiteral("HH:mm"));
}

QString rowKey(Source source, const QString &name)
{
    return emotestats::sourceKey(source) + u'\t' + name;
}

QString viewFile()
{
    const auto &misc = getApp()->getPaths().miscDirectory;
    return misc.isEmpty()
               ? QString()
               : QDir(misc).filePath(QStringLiteral("EmoteStats/view.json"));
}

class SortItem : public QTableWidgetItem
{
public:
    using QTableWidgetItem::QTableWidgetItem;
    SortItem(const SortItem &) = delete;
    SortItem(SortItem &&) = delete;
    SortItem &operator=(const SortItem &) = delete;
    SortItem &operator=(SortItem &&) = delete;
    ~SortItem() override = default;

    bool operator<(const QTableWidgetItem &other) const override
    {
        auto a = this->data(SORT_ROLE);
        auto b = other.data(SORT_ROLE);
        if (a.isValid() && b.isValid())
        {
            return a.toDouble() < b.toDouble();
        }
        return this->text().compare(other.text(), Qt::CaseInsensitive) < 0;
    }
};

/// Draws the bar and the percentage of "Share of chat"
class ShareDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        constexpr int textWidth = 48;
        constexpr int maxTrack = 120;

        painter->save();
        if (option.state.testFlag(QStyle::State_Selected))
        {
            painter->fillRect(option.rect, option.palette.highlight());
        }
        auto rect = option.rect.adjusted(8, 0, -8, 0);
        QRect textRect(rect.right() - textWidth + 1, rect.top(), textWidth,
                       rect.height());
        painter->setPen(TEXT_COLOR);
        painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter,
                          index.data().toString());

        const int track = std::clamp(rect.width() - textWidth - 8, 0, maxTrack);
        if (track >= 30)
        {
            QRect trackRect(textRect.left() - 8 - track, rect.center().y() - 4,
                            track, 8);
            painter->fillRect(trackRect, TRACK_COLOR);
            const auto share =
                std::clamp(index.data(SHARE_ROLE).toDouble(), 0.0, 1.0);
            const int fill = static_cast<int>(std::lround(share * track));
            if (fill > 0)
            {
                painter->fillRect(
                    QRect(trackRect.left(), trackRect.top(), fill, 8),
                    BAR_COLOR);
            }
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        return {200, std::max(size.height(), 30)};
    }
};

}  // namespace

namespace emotestats_ui {

/// Draws the emote's picture in front of its name, or a gray tile with its
/// first letters while the picture loads or when there is none
class EmoteDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setEmotes(QHash<QString, EmotePtr> emotes)
    {
        this->emotes_ = std::move(emotes);
    }

    /// A picture was still loading at the last paint
    bool takePending() const
    {
        return std::exchange(this->pending_, false);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        constexpr int size = 24;
        constexpr int maxWidth = 56;

        painter->save();
        if (option.state.testFlag(QStyle::State_Selected))
        {
            painter->fillRect(option.rect, option.palette.highlight());
        }
        const auto name = index.data().toString();
        const int left = option.rect.left() + 8;
        const int top = option.rect.center().y() - (size / 2);

        std::optional<QPixmap> pixmap;
        auto it = this->emotes_.constFind(index.data(KEY_ROLE).toString());
        if (it != this->emotes_.constEnd() && *it)
        {
            const auto &image = (*it)->images.getImage(1);
            if (image && !image->isEmpty())
            {
                pixmap = image->pixmapOrLoad();
                if (!pixmap)
                {
                    this->pending_ = true;
                }
            }
        }

        int iconWidth = size;
        if (pixmap && !pixmap->isNull() && pixmap->height() > 0)
        {
            iconWidth = std::clamp(
                static_cast<int>(
                    std::lround(static_cast<double>(pixmap->width()) * size /
                                static_cast<double>(pixmap->height()))),
                1, maxWidth);
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            painter->drawPixmap(QRect(left, top, iconWidth, size), *pixmap);
        }
        else
        {
            QPainterPath tile;
            tile.addRoundedRect(QRectF(left, top, size, size), 3, 3);
            painter->setRenderHint(QPainter::Antialiasing);
            painter->fillPath(tile, TILE_COLOR);
            auto font = option.font;
            font.setPixelSize(10);
            font.setBold(true);
            painter->setFont(font);
            painter->setPen(TILE_TEXT_COLOR);
            painter->drawText(QRect(left, top, size, size), Qt::AlignCenter,
                              name.left(2));
            painter->setFont(option.font);
        }

        const QRect textRect(left + iconWidth + 8, option.rect.top(),
                             option.rect.right() - left - iconWidth - 8,
                             option.rect.height());
        painter->setPen(TEXT_COLOR);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                          option.fontMetrics.elidedText(name, Qt::ElideRight,
                                                        textRect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        return {size.width() + 72, std::max(size.height(), 30)};
    }

private:
    QHash<QString, EmotePtr> emotes_;
    mutable bool pending_ = false;
};

}  // namespace emotestats_ui

EmoteStatsDialog::EmoteStatsDialog(const std::shared_ptr<Channel> &channel,
                                   QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::DisableCustomScaling,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , channel_(channel->getName().toLower())
    , weakChannel_(channel)
    , refreshTimer_(new QTimer(this))
    , iconTimer_(new QTimer(this))
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle("Emote stats - " + this->channel_);
    this->resize(780, 820);

    this->loadView();
    this->initUi();
    this->themeChangedEvent();

    // new numbers from the server or the log import
    this->refreshTimer_->setSingleShot(true);
    this->refreshTimer_->setInterval(std::chrono::milliseconds(1500));
    QObject::connect(this->refreshTimer_, &QTimer::timeout, this, [this] {
        this->reload();
    });
    this->signalHolder_.managedConnect(emotestats::updated(), [this] {
        this->updateFooter();
        if (!this->refreshTimer_->isActive())
        {
            this->refreshTimer_->start();
        }
    });

    // live counts
    auto *tick = new QTimer(this);
    tick->setInterval(std::chrono::minutes(1));
    QObject::connect(tick, &QTimer::timeout, this, [this] {
        this->reload();
    });
    tick->start();

    // numbers the mod tracker server counted while the app was closed
    auto *syncTimer = new QTimer(this);
    syncTimer->setInterval(std::chrono::minutes(3));
    QObject::connect(syncTimer, &QTimer::timeout, this, [this] {
        emotestats::syncFromServer(this->channel_);
    });
    syncTimer->start();
    emotestats::syncFromServer(this->channel_);

    // repaints while emote pictures load
    this->iconTimer_->setInterval(std::chrono::milliseconds(400));
    QObject::connect(this->iconTimer_, &QTimer::timeout, this, [this] {
        if (this->ui_.emoteDelegate->takePending())
        {
            this->ui_.table->viewport()->update();
        }
    });
    this->iconTimer_->start();

    this->reload();
}

void EmoteStatsDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    QPalette palette;
    palette.setColor(QPalette::Window, WINDOW_COLOR);
    palette.setColor(QPalette::WindowText, TEXT_COLOR);
    palette.setColor(QPalette::Base, ROW_COLOR);
    palette.setColor(QPalette::AlternateBase, ROW_ALT_COLOR);
    palette.setColor(QPalette::Text, TEXT_COLOR);
    palette.setColor(QPalette::Button, WINDOW_COLOR);
    palette.setColor(QPalette::ButtonText, TEXT_COLOR);
    palette.setColor(QPalette::Highlight, HOVER_COLOR);
    palette.setColor(QPalette::HighlightedText, TEXT_COLOR);
    palette.setColor(QPalette::ToolTipBase, ROW_ALT_COLOR);
    palette.setColor(QPalette::ToolTipText, TEXT_COLOR);
    this->setPalette(palette);
}

void EmoteStatsDialog::initUi()
{
    this->setStyleSheet(R"(
        * { font-family: "Segoe UI"; font-size: 13px; }
        QLabel { color: #e4e4e4; }
        QLabel#dim { color: #8f8f8f; }
        QLabel#foot { color: #8f8f8f; font-size: 12px; }
        QLabel#warn { color: #c9a24a; font-weight: 600; }
        QFrame#segments { border: 1px solid #2c2c2c; }
        QPushButton#seg, QPushButton#segFirst { background: transparent;
            color: #8f8f8f; border: none; border-left: 1px solid #2c2c2c;
            padding: 4px 10px; }
        QPushButton#segFirst { border-left: none; }
        QPushButton#seg:hover, QPushButton#segFirst:hover { color: #e4e4e4; }
        QPushButton#seg:checked, QPushButton#segFirst:checked {
            background: #24384f; color: #e4e4e4; }
        QLineEdit { background: #101010; color: #e4e4e4;
            border: 1px solid #2c2c2c; padding: 4px 8px; }
        QLineEdit:focus { border-color: #4a8bd6; }
        QTableWidget { background: #1b1b1b; alternate-background-color: #1f1f1f;
            color: #e4e4e4; border: 1px solid #2c2c2c; gridline-color: #1b1b1b; }
        QTableWidget::item:hover { background: #262a30; }
        QHeaderView::section { background: #1c1c1c; color: #8f8f8f;
            border: none; border-bottom: 1px solid #2c2c2c;
            padding: 5px 8px; font-weight: 600; }
        QFrame#unused { border: 1px solid #2c2c2c; }
        QFrame#unused QLabel { border: none; }
    )");

    auto *root = new QVBoxLayout(this->getLayoutContainer());
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto makeSegments = [this](const auto &names, int checked,
                               QButtonGroup *&group) {
        auto *frame = new QFrame;
        frame->setObjectName("segments");
        auto *layout = new QHBoxLayout(frame);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        group = new QButtonGroup(this);
        int i = 0;
        for (const auto *name : names)
        {
            auto *button = new QPushButton(name);
            button->setObjectName(i == 0 ? "segFirst" : "seg");
            button->setCheckable(true);
            button->setChecked(i == checked);
            button->setCursor(Qt::PointingHandCursor);
            group->addButton(button, i);
            layout->addWidget(button);
            i++;
        }
        return frame;
    };

    // range, source, search
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(16);
        row->addWidget(
            makeSegments(RANGE_NAMES, this->rangeIndex_, this->ui_.ranges));
        row->addWidget(
            makeSegments(SOURCE_NAMES, this->sourceIndex_, this->ui_.sources));
        this->ui_.search = new QLineEdit;
        this->ui_.search->setPlaceholderText("Search emotes");
        this->ui_.search->setClearButtonEnabled(true);
        row->addWidget(this->ui_.search, 1);
        root->addLayout(row);

        QObject::connect(this->ui_.ranges, &QButtonGroup::idClicked, this,
                         [this](int id) {
                             if (id == this->rangeIndex_)
                             {
                                 return;
                             }
                             this->rangeIndex_ = id;
                             this->saveView();
                             this->reload();
                         });
        QObject::connect(this->ui_.sources, &QButtonGroup::idClicked, this,
                         [this](int id) {
                             if (id == this->sourceIndex_)
                             {
                                 return;
                             }
                             this->sourceIndex_ = id;
                             this->saveView();
                             this->fillTable();
                         });
        QObject::connect(this->ui_.search, &QLineEdit::textChanged, this,
                         [this] {
                             this->applySearch();
                         });
    }

    this->ui_.summary = new QLabel;
    this->ui_.summary->setObjectName("dim");
    this->ui_.summary->setTextFormat(Qt::RichText);
    root->addWidget(this->ui_.summary);

    // table
    {
        auto *table = new QTableWidget(0, ColCount);
        table->setHorizontalHeaderLabels({
            "#",
            "Emote",
            "Messages",
            "Chatters",
            "Share of chat",
        });
        table->horizontalHeaderItem(ColMessages)
            ->setToolTip("Chat messages with this emote, each message counts "
                         "once");
        table->horizontalHeaderItem(ColChatters)
            ->setToolTip("Different people who used it");
        table->horizontalHeaderItem(ColShare)->setToolTip(
            "Share of all chat messages in this range that used it");
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(30);
        table->setShowGrid(false);
        table->setAlternatingRowColors(true);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setFocusPolicy(Qt::NoFocus);
        table->setWordWrap(false);
        table->setMouseTracking(true);

        auto *header = table->horizontalHeader();
        header->setHighlightSections(false);
        header->setSectionResizeMode(QHeaderView::ResizeToContents);
        header->setSectionResizeMode(ColEmote, QHeaderView::Stretch);
        header->setSectionResizeMode(ColShare, QHeaderView::Fixed);
        header->resizeSection(ColShare, 200);
        header->setDefaultAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table->horizontalHeaderItem(ColRank)->setTextAlignment(
            Qt::AlignLeft | Qt::AlignVCenter);
        table->horizontalHeaderItem(ColEmote)->setTextAlignment(
            Qt::AlignLeft | Qt::AlignVCenter);

        this->ui_.emoteDelegate = new emotestats_ui::EmoteDelegate(table);
        table->setItemDelegateForColumn(ColEmote, this->ui_.emoteDelegate);
        table->setItemDelegateForColumn(ColShare, new ShareDelegate(table));
        table->setSortingEnabled(true);
        table->sortByColumn(ColMessages, Qt::DescendingOrder);

        this->ui_.table = table;
        root->addWidget(table, 1);
    }

    // emotes of the channel nobody used
    {
        this->ui_.unused = new QFrame;
        this->ui_.unused->setObjectName("unused");
        auto *layout = new QVBoxLayout(this->ui_.unused);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(4);
        this->ui_.unusedTitle = new QLabel;
        this->ui_.unusedTitle->setObjectName("warn");
        layout->addWidget(this->ui_.unusedTitle);
        this->ui_.unusedNames = new QLabel;
        this->ui_.unusedNames->setWordWrap(true);
        this->ui_.unusedNames->setTextInteractionFlags(
            Qt::TextSelectableByMouse);
        layout->addWidget(this->ui_.unusedNames);
        this->ui_.unused->hide();
        root->addWidget(this->ui_.unused);
    }

    // footer
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        this->ui_.syncDot = new QLabel;
        this->ui_.syncDot->setFixedSize(7, 7);
        this->ui_.syncDot->hide();
        row->addWidget(this->ui_.syncDot);
        this->ui_.footNote = new QLabel;
        this->ui_.footNote->setObjectName("foot");
        this->ui_.footNote->setWordWrap(true);
        row->addWidget(this->ui_.footNote, 1);
        root->addLayout(row);
    }
    this->updateFooter();
}

void EmoteStatsDialog::loadView()
{
    QFile file(viewFile());
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }
    const auto obj = QJsonDocument::fromJson(file.readAll()).object();
    this->rangeIndex_ = std::clamp(obj.value(u"range").toInt(1), 0,
                                   static_cast<int>(RANGE_NAMES.size()) - 1);
    this->sourceIndex_ = std::clamp(obj.value(u"source").toInt(0), 0,
                                    static_cast<int>(SOURCE_NAMES.size()) - 1);
}

void EmoteStatsDialog::saveView() const
{
    const auto path = viewFile();
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).path()))
    {
        return;
    }
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(QJsonObject{
                                     {"range", this->rangeIndex_},
                                     {"source", this->sourceIndex_},
                                 })
                       .toJson(QJsonDocument::Compact));
        file.commit();
    }
}

std::shared_ptr<TwitchChannel> EmoteStatsDialog::twitchChannel() const
{
    return std::dynamic_pointer_cast<TwitchChannel>(this->weakChannel_.lock());
}

EmoteStatsDialog::Range EmoteStatsDialog::currentRange() const
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    // the current hour is included
    const auto end = now + HOUR_MS;
    switch (this->rangeIndex_)
    {
        case 0: {
            const auto streams = modactivity::streams(this->channel_);
            if (streams.empty())
            {
                return {
                    .from = now - (12 * HOUR_MS),
                    .to = end,
                    .label = "No stream seen yet, last 12 hours",
                    .unusedTitle = "Not used in the last 12 hours",
                };
            }
            const auto &stream = streams.back();
            if (now - stream.end <= LIVE_WINDOW_MS)
            {
                return {
                    .from = stream.start,
                    .to = end,
                    .label = "Stream started " + formatTime(stream.start),
                    .unusedTitle = "Not used in this stream",
                };
            }
            return {
                .from = stream.start,
                .to = stream.end + STREAM_TAIL_MS,
                .label =
                    QString("Last stream %1, %2 to %3")
                        .arg(formatDay(stream.start), formatTime(stream.start),
                             formatTime(stream.end)),
                .unusedTitle = "Not used in the last stream",
            };
        }
        case 1:
        case 2: {
            const qint64 days = this->rangeIndex_ == 1 ? 7 : 30;
            const auto from = now - (days * DAY_MS);
            return {
                .from = from,
                .to = end,
                .label =
                    QString("%1 to %2").arg(formatDay(from), formatDay(now)),
                .unusedTitle = QString("Not used in these %1 days").arg(days),
            };
        }
        default:
            return {
                .from = 0,
                .to = end,
                .label = "All time",
                .unusedTitle = "Never used",
            };
    }
}

void EmoteStatsDialog::reload()
{
    if (this->loading_)
    {
        this->reloadAgain_ = true;
        return;
    }
    this->loading_ = true;
    this->range_ = this->currentRange();

    emotestats::flush();
    std::weak_ptr<bool> alive = this->lifetime_;
    QThreadPool::globalInstance()->start([this, alive, channel = this->channel_,
                                          from = this->range_.from,
                                          to = this->range_.to] {
        auto stats = emotestats::load(channel, from, to);
        postToThread([this, alive, stats = std::move(stats)]() mutable {
            if (!alive.lock())
            {
                return;
            }
            this->stats_ = std::move(stats);
            this->loaded_ = true;
            this->loading_ = false;
            this->fillTable();
            if (!emotestats::logsImported(this->channel_))
            {
                this->startLogImport();
            }
            if (std::exchange(this->reloadAgain_, false))
            {
                this->reload();
            }
        });
    });
}

void EmoteStatsDialog::fillTable()
{
    if (!this->loaded_)
    {
        return;
    }
    const bool all =
        std::cmp_greater_equal(this->sourceIndex_, SOURCE_ORDER.size());
    const auto shownSource =
        all ? Source::SevenTv
            : SOURCE_ORDER.at(static_cast<size_t>(this->sourceIndex_));
    auto shows = [&](Source source) {
        return all || source == shownSource;
    };

    // used emotes, then the channel's emotes nobody used
    std::vector<emotestats::Row> rows;
    QSet<QString> used;
    for (const auto &row : this->stats_.rows)
    {
        if (shows(row.source) && row.messages > 0)
        {
            rows.push_back(row);
            used.insert(rowKey(row.source, row.name));
        }
    }
    std::ranges::sort(rows, [](const auto &a, const auto &b) {
        if (a.messages != b.messages)
        {
            return a.messages > b.messages;
        }
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    QHash<QString, EmotePtr> emotes;
    QStringList unused;
    auto channel = this->twitchChannel();
    if (channel)
    {
        const std::array<std::pair<Source, std::shared_ptr<const EmoteMap>>, 3>
            channelSets{{
                {Source::SevenTv, channel->seventvEmotes()},
                {Source::Bttv, channel->bttvEmotes()},
                {Source::Ffz, channel->ffzEmotes()},
            }};
        for (const auto &[source, map] : channelSets)
        {
            if (!shows(source) || !map)
            {
                continue;
            }
            for (const auto &[name, emote] : *map)
            {
                const auto key = rowKey(source, name.string);
                emotes.insert(key, emote);
                if (!used.contains(key))
                {
                    used.insert(key);
                    unused.append(name.string);
                    rows.push_back({.source = source, .name = name.string});
                }
            }
        }
    }
    unused.sort(Qt::CaseInsensitive);

    // pictures of the used emotes that are not channel emotes
    {
        auto account = getApp()->getAccounts()->twitch.getCurrent();
        auto twitchEmotes = account->accessEmotes();
        for (const auto &row : rows)
        {
            const auto key = rowKey(row.source, row.name);
            if (emotes.contains(key))
            {
                continue;
            }
            const EmoteName name{row.name};
            std::optional<EmotePtr> emote;
            switch (row.source)
            {
                case Source::Twitch: {
                    const auto &map = *twitchEmotes;
                    if (map)
                    {
                        auto it = map->find(name);
                        if (it != map->end())
                        {
                            emote = it->second;
                        }
                    }
                    break;
                }
                case Source::SevenTv:
                    emote = getApp()->getSeventvEmotes()->globalEmote(name);
                    break;
                case Source::Bttv:
                    emote = getApp()->getBttvEmotes()->emote(name);
                    break;
                case Source::Ffz:
                    emote = getApp()->getFfzEmotes()->emote(name);
                    break;
            }
            if (emote && *emote)
            {
                emotes.insert(key, *emote);
            }
        }
    }
    this->ui_.emoteDelegate->setEmotes(std::move(emotes));

    // summary
    const auto total = this->stats_.totalMessages;
    auto label = this->range_.label;
    if (this->rangeIndex_ == 3)
    {
        label = this->stats_.firstMsecs > 0
                    ? "Since " + formatDay(this->stats_.firstMsecs)
                    : QString("All time");
    }
    QStringList parts{label.toHtmlEscaped()};
    if (total > 0)
    {
        parts << QString("Chat messages <b>%1</b>").arg(formatCount(total));
    }
    if (!rows.empty() && rows.front().messages > 0)
    {
        this->topEmote_ = rows.front().name;
        parts << QString("Top emote <b>%1</b> in <b>%2</b> of messages")
                     .arg(rows.front().name.toHtmlEscaped(),
                          formatShare(rows.front().messages, total));
    }
    else
    {
        this->topEmote_.clear();
        parts << QStringLiteral("No emotes counted in this range yet");
    }
    this->ui_.summary->setText(
        parts.join(QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;")));

    // table
    auto *table = this->ui_.table;
    const auto sortColumn = table->horizontalHeader()->sortIndicatorSection();
    const auto sortOrder = table->horizontalHeader()->sortIndicatorOrder();
    table->setSortingEnabled(false);
    table->clearContents();
    table->setRowCount(static_cast<int>(rows.size()));
    const auto maxMessages = rows.empty() ? 0 : rows.front().messages;
    for (int r = 0; std::cmp_less(r, rows.size()); ++r)
    {
        const auto &row = rows.at(static_cast<size_t>(r));
        auto set = [&](int column, const QString &text, double sortValue,
                       Qt::Alignment align) {
            auto *item = new SortItem(text);
            item->setData(SORT_ROLE, sortValue);
            item->setTextAlignment(align | Qt::AlignVCenter);
            table->setItem(r, column, item);
            return item;
        };
        set(ColRank, QString::number(r + 1), r + 1, Qt::AlignLeft)
            ->setForeground(DIM_COLOR);
        auto *emote = new SortItem(row.name);
        emote->setData(KEY_ROLE, rowKey(row.source, row.name));
        emote->setToolTip(row.name + " (" + emotestats::sourceName(row.source) +
                          ")");
        table->setItem(r, ColEmote, emote);
        set(ColMessages, formatCount(row.messages),
            static_cast<double>(row.messages), Qt::AlignRight);
        set(ColChatters, formatCount(row.chatters),
            static_cast<double>(row.chatters), Qt::AlignRight);
        auto *share = set(ColShare, formatShare(row.messages, total),
                          static_cast<double>(row.messages), Qt::AlignRight);
        share->setData(SHARE_ROLE, maxMessages > 0
                                       ? static_cast<double>(row.messages) /
                                             static_cast<double>(maxMessages)
                                       : 0.0);
    }
    table->setSortingEnabled(true);
    table->sortByColumn(sortColumn, sortOrder);
    this->applySearch();

    // not used
    if (unused.isEmpty())
    {
        this->ui_.unused->hide();
    }
    else
    {
        this->ui_.unusedTitle->setText(QString("%1 (%2)").arg(
            this->range_.unusedTitle, formatCount(unused.size())));
        auto names = unused.mid(0, MAX_UNUSED_NAMES).join(", ");
        if (unused.size() > MAX_UNUSED_NAMES)
        {
            names += QString(" and %1 more (sort by Messages to see all)")
                         .arg(unused.size() - MAX_UNUSED_NAMES);
        }
        this->ui_.unusedNames->setText(names);
        this->ui_.unused->show();
    }
    this->updateFooter();
}

void EmoteStatsDialog::applySearch() const
{
    const auto text = this->ui_.search->text().trimmed();
    auto *table = this->ui_.table;
    for (int r = 0; r < table->rowCount(); ++r)
    {
        const auto *item = table->item(r, ColEmote);
        table->setRowHidden(
            r, !text.isEmpty() && item != nullptr &&
                   !item->text().contains(text, Qt::CaseInsensitive));
    }
}

void EmoteStatsDialog::updateFooter()
{
    const auto status = emotestats::syncStatus(this->channel_);
    const auto example =
        this->topEmote_.isEmpty() ? QStringLiteral("KEKW") : this->topEmote_;
    auto text = QString("Each message counts once per emote, so \"%1 %1 %1 "
                        "%1\" is 1. ")
                    .arg(example);

    QString dotColor;
    if (!status.configured)
    {
        text += "Counted while Chatterino9 is open";
    }
    else if (status.notCounting)
    {
        text += QString("%1 does not count emotes yet (setup.ps1 update), "
                        "counted while Chatterino9 is open")
                    .arg(status.host);
        dotColor = "#e0a03c";
    }
    else if (status.ok)
    {
        const auto synced = status.lastSync.isValid()
                                ? QLocale().toString(status.lastSync.time(),
                                                     QLocale::ShortFormat)
                                : QString();
        text += QString("Counted around the clock on %1, last synced %2")
                    .arg(status.host, synced);
        dotColor = status.connected ? "#4fbf72" : "#e0a03c";
    }
    else
    {
        text += QString("%1 not reachable, showing what this PC counted")
                    .arg(status.host);
        dotColor = "#e0a03c";
    }

    if (emotestats::importRunning(this->channel_))
    {
        text += ", reading your chat logs from before...";
    }
    else if (emotestats::logsImported(this->channel_))
    {
        text += ", plus your chat logs from before.";
    }
    else
    {
        text += '.';
    }

    this->ui_.footNote->setText(text);
    this->ui_.syncDot->setVisible(!dotColor.isEmpty());
    this->ui_.syncDot->setStyleSheet(
        QString("background: %1; border-radius: 3px;").arg(dotColor));
}

void EmoteStatsDialog::startLogImport()
{
    if (emotestats::logsImported(this->channel_) ||
        emotestats::importRunning(this->channel_))
    {
        return;
    }
    auto channel = this->twitchChannel();
    if (!channel)
    {
        return;
    }
    const auto seventv = channel->seventvEmotes();
    const auto bttv = channel->bttvEmotes();
    const auto ffz = channel->ffzEmotes();
    // the channel's emotes are not loaded yet, try again on the next reload
    if ((!seventv || seventv->empty()) && (!bttv || bttv->empty()) &&
        (!ffz || ffz->empty()))
    {
        return;
    }

    auto namesOf = [](const std::shared_ptr<const EmoteMap> &map) {
        QSet<QString> names;
        if (map)
        {
            for (const auto &[name, emote] : *map)
            {
                names.insert(name.string);
            }
        }
        return names;
    };

    emotestats::ImportNames names{
        .ffzChannel = namesOf(ffz),
        .bttvChannel = namesOf(bttv),
        .seventvChannel = namesOf(seventv),
        .ffzGlobal = namesOf(getApp()->getFfzEmotes()->emotes()),
        .bttvGlobal = namesOf(getApp()->getBttvEmotes()->emotes()),
        .seventvGlobal = namesOf(getApp()->getSeventvEmotes()->globalEmotes()),
    };
    {
        // Twitch emotes: the ones this account can use and the ones counted
        auto account = getApp()->getAccounts()->twitch.getCurrent();
        names.twitch = namesOf(*account->accessEmotes());
    }
    for (const auto &row : this->stats_.rows)
    {
        if (row.source == Source::Twitch)
        {
            names.twitch.insert(row.name);
        }
    }

    const auto logPath = getSettings()->logPath.getValue();
    const auto base =
        logPath.isEmpty() ? getApp()->getPaths().messageLogDirectory : logPath;
    emotestats::importLogs(this->channel_,
                           base + "/Twitch/Channels/" + this->channel_,
                           std::move(names));
    this->updateFooter();
}

}  // namespace chatterino
