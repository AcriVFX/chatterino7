// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/NotebookGroup.hpp"

#include "Application.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/Notebook.hpp"

#include <QApplication>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>

#include <algorithm>
#include <cmath>

namespace chatterino {

namespace {

constexpr int OFFLINE_POPUP_COLUMNS = 3;

QFont headerFont(float scale)
{
    auto font = getApp()->getFonts()->getFont(FontStyle::UiTabs, scale);
    font.setBold(true);
    font.setCapitalization(QFont::AllUppercase);
    font.setPointSizeF(font.pointSizeF() * 0.9);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5 * scale);
    return font;
}

QFont countFont(float scale)
{
    auto font = getApp()->getFonts()->getFont(FontStyle::UiTabs, scale);
    font.setBold(true);
    return font;
}

/// Strongest highlight among the group's tabs that are not shown right now
HighlightState hiddenHighlight(const Notebook *notebook,
                               const NotebookTabGroup *group)
{
    auto state = HighlightState::None;
    for (const auto *tab : notebook->tabsInGroup(group))
    {
        if (notebook->isTabShown(tab))
        {
            continue;
        }
        if (tab->highlightState() == HighlightState::Highlighted)
        {
            return HighlightState::Highlighted;
        }
        if (tab->highlightState() == HighlightState::NewMessage)
        {
            state = HighlightState::NewMessage;
        }
    }
    return state;
}

const Theme::TabColors &colorsFor(const Theme *theme, HighlightState state)
{
    switch (state)
    {
        case HighlightState::Highlighted:
            return theme->tabs.highlighted;
        case HighlightState::NewMessage:
            return theme->tabs.newMessage;
        default:
            return theme->tabs.regular;
    }
}

}  // namespace

QColor notebookGroupMarkerColor(const Theme *theme)
{
    auto color = theme->tabs.regular.text;
    color.setAlpha(70);
    return color;
}

QRect notebookGroupMarkerRect(QRect rect, NotebookTabLocation location,
                              int thickness)
{
    switch (location)
    {
        case NotebookTabLocation::Left:
            return {rect.right() - thickness + 1, rect.top(), thickness,
                    rect.height()};
        case NotebookTabLocation::Right:
            return {rect.left(), rect.top(), thickness, rect.height()};
        case NotebookTabLocation::Bottom:
            return {rect.left(), rect.top(), rect.width(), thickness};
        case NotebookTabLocation::Top:
        default:
            return {rect.left(), rect.bottom() - thickness + 1, rect.width(),
                    thickness};
    }
}

NotebookGroupWidget::NotebookGroupWidget(Notebook *notebook,
                                         NotebookTabGroup *group)
    : Button(notebook)
    , notebook_(notebook)
    , group_(group)
{
    this->setContentCacheEnabled(false);
    this->setMouseEffectColor(this->theme->tabs.regular.text);
}

void NotebookGroupWidget::themeChangedEvent()
{
    this->setMouseEffectColor(this->theme->tabs.regular.text);
    this->update();
}

void NotebookGroupWidget::paintBackground(QPainter &painter, bool dashed) const
{
    auto scale = this->scale();
    bool windowFocused = this->window() == QApplication::activeWindow();
    const auto &colors = this->theme->tabs.regular;

    auto rect = this->rect();
    rect.setTop(static_cast<int>(std::ceil(scale)));

    if (dashed)
    {
        QPen pen(notebookGroupMarkerColor(this->theme));
        pen.setStyle(Qt::DashLine);
        pen.setWidthF(scale);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect.adjusted(0, 0, -1, -1));
    }
    else
    {
        auto background = windowFocused ? colors.backgrounds.regular
                                        : colors.backgrounds.unfocused;
        painter.fillRect(rect, background.darker(115));
    }

    auto thickness = std::max(1, static_cast<int>(2 * scale));
    painter.fillRect(
        notebookGroupMarkerRect(this->rect(), this->notebook_->tabLocation(),
                                thickness),
        notebookGroupMarkerColor(this->theme));
}

void NotebookGroupWidget::paintChevron(QPainter &painter, QPointF center,
                                       int direction) const
{
    // direction: 0 = down (expanded), 1 = right (collapsed), 2 = up (open)
    auto s = 2.5 * this->scale();
    QPainterPath path;
    switch (direction)
    {
        case 1:
            path.moveTo(center + QPointF(-s / 2, -s));
            path.lineTo(center + QPointF(s / 2, 0));
            path.lineTo(center + QPointF(-s / 2, s));
            break;
        case 2:
            path.moveTo(center + QPointF(-s, s / 2));
            path.lineTo(center + QPointF(0, -s / 2));
            path.lineTo(center + QPointF(s, s / 2));
            break;
        default:
            path.moveTo(center + QPointF(-s, -s / 2));
            path.lineTo(center + QPointF(0, s / 2));
            path.lineTo(center + QPointF(s, -s / 2));
            break;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen = painter.pen();
    pen.setWidthF(1.3 * this->scale());
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    painter.restore();
}

NotebookGroupHeader::NotebookGroupHeader(Notebook *notebook,
                                         NotebookTabGroup *group)
    : NotebookGroupWidget(notebook, group)
{
    QObject::connect(
        this, &Button::clicked, this, [this](Qt::MouseButton button) {
            if (button == Qt::LeftButton)
            {
                this->notebook_->toggleTabGroupCollapsed(this->group_);
            }
            else if (button == Qt::RightButton)
            {
                this->notebook_->showTabGroupMenu(
                    this->group_, QCursor::pos() + QPoint(0, 8));
            }
        });
}

int NotebookGroupHeader::desiredWidth() const
{
    auto scale = this->scale();
    QFontMetrics metrics(headerFont(scale));
    int width = static_cast<int>((6 + 8 + 6 + 9) * scale) +
                metrics.horizontalAdvance(this->group_->name);

    int live = 0;
    for (const auto *tab : this->notebook_->tabsInGroup(this->group_))
    {
        live += tab->isLive() ? 1 : 0;
    }
    if (live > 0)
    {
        QFontMetrics countMetrics(countFont(scale));
        width += static_cast<int>((6 + 4 + 3) * scale) +
                 countMetrics.horizontalAdvance(QString::number(live));
    }
    return width;
}

void NotebookGroupHeader::paintContent(QPainter &painter)
{
    auto scale = this->scale();
    this->paintBackground(painter, false);

    auto highlight = this->group_->collapsed
                         ? hiddenHighlight(this->notebook_, this->group_)
                         : HighlightState::None;
    const auto &colors = colorsFor(this->theme, highlight);

    if (highlight == HighlightState::Highlighted)
    {
        painter.fillRect(0, static_cast<int>(std::ceil(scale)), this->width(),
                         static_cast<int>(std::ceil(scale)),
                         colors.line.regular);
    }

    painter.setPen(colors.text);

    auto centerY = this->height() / 2.0;
    this->paintChevron(painter, {(6 + 4) * scale, centerY},
                       this->group_->collapsed ? 1 : 0);

    auto font = headerFont(scale);
    painter.setFont(font);
    QFontMetrics metrics(font);
    int x = static_cast<int>((6 + 8 + 6) * scale);
    QRect textRect(x, 0, metrics.horizontalAdvance(this->group_->name),
                   this->height());
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                     this->group_->name);
    x = textRect.right() + 1;

    int live = 0;
    for (const auto *tab : this->notebook_->tabsInGroup(this->group_))
    {
        live += tab->isLive() ? 1 : 0;
    }
    if (live > 0)
    {
        x += static_cast<int>(6 * scale);
        auto diameter = 4 * scale;
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(this->theme->tabs.liveIndicator);
        painter.drawEllipse(
            QRectF(x, centerY - diameter / 2, diameter, diameter));
        painter.restore();
        x += static_cast<int>((4 + 3) * scale);

        auto count = countFont(scale);
        painter.setFont(count);
        painter.setPen(this->theme->tabs.liveIndicator.lighter(130));
        painter.drawText(QRect(x, 0, this->width() - x, this->height()),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(live));
    }
}

NotebookGroupOfflineButton::NotebookGroupOfflineButton(Notebook *notebook,
                                                       NotebookTabGroup *group)
    : NotebookGroupWidget(notebook, group)
{
    QObject::connect(this, &Button::leftClicked, this, [this] {
        this->showPopup();
    });
}

QString NotebookGroupOfflineButton::text() const
{
    int hidden = 0;
    for (const auto *tab : this->notebook_->tabsInGroup(this->group_))
    {
        hidden += this->notebook_->isTabShown(tab) ? 0 : 1;
    }
    return QString("%1 offline").arg(hidden);
}

int NotebookGroupOfflineButton::desiredWidth() const
{
    auto scale = this->scale();
    QFontMetrics metrics(countFont(scale));
    return static_cast<int>((10 + 5 + 8 + 10) * scale) +
           metrics.horizontalAdvance(this->text());
}

void NotebookGroupOfflineButton::paintContent(QPainter &painter)
{
    auto scale = this->scale();
    this->paintBackground(painter, true);

    auto highlight = hiddenHighlight(this->notebook_, this->group_);
    const auto &colors = colorsFor(this->theme, highlight);
    painter.setPen(colors.text);

    auto font = countFont(scale);
    if (highlight == HighlightState::None)
    {
        font.setBold(false);
    }
    painter.setFont(font);
    QFontMetrics metrics(font);
    auto text = this->text();
    int x = static_cast<int>(10 * scale);
    QRect textRect(x, 0, metrics.horizontalAdvance(text), this->height());
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);

    this->paintChevron(
        painter,
        {static_cast<qreal>(textRect.right()) + ((5 + 4) * scale),
         this->height() / 2.0},
        0);
}

void NotebookGroupOfflineButton::showPopup()
{
    auto *popup = new QWidget(this, Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);

    const auto *theme = this->theme;
    auto background = theme->window.background;
    auto border = notebookGroupMarkerColor(theme);
    auto hover = theme->tabs.regular.backgrounds.regular;
    popup->setStyleSheet(
        QString("QWidget#offlinePopup { background: %1; border: 1px solid "
                "%2; }"
                "QPushButton { background: transparent; border: none; "
                "text-align: left; padding: 4px 10px; color: %3; }"
                "QPushButton:hover { background: %4; }"
                "QPushButton[unread=\"true\"] { color: %5; font-weight: bold; "
                "}"
                "QLabel { color: %6; padding: 4px 10px 6px 10px; }")
            .arg(background.name(), border.name(QColor::HexArgb),
                 theme->tabs.regular.text.name(), hover.name(),
                 theme->tabs.newMessage.text.name(),
                 theme->tabs.regular.text.darker(130).name()));
    popup->setObjectName("offlinePopup");

    auto *layout = new QGridLayout(popup);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setHorizontalSpacing(2);
    layout->setVerticalSpacing(0);

    auto *title =
        new QLabel(QString("OFFLINE IN %1").arg(this->group_->name.toUpper()));
    auto titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 0.85);
    title->setFont(titleFont);
    layout->addWidget(title, 0, 0, 1, OFFLINE_POPUP_COLUMNS);

    int index = 0;
    for (auto *tab : this->notebook_->tabsInGroup(this->group_))
    {
        if (this->notebook_->isTabShown(tab))
        {
            continue;
        }

        auto *button = new QPushButton(tab->getTitle());
        button->setCursor(Qt::PointingHandCursor);
        button->setFont(
            getApp()->getFonts()->getFont(FontStyle::UiTabs, this->scale()));
        button->setProperty("unread",
                            tab->highlightState() != HighlightState::None);
        auto *page = tab->page;
        QObject::connect(button, &QPushButton::clicked, popup,
                         [this, popup, page] {
                             popup->close();
                             this->notebook_->select(page);
                         });
        layout->addWidget(button, 1 + index / OFFLINE_POPUP_COLUMNS,
                          index % OFFLINE_POPUP_COLUMNS);
        index++;
    }

    popup->adjustSize();

    auto pos = this->mapToGlobal(QPoint(0, this->height()));
    if (auto *screen = this->screen())
    {
        auto available = screen->availableGeometry();
        pos.setX(std::min(pos.x(), available.right() - popup->width()));
        pos.setX(std::max(pos.x(), available.left()));
    }
    popup->move(pos);
    popup->show();
}

}  // namespace chatterino
