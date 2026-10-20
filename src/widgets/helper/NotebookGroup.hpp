// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/buttons/Button.hpp"
#include "widgets/NotebookEnums.hpp"

#include <QString>

class QColor;

namespace chatterino {

class Notebook;
class NotebookGroupHeader;
class NotebookGroupOfflineButton;
class Theme;

/// A named group of tabs in a Notebook ("folder"). Tabs refer to their group
/// by name (NotebookTab::getGroup), the Notebook owns the groups.
struct NotebookTabGroup {
    QString name;

    /// Only the group header is shown
    bool collapsed = false;

    /// Offline tabs of this group are hidden behind the "N offline" button
    bool liveOnly = false;

    NotebookGroupHeader *header = nullptr;
    NotebookGroupOfflineButton *offlineButton = nullptr;
};

/// The 2px line under every tab, header and button of a group
QColor notebookGroupMarkerColor(const Theme *theme);
QRect notebookGroupMarkerRect(QRect rect, NotebookTabLocation location,
                              int thickness);

/// Base for the widgets a group adds to the tab bar
class NotebookGroupWidget : public Button
{
    Q_OBJECT

public:
    NotebookGroupWidget(Notebook *notebook, NotebookTabGroup *group);

    /// Width this widget wants in a horizontal tab bar
    virtual int desiredWidth() const = 0;

protected:
    void themeChangedEvent() override;

    void paintBackground(QPainter &painter, bool dashed) const;
    void paintChevron(QPainter &painter, QPointF center, int direction) const;

    Notebook *notebook_;
    NotebookTabGroup *group_;
};

/// Chevron, group name and live count. Click to collapse or expand,
/// right-click for the group's settings.
class NotebookGroupHeader : public NotebookGroupWidget
{
    Q_OBJECT

public:
    NotebookGroupHeader(Notebook *notebook, NotebookTabGroup *group);

    int desiredWidth() const override;

protected:
    void paintContent(QPainter &painter) override;
};

/// "N offline" button after the live tabs of a live-only group. Opens a list
/// of the group's hidden tabs.
class NotebookGroupOfflineButton : public NotebookGroupWidget
{
    Q_OBJECT

public:
    NotebookGroupOfflineButton(Notebook *notebook, NotebookTabGroup *group);

    int desiredWidth() const override;

protected:
    void paintContent(QPainter &painter) override;

private:
    QString text() const;
    void showPopup();
};

}  // namespace chatterino
