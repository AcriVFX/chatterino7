// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"
#include "widgets/NotebookEnums.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QWidget>

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace chatterino {

class Button;
class PixmapButton;
class Window;
class DrawnButton;
class NotebookTab;
class NotebookGroupWidget;
struct NotebookTabGroup;
class SplitContainer;
class Split;

using TabVisibilityFilter = std::function<bool(const NotebookTab *)>;

class Notebook : public BaseWidget
{
    Q_OBJECT

public:
    explicit Notebook(QWidget *parent);
    ~Notebook() override;

    NotebookTab *addPage(QWidget *page, QString title = QString(),
                         bool select = false);

    /**
     * @brief Adds a page to the Notebook at a given position.
     *
     * @param position if set to -1, adds the page to the end
     **/
    NotebookTab *addPageAt(QWidget *page, int position,
                           QString title = QString(), bool select = false);
    void removePage(QWidget *page);
    void duplicatePage(QWidget *page);
    void removeCurrentPage();

    /**
     * @brief Returns index of page in Notebook, or -1 if not found.
     **/
    int indexOf(QWidget *page) const;

    /**
     * @brief Returns the visible index of page in Notebook, or -1 if not found.
     * Given page should be visible according to the set TabVisibilityFilter.
     **/
    int visibleIndexOf(QWidget *page) const;

    /**
     * @brief Returns the number of visible tabs in Notebook. 
     **/
    int getVisibleTabCount() const;

    /**
     * @brief Selects the Notebook tab containing the given page.
     **/
    virtual void select(QWidget *page, bool focusPage = true);

    /**
     * @brief Selects the Notebook tab at the given index. Ignores whether tabs
     * are visible or not. 
     **/
    void selectIndex(int index, bool focusPage = true);

    /**
     * @brief Selects the index'th visible tab in the Notebook.
     * 
     * For example, selecting the 0th visible tab selects the first tab in this 
     * Notebook that is visible according to the TabVisibilityFilter. If no filter
     * is set, equivalent to Notebook::selectIndex.
     **/
    void selectVisibleIndex(int index, bool focusPage = true);

    /**
     * @brief Selects the next visible tab. Wraps to the start if required. 
     **/
    void selectNextTab(bool focusPage = true);

    /**
     * @brief Selects the previous visible tab. Wraps to the end if required. 
     **/
    void selectPreviousTab(bool focusPage = true);

    /**
     * @brief Selects the last visible tab. 
     **/
    void selectLastTab(bool focusPage = true);

    int getPageCount() const;
    QWidget *getPageAt(int index) const;
    int getSelectedIndex() const;
    QWidget *getSelectedPage() const;

    QWidget *tabAt(QPoint point, int &index, int maxWidth = 2000000000);
    void rearrangePage(QWidget *page, int index);

    bool getAllowUserTabManagement() const;
    void setAllowUserTabManagement(bool value);

    bool getShowAddButton() const;
    void setShowAddButton(bool value);

    void setTabLocation(NotebookTabLocation location);

    bool isNotebookLayoutLocked() const;
    virtual void setLockNotebookLayout(bool value);

    virtual void addNotebookActionsToMenu(QMenu *menu);

    // Update layout and tab visibility
    void refresh();

    NotebookTabLocation tabLocation() const;

    /// Returns true if the tab is shown in the tab bar right now
    bool isTabShown(const NotebookTab *tab) const;

    // Tab groups ("folders" in the tab bar)
    const std::vector<std::unique_ptr<NotebookTabGroup>> &tabGroups() const;
    NotebookTabGroup *findTabGroup(const QString &name) const;
    /// Tabs of the group, in tab order
    std::vector<NotebookTab *> tabsInGroup(const NotebookTabGroup *group) const;

    /// Creates the group if it doesn't exist yet
    NotebookTabGroup *addTabGroup(const QString &name, bool collapsed = false,
                                  bool liveOnly = false);
    /// Sets the tab's group without moving it, used when loading the layout.
    /// Creates the group if needed.
    void assignTabGroup(NotebookTab *tab, const QString &name);
    /// Call after assignTabGroup for all loaded tabs: regroups the tab order,
    /// drops empty groups and applies collapsed/live-only
    void finishLoadingTabGroups();
    /// Moves the tab into the group (to its end), or out of its group if
    /// name is empty. Does nothing while the tab layout is locked.
    void moveTabToGroup(NotebookTab *tab, const QString &name);
    void promptNewTabGroup(NotebookTab *tab);
    void toggleTabGroupCollapsed(NotebookTabGroup *group);
    void showTabGroupMenu(NotebookTabGroup *group, QPoint globalPos);
    void tabHighlightChanged(NotebookTab *tab) const;

protected:
    bool getShowTabs() const;
    void setShowTabs(bool value);

    void scaleChangedEvent(float scale_) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *) override;

    DrawnButton *addButton_;

    template <typename T>
    T *addCustomButton(auto &&...args)
    {
        auto *btn = new T(std::forward<decltype(args)>(args)..., this);
        this->customButtons_.push_back(btn);

        return btn;
    }

    struct Item {
        NotebookTab *tab{};
        QWidget *page{};
        QWidget *selectedWidget{};
    };

    const QList<Item> items()
    {
        return this->items_;
    }

    /**
     * @brief Apply the given tab visibility filter
     *
     * An empty function can be provided to denote that no filter will be applied
     *
     * Tabs will be redrawn after this function is called.
     **/
    void setTabVisibilityFilter(TabVisibilityFilter filter);

    /**
     * @brief shouldShowTab has the final say whether a tab should be visible right now.
     **/
    bool shouldShowTab(const NotebookTab *tab) const;

    void performLayout(bool animate = false);

    void sortTabsAlphabetically();

private:
    /// One thing in the tab bar: a tab or a group header/offline button
    struct LayoutUnit {
        NotebookTab *tab{};
        NotebookGroupWidget *groupWidget{};
        /// Extra space before this unit, between groups
        bool gapBefore{};
    };

    struct LayoutContext {
        int left = 0;
        int right = 0;
        int bottom = 0;
        float scale = 0;
        int tabHeight = 0;
        int minimumTabAreaSpace = 0;
        int addButtonWidth = 0;
        int lineThickness = 0;
        int tabSpacer = 0;

        int buttonWidth = 0;
        int buttonHeight = 0;
        int groupSpacing = 0;

        std::span<LayoutUnit> units;
    };

    void performHorizontalLayout(const LayoutContext &ctx, bool animated);
    void performVerticalLayout(const LayoutContext &ctx, bool animated);

    /**
     * @brief Show a popup informing the user of some big tab visibility changes
     **/
    void showTabVisibilityInfoPopup();

    /**
     * @brief Updates the visibility state of all tabs
     **/
    void updateTabVisibility();
    void resizeAddButton();

    bool containsPage(QWidget *page);

    std::vector<LayoutUnit> buildLayoutUnits();
    bool groupAllowsTab(const NotebookTab *tab) const;
    void updateVisibilityFilter();
    /// Keeps the tabs of each group next to each other, at the position of
    /// the group's first tab
    void normalizeGroupOrder();
    void pruneEmptyTabGroups();
    void removeTabGroup(const QString &name);
    void renameTabGroup(const QString &name);
    void moveTabGroup(const QString &name, int direction);
    bool canMoveTabGroup(const QString &name, int direction) const;
    void tabGroupsChanged();
    Item *findItem(QWidget *page);

    static bool containsChild(const QObject *obj, const QObject *child);
    NotebookTab *getTabFromPage(QWidget *page);

    // Returns the number of buttons in `customButtons_` that are visible
    size_t visibleButtonCount() const;

    QList<Item> items_;
    QMenu *menu_ = nullptr;
    QWidget *selectedPage_ = nullptr;

    std::vector<Button *> customButtons_;

    bool allowUserTabManagement_ = false;
    bool showTabs_ = true;
    bool showAddButton_ = false;
    int lineOffset_ = 20;
    bool lockNotebookLayout_ = false;

    bool refreshPaused_ = false;
    bool refreshRequested_ = false;

    NotebookTabLocation tabLocation_ = NotebookTabLocation::Top;

    QAction *lockNotebookLayoutAction_;
    QAction *toggleTopMostAction_;

    // This filter, if set, is used to figure out the visibility of
    // the tabs in this notebook. It combines the user's filter (e.g. "Only
    // show live tabs") with the tab groups.
    TabVisibilityFilter tabVisibilityFilter_;
    TabVisibilityFilter userTabVisibilityFilter_;

    std::vector<std::unique_ptr<NotebookTabGroup>> tabGroups_;
};

class SplitNotebook : public Notebook
{
public:
    SplitNotebook(Window *parent);

    SplitContainer *addPage(bool select = false);
    SplitContainer *getOrAddSelectedPage();
    /// Returns `nullptr` when no page is selected.
    SplitContainer *getSelectedPage();
    void select(QWidget *page, bool focusPage = true) override;
    void themeChangedEvent() override;

    void addNotebookActionsToMenu(QMenu *menu) override;

    void forEachSplit(const std::function<void(Split *)> &cb);

    /**
     * Toggles between the "Show all tabs" and "Hide all tabs" tab visibility states
     */
    void toggleTabVisibility();

    QAction *showAllTabsAction;
    QAction *onlyShowLiveTabsAction;
    QAction *hideAllTabsAction;

protected:
    void showEvent(QShowEvent *event) override;

private:
    QAction *sortTabsAlphabeticallyAction_;

    void addCustomButtons();

    pajlada::Signals::SignalHolder signalHolder_;

    // Main window on Windows has basically a duplicate of this in Window
    PixmapButton *streamerModeIcon_{};
    void updateStreamerModeIcon();

    void setLockNotebookLayout(bool value) override;
};

}  // namespace chatterino
