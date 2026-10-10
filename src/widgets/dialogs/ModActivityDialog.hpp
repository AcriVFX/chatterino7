// Chatterino9: Mod activity window. Shows what each moderator (and, apart,
// each bot) did in a channel: timeouts, bans, unbans, deleted messages,
// warnings and AutoMod decisions, per stream, day, week or longer.

#pragma once

#include "util/ModActivity.hpp"
#include "widgets/BaseWindow.hpp"

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>

#include <array>
#include <memory>
#include <vector>

class QCheckBox;
class QDateEdit;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTabBar;
class QTableWidget;
class QTimer;
class QToolButton;
class QTreeWidget;

namespace chatterino {

namespace modactivity_ui {
class BarChart;
}  // namespace modactivity_ui

class ModActivityDialog : public BaseWindow
{
public:
    ModActivityDialog(QString channel, QWidget *parent = nullptr);

    /// Columns of both tables
    enum Column {
        ColName,
        ColTimeouts,
        ColBans,
        ColUnbans,
        ColDeleted,
        ColWarnings,
        ColAutomod,
        ColTotal,
        ColChange,
        ColStreams,
        ColPerStream,
        ColLastActive,
        ColCount,
    };

    /// Action types the user can show or hide (Unbans include removed
    /// timeouts, AutoMod includes approvals and denials)
    enum Group {
        GroupTimeouts,
        GroupBans,
        GroupUnbans,
        GroupDeleted,
        GroupWarnings,
        GroupAutomod,
        GroupCount,
    };

    enum class Bucket { Days, Weeks, Months };

protected:
    void themeChangedEvent() override;

private:
    struct Range {
        qint64 from = 0;
        qint64 to = 0;
        QString label;
        bool isStream = false;
    };

    struct Row {
        QString name;
        std::array<int, GroupCount> counts{};
        int total = 0;
        int prevTotal = 0;
        int streamsActive = 0;
        qint64 lastActive = 0;
    };

    void initUi();
    QWidget *createFilterPopup();
    void loadView();
    void saveView() const;

    void reload();
    void refresh();
    void rebuildModList();
    void updateModsButton();
    void fillTable(QTableWidget *table, const std::vector<Row> &rows,
                   int streamCount);
    void updateDetail();
    void exportCsv();

    Range currentRange() const;
    bool isShown(const QString &name) const;
    bool groupEnabled(int group) const;
    QString selectedName() const;

    QString channel_;

    // all saved data of the channel, refreshed on every change
    std::vector<modactivity::Action> actions_;
    std::vector<modactivity::Stream> streams_;
    /// every moderator and bot seen, busiest first
    QStringList allNames_;
    QHash<QString, int> allTimeCount_;
    QHash<QString, qint64> lastActive_;

    // view state (saved to Misc/ModActivity/view.json)
    int rangeIndex_ = 3;
    bool excludeMode_ = false;
    QSet<QString> marked_;
    QSet<QString> knownNames_;
    std::array<bool, GroupCount> groups_{true, true, true, true, true, false};
    bool showBots_ = true;
    bool compare_ = true;
    Bucket bucket_ = Bucket::Weeks;
    QString selected_;

    // last computed rows
    std::vector<Row> modRows_;
    std::vector<Row> botRows_;
    Range range_;
    int streamCount_ = 0;
    std::vector<modactivity::Stream> rangeStreams_;

    struct {
        QTabBar *tabs = nullptr;
        QDateEdit *fromDate = nullptr;
        QDateEdit *toDate = nullptr;
        QWidget *customRange = nullptr;
        QToolButton *modsButton = nullptr;
        QLineEdit *modSearch = nullptr;
        QRadioButton *includeMode = nullptr;
        QRadioButton *excludeMode = nullptr;
        QTreeWidget *modList = nullptr;
        std::array<QPushButton *, GroupCount> chips{};
        QCheckBox *showBots = nullptr;
        QCheckBox *compare = nullptr;
        QLabel *summary = nullptr;
        QTableWidget *mods = nullptr;
        QLabel *botsHeader = nullptr;
        QTableWidget *bots = nullptr;
        QLabel *detailTitle = nullptr;
        std::array<QPushButton *, 3> bucketButtons{};
        modactivity_ui::BarChart *chart = nullptr;
        QLabel *legend = nullptr;
        QGridLayout *facts = nullptr;
        QWidget *detail = nullptr;
    } ui_;

    QTimer *refreshTimer_ = nullptr;
    bool updatingUi_ = false;
};

}  // namespace chatterino
