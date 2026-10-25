// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/HighlightCategoriesTab.hpp"

#include "controllers/filters/FilterRecord.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "singletons/Settings.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"
#include "widgets/helper/color/Checkerboard.hpp"
#include "widgets/helper/color/ColorItemDelegate.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace chatterino {

namespace hc = highlightcategories;

namespace {

enum Column { Swatch = 0, Name = 1, Tag = 2, LaneColumn = 3, Words = 4 };

constexpr int UNSORTED_ROLE = Qt::UserRole + 1;

hc::Rule toRule(const HighlightPhrase &phrase)
{
    hc::Rule rule;
    rule.pattern = phrase.getPattern();
    rule.showInMentions = phrase.showInMentions();
    rule.alert = phrase.hasAlert();
    rule.sound = phrase.hasSound();
    rule.regex = phrase.isRegex();
    rule.caseSensitive = phrase.isCaseSensitive();
    rule.soundUrl = phrase.getSoundUrl().toString();
    rule.color = phrase.getColor() ? *phrase.getColor()
                                   : HighlightPhrase::FALLBACK_HIGHLIGHT_COLOR;
    return rule;
}

HighlightPhrase toPhrase(const hc::Rule &rule)
{
    return HighlightPhrase{
        rule.pattern, rule.showInMentions, rule.alert,    rule.sound,
        rule.regex,   rule.caseSensitive,  rule.soundUrl, rule.color};
}

QColor laneColor(const QString &lane)
{
    if (lane == u"Urgent")
    {
        return {0xff, 0x8a, 0x80};
    }
    if (lane == u"Check")
    {
        return {0xff, 0xd1, 0x66};
    }
    if (lane == u"FYI")
    {
        return {0x8f, 0xc1, 0xff};
    }
    return {0x9a, 0x9a, 0x9a};
}

QIcon swatchIcon(const QColor &color)
{
    QPixmap pixmap(44, 18);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    drawCheckerboard(painter, pixmap.rect(), 6);
    painter.fillRect(pixmap.rect(), color);
    painter.setPen(QColor(0x55, 0x55, 0x55));
    painter.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
    return {pixmap};
}

/// Paints the tag as a small grey pill, like the lane tags in chat
class TagPillDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        auto text = index.data(Qt::DisplayRole).toString();
        if (text.isEmpty())
        {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem opt = option;
        opt.text.clear();
        QStyledItemDelegate::paint(painter, opt, index);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        auto font = option.font;
        font.setBold(true);
        font.setPointSizeF(font.pointSizeF() * 0.8);
        painter->setFont(font);
        QFontMetrics metrics(font);
        auto width = metrics.horizontalAdvance(text) + 10;
        auto height = metrics.height() + 2;
        QRect pill(0, 0, std::min(width, option.rect.width() - 4), height);
        pill.moveCenter(option.rect.center());
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x3a, 0x3a, 0x3a));
        painter->drawRoundedRect(pill, 3, 3);
        painter->setPen(QColor(0xdd, 0xdd, 0xdd));
        painter->drawText(pill, Qt::AlignCenter, text);
        painter->restore();
    }
};

}  // namespace

HighlightCategoriesTab::HighlightCategoriesTab(
    std::function<void()> showMessagesTab)
    : showMessagesTab_(std::move(showMessagesTab))
{
    auto *layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(
        "Words you add to a category get its color and tag, so they show up "
        "in that category's lane.\n"
        "Categories are checked from top to bottom; the first one that "
        "matches wins."));

    auto *buttons = new QHBoxLayout;
    auto *newButton = new QPushButton("New category");
    this->upButton_ = new QPushButton("Move up");
    this->downButton_ = new QPushButton("Move down");
    buttons->addWidget(newButton);
    buttons->addWidget(this->upButton_);
    buttons->addWidget(this->downButton_);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto *split = new QHBoxLayout;
    layout->addLayout(split, 1);

    // category list
    this->list_ = new QTreeWidget;
    this->list_->setColumnCount(5);
    this->list_->setHeaderLabels({"", "Category", "Tag", "Lane", "Words"});
    this->list_->setRootIsDecorated(false);
    this->list_->setUniformRowHeights(true);
    this->list_->setSelectionMode(QAbstractItemView::SingleSelection);
    this->list_->setItemDelegateForColumn(Swatch,
                                          new ColorItemDelegate(this->list_));
    this->list_->setItemDelegateForColumn(Tag,
                                          new TagPillDelegate(this->list_));
    auto *header = this->list_->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Fixed);
    header->setSectionResizeMode(Name, QHeaderView::Stretch);
    header->resizeSection(Swatch, 22);
    header->resizeSection(Tag, 78);
    header->resizeSection(LaneColumn, 58);
    header->resizeSection(Words, 46);
    split->addWidget(this->list_, 11);

    // detail: one page for a category, one for the Unsorted row
    this->detail_ = new QStackedWidget;
    split->addWidget(this->detail_, 10);

    auto *categoryPage = new QWidget;
    auto *cat = new QVBoxLayout(categoryPage);
    cat->setContentsMargins(6, 0, 0, 0);
    auto *form = new QFormLayout;
    this->name_ = new QLineEdit;
    this->tag_ = new QLineEdit;
    this->color_ = new QPushButton;
    this->color_->setIconSize({44, 18});
    this->color_->setCursor(Qt::PointingHandCursor);
    this->lane_ = new QComboBox;
    form->addRow("Name", this->name_);
    form->addRow("Tag", this->tag_);
    auto *colorRow = new QHBoxLayout;
    colorRow->addWidget(this->color_);
    colorRow->addStretch(1);
    form->addRow("Color", colorRow);
    form->addRow("Lane", this->lane_);
    cat->addLayout(form);

    auto *checks = new QHBoxLayout;
    this->mentions_ = new QCheckBox("Show in Mentions");
    this->alert_ = new QCheckBox("Flash taskbar");
    this->sound_ = new QCheckBox("Play sound");
    checks->addWidget(this->mentions_);
    checks->addWidget(this->alert_);
    checks->addWidget(this->sound_);
    checks->addStretch(1);
    cat->addLayout(checks);

    this->wordsTitle_ = new QLabel;
    this->wordsTitle_->setTextFormat(Qt::RichText);
    cat->addWidget(this->wordsTitle_);

    this->words_ = new QListWidget;
    this->words_->setFlow(QListView::LeftToRight);
    this->words_->setWrapping(true);
    this->words_->setResizeMode(QListView::Adjust);
    this->words_->setMovement(QListView::Static);
    this->words_->setSpacing(2);
    this->words_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    this->words_->setStyleSheet(
        "QListWidget::item { border: 1px solid #4d4d4d; padding: 1px 6px; }"
        "QListWidget::item:selected { background: #2a6fc9; "
        "border-color: #5fa8ff; color: white; }");
    cat->addWidget(this->words_, 1);

    auto *addRow = new QHBoxLayout;
    this->wordInput_ = new QLineEdit;
    this->wordInput_->setPlaceholderText("Add a word or phrase");
    auto *addButton = new QPushButton("Add");
    this->removeButton_ = new QPushButton("Remove");
    addRow->addWidget(this->wordInput_, 1);
    addRow->addWidget(addButton);
    addRow->addWidget(this->removeButton_);
    cat->addLayout(addRow);

    this->status_ = new QLabel;
    this->status_->setWordWrap(true);
    this->status_->setStyleSheet("color: #7bd88f");
    cat->addWidget(this->status_);

    this->advanced_ = new QLabel;
    this->advanced_->setTextFormat(Qt::RichText);
    this->advanced_->setWordWrap(true);
    cat->addWidget(this->advanced_);
    this->detail_->addWidget(categoryPage);

    auto *unsortedPage = new QWidget;
    auto *uns = new QVBoxLayout(unsortedPage);
    uns->setContentsMargins(6, 0, 0, 0);
    this->unsortedTitle_ = new QLabel;
    this->unsortedTitle_->setTextFormat(Qt::RichText);
    uns->addWidget(this->unsortedTitle_);
    uns->addWidget(new QLabel(
        "These highlight but have no tag, so no lane picks them up."));
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *rowsHolder = new QWidget;
    this->unsortedRows_ = new QVBoxLayout(rowsHolder);
    this->unsortedRows_->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(rowsHolder);
    uns->addWidget(scroll, 1);
    this->unsortedStatus_ = new QLabel;
    this->unsortedStatus_->setWordWrap(true);
    this->unsortedStatus_->setStyleSheet("color: #7bd88f");
    uns->addWidget(this->unsortedStatus_);
    this->detail_->addWidget(unsortedPage);

    // signals
    QObject::connect(
        this->list_, &QTreeWidget::currentItemChanged, this,
        [this](QTreeWidgetItem *item) {
            if (this->updating_ || item == nullptr)
            {
                return;
            }
            this->unsortedSelected_ = item->data(Name, UNSORTED_ROLE).toBool();
            this->selected_ = item->data(Name, Qt::UserRole).toString();
            this->lastAdded_.clear();
            this->status_->clear();
            this->unsortedStatus_->clear();
            this->refreshDetail();
        });
    QObject::connect(newButton, &QPushButton::clicked, this, [this] {
        this->openNewCategoryDialog();
    });
    QObject::connect(this->upButton_, &QPushButton::clicked, this, [this] {
        this->moveSelected(-1);
    });
    QObject::connect(this->downButton_, &QPushButton::clicked, this, [this] {
        this->moveSelected(1);
    });
    QObject::connect(this->name_, &QLineEdit::editingFinished, this, [this] {
        if (this->name_->isModified())
        {
            this->name_->setModified(false);
            this->renameTo(this->name_->text());
        }
    });
    QObject::connect(this->tag_, &QLineEdit::editingFinished, this, [this] {
        if (this->tag_->isModified())
        {
            this->tag_->setModified(false);
            this->renameTo(hc::withTag(this->selected_, this->tag_->text()));
        }
    });
    QObject::connect(this->color_, &QPushButton::clicked, this, [this] {
        this->openColorDialog();
    });
    QObject::connect(
        this->lane_, &QComboBox::activated, this, [this](int index) {
            auto cat = this->current();
            if (!cat)
            {
                return;
            }
            auto was = laneOf(cat->tag);
            auto lane = this->lane_->itemData(index).toString();
            if (lane == was)
            {
                return;
            }
            setLane(cat->tag, lane);
            auto shared = 0;
            for (const auto &c : hc::categories(rules()))
            {
                shared += c.tag == cat->tag ? 1 : 0;
            }
            this->refresh();
            this->showStatus(QString("%1 moved from %2 to %3.%4")
                                 .arg(cat->tag, was.isEmpty() ? "none" : was,
                                      lane.isEmpty() ? "none" : lane,
                                      shared > 1 ? QString(" This moves all "
                                                           "%1 categories.")
                                                       .arg(shared)
                                                 : QString()));
        });
    for (auto *box : {this->mentions_, this->alert_, this->sound_})
    {
        QObject::connect(box, &QCheckBox::clicked, this, [this] {
            this->apply(hc::setCategoryFlags(
                rules(), this->selected_, this->mentions_->isChecked(),
                this->alert_->isChecked(), this->sound_->isChecked()));
        });
    }
    QObject::connect(this->wordInput_, &QLineEdit::returnPressed, this, [this] {
        this->addWord();
    });
    QObject::connect(addButton, &QPushButton::clicked, this, [this] {
        this->addWord();
    });
    QObject::connect(this->removeButton_, &QPushButton::clicked, this, [this] {
        this->removeSelectedWords();
    });
    auto *deleteKey = new QShortcut(QKeySequence::Delete, this->words_, nullptr,
                                    nullptr, Qt::WidgetShortcut);
    QObject::connect(deleteKey, &QShortcut::activated, this, [this] {
        this->removeSelectedWords();
    });
    QObject::connect(this->words_, &QListWidget::itemSelectionChanged, this,
                     [this] {
                         this->removeButton_->setEnabled(
                             !this->words_->selectedItems().isEmpty());
                     });
    QObject::connect(this->advanced_, &QLabel::linkActivated, this, [this] {
        if (this->showMessagesTab_)
        {
            this->showMessagesTab_();
        }
    });

    // Rules or filters changed elsewhere (Messages tab, Filters page, Cancel)
    this->connections_.managedConnect(
        getSettings()->highlightedMessages.delayedItemsChanged, [this] {
            this->refresh();
        });
    this->connections_.managedConnect(
        getSettings()->filterRecords.delayedItemsChanged, [this] {
            this->refresh();
        });

    this->refresh();
}

std::vector<hc::Rule> HighlightCategoriesTab::rules()
{
    std::vector<hc::Rule> out;
    for (const auto &phrase : getSettings()->highlightedMessages.raw())
    {
        out.push_back(toRule(phrase));
    }
    return out;
}

std::vector<HighlightCategoriesTab::Lane> HighlightCategoriesTab::lanes()
{
    std::vector<Lane> out;
    for (const auto &record : getSettings()->filterRecords.raw())
    {
        if (auto tags = hc::laneTags(record->getFilter()))
        {
            out.push_back(Lane{record->getName(), *tags});
        }
    }
    return out;
}

QString HighlightCategoriesTab::laneOf(const QString &tag)
{
    for (const auto &lane : lanes())
    {
        if (lane.tags.contains(tag))
        {
            return lane.name;
        }
    }
    return {};
}

void HighlightCategoriesTab::setLane(const QString &tag, const QString &lane)
{
    auto &records = getSettings()->filterRecords;
    auto snapshot = records.raw();
    for (int i = 0; i < static_cast<int>(snapshot.size()); i++)
    {
        const auto &record = snapshot[i];
        if (!hc::laneTags(record->getFilter()))
        {
            continue;
        }
        auto filter = hc::withLaneTag(record->getFilter(), tag,
                                      record->getName() == lane);
        if (filter != record->getFilter())
        {
            records.removeAt(i);
            records.insert(std::make_shared<FilterRecord>(
                               record->getName(), filter, record->getId()),
                           i);
        }
    }
}

void HighlightCategoriesTab::retag(const QString &oldTag, const QString &newTag,
                                   bool keepOld)
{
    auto &records = getSettings()->filterRecords;
    auto snapshot = records.raw();
    for (int i = 0; i < static_cast<int>(snapshot.size()); i++)
    {
        const auto &record = snapshot[i];
        auto tags = hc::laneTags(record->getFilter());
        if (!tags || !tags->contains(oldTag))
        {
            continue;
        }
        auto filter = hc::withLaneTag(record->getFilter(), newTag, true);
        if (!keepOld)
        {
            filter = hc::withLaneTag(filter, oldTag, false);
        }
        if (filter != record->getFilter())
        {
            records.removeAt(i);
            records.insert(std::make_shared<FilterRecord>(
                               record->getName(), filter, record->getId()),
                           i);
        }
    }
}

void HighlightCategoriesTab::apply(const std::vector<Rule> &next)
{
    auto &vector = getSettings()->highlightedMessages;
    auto current = rules();
    if (current == next)
    {
        return;
    }

    // only replace the changed middle part
    size_t prefix = 0;
    while (prefix < current.size() && prefix < next.size() &&
           current[prefix] == next[prefix])
    {
        prefix++;
    }
    size_t suffix = 0;
    while (suffix < current.size() - prefix && suffix < next.size() - prefix &&
           current[current.size() - 1 - suffix] ==
               next[next.size() - 1 - suffix])
    {
        suffix++;
    }
    for (size_t i = prefix; i < current.size() - suffix; i++)
    {
        vector.removeAt(static_cast<int>(prefix));
    }
    for (size_t i = prefix; i < next.size() - suffix; i++)
    {
        vector.insert(toPhrase(next[i]), static_cast<int>(i));
    }
    this->refresh();
}

std::optional<hc::Category> HighlightCategoriesTab::current() const
{
    if (this->unsortedSelected_ || this->selected_.isEmpty())
    {
        return std::nullopt;
    }
    for (auto &cat : hc::categories(rules()))
    {
        if (cat.name == this->selected_)
        {
            return cat;
        }
    }
    return std::nullopt;
}

void HighlightCategoriesTab::showStatus(const QString &text)
{
    this->status_->setText(text);
    this->unsortedStatus_->setText(text);
}

void HighlightCategoriesTab::refresh()
{
    auto all = rules();
    auto cats = hc::categories(all);
    auto unsorted = hc::unsortedRules(all);
    auto laneList = lanes();

    if (!this->unsortedSelected_ &&
        std::none_of(cats.begin(), cats.end(), [&](const auto &c) {
            return c.name == this->selected_;
        }))
    {
        this->selected_ = cats.empty() ? QString() : cats.front().name;
    }

    this->updating_ = true;
    this->list_->clear();
    QTreeWidgetItem *selectedItem = nullptr;
    for (const auto &cat : cats)
    {
        QString lane;
        for (const auto &l : laneList)
        {
            if (l.tags.contains(cat.tag))
            {
                lane = l.name;
                break;
            }
        }
        auto *item = new QTreeWidgetItem(this->list_);
        item->setData(Swatch, Qt::DecorationRole, all[cat.rules.front()].color);
        item->setText(Name, cat.name);
        item->setToolTip(Name, cat.name);
        item->setData(Name, Qt::UserRole, cat.name);
        item->setText(Tag, cat.tag);
        item->setText(LaneColumn, lane.isEmpty() ? "none" : lane);
        item->setForeground(LaneColumn, laneColor(lane));
        item->setTextAlignment(LaneColumn, Qt::AlignCenter);
        item->setText(Words, QString::number(cat.words.size()));
        item->setTextAlignment(Words, Qt::AlignRight | Qt::AlignVCenter);
        if (!this->unsortedSelected_ && cat.name == this->selected_)
        {
            selectedItem = item;
        }
    }
    auto *unsortedItem = new QTreeWidgetItem(this->list_);
    unsortedItem->setText(Name, "Unsorted (no category)");
    unsortedItem->setData(Name, UNSORTED_ROLE, true);
    auto font = unsortedItem->font(Name);
    font.setItalic(true);
    unsortedItem->setFont(Name, font);
    unsortedItem->setForeground(Name, QColor(0xc9, 0xa2, 0x27));
    unsortedItem->setText(LaneColumn, "none");
    unsortedItem->setForeground(LaneColumn, laneColor({}));
    unsortedItem->setTextAlignment(LaneColumn, Qt::AlignCenter);
    unsortedItem->setText(Words, QString::number(unsorted.size()));
    unsortedItem->setTextAlignment(Words, Qt::AlignRight | Qt::AlignVCenter);
    if (this->unsortedSelected_)
    {
        selectedItem = unsortedItem;
    }
    if (selectedItem != nullptr)
    {
        this->list_->setCurrentItem(selectedItem);
        this->list_->scrollToItem(selectedItem);
    }
    this->updating_ = false;

    this->refreshDetail();
}

void HighlightCategoriesTab::refreshDetail()
{
    auto selectedIndex =
        this->list_->indexOfTopLevelItem(this->list_->currentItem());
    this->upButton_->setEnabled(!this->unsortedSelected_ && selectedIndex > 0);
    this->downButton_->setEnabled(
        !this->unsortedSelected_ && selectedIndex >= 0 &&
        selectedIndex < this->list_->topLevelItemCount() - 2);

    if (this->unsortedSelected_)
    {
        this->detail_->setCurrentIndex(1);
        this->refreshUnsorted();
        return;
    }
    this->detail_->setCurrentIndex(0);

    auto cat = this->current();
    this->detail_->widget(0)->setEnabled(cat.has_value());
    if (!cat)
    {
        return;
    }
    auto all = rules();
    const auto &first = all[cat->rules.front()];

    if (!(this->name_->hasFocus() && this->name_->isModified()))
    {
        this->name_->setText(cat->name);
    }
    if (!(this->tag_->hasFocus() && this->tag_->isModified()))
    {
        this->tag_->setText(cat->tag);
    }
    this->color_->setIcon(swatchIcon(first.color));
    this->color_->setText(" " + first.color.name(QColor::HexArgb).toUpper());

    this->lane_->clear();
    auto lane = laneOf(cat->tag);
    for (const auto &l : lanes())
    {
        this->lane_->addItem(l.name, l.name);
    }
    this->lane_->addItem("None", QString());
    this->lane_->setCurrentIndex(this->lane_->findData(lane));

    this->mentions_->setChecked(first.showInMentions);
    this->alert_->setChecked(first.alert);
    this->sound_->setChecked(first.sound);

    this->wordsTitle_->setText(
        QString("<b>Words</b> &nbsp;<span style=\"color: #9a9a9a\">%1 · "
                "whole words, any case</span>")
            .arg(cat->words.size()));

    auto words = cat->words;
    std::sort(words.begin(), words.end(), [](const auto &a, const auto &b) {
        return QString::localeAwareCompare(a, b) < 0;
    });
    this->words_->clear();
    for (const auto &word : words)
    {
        auto *item = new QListWidgetItem(word, this->words_);
        if (word == this->lastAdded_)
        {
            item->setSelected(true);
            this->words_->scrollToItem(item);
        }
    }
    if (words.isEmpty())
    {
        this->words_->setToolTip("No words yet. Add the first one below.");
    }
    else
    {
        this->words_->setToolTip(QString());
    }
    this->removeButton_->setEnabled(!this->words_->selectedItems().isEmpty());

    if (cat->advancedCount > 0)
    {
        this->advanced_->setText(
            QString("<span style=\"color: #9a9a9a\">Also %1 advanced "
                    "pattern%2 in this category.</span> <a href=\"#\">Edit in "
                    "Messages tab</a>")
                .arg(cat->advancedCount)
                .arg(cat->advancedCount == 1 ? "" : "s"));
        this->advanced_->show();
    }
    else
    {
        this->advanced_->hide();
    }
}

void HighlightCategoriesTab::refreshUnsorted()
{
    while (auto *child = this->unsortedRows_->takeAt(0))
    {
        if (auto *w = child->widget())
        {
            w->deleteLater();
        }
        delete child;
    }

    auto all = rules();
    auto cats = hc::categories(all);
    auto unsorted = hc::unsortedRules(all);
    this->unsortedTitle_->setText(
        QString("<b>Unsorted</b> &nbsp;<span style=\"color: #9a9a9a\">%1 "
                "rule%2 without a category</span>")
            .arg(unsorted.size())
            .arg(unsorted.size() == 1 ? "" : "s"));

    if (unsorted.empty())
    {
        auto *empty = new QLabel("<i>Nothing unsorted.</i>");
        this->unsortedRows_->addWidget(empty);
    }
    for (auto index : unsorted)
    {
        const auto rule = all[index];
        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 2, 0, 2);
        auto *pattern = new QLabel(rule.pattern);
        pattern->setToolTip(rule.regex ? "Regex rule" : "Plain word");
        pattern->setTextFormat(Qt::PlainText);
        pattern->setMinimumWidth(60);
        auto *target = new QComboBox;
        target->addItem("Pick a category", QString());
        for (const auto &cat : cats)
        {
            target->addItem(cat.name, cat.name);
        }
        auto *move = new QPushButton("Move");
        rowLayout->addWidget(pattern, 1);
        rowLayout->addWidget(target, 2);
        rowLayout->addWidget(move);
        this->unsortedRows_->addWidget(row);

        QObject::connect(
            move, &QPushButton::clicked, this, [this, rule, target] {
                auto name = target->currentData().toString();
                if (name.isEmpty())
                {
                    this->showStatus("Pick a category first.");
                    return;
                }
                // find the rule again, the list may have changed
                auto now = rules();
                for (auto i : hc::unsortedRules(now))
                {
                    if (now[i] == rule)
                    {
                        this->apply(hc::moveUnsortedRule(now, i, name));
                        this->showStatus(QString("\"%1\" moved to %2.")
                                             .arg(rule.pattern, name));
                        return;
                    }
                }
            });
    }
    this->unsortedRows_->addStretch(1);
}

void HighlightCategoriesTab::renameTo(const QString &newName)
{
    auto cat = this->current();
    if (!cat)
    {
        return;
    }
    auto clean = hc::cleanName(newName);
    if (clean == cat->name)
    {
        this->refreshDetail();
        return;
    }
    auto cats = hc::categories(rules());
    if (clean.isEmpty())
    {
        this->showStatus("A category needs a name.");
    }
    else if (hc::categoryName(clean) != clean)
    {
        this->showStatus("A name can't end in a number.");
    }
    else if (std::any_of(cats.begin(), cats.end(), [&](const auto &c) {
                 return c.name == clean;
             }))
    {
        this->showStatus("Another category already has that name.");
    }
    else
    {
        auto oldTag = cat->tag;
        auto newTag = hc::tagOf(clean);
        auto stillUsed =
            std::count_if(cats.begin(), cats.end(), [&](const auto &c) {
                return c.tag == oldTag;
            }) > 1;
        this->selected_ = clean;
        this->apply(hc::renameCategory(rules(), cat->name, clean));
        if (oldTag != newTag)
        {
            retag(oldTag, newTag, stillUsed);
            this->refresh();
            this->showStatus(
                QString("Tag is now %1, lanes updated.").arg(newTag));
        }
        return;
    }
    this->refreshDetail();
}

void HighlightCategoriesTab::addWord()
{
    auto cat = this->current();
    auto word = hc::normalizeWord(this->wordInput_->text());
    if (!cat || word.isEmpty())
    {
        return;
    }
    if (cat->words.contains(word))
    {
        this->showStatus(
            QString("\"%1\" is already in this category.").arg(word));
        return;
    }
    QString other;
    for (const auto &c : hc::categories(rules()))
    {
        if (c.name != cat->name && c.words.contains(word))
        {
            other = c.name;
            break;
        }
    }
    this->lastAdded_ = word;
    this->wordInput_->clear();
    this->apply(hc::addWord(rules(), cat->name, word));
    if (other.isEmpty())
    {
        this->showStatus(
            QString("Added \"%1\". New messages with it get the %2 tag.")
                .arg(word, cat->tag));
    }
    else
    {
        this->showStatus(QString("Added \"%1\". It is also in %2; the one "
                                 "higher in the list wins.")
                             .arg(word, other));
    }
    this->wordInput_->setFocus();
}

void HighlightCategoriesTab::removeSelectedWords()
{
    auto cat = this->current();
    auto selected = this->words_->selectedItems();
    if (!cat || selected.isEmpty())
    {
        return;
    }
    QStringList words;
    for (auto *item : selected)
    {
        words.append(item->text());
    }
    this->lastAdded_.clear();
    this->apply(hc::removeWords(rules(), cat->name, words));
    this->showStatus(words.size() == 1
                         ? QString("Removed \"%1\".").arg(words.front())
                         : QString("Removed %1 words.").arg(words.size()));
}

void HighlightCategoriesTab::moveSelected(int direction)
{
    if (this->unsortedSelected_ || this->selected_.isEmpty())
    {
        return;
    }
    this->status_->clear();
    this->apply(hc::moveCategory(rules(), this->selected_, direction));
}

void HighlightCategoriesTab::openColorDialog()
{
    auto cat = this->current();
    if (!cat)
    {
        return;
    }
    auto name = cat->name;
    auto initial = rules()[cat->rules.front()].color;
    auto *dialog = new ColorPickerDialog(initial, this);
    QObject::connect(
        dialog, &ColorPickerDialog::colorConfirmed, this,
        [this, name](QColor selected) {
            if (selected.isValid())
            {
                this->apply(hc::setCategoryColor(rules(), name, selected));
            }
        });
    dialog->show();
}

void HighlightCategoriesTab::openNewCategoryDialog()
{
    auto *dialog = new QDialog(this);
    dialog->setWindowTitle("New category");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(dialog);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit;
    name->setPlaceholderText("e.g. SCAM: Fake giveaways");
    auto *tag = new QLineEdit;
    tag->setPlaceholderText("first word of the name");
    auto *lane = new QComboBox;
    for (const auto &l : lanes())
    {
        lane->addItem(l.name, l.name);
    }
    lane->addItem("None", QString());
    auto color = std::make_shared<QColor>(0xff, 0x4f, 0xa0, 0x5a);
    auto *colorButton = new QPushButton;
    colorButton->setIconSize({44, 18});
    auto updateColor = [colorButton, color] {
        colorButton->setIcon(swatchIcon(*color));
        colorButton->setText(" " + color->name(QColor::HexArgb).toUpper());
    };
    updateColor();
    auto *place = new QComboBox;
    auto cats = hc::categories(rules());
    for (const auto &cat : cats)
    {
        place->addItem("Above " + cat.name, cat.name);
    }
    place->addItem("At the bottom", QString());
    auto at = place->findData(this->selected_);
    place->setCurrentIndex(
        at >= 0 && !this->unsortedSelected_ ? at : place->count() - 1);

    form->addRow("Name", name);
    form->addRow("Tag", tag);
    form->addRow("Lane", lane);
    auto *colorRow = new QHBoxLayout;
    colorRow->addWidget(colorButton);
    colorRow->addStretch(1);
    form->addRow("Color", colorRow);
    form->addRow("Place", place);
    layout->addLayout(form);
    auto *error = new QLabel;
    error->setStyleSheet("color: #ff8a80");
    error->hide();
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox;
    buttons->addButton("Create", QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    QObject::connect(colorButton, &QPushButton::clicked, dialog,
                     [dialog, color, updateColor] {
                         auto *picker = new ColorPickerDialog(*color, dialog);
                         QObject::connect(
                             picker, &ColorPickerDialog::colorConfirmed, dialog,
                             [color, updateColor](QColor c) {
                                 if (c.isValid())
                                 {
                                     *color = c;
                                     updateColor();
                                 }
                             });
                         picker->show();
                     });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog,
                     &QDialog::reject);
    QObject::connect(
        buttons, &QDialogButtonBox::accepted, dialog,
        [this, dialog, name, tag, lane, place, color, error] {
            auto label = hc::withTag(hc::cleanName(name->text()), tag->text());
            auto cats = hc::categories(rules());
            QString problem;
            if (label.isEmpty())
            {
                problem = "Give the category a name.";
            }
            else if (hc::categoryName(label) != label)
            {
                problem = "A name can't end in a number.";
            }
            else if (std::any_of(cats.begin(), cats.end(), [&](const auto &c) {
                         return c.name == label;
                     }))
            {
                problem = "Another category already has that name.";
            }
            else if (hc::tagOf(label).isEmpty())
            {
                problem = "The name needs a word with letters or numbers.";
            }
            if (!problem.isEmpty())
            {
                error->setText(problem);
                error->show();
                return;
            }

            auto laneName = lane->currentData().toString();
            this->unsortedSelected_ = false;
            this->selected_ = label;
            this->apply(hc::createCategory(rules(), label, *color,
                                           place->currentData().toString()));
            if (!laneName.isEmpty())
            {
                setLane(hc::tagOf(label), laneName);
                this->refresh();
            }
            this->showStatus(
                laneName.isEmpty()
                    ? QString("Created. Add its first word below.")
                    : QString("Created. Added %1 to your %2 filter. Add its "
                              "first word below.")
                          .arg(hc::tagOf(label), laneName));
            this->wordInput_->setFocus();
            dialog->accept();
        });

    dialog->show();
}

}  // namespace chatterino
