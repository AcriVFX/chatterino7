// Chatterino9: Emote stats window. Ranks the emotes of a channel by how many
// chat messages used them in this stream, the last 7 or 30 days or all time,
// and lists the channel's emotes nobody used. Opened from a usercard it shows
// only that chatter's messages.

#pragma once

#include "util/EmoteStats.hpp"
#include "widgets/BaseWindow.hpp"

#include <QHash>
#include <QSet>
#include <QString>

#include <cstdint>
#include <memory>

class QButtonGroup;
class QFrame;
class QLabel;
class QLineEdit;
class QTableWidget;
class QTimer;

namespace chatterino {

class Channel;
class TwitchChannel;

namespace emotestats_ui {
class EmoteDelegate;
}  // namespace emotestats_ui

class EmoteStatsDialog : public BaseWindow
{
public:
    /// `user`: only this chatter's messages (empty: everyone's)
    EmoteStatsDialog(const std::shared_ptr<Channel> &channel,
                     QWidget *parent = nullptr, const QString &user = {});

    // NOLINTNEXTLINE(cppcoreguidelines-use-enum-class): used as column index
    enum Column : std::uint8_t {
        ColRank,
        ColEmote,
        ColAddedBy,
        ColMessages,
        ColChatters,
        ColShare,
        ColCount,
    };

protected:
    void themeChangedEvent() override;

private:
    struct Range {
        qint64 from = 0;
        qint64 to = 0;
        /// "Oct 6 to Oct 13", "Stream started 18:02", ...
        QString label;
        /// Title of the box with the emotes nobody used
        QString unusedTitle;
    };

    void initUi();
    void loadView();
    void saveView() const;

    /// Reads the numbers again (in a worker thread) and refills the table
    void reload();
    /// Refills the table from the numbers already read
    void fillTable();
    void applySearch() const;
    void updateFooter();
    /// Reads the chat logs once, the first time the window opens
    void startLogImport();
    /// Asks Twitch which emotes belong to the channel (its sub, bits and
    /// follower emotes), once per window
    void loadTwitchChannelEmotes();
    /// Asks 7TV who added each emote of the channel's set, now and then
    void loadSeventvAdders();
    /// Asks 7TV for the names of the 7TV users `ids`
    void loadSeventvUserNames(const QStringList &ids);
    /// Shows only `login`'s messages, or everyone's when it is empty
    void setUser(const QString &login);

    Range currentRange() const;
    std::shared_ptr<TwitchChannel> twitchChannel() const;

    QString channel_;
    std::weak_ptr<Channel> weakChannel_;
    /// Only this chatter's messages, empty for everyone's
    QString user_;

    // view state (saved to Misc/EmoteStats/view.json)
    int rangeIndex_ = 1;
    int sourceIndex_ = 0;

    emotestats::Stats stats_;
    Range range_;
    bool loaded_ = false;
    bool loading_ = false;
    bool reloadAgain_ = false;
    QString topEmote_;
    /// The channel's own Twitch emotes, name to id
    QHash<QString, QString> twitchChannelEmotes_;
    /// twitchChannelEmotes_ came from Helix at least once; until then (or
    /// when logged out) Twitch rows are not filtered
    bool twitchEmotesLoaded_ = false;
    bool twitchEmotesLoading_ = false;
    qint64 twitchEmotesRequestedAt_ = 0;
    /// 7TV emote name to the 7TV user who added it ("" when 7TV doesn't
    /// know), kept for emotes removed since (Misc/EmoteStats/<ch>/
    /// 7tv-added-by.json)
    QHash<QString, QString> addedBy_;
    /// 7TV user id to name, "" for deleted accounts
    /// (Misc/EmoteStats/7tv-users.json)
    QHash<QString, QString> seventvUsers_;
    QSet<QString> seventvUsersRequested_;
    bool addersLoading_ = false;
    qint64 addersRequestedAt_ = 0;

    struct {
        QButtonGroup *ranges = nullptr;
        QButtonGroup *sources = nullptr;
        QLineEdit *search = nullptr;
        QFrame *userBar = nullptr;
        QLabel *userLabel = nullptr;
        QLabel *summary = nullptr;
        QTableWidget *table = nullptr;
        emotestats_ui::EmoteDelegate *emoteDelegate = nullptr;
        QFrame *unused = nullptr;
        QLabel *unusedTitle = nullptr;
        QLabel *unusedNames = nullptr;
        QFrame *removedNote = nullptr;
        QLabel *syncDot = nullptr;
        QLabel *footNote = nullptr;
    } ui_;

    QTimer *refreshTimer_ = nullptr;
    QTimer *iconTimer_ = nullptr;
    /// Saves and shows the 7TV names that arrived
    QTimer *namesTimer_ = nullptr;
    /// Lets the worker thread know the window is still open
    std::shared_ptr<bool> lifetime_ = std::make_shared<bool>(true);
    /// Keeps the sums of past days between reloads
    std::shared_ptr<emotestats::LoadCache> loadCache_ =
        emotestats::makeLoadCache();
};

}  // namespace chatterino
