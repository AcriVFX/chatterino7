// Chatterino9: see ModActivityDialog.hpp

#include "widgets/dialogs/ModActivityDialog.hpp"

#include "Application.hpp"
#include "singletons/Paths.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDateEdit>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <utility>

namespace chatterino {

namespace {

using modactivity::Kind;

constexpr qint64 MINUTE_MS = 60LL * 1000;
constexpr qint64 HOUR_MS = 60 * MINUTE_MS;
constexpr qint64 DAY_MS = 24 * HOUR_MS;
/// Actions shortly after the last "seen live" still belong to the stream
constexpr qint64 STREAM_TAIL_MS = 5 * MINUTE_MS;
/// A stream seen live this recently counts as still running
constexpr qint64 LIVE_WINDOW_MS = 12 * MINUTE_MS;

constexpr int SHARE_ROLE = Qt::UserRole + 1;
constexpr int SORT_ROLE = Qt::UserRole;
constexpr int NAME_ROLE = Qt::UserRole + 2;

const QColor &windowColor()
{
    static const QColor color(0x1b1b1c);
    return color;
}
const QColor &panelColor()
{
    static const QColor color(0x232324);
    return color;
}
const QColor &rowColor()
{
    static const QColor color(0x2a2a2b);
    return color;
}
const QColor &lineColor()
{
    static const QColor color(0x353537);
    return color;
}
const QColor &textColor()
{
    static const QColor color(0xf2f2f2);
    return color;
}
const QColor &dimColor()
{
    static const QColor color(0x9a9a9e);
    return color;
}
const QColor &faintColor()
{
    static const QColor color(0x66666b);
    return color;
}
const QColor &accentColor()
{
    static const QColor color(0x4f8ff7);
    return color;
}
const QColor &botColor()
{
    static const QColor color(0x8a7cff);
    return color;
}
const QColor &upColor()
{
    static const QColor color(0x4fbf72);
    return color;
}
const QColor &downColor()
{
    static const QColor color(0xe0503c);
    return color;
}

struct GroupInfo {
    const char *label;
    const char *color;
    const char *tooltip;
};

constexpr std::array<GroupInfo, ModActivityDialog::GroupCount> GROUPS{{
    {.label = "Timeouts", .color = "#e0a03c", .tooltip = "Timeouts"},
    {.label = "Bans", .color = "#e0503c", .tooltip = "Permanent bans"},
    {.label = "Unbans",
     .color = "#4fbf72",
     .tooltip = "Unbans and removed timeouts"},
    {.label = "Deleted", .color = "#8f9bb3", .tooltip = "Deleted messages"},
    {.label = "Warnings", .color = "#d6c94a", .tooltip = "Warnings"},
    {.label = "AutoMod",
     .color = "#4fb9c9",
     .tooltip = "AutoMod messages approved or denied"},
}};

constexpr std::array<const char *, 8> RANGE_NAMES{
    "This stream", "Today",     "7 days",   "30 days",
    "90 days",     "This year", "All time", "Custom",
};

int groupOf(Kind kind)
{
    switch (kind)
    {
        case Kind::Timeout:
            return ModActivityDialog::GroupTimeouts;
        case Kind::Ban:
            return ModActivityDialog::GroupBans;
        case Kind::Unban:
        case Kind::Untimeout:
            return ModActivityDialog::GroupUnbans;
        case Kind::Delete:
            return ModActivityDialog::GroupDeleted;
        case Kind::Warn:
            return ModActivityDialog::GroupWarnings;
        case Kind::AutomodApprove:
        case Kind::AutomodDeny:
            return ModActivityDialog::GroupAutomod;
    }
    return ModActivityDialog::GroupTimeouts;
}

QColor groupColor(int group)
{
    return {GROUPS.at(static_cast<size_t>(group)).color};
}

const QLocale &cLocale()
{
    static const QLocale locale = QLocale::c();
    return locale;
}

QString formatSeconds(qint64 s)
{
    if (s < 60)
    {
        return QString::number(s) + "s";
    }
    if (s < 3600)
    {
        return QString::number(std::llround(static_cast<double>(s) / 60)) + "m";
    }
    if (s < 86400)
    {
        auto h = s / 3600;
        auto m = (s % 3600) / 60;
        return m == 0 ? QString::number(h) + "h"
                      : QString("%1h %2m").arg(h).arg(m);
    }
    return QString::number(std::llround(static_cast<double>(s) / 86400)) + "d";
}

QString formatAgo(qint64 msecs, qint64 now)
{
    if (msecs <= 0)
    {
        return "never";
    }
    auto diff = std::max<qint64>(0, now - msecs);
    if (diff < 2 * MINUTE_MS)
    {
        return "now";
    }
    if (diff < HOUR_MS)
    {
        return QString::number(diff / MINUTE_MS) + "m ago";
    }
    if (diff < DAY_MS)
    {
        return QString::number(diff / HOUR_MS) + "h ago";
    }
    auto days = QDateTime::fromMSecsSinceEpoch(msecs).date().daysTo(
        QDateTime::fromMSecsSinceEpoch(now).date());
    if (days <= 1)
    {
        return "yesterday";
    }
    return QString::number(days) + " days ago";
}

QString formatDay(qint64 msecs)
{
    return cLocale().toString(QDateTime::fromMSecsSinceEpoch(msecs).date(),
                              "d MMM");
}

QString plural(qint64 n, const QString &word)
{
    return QString::number(n) + " " + word + (n == 1 ? "" : "s");
}

QString csvField(const QString &value)
{
    if (value.contains(u',') || value.contains(u'"') || value.contains(u'\n'))
    {
        auto escaped = value;
        escaped.replace("\"", "\"\"");
        return "\"" + escaped + "\"";
    }
    return value;
}

/// Sorts by SORT_ROLE when set, else by text
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

/// Draws the share bar in front of the moderator's name
class NameDelegate : public QStyledItemDelegate
{
public:
    NameDelegate(const QColor &color, QObject *parent)
        : QStyledItemDelegate(parent)
        , color_(color)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        constexpr int barMax = 60;
        constexpr int barSlot = barMax + 8;

        if (option.state.testFlag(QStyle::State_Selected))
        {
            painter->fillRect(option.rect, option.palette.highlight());
        }
        auto share = index.data(SHARE_ROLE).toDouble();
        if (share > 0)
        {
            int w = std::max(1, static_cast<int>(std::lround(share * barMax)));
            QRect bar(option.rect.left() + 6, option.rect.center().y() - 3, w,
                      6);
            auto c = this->color_;
            c.setAlphaF(0.85F);
            painter->fillRect(bar, c);
        }
        QStyleOptionViewItem shifted(option);
        shifted.rect.setLeft(option.rect.left() + barSlot);
        QStyledItemDelegate::paint(painter, shifted, index);
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setWidth(size.width() + 68);
        return size;
    }

private:
    QColor color_;
};

}  // namespace

namespace modactivity_ui {

/// Stacked bars per day, week or month for the selected moderator
class BarChart : public QWidget
{
public:
    struct Bar {
        QString label;
        QString tooltip;
        std::array<int, ModActivityDialog::GroupCount> counts{};
    };

    explicit BarChart(QWidget *parent)
        : QWidget(parent)
    {
        this->setMouseTracking(true);
        this->setMinimumHeight(110);
    }

    void setBars(std::vector<Bar> bars,
                 std::array<bool, ModActivityDialog::GroupCount> enabled)
    {
        this->bars_ = std::move(bars);
        this->enabled_ = enabled;
        this->update();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        if (this->bars_.empty())
        {
            return;
        }
        constexpr int labelHeight = 16;
        constexpr int gap = 6;
        constexpr int maxBarWidth = 34;
        const int chartHeight = this->height() - labelHeight - 4;
        const int n = static_cast<int>(this->bars_.size());
        const int slot =
            std::max(4, (this->width() - (gap * (n - 1))) / std::max(1, n));
        const int barWidth = std::min(slot, maxBarWidth);

        int max = 1;
        for (const auto &bar : this->bars_)
        {
            max = std::max(max, this->totalOf(bar));
        }

        painter.setPen(lineColor());
        painter.drawLine(0, chartHeight, this->width(), chartHeight);

        auto font = this->font();
        font.setPixelSize(10);
        painter.setFont(font);

        for (int i = 0; i < n; ++i)
        {
            const auto &bar = this->bars_[static_cast<size_t>(i)];
            const int x = (i * (slot + gap)) + ((slot - barWidth) / 2);
            int y = chartHeight;
            for (int g = 0; g < ModActivityDialog::GroupCount; ++g)
            {
                auto count = bar.counts.at(static_cast<size_t>(g));
                if (!this->enabled_.at(static_cast<size_t>(g)) || count == 0)
                {
                    continue;
                }
                int h = std::max(
                    1, static_cast<int>(std::lround(static_cast<double>(count) *
                                                    (chartHeight - 2) / max)));
                y -= h;
                painter.fillRect(QRect(x, y, barWidth, h), groupColor(g));
            }
            painter.setPen(dimColor());
            painter.drawText(
                QRect(i * (slot + gap), chartHeight + 2, slot, labelHeight),
                Qt::AlignHCenter | Qt::AlignTop, bar.label);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int n = static_cast<int>(this->bars_.size());
        if (n == 0)
        {
            return;
        }
        constexpr int gap = 6;
        const int slot =
            std::max(4, (this->width() - (gap * (n - 1))) / std::max(1, n));
        const int i =
            static_cast<int>(event->position().x()) / std::max(1, slot + gap);
        if (i < 0 || i >= n)
        {
            QToolTip::hideText();
            return;
        }
        QToolTip::showText(event->globalPosition().toPoint(),
                           this->bars_[static_cast<size_t>(i)].tooltip, this);
    }

private:
    int totalOf(const Bar &bar) const
    {
        int total = 0;
        for (int g = 0; g < ModActivityDialog::GroupCount; ++g)
        {
            if (this->enabled_.at(static_cast<size_t>(g)))
            {
                total += bar.counts.at(static_cast<size_t>(g));
            }
        }
        return total;
    }

    std::vector<Bar> bars_;
    std::array<bool, ModActivityDialog::GroupCount> enabled_{};
};

}  // namespace modactivity_ui

ModActivityDialog::ModActivityDialog(QString channel, QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::DisableCustomScaling,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , channel_(std::move(channel).toLower())
    , refreshTimer_(new QTimer(this))
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowTitle("Mod activity - " + this->channel_);
    this->resize(1060, 800);

    this->loadView();
    this->initUi();
    this->themeChangedEvent();

    this->refreshTimer_->setSingleShot(true);
    this->refreshTimer_->setInterval(std::chrono::milliseconds(1500));
    QObject::connect(this->refreshTimer_, &QTimer::timeout, this, [this] {
        this->reload();
    });
    this->signalHolder_.managedConnect(modactivity::updated(), [this] {
        if (!this->refreshTimer_->isActive())
        {
            this->refreshTimer_->start();
        }
    });

    // keeps "Last active" and the live stream current
    auto *tick = new QTimer(this);
    tick->setInterval(std::chrono::minutes(1));
    QObject::connect(tick, &QTimer::timeout, this, [this] {
        this->reload();
    });
    tick->start();

    this->reload();
}

void ModActivityDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    QPalette palette;
    palette.setColor(QPalette::Window, windowColor());
    palette.setColor(QPalette::WindowText, textColor());
    palette.setColor(QPalette::Base, windowColor());
    palette.setColor(QPalette::AlternateBase, panelColor());
    palette.setColor(QPalette::Text, textColor());
    palette.setColor(QPalette::Button, panelColor());
    palette.setColor(QPalette::ButtonText, textColor());
    palette.setColor(QPalette::Highlight, rowColor());
    palette.setColor(QPalette::HighlightedText, textColor());
    palette.setColor(QPalette::ToolTipBase, panelColor());
    palette.setColor(QPalette::ToolTipText, textColor());
    this->setPalette(palette);
}

void ModActivityDialog::initUi()
{
    this->setStyleSheet(R"(
        * { font-family: "Segoe UI"; font-size: 13px; }
        QLabel, QCheckBox, QRadioButton { color: #f2f2f2; }
        QLabel#dim { color: #9a9a9e; }
        QLabel#section { color: #9a9a9e; font-size: 11px; font-weight: 600; }
        QLabel#foot { color: #66666b; font-size: 11px; }
        QTabBar::tab { background: transparent; color: #9a9a9e;
            padding: 6px 11px; border: none;
            border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { color: #f2f2f2;
            border-bottom: 2px solid #4f8ff7; }
        QTabBar::tab:hover { color: #f2f2f2; }
        QTableWidget { background: #1b1b1c; border: none;
            gridline-color: #262628; }
        QHeaderView::section { background: #1b1b1c; color: #9a9a9e;
            border: none; border-bottom: 1px solid #353537;
            padding: 4px 7px; font-weight: 600; }
        QPushButton { background: #232324; color: #f2f2f2;
            border: 1px solid #353537; padding: 4px 10px; }
        QPushButton:hover { border-color: #4f8ff7; }
        QPushButton#chip { background: transparent; color: #9a9a9e;
            padding: 3px 9px; }
        QPushButton#chip:checked { background: #2a2a2b; color: #f2f2f2; }
        QPushButton#seg { padding: 2px 9px; }
        QPushButton#seg:checked { background: #4f8ff7; color: #ffffff;
            border-color: #4f8ff7; }
        QPushButton#link { background: transparent; border: none;
            color: #4f8ff7; padding: 2px 4px; }
        QToolButton#mods { background: #232324; color: #f2f2f2;
            border: 1px solid #353537; padding: 4px 10px; }
        QToolButton#mods:hover, QToolButton#mods:pressed {
            border-color: #4f8ff7; }
        QToolButton#mods::menu-indicator { image: none; }
        QLineEdit, QDateEdit { background: #121213; color: #f2f2f2;
            border: 1px solid #353537; padding: 3px 6px; }
        QWidget#detail { background: #2a2a2b; }
        QWidget#detail QLabel { background: transparent; }
        QMenu { background: #232324; border: 1px solid #353537; }
        QTreeWidget { background: #232324; color: #f2f2f2; border: none; }
        QRadioButton#seg { padding: 3px 8px; }
    )");

    auto *root = new QVBoxLayout(this->getLayoutContainer());
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);

    // range tabs
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(6);
        this->ui_.tabs = new QTabBar;
        this->ui_.tabs->setDrawBase(false);
        this->ui_.tabs->setExpanding(false);
        for (const auto *name : RANGE_NAMES)
        {
            this->ui_.tabs->addTab(name);
        }
        this->ui_.tabs->setCurrentIndex(this->rangeIndex_);
        row->addWidget(this->ui_.tabs);

        this->ui_.customRange = new QWidget;
        auto *custom = new QHBoxLayout(this->ui_.customRange);
        custom->setContentsMargins(6, 0, 0, 0);
        custom->setSpacing(6);
        auto today = QDate::currentDate();
        this->ui_.fromDate = new QDateEdit(today.addDays(-30));
        this->ui_.toDate = new QDateEdit(today);
        for (auto *edit : {this->ui_.fromDate, this->ui_.toDate})
        {
            edit->setCalendarPopup(true);
            edit->setDisplayFormat("dd.MM.yyyy");
        }
        auto *to = new QLabel("to");
        to->setObjectName("dim");
        custom->addWidget(this->ui_.fromDate);
        custom->addWidget(to);
        custom->addWidget(this->ui_.toDate);
        row->addWidget(this->ui_.customRange);
        row->addStretch(1);
        root->addLayout(row);

        auto *line = new QFrame;
        line->setFixedHeight(1);
        line->setStyleSheet("background: #353537;");
        root->addWidget(line);

        QObject::connect(this->ui_.tabs, &QTabBar::currentChanged, this,
                         [this](int index) {
                             this->rangeIndex_ = index;
                             this->saveView();
                             this->refresh();
                         });
        for (auto *edit : {this->ui_.fromDate, this->ui_.toDate})
        {
            QObject::connect(edit, &QDateEdit::dateChanged, this, [this] {
                if (this->rangeIndex_ != 7)
                {
                    this->ui_.tabs->setCurrentIndex(7);
                }
                else
                {
                    this->refresh();
                }
            });
        }
    }

    // filters
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(14);

        this->ui_.modsButton = new QToolButton;
        this->ui_.modsButton->setObjectName("mods");
        this->ui_.modsButton->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(this->ui_.modsButton);
        auto *action = new QWidgetAction(menu);
        action->setDefaultWidget(this->createFilterPopup());
        menu->addAction(action);
        this->ui_.modsButton->setMenu(menu);
        row->addWidget(this->ui_.modsButton);

        auto *chips = new QHBoxLayout;
        chips->setSpacing(4);
        for (int g = 0; g < GroupCount; ++g)
        {
            const auto &info = GROUPS.at(static_cast<size_t>(g));
            QPixmap swatch(8, 8);
            swatch.fill(QColor(info.color));
            auto *chip = new QPushButton(QIcon(swatch), info.label);
            chip->setObjectName("chip");
            chip->setCheckable(true);
            chip->setChecked(this->groups_.at(static_cast<size_t>(g)));
            chip->setToolTip(info.tooltip);
            chip->setCursor(Qt::PointingHandCursor);
            this->ui_.chips.at(static_cast<size_t>(g)) = chip;
            chips->addWidget(chip);
            QObject::connect(chip, &QPushButton::toggled, this,
                             [this, g](bool on) {
                                 this->groups_.at(static_cast<size_t>(g)) = on;
                                 this->saveView();
                                 this->refresh();
                             });
        }
        row->addLayout(chips);

        this->ui_.showBots = new QCheckBox("Show bots");
        this->ui_.showBots->setChecked(this->showBots_);
        QObject::connect(this->ui_.showBots, &QCheckBox::toggled, this,
                         [this](bool on) {
                             this->showBots_ = on;
                             this->saveView();
                             this->refresh();
                         });
        row->addWidget(this->ui_.showBots);

        this->ui_.compare = new QCheckBox("Compare with previous period");
        this->ui_.compare->setChecked(this->compare_);
        QObject::connect(this->ui_.compare, &QCheckBox::toggled, this,
                         [this](bool on) {
                             this->compare_ = on;
                             this->saveView();
                             this->refresh();
                         });
        row->addWidget(this->ui_.compare);
        row->addStretch(1);
        root->addLayout(row);
    }

    this->ui_.summary = new QLabel;
    this->ui_.summary->setObjectName("dim");
    this->ui_.summary->setTextFormat(Qt::RichText);
    root->addWidget(this->ui_.summary);

    auto makeTable = [this](const QColor &barColor) {
        auto *table = new QTableWidget(0, ColCount);
        table->setHorizontalHeaderLabels({
            "Moderator",
            "Timeouts",
            "Bans",
            "Unbans",
            "Deleted",
            "Warnings",
            "AutoMod",
            "Total",
            "vs prev",
            "Streams active",
            "Per stream",
            "Last active",
        });
        for (int g = 0; g < GroupCount; ++g)
        {
            table->horizontalHeaderItem(ColTimeouts + g)
                ->setToolTip(GROUPS.at(static_cast<size_t>(g)).tooltip);
        }
        table->horizontalHeaderItem(ColChange)->setToolTip(
            "Total compared with the period of the same length before");
        table->horizontalHeaderItem(ColStreams)
            ->setToolTip("Streams in this range with at least one action");
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(26);
        table->setShowGrid(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setFocusPolicy(Qt::NoFocus);
        table->setSortingEnabled(true);
        table->setWordWrap(false);
        table->horizontalHeader()->setSectionResizeMode(
            QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(ColName,
                                                        QHeaderView::Stretch);
        table->horizontalHeader()->setHighlightSections(false);
        table->horizontalHeader()->setDefaultAlignment(Qt::AlignRight |
                                                       Qt::AlignVCenter);
        table->horizontalHeaderItem(ColName)->setTextAlignment(
            Qt::AlignLeft | Qt::AlignVCenter);
        table->sortByColumn(ColTotal, Qt::DescendingOrder);
        table->setItemDelegateForColumn(ColName,
                                        new NameDelegate(barColor, table));
        QObject::connect(
            table, &QTableWidget::itemSelectionChanged, this, [this, table] {
                if (this->updatingUi_)
                {
                    return;
                }
                auto items = table->selectedItems();
                if (items.isEmpty())
                {
                    return;
                }
                auto *other =
                    table == this->ui_.mods ? this->ui_.bots : this->ui_.mods;
                this->updatingUi_ = true;
                other->clearSelection();
                this->updatingUi_ = false;
                this->selected_ = table->item(items.first()->row(), ColName)
                                      ->data(NAME_ROLE)
                                      .toString();
                this->updateDetail();
            });
        return table;
    };

    auto *modsHeader = new QLabel("MODERATORS");
    modsHeader->setObjectName("section");
    root->addWidget(modsHeader);
    this->ui_.mods = makeTable(accentColor());
    root->addWidget(this->ui_.mods, 3);

    // detail of the selected moderator
    {
        this->ui_.detail = new QWidget;
        this->ui_.detail->setObjectName("detail");
        this->ui_.detail->setAttribute(Qt::WA_StyledBackground);
        auto *detail = new QHBoxLayout(this->ui_.detail);
        detail->setContentsMargins(12, 10, 12, 12);
        detail->setSpacing(18);

        auto *left = new QVBoxLayout;
        left->setSpacing(8);
        auto *top = new QHBoxLayout;
        this->ui_.detailTitle = new QLabel;
        this->ui_.detailTitle->setStyleSheet("font-weight: 600;");
        top->addWidget(this->ui_.detailTitle);
        top->addStretch(1);
        auto *buckets = new QButtonGroup(this);
        constexpr std::array<const char *, 3> bucketNames{"Days", "Weeks",
                                                          "Months"};
        for (int i = 0; i < 3; ++i)
        {
            auto *button =
                new QPushButton(bucketNames.at(static_cast<size_t>(i)));
            button->setObjectName("seg");
            button->setCheckable(true);
            button->setChecked(static_cast<int>(this->bucket_) == i);
            button->setCursor(Qt::PointingHandCursor);
            buckets->addButton(button, i);
            top->addWidget(button);
            this->ui_.bucketButtons.at(static_cast<size_t>(i)) = button;
        }
        QObject::connect(buckets, &QButtonGroup::idClicked, this,
                         [this](int id) {
                             this->bucket_ = static_cast<Bucket>(id);
                             this->saveView();
                             this->updateDetail();
                         });
        left->addLayout(top);
        this->ui_.chart = new modactivity_ui::BarChart(this->ui_.detail);
        left->addWidget(this->ui_.chart, 1);
        this->ui_.legend = new QLabel;
        this->ui_.legend->setObjectName("dim");
        this->ui_.legend->setTextFormat(Qt::RichText);
        left->addWidget(this->ui_.legend);
        detail->addLayout(left, 2);

        this->ui_.facts = new QGridLayout;
        this->ui_.facts->setHorizontalSpacing(14);
        this->ui_.facts->setVerticalSpacing(5);
        auto *factsBox = new QVBoxLayout;
        factsBox->addLayout(this->ui_.facts);
        factsBox->addStretch(1);
        detail->addLayout(factsBox, 1);

        this->ui_.detail->setMinimumHeight(190);
        root->addWidget(this->ui_.detail, 0);
    }

    this->ui_.botsHeader = new QLabel("BOTS");
    this->ui_.botsHeader->setObjectName("section");
    root->addWidget(this->ui_.botsHeader);
    this->ui_.bots = makeTable(botColor());
    this->ui_.bots->horizontalHeaderItem(ColName)->setText("Bot");
    root->addWidget(this->ui_.bots, 1);

    // footer
    {
        auto *row = new QHBoxLayout;
        auto *note = new QLabel(
            "Counts what Chatterino9 sees while it is open, kept with no time "
            "limit. Click a column to sort, click a mod for details.");
        note->setObjectName("foot");
        note->setWordWrap(true);
        row->addWidget(note, 1);
        auto *exportButton = new QPushButton("Export CSV");
        QObject::connect(exportButton, &QPushButton::clicked, this, [this] {
            this->exportCsv();
        });
        row->addWidget(exportButton);
        root->addLayout(row);
    }
}

QWidget *ModActivityDialog::createFilterPopup()
{
    auto *popup = new QWidget;
    popup->setMinimumWidth(290);
    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    this->ui_.modSearch = new QLineEdit;
    this->ui_.modSearch->setPlaceholderText("Find mod...");
    this->ui_.modSearch->setClearButtonEnabled(true);
    layout->addWidget(this->ui_.modSearch);

    auto *modes = new QHBoxLayout;
    modes->setSpacing(0);
    this->ui_.includeMode = new QRadioButton("Include checked");
    this->ui_.excludeMode = new QRadioButton("Exclude checked");
    this->ui_.includeMode->setChecked(!this->excludeMode_);
    this->ui_.excludeMode->setChecked(this->excludeMode_);
    modes->addWidget(this->ui_.includeMode);
    modes->addWidget(this->ui_.excludeMode);
    layout->addLayout(modes);
    QObject::connect(this->ui_.excludeMode, &QRadioButton::toggled, this,
                     [this](bool on) {
                         this->excludeMode_ = on;
                         this->saveView();
                         this->rebuildModList();
                         this->refresh();
                     });

    this->ui_.modList = new QTreeWidget;
    this->ui_.modList->setColumnCount(2);
    this->ui_.modList->setHeaderHidden(true);
    this->ui_.modList->setRootIsDecorated(false);
    this->ui_.modList->setIndentation(8);
    this->ui_.modList->setFocusPolicy(Qt::NoFocus);
    this->ui_.modList->setMinimumHeight(280);
    this->ui_.modList->header()->setStretchLastSection(false);
    this->ui_.modList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    this->ui_.modList->header()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    layout->addWidget(this->ui_.modList);
    QObject::connect(this->ui_.modList, &QTreeWidget::itemChanged, this,
                     [this](QTreeWidgetItem *item, int column) {
                         if (this->updatingUi_ || column != 0)
                         {
                             return;
                         }
                         auto name = item->data(0, NAME_ROLE).toString();
                         if (name.isEmpty())
                         {
                             return;
                         }
                         if (item->checkState(0) == Qt::Checked)
                         {
                             this->marked_.insert(name);
                         }
                         else
                         {
                             this->marked_.remove(name);
                         }
                         this->saveView();
                         this->rebuildModList();
                         this->refresh();
                     });
    QObject::connect(this->ui_.modSearch, &QLineEdit::textChanged, this,
                     [this] {
                         this->rebuildModList();
                     });

    auto *links = new QHBoxLayout;
    auto addLink = [&](const QString &text, auto &&onClick) {
        auto *link = new QPushButton(text);
        link->setObjectName("link");
        link->setCursor(Qt::PointingHandCursor);
        QObject::connect(link, &QPushButton::clicked, this,
                         std::forward<decltype(onClick)>(onClick));
        links->addWidget(link);
    };
    addLink("All", [this] {
        for (const auto &name : std::as_const(this->allNames_))
        {
            this->marked_.insert(name);
        }
        this->saveView();
        this->rebuildModList();
        this->refresh();
    });
    links->addStretch(1);
    addLink("None", [this] {
        this->marked_.clear();
        this->saveView();
        this->rebuildModList();
        this->refresh();
    });
    links->addStretch(1);
    addLink("Only human mods", [this] {
        this->marked_.clear();
        for (const auto &name : std::as_const(this->allNames_))
        {
            if (!modactivity::isBot(name))
            {
                this->marked_.insert(name);
            }
        }
        this->ui_.includeMode->setChecked(true);
        this->excludeMode_ = false;
        this->saveView();
        this->rebuildModList();
        this->refresh();
    });
    layout->addLayout(links);

    return popup;
}

void ModActivityDialog::loadView()
{
    QFile file(QDir(getApp()->getPaths().miscDirectory)
                   .filePath("ModActivity/view.json"));
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }
    auto obj = QJsonDocument::fromJson(file.readAll()).object();
    this->rangeIndex_ = std::clamp(obj.value("range").toInt(3), 0,
                                   static_cast<int>(RANGE_NAMES.size()) - 1);
    this->excludeMode_ = obj.value("exclude").toBool(false);
    const auto markedArray = obj.value("marked").toArray();
    for (const auto &v : markedArray)
    {
        this->marked_.insert(v.toString());
    }
    const auto knownArray = obj.value("known").toArray();
    for (const auto &v : knownArray)
    {
        this->knownNames_.insert(v.toString());
    }
    auto groups = obj.value("groups").toArray();
    if (groups.size() == GroupCount)
    {
        for (int g = 0; g < GroupCount; ++g)
        {
            this->groups_.at(static_cast<size_t>(g)) = groups.at(g).toBool();
        }
    }
    this->showBots_ = obj.value("bots").toBool(true);
    this->compare_ = obj.value("compare").toBool(true);
    this->bucket_ =
        static_cast<Bucket>(std::clamp(obj.value("bucket").toInt(1), 0, 2));
}

void ModActivityDialog::saveView() const
{
    auto dir = QDir(getApp()->getPaths().miscDirectory).filePath("ModActivity");
    if (!QDir().mkpath(dir))
    {
        return;
    }
    QJsonArray marked;
    for (const auto &name : this->marked_)
    {
        marked.append(name);
    }
    QJsonArray known;
    for (const auto &name : this->knownNames_)
    {
        known.append(name);
    }
    QJsonArray groups;
    for (bool on : this->groups_)
    {
        groups.append(on);
    }
    QJsonObject obj{
        {"range", this->rangeIndex_},
        {"exclude", this->excludeMode_},
        {"marked", marked},
        {"known", known},
        {"groups", groups},
        {"bots", this->showBots_},
        {"compare", this->compare_},
        {"bucket", static_cast<int>(this->bucket_)},
    };
    QFile file(QDir(dir).filePath("view.json"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }
}

void ModActivityDialog::reload()
{
    this->actions_ = modactivity::actions(this->channel_);
    this->streams_ = modactivity::streams(this->channel_);

    this->allTimeCount_.clear();
    this->lastActive_.clear();
    for (const auto &action : this->actions_)
    {
        this->allTimeCount_[action.moderator] += 1;
        // actions are oldest first, so the last one wins
        this->lastActive_.insert(action.moderator, action.msecs);
    }
    QStringList names = this->allTimeCount_.keys();
    std::ranges::sort(names, [this](const QString &a, const QString &b) {
        auto ca = this->allTimeCount_.value(a);
        auto cb = this->allTimeCount_.value(b);
        return ca != cb ? ca > cb : a < b;
    });
    this->allNames_ = names;

    bool changed = false;
    for (const auto &name : std::as_const(this->allNames_))
    {
        if (!this->knownNames_.contains(name))
        {
            // new names show up in both modes
            if (!this->excludeMode_)
            {
                this->marked_.insert(name);
            }
            this->knownNames_.insert(name);
            changed = true;
        }
    }
    if (changed)
    {
        this->saveView();
    }

    this->rebuildModList();
    this->refresh();
}

void ModActivityDialog::rebuildModList()
{
    if (this->ui_.modList == nullptr)
    {
        return;
    }
    this->updatingUi_ = true;
    auto *scroll = this->ui_.modList->verticalScrollBar();
    auto scrollPos = scroll != nullptr ? scroll->value() : 0;
    this->ui_.modList->clear();

    const auto search = this->ui_.modSearch->text().trimmed().toLower();
    auto *mods = new QTreeWidgetItem(this->ui_.modList, {"MODERATORS"});
    auto *bots = new QTreeWidgetItem(this->ui_.modList, {"BOTS"});
    for (auto *group : {mods, bots})
    {
        group->setFlags(Qt::ItemIsEnabled);
        group->setForeground(0, dimColor());
        auto font = group->font(0);
        font.setPixelSize(11);
        font.setBold(true);
        group->setFont(0, font);
    }

    for (const auto &name : std::as_const(this->allNames_))
    {
        if (!search.isEmpty() && !name.contains(search))
        {
            continue;
        }
        auto *parent = modactivity::isBot(name) ? bots : mods;
        auto *item = new QTreeWidgetItem(
            parent,
            {name, cLocale().toString(this->allTimeCount_.value(name))});
        item->setData(0, NAME_ROLE, name);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(
            0, this->marked_.contains(name) ? Qt::Checked : Qt::Unchecked);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setForeground(1, dimColor());
        if (!this->isShown(name))
        {
            auto font = item->font(0);
            font.setStrikeOut(true);
            item->setFont(0, font);
            item->setForeground(0, faintColor());
        }
    }
    mods->setHidden(mods->childCount() == 0);
    bots->setHidden(bots->childCount() == 0);
    this->ui_.modList->expandAll();
    if (scroll != nullptr)
    {
        scroll->setValue(scrollPos);
    }
    this->updatingUi_ = false;
    this->updateModsButton();
}

void ModActivityDialog::updateModsButton()
{
    int shown = 0;
    for (const auto &name : std::as_const(this->allNames_))
    {
        if (this->isShown(name))
        {
            ++shown;
        }
    }
    this->ui_.modsButton->setText(
        QString("Mods   %1 of %2  ").arg(shown).arg(this->allNames_.size()) +
        QChar(0x25BE));
}

bool ModActivityDialog::isShown(const QString &name) const
{
    return this->excludeMode_ != this->marked_.contains(name);
}

bool ModActivityDialog::groupEnabled(int group) const
{
    return this->groups_.at(static_cast<size_t>(group));
}

ModActivityDialog::Range ModActivityDialog::currentRange() const
{
    const auto nowDt = QDateTime::currentDateTime();
    const auto now = nowDt.toMSecsSinceEpoch();
    const auto todayStart =
        QDateTime(nowDt.date(), QTime(0, 0)).toMSecsSinceEpoch();
    auto span = [&](qint64 from, const QString &prefix) {
        return Range{
            .from = from,
            .to = now,
            .label = prefix.isEmpty()
                         ? formatDay(from) + " to " + formatDay(now)
                         : prefix,
        };
    };

    switch (this->rangeIndex_)
    {
        case 0: {
            if (this->streams_.empty())
            {
                return span(todayStart,
                            "No stream recorded yet, showing <b>today</b>");
            }
            const auto &s = this->streams_.back();
            auto start = QDateTime::fromMSecsSinceEpoch(s.start);
            if (now - s.end < LIVE_WINDOW_MS)
            {
                auto mins = (now - s.start) / MINUTE_MS;
                return {
                    .from = s.start,
                    .to = now,
                    .label = QString("Stream started <b>%1</b> (live, "
                                     "<b>%2h %3m</b>)")
                                 .arg(start.toString("HH:mm"))
                                 .arg(mins / 60)
                                 .arg(mins % 60),
                    .isStream = true,
                };
            }
            auto end = QDateTime::fromMSecsSinceEpoch(s.end);
            return {
                .from = s.start,
                .to = s.end + STREAM_TAIL_MS,
                .label = QString("Last stream <b>%1, %2 to %3</b>")
                             .arg(formatDay(s.start), start.toString("HH:mm"),
                                  end.toString("HH:mm")),
                .isStream = true,
            };
        }
        case 1:
            return span(todayStart, "Today since <b>00:00</b>");
        case 2:
            return span(now - (7 * DAY_MS), {});
        case 3:
            return span(now - (30 * DAY_MS), {});
        case 4:
            return span(now - (90 * DAY_MS), {});
        case 5: {
            auto jan1 = QDateTime(QDate(nowDt.date().year(), 1, 1), QTime(0, 0))
                            .toMSecsSinceEpoch();
            return span(jan1, {});
        }
        case 6: {
            auto first = this->actions_.empty() ? todayStart
                                                : this->actions_.front().msecs;
            return span(first,
                        "All time since <b>" +
                            cLocale().toString(
                                QDateTime::fromMSecsSinceEpoch(first).date(),
                                "d MMM yyyy") +
                            "</b>");
        }
        default: {
            auto fromDate = this->ui_.fromDate->date();
            auto toDate = this->ui_.toDate->date();
            if (toDate < fromDate)
            {
                std::swap(fromDate, toDate);
            }
            auto from = QDateTime(fromDate, QTime(0, 0)).toMSecsSinceEpoch();
            auto to =
                QDateTime(toDate.addDays(1), QTime(0, 0)).toMSecsSinceEpoch();
            return {
                .from = from,
                .to = to,
                .label = "<b>" + formatDay(from) + " to " + formatDay(to - 1) +
                         "</b>",
            };
        }
    }
}

void ModActivityDialog::refresh()
{
    this->range_ = this->currentRange();
    if (!this->range_.label.contains("<b>"))
    {
        this->range_.label = "Range <b>" + this->range_.label + "</b>";
    }
    this->ui_.customRange->setVisible(this->rangeIndex_ == 7);

    const auto from = this->range_.from;
    const auto to = this->range_.to;
    const auto prevFrom = from - (to - from);

    if (this->rangeIndex_ == 0)
    {
        this->ui_.compare->setText("Compare with previous stream");
    }
    else if (this->rangeIndex_ >= 2 && this->rangeIndex_ <= 4)
    {
        this->ui_.compare->setText("Compare with previous " +
                                   this->ui_.tabs->tabText(this->rangeIndex_));
    }
    else
    {
        this->ui_.compare->setText("Compare with previous period");
    }

    // streams overlapping the range
    this->rangeStreams_.clear();
    const modactivity::Stream *prevStream = nullptr;
    for (const auto &s : this->streams_)
    {
        if (s.start < to && s.end + STREAM_TAIL_MS > from)
        {
            this->rangeStreams_.push_back(s);
        }
        else if (s.start < from)
        {
            prevStream = &s;
        }
    }
    this->streamCount_ = static_cast<int>(this->rangeStreams_.size());

    // "This stream" compares with the stream before it
    auto cmpFrom = prevFrom;
    auto cmpTo = from;
    if (this->range_.isStream && prevStream != nullptr)
    {
        cmpFrom = prevStream->start;
        cmpTo = prevStream->end + STREAM_TAIL_MS;
    }

    std::map<QString, Row> humans;
    std::map<QString, Row> bots;
    for (const auto &name : std::as_const(this->allNames_))
    {
        if (!this->isShown(name))
        {
            continue;
        }
        bool bot = modactivity::isBot(name);
        if (bot && !this->showBots_)
        {
            continue;
        }
        auto &row = (bot ? bots : humans)[name];
        row.name = name;
        row.lastActive = this->lastActive_.value(name);
    }
    auto rowFor = [&](const QString &name) -> Row * {
        auto it = humans.find(name);
        if (it != humans.end())
        {
            return &it->second;
        }
        it = bots.find(name);
        return it != bots.end() ? &it->second : nullptr;
    };

    const auto lowest = std::min(from, cmpFrom);
    const auto highest = std::max(to, cmpTo);
    auto begin = std::ranges::lower_bound(this->actions_, lowest, {},
                                          &modactivity::Action::msecs);
    for (auto it = begin; it != this->actions_.end() && it->msecs < highest;
         ++it)
    {
        int group = groupOf(it->kind);
        if (!this->groupEnabled(group))
        {
            continue;
        }
        auto *row = rowFor(it->moderator);
        if (row == nullptr)
        {
            continue;
        }
        if (it->msecs >= from && it->msecs < to)
        {
            row->counts.at(static_cast<size_t>(group)) += 1;
            row->total += 1;
        }
        if (it->msecs >= cmpFrom && it->msecs < cmpTo)
        {
            row->prevTotal += 1;
        }
    }

    for (const auto &s : this->rangeStreams_)
    {
        QSet<QString> active;
        auto lo = std::ranges::lower_bound(this->actions_, s.start, {},
                                           &modactivity::Action::msecs);
        for (auto it = lo;
             it != this->actions_.end() && it->msecs < s.end + STREAM_TAIL_MS;
             ++it)
        {
            if (this->groupEnabled(groupOf(it->kind)))
            {
                active.insert(it->moderator);
            }
        }
        for (const auto &name : active)
        {
            if (auto *row = rowFor(name))
            {
                row->streamsActive += 1;
            }
        }
    }

    this->modRows_.clear();
    this->botRows_.clear();
    int humanTotal = 0;
    int humanPrev = 0;
    int botTotal = 0;
    int activeMods = 0;
    for (auto &[name, row] : humans)
    {
        humanTotal += row.total;
        humanPrev += row.prevTotal;
        if (row.total > 0)
        {
            ++activeMods;
        }
        this->modRows_.push_back(row);
    }
    for (auto &[name, row] : bots)
    {
        botTotal += row.total;
        this->botRows_.push_back(row);
    }

    // summary line
    QStringList parts{this->range_.label};
    if (!this->streams_.empty() && !this->range_.isStream)
    {
        parts << QString("Streams <b>%1</b>").arg(this->streamCount_);
    }
    auto humanText = QString("Human mod actions <b>%1</b>")
                         .arg(cLocale().toString(humanTotal));
    if (this->compare_ && humanPrev > 0)
    {
        auto pct = std::lround((humanTotal - humanPrev) * 100.0 / humanPrev);
        humanText += QString(" <span style=\"color:%1\">%2%3%</span>")
                         .arg(pct >= 0 ? upColor().name() : downColor().name(),
                              pct >= 0 ? QString("+") : QString())
                         .arg(pct);
    }
    parts << humanText;
    if (this->showBots_)
    {
        parts << QString("Bot actions <b>%1</b>")
                     .arg(cLocale().toString(botTotal));
    }
    parts << QString("Active mods <b>%1 of %2</b>")
                 .arg(activeMods)
                 .arg(this->modRows_.size());
    this->ui_.summary->setText(parts.join("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;"));

    this->updatingUi_ = true;
    this->fillTable(this->ui_.mods, this->modRows_, this->streamCount_);
    this->fillTable(this->ui_.bots, this->botRows_, this->streamCount_);
    this->ui_.bots->setVisible(this->showBots_ && !this->botRows_.empty());
    this->ui_.botsHeader->setVisible(this->ui_.bots->isVisible());

    // keep the selection
    bool found = false;
    for (auto *table : {this->ui_.mods, this->ui_.bots})
    {
        table->clearSelection();
        for (int r = 0; r < table->rowCount() && !found; ++r)
        {
            if (table->item(r, ColName)->data(NAME_ROLE).toString() ==
                this->selected_)
            {
                table->selectRow(r);
                found = true;
            }
        }
    }
    if (!found)
    {
        if (this->ui_.mods->rowCount() > 0)
        {
            this->ui_.mods->selectRow(0);
            this->selected_ =
                this->ui_.mods->item(0, ColName)->data(NAME_ROLE).toString();
        }
        else if (this->ui_.bots->isVisible() && this->ui_.bots->rowCount() > 0)
        {
            this->ui_.bots->selectRow(0);
            this->selected_ =
                this->ui_.bots->item(0, ColName)->data(NAME_ROLE).toString();
        }
        else
        {
            this->selected_.clear();
        }
    }
    this->updatingUi_ = false;

    this->updateModsButton();
    this->updateDetail();
}

void ModActivityDialog::fillTable(QTableWidget *table,
                                  const std::vector<Row> &rows, int streamCount)
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const bool bot = table == this->ui_.bots;
    int maxTotal = 1;
    for (const auto &row : rows)
    {
        maxTotal = std::max(maxTotal, row.total);
    }

    auto sortColumn = table->horizontalHeader()->sortIndicatorSection();
    auto sortOrder = table->horizontalHeader()->sortIndicatorOrder();
    table->setSortingEnabled(false);
    table->clearContents();
    table->setRowCount(static_cast<int>(rows.size()));

    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto &row = rows[i];
        const auto r = static_cast<int>(i);
        const bool idle = row.total == 0;

        auto set = [&](int col, const QString &text, double sortKey,
                       const QColor &color) {
            auto *item = new SortItem(text);
            item->setData(SORT_ROLE, sortKey);
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            item->setForeground(idle ? faintColor() : color);
            table->setItem(r, col, item);
        };

        auto *name = new SortItem(row.name);
        name->setData(NAME_ROLE, row.name);
        name->setData(SHARE_ROLE,
                      idle ? 0.0 : static_cast<double>(row.total) / maxTotal);
        auto nameColor = bot ? botColor() : textColor();
        name->setForeground(idle ? faintColor() : nameColor);
        if (!idle)
        {
            auto font = name->font();
            font.setBold(true);
            name->setFont(font);
        }
        table->setItem(r, ColName, name);

        for (int g = 0; g < GroupCount; ++g)
        {
            auto count = row.counts.at(static_cast<size_t>(g));
            set(ColTimeouts + g, cLocale().toString(count), count,
                count == 0 ? faintColor() : groupColor(g));
        }
        set(ColTotal, cLocale().toString(row.total), row.total, textColor());

        if (row.prevTotal == 0 && row.total == 0)
        {
            set(ColChange, "-", 0, faintColor());
        }
        else if (row.prevTotal == 0)
        {
            set(ColChange, "new", 1e9, upColor());
        }
        else
        {
            auto pct = std::lround((row.total - row.prevTotal) * 100.0 /
                                   row.prevTotal);
            set(ColChange, QString("%1%2%").arg(pct >= 0 ? "+" : "").arg(pct),
                static_cast<double>(pct), pct >= 0 ? upColor() : downColor());
            if (idle)
            {
                table->item(r, ColChange)->setForeground(downColor());
            }
        }

        if (streamCount == 0)
        {
            set(ColStreams, "-", 0, faintColor());
            set(ColPerStream, "-", 0, faintColor());
        }
        else
        {
            set(ColStreams,
                QString("%1 of %2").arg(row.streamsActive).arg(streamCount),
                row.streamsActive, textColor());
            if (row.streamsActive > 0)
            {
                auto per = std::lround(static_cast<double>(row.total) /
                                       row.streamsActive);
                set(ColPerStream, QString::number(per),
                    static_cast<double>(per), textColor());
            }
            else
            {
                set(ColPerStream, "0", 0, faintColor());
            }
        }

        auto ago = formatAgo(row.lastActive, now);
        set(ColLastActive, ago, static_cast<double>(row.lastActive),
            ago == "now" ? upColor() : textColor());
        if (idle)
        {
            table->item(r, ColLastActive)->setForeground(faintColor());
        }
    }

    for (int g = 0; g < GroupCount; ++g)
    {
        table->setColumnHidden(ColTimeouts + g, !this->groupEnabled(g));
    }
    table->setColumnHidden(ColChange, !this->compare_);
    table->setSortingEnabled(true);
    table->sortByColumn(sortColumn, sortOrder);

    if (bot)
    {
        // show every bot without an extra scrollbar
        int height = table->horizontalHeader()->height() + 4;
        for (int r = 0; r < table->rowCount(); ++r)
        {
            height += table->rowHeight(r);
        }
        table->setMaximumHeight(std::max(60, height));
    }
}

void ModActivityDialog::updateDetail()
{
    // clear the facts grid
    while (auto *item = this->ui_.facts->takeAt(0))
    {
        delete item->widget();
        delete item;
    }

    const auto name = this->selected_;
    this->ui_.detail->setVisible(!name.isEmpty());
    if (name.isEmpty())
    {
        return;
    }
    const bool bot = modactivity::isBot(name);
    this->ui_.detailTitle->setText(name);

    QStringList legend;
    for (int g = 0; g < GroupCount; ++g)
    {
        if (this->groupEnabled(g))
        {
            legend << QString("<span style=\"color:%1\">&#9632;</span> %2")
                          .arg(groupColor(g).name(),
                               QString::fromLatin1(
                                   GROUPS.at(static_cast<size_t>(g)).label));
        }
    }
    this->ui_.legend->setText(legend.join("&nbsp;&nbsp;&nbsp;"));

    // chart buckets, ending at the end of the range
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto endDate =
        QDateTime::fromMSecsSinceEpoch(std::min(this->range_.to, now) - 1)
            .date();
    std::vector<std::pair<QDate, QDate>> spans;  // [start, end)
    std::vector<modactivity_ui::BarChart::Bar> bars;
    switch (this->bucket_)
    {
        case Bucket::Days:
            for (int i = 13; i >= 0; --i)
            {
                auto d = endDate.addDays(-i);
                spans.emplace_back(d, d.addDays(1));
                bars.push_back({.label = cLocale().toString(d, "ddd d"),
                                .tooltip = cLocale().toString(d, "ddd d MMM"),
                                .counts = {}});
            }
            break;
        case Bucket::Weeks: {
            auto monday = endDate.addDays(1 - endDate.dayOfWeek());
            for (int i = 11; i >= 0; --i)
            {
                auto d = monday.addDays(static_cast<qint64>(-7) * i);
                spans.emplace_back(d, d.addDays(7));
                bars.push_back(
                    {.label = QString("W%1").arg(d.weekNumber()),
                     .tooltip =
                         QString("Week %1 (%2 to %3)")
                             .arg(d.weekNumber())
                             .arg(cLocale().toString(d, "d MMM"),
                                  cLocale().toString(d.addDays(6), "d MMM")),
                     .counts = {}});
            }
            break;
        }
        case Bucket::Months: {
            QDate first(endDate.year(), endDate.month(), 1);
            for (int i = 11; i >= 0; --i)
            {
                auto d = first.addMonths(-i);
                spans.emplace_back(d, d.addMonths(1));
                bars.push_back({.label = cLocale().toString(d, "MMM"),
                                .tooltip = cLocale().toString(d, "MMMM yyyy"),
                                .counts = {}});
            }
            break;
        }
    }
    const auto chartFrom =
        QDateTime(spans.front().first, QTime(0, 0)).toMSecsSinceEpoch();
    const auto chartTo =
        QDateTime(spans.back().second, QTime(0, 0)).toMSecsSinceEpoch();
    std::vector<qint64> bounds;
    bounds.reserve(spans.size() + 1);
    for (const auto &span : spans)
    {
        bounds.push_back(
            QDateTime(span.first, QTime(0, 0)).toMSecsSinceEpoch());
    }
    bounds.push_back(chartTo);

    auto begin = std::ranges::lower_bound(this->actions_, chartFrom, {},
                                          &modactivity::Action::msecs);
    for (auto it = begin; it != this->actions_.end() && it->msecs < chartTo;
         ++it)
    {
        if (it->moderator != name)
        {
            continue;
        }
        auto pos = std::ranges::upper_bound(bounds, it->msecs);
        auto index = std::distance(bounds.begin(), pos) - 1;
        if (index >= 0 && std::cmp_less(index, bars.size()))
        {
            bars[static_cast<size_t>(index)].counts.at(
                static_cast<size_t>(groupOf(it->kind))) += 1;
        }
    }
    for (auto &bar : bars)
    {
        QStringList lines{bar.tooltip};
        for (int g = 0; g < GroupCount; ++g)
        {
            if (this->groupEnabled(g))
            {
                lines << QString("%1: %2")
                             .arg(GROUPS.at(static_cast<size_t>(g)).label)
                             .arg(bar.counts.at(static_cast<size_t>(g)));
            }
        }
        bar.tooltip = lines.join("\n");
    }
    this->ui_.chart->setBars(std::move(bars), this->groups_);

    // facts within the range
    const auto from = this->range_.from;
    const auto to = this->range_.to;
    const Row *row = nullptr;
    int tableTotal = 0;
    for (const auto *rows : {&this->modRows_, &this->botRows_})
    {
        for (const auto &r : *rows)
        {
            if (r.name == name)
            {
                row = &r;
            }
        }
    }
    for (const auto &r : bot ? this->botRows_ : this->modRows_)
    {
        tableTotal += r.total;
    }

    std::array<int, 24> hours{};
    std::map<int, int> timeoutLengths;
    qint64 timeoutSum = 0;
    int timeoutCount = 0;
    int unbansOfOthers = 0;
    QSet<QDate> activeDays;
    QHash<QString, QString> lastPunisher;
    for (const auto &action : this->actions_)
    {
        if (action.msecs >= to)
        {
            break;
        }
        const bool inRange = action.msecs >= from;
        if (action.kind == Kind::Timeout || action.kind == Kind::Ban)
        {
            if (inRange && action.moderator == name &&
                action.kind == Kind::Timeout &&
                this->groupEnabled(GroupTimeouts))
            {
                timeoutLengths[action.seconds] += 1;
                timeoutSum += action.seconds;
                ++timeoutCount;
            }
            lastPunisher.insert(action.target, action.moderator);
        }
        else if (action.kind == Kind::Unban || action.kind == Kind::Untimeout)
        {
            if (inRange && action.moderator == name)
            {
                auto by = lastPunisher.value(action.target);
                if (!by.isEmpty() && by != name)
                {
                    ++unbansOfOthers;
                }
            }
            lastPunisher.remove(action.target);
        }
        if (!inRange || action.moderator != name ||
            !this->groupEnabled(groupOf(action.kind)))
        {
            continue;
        }
        auto dt = QDateTime::fromMSecsSinceEpoch(action.msecs);
        hours.at(static_cast<size_t>(dt.time().hour())) += 1;
        activeDays.insert(dt.date());
    }

    int factRow = 0;
    auto addFact = [&](const QString &key, const QString &value) {
        auto *keyLabel = new QLabel(key);
        keyLabel->setObjectName("dim");
        auto *valueLabel = new QLabel(value);
        valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        this->ui_.facts->addWidget(keyLabel, factRow, 0);
        this->ui_.facts->addWidget(valueLabel, factRow, 1);
        ++factRow;
    };

    addFact(
        bot ? "Share of bot actions" : "Share of human actions",
        (row != nullptr && tableTotal > 0)
            ? QString("%1%").arg(std::lround(row->total * 100.0 / tableTotal))
            : QString("-"));
    addFact("Streams active", (row != nullptr && this->streamCount_ > 0)
                                  ? QString("%1 of %2")
                                        .arg(row->streamsActive)
                                        .arg(this->streamCount_)
                                  : QString("-"));

    // longest stretch of days without an action
    {
        auto firstDay = QDateTime::fromMSecsSinceEpoch(from).date();
        auto lastDay =
            QDateTime::fromMSecsSinceEpoch(std::min(to, now) - 1).date();
        std::vector<QDate> days(activeDays.begin(), activeDays.end());
        std::ranges::sort(days);
        qint64 longest = 0;
        auto prev = firstDay.addDays(-1);
        for (const auto &d : days)
        {
            longest = std::max(longest, prev.daysTo(d) - 1);
            prev = d;
        }
        longest = std::max(longest, prev.daysTo(lastDay));
        addFact("Longest gap",
                longest <= 0 ? QString("none") : plural(longest, "day"));
    }

    auto *busiest = std::ranges::max_element(hours);
    if (*busiest > 0)
    {
        auto hour = static_cast<int>(std::distance(hours.begin(), busiest));
        addFact("Busiest hour", QString("%1:00 to %2:00")
                                    .arg(hour, 2, 10, QChar('0'))
                                    .arg((hour + 1) % 24, 2, 10, QChar('0')));
    }
    else
    {
        addFact("Busiest hour", "-");
    }

    if (timeoutCount > 0)
    {
        auto most = std::ranges::max_element(
            timeoutLengths, {}, &std::pair<const int, int>::second);
        addFact("Most used timeout", formatSeconds(most->first));
        addFact("Average timeout", formatSeconds(timeoutSum / timeoutCount));
    }
    else
    {
        addFact("Most used timeout", "-");
        addFact("Average timeout", "-");
    }
    addFact("Unbans of other mods' actions", QString::number(unbansOfOthers));
    addFact("Last active", formatAgo(this->lastActive_.value(name), now));
}

void ModActivityDialog::exportCsv()
{
    auto suggested =
        QDir(QDir::homePath())
            .filePath(QString("mod-activity-%1-%2.csv")
                          .arg(this->channel_,
                               QDate::currentDate().toString("yyyy-MM-dd")));
    auto path = QFileDialog::getSaveFileName(this, "Export CSV", suggested,
                                             "CSV files (*.csv)");
    if (path.isEmpty())
    {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate |
                   QIODevice::Text))
    {
        return;
    }
    QTextStream out(&file);
    auto *header = this->ui_.mods;
    QStringList cols{"Type"};
    for (int c = 0; c < ColCount; ++c)
    {
        if (!header->isColumnHidden(c))
        {
            cols << csvField(c == ColName
                                 ? QString("Name")
                                 : header->horizontalHeaderItem(c)->text());
        }
    }
    out << cols.join(',') << '\n';
    for (auto *table : {this->ui_.mods, this->ui_.bots})
    {
        if (!table->isVisible() && table == this->ui_.bots)
        {
            continue;
        }
        const QString type = table == this->ui_.mods ? "Moderator" : "Bot";
        for (int r = 0; r < table->rowCount(); ++r)
        {
            QStringList fields{type};
            for (int c = 0; c < ColCount; ++c)
            {
                if (!header->isColumnHidden(c))
                {
                    auto *item = table->item(r, c);
                    auto text = item != nullptr ? item->text() : QString();
                    // plain numbers, no thousands separators
                    if (c >= ColTimeouts && c <= ColTotal)
                    {
                        text.remove(u',');
                    }
                    fields << csvField(text);
                }
            }
            out << fields.join(',') << '\n';
        }
    }
}

}  // namespace chatterino
