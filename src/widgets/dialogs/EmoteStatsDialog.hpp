// Chatterino9: Emote stats window. Ranks the emotes of a channel by how many
// chat messages used them in this stream, the last 7 or 30 days or all time,
// and lists the channel's emotes nobody used.

#pragma once

#include "util/EmoteStats.hpp"
#include "widgets/BaseWindow.hpp"

#include <QHash>
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
    EmoteStatsDialog(const std::shared_ptr<Channel> &channel,
                     QWidget *parent = nullptr);

    // NOLINTNEXTLINE(cppcoreguidelines-use-enum-class): used as column index
    enum Column : std::uint8_t {
        ColRank,
        ColEmote,
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

    Range currentRange() const;
    std::shared_ptr<TwitchChannel> twitchChannel() const;

    QString channel_;
    std::weak_ptr<Channel> weakChannel_;

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
    bool twitchEmotesRequested_ = false;

    struct {
        QButtonGroup *ranges = nullptr;
        QButtonGroup *sources = nullptr;
        QLineEdit *search = nullptr;
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
    /// Lets the worker thread know the window is still open
    std::shared_ptr<bool> lifetime_ = std::make_shared<bool>(true);
};

}  // namespace chatterino
