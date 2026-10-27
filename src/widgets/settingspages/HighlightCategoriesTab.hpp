// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/highlights/HighlightCategories.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QWidget>

#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QVBoxLayout;

namespace chatterino {

/// "Categories" tab of the highlights settings: edit the word lists, color,
/// lane and order of labeled message highlight rules
/// (see controllers/highlights/HighlightCategories.hpp)
class HighlightCategoriesTab : public QWidget
{
public:
    explicit HighlightCategoriesTab(std::function<void()> showMessagesTab);

private:
    using Rule = highlightcategories::Rule;
    using Category = highlightcategories::Category;

    struct Lane {
        QString name;
        QStringList tags;
    };

    static std::vector<Rule> rules();
    static std::vector<Lane> lanes();
    static QString laneOf(const QString &tag);
    /// Moves tag into the lane filter named lane (empty = no lane)
    static void setLane(const QString &tag, const QString &lane);
    /// Adds newTag next to oldTag in every lane, dropping oldTag unless kept
    static void retag(const QString &oldTag, const QString &newTag,
                      bool keepOld);

    void apply(const std::vector<Rule> &next);
    void refresh();
    void refreshDetail();
    void refreshUnsorted();
    std::optional<Category> current() const;
    void showStatus(const QString &text);

    void renameTo(const QString &newName);
    void addWord();
    void removeSelectedWords();
    void moveSelected(int direction);
    void openNewCategoryDialog();
    void openColorDialog();

    std::function<void()> showMessagesTab_;

    QTreeWidget *list_{};
    QPushButton *upButton_{};
    QPushButton *downButton_{};
    QStackedWidget *detail_{};

    QLineEdit *name_{};
    QLineEdit *tag_{};
    QPushButton *color_{};
    QComboBox *lane_{};
    QCheckBox *mentions_{};
    QCheckBox *alert_{};
    QCheckBox *sound_{};
    QLabel *wordsTitle_{};
    QListWidget *words_{};
    QLineEdit *wordInput_{};
    QPushButton *removeButton_{};
    QLabel *status_{};
    QLabel *advanced_{};

    QVBoxLayout *unsortedRows_{};
    QLabel *unsortedTitle_{};
    QLabel *unsortedStatus_{};

    /// Selected category, empty with unsortedSelected_ for the Unsorted row
    QString selected_;
    bool unsortedSelected_ = false;
    bool updating_ = false;
    QString lastAdded_;

    pajlada::Signals::SignalHolder connections_;
};

}  // namespace chatterino
