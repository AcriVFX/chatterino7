// Chatterino9: counts how often each emote is used in a channel for the
// Emote stats window.
//
// Every live chat message counts once per emote in it, together with who sent
// it, in one bucket per hour. Three sources fill the buckets and are merged
// when the window reads them:
// - what this app counted while it was open (Misc/EmoteStats/<channel>/local)
// - what the mod tracker server counted around the clock (.../server, synced
//   when remote.json names a server, see ModActivity.hpp)
// - the chat logs from before, read once (.../logs)
// All three count the same messages, so per hour and emote the highest count
// and all chatters seen win. Files are one per UTC day, YYYY-MM-DD.tsv, with
// lines "HH, source, name, messages, chatters" (tab separated, chatters
// comma separated, source "*" with name "*" is every chat message).

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QSet>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace chatterino {

class TwitchChannel;

namespace emotestats {

enum class Source : std::uint8_t {
    Twitch,
    Bttv,
    Ffz,
    SevenTv,
};

/// "twitch", "bttv", "ffz", "7tv" as used in the files
QString sourceKey(Source source);
std::optional<Source> sourceFromKey(QStringView key);
/// "Twitch", "BTTV", "FFZ", "7TV"
QString sourceName(Source source);

struct Use {
    Source source = Source::Twitch;
    QString name;
};

/// Where the third-party emote `name` in `channel` comes from, in the order
/// chat looks emotes up: FFZ, BTTV and 7TV channel emotes, then FFZ, BTTV and
/// 7TV global emotes. 7TV personal emotes are not counted.
std::optional<Source> classify(const TwitchChannel &channel,
                               const QString &name);

/// Whether live chat in `channel` is counted. Only papaplatte is: counting
/// every joined channel filled the disk for channels nobody looks at.
bool countsChannel(const QString &channel);

/// Counts one live chat message of `login` in `channel`. `uses` may repeat an
/// emote, it counts once. History loaded on join must not be passed in; a
/// message id seen before is ignored.
void recordMessage(const QString &channel, const QString &messageId,
                   const QString &login, const QDateTime &time,
                   const std::vector<Use> &uses);

/// Saves the counts not saved yet (they are saved every 30 seconds anyway).
/// The files are written on a background thread; load() waits for them.
void flush();

/// flush() and wait until the files are written. For quitting.
void flushAndWait();

struct Row {
    Source source = Source::Twitch;
    QString name;
    /// Messages with this emote
    qint64 messages = 0;
    /// Different people who used it
    qint64 chatters = 0;
};

struct Stats {
    /// Every emote used in the range, in no particular order
    std::vector<Row> rows;
    /// Every chat message in the range
    qint64 totalMessages = 0;
    /// Start (UTC midnight) of the oldest day with data, 0 if there is none
    qint64 firstMsecs = 0;
};

/// Adds up what was counted for `channel` in the hours that start in
/// [`from`, `to`) (msecs since epoch). Reads files: call flush() first and
/// run this off the GUI thread.
Stats load(const QString &channel, qint64 from, qint64 to);

// ---------------------------------------------------------- log import ---

/// Emote names the log import looks for, taken from the channel's current
/// emote sets (the logs don't say which word was an emote)
struct ImportNames {
    QSet<QString> twitch;
    QSet<QString> ffzChannel;
    QSet<QString> bttvChannel;
    QSet<QString> seventvChannel;
    QSet<QString> ffzGlobal;
    QSet<QString> bttvGlobal;
    QSet<QString> seventvGlobal;
};

/// The chat logs of `channel` were read already
bool logsImported(const QString &channel);

/// Reads the chat logs of `channel` in `logDirectory` (the folder with
/// <channel>-YYYY-MM-DD.log files) once, in a worker thread. Fires updated()
/// when done. Must be called from the GUI thread.
void importLogs(const QString &channel, const QString &logDirectory,
                ImportNames names);

bool importRunning(const QString &channel);

// ---------------------------------------------------------- server sync ---

struct SyncStatus {
    /// Misc/ModActivity/remote.json names a server
    bool configured = false;
    /// The last sync attempt worked
    bool ok = false;
    /// The server runs an older version that doesn't count emotes
    bool notCounting = false;
    /// The server is connected to chat right now
    bool connected = false;
    QString host;
    /// Last successful sync, invalid if there never was one
    QDateTime lastSync;
};

/// Downloads the days the server counted that changed since the last sync.
/// Does nothing without remote.json or while a sync is running. GUI thread.
void syncFromServer(const QString &channel);

SyncStatus syncStatus(const QString &channel);

/// Fires in the GUI thread after a sync brought new numbers, after the log
/// import finished and after each sync attempt
pajlada::Signals::NoArgSignal &updated();

}  // namespace emotestats

}  // namespace chatterino
