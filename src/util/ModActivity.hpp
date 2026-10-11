// Chatterino9: counts what each moderator does in a channel (timeouts, bans,
// unbans, deleted messages, warnings, AutoMod) for the Mod activity window.
// Everything comes from live EventSub moderation events: what the app sees
// while it is open, plus what the mod tracker server counted around the
// clock when Misc/ModActivity/remote.json names one. Kept with no time limit
// in Misc/ModActivity/<channel>/.

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QString>

#include <cstdint>
#include <vector>

namespace chatterino::modactivity {

enum class Kind : std::uint8_t {
    Timeout,
    Ban,
    Unban,
    Untimeout,
    Delete,
    Warn,
    AutomodApprove,
    AutomodDeny,
};

struct Action {
    qint64 msecs = 0;
    Kind kind = Kind::Timeout;
    QString moderator;
    QString target;
    /// Timeout length in seconds, 0 for everything else
    int seconds = 0;
};

struct Stream {
    QString id;
    qint64 start = 0;
    /// Last time the stream was seen live
    qint64 end = 0;
};

/// Saves one moderation action in `channel`
void record(const QString &channel, const QDateTime &time, Kind kind,
            const QString &moderator, const QString &target, int seconds = 0);

/// Called on every stream status refresh while `channel` is live
void streamSeen(const QString &channel, const QString &streamId,
                const QDateTime &startedAt);

/// Called when `channel` goes offline
void streamEnded(const QString &channel);

/// All saved actions of `channel`, oldest first
std::vector<Action> actions(const QString &channel);

/// All streams of `channel` seen by the app, oldest first
std::vector<Stream> streams(const QString &channel);

/// Known chat bots (Fossabot, Nightbot, ...) are listed apart from mods
bool isBot(const QString &login);

/// Fires in the GUI thread after a new action was saved
pajlada::Signals::NoArgSignal &updated();

struct SyncStatus {
    /// Misc/ModActivity/remote.json names a server
    bool configured = false;
    /// The last sync attempt worked
    bool ok = false;
    /// The server has no valid Twitch login and is not counting
    bool needsSignIn = false;
    /// Host name of the server, e.g. mods.acri.fyi
    QString host;
    /// Last successful sync, invalid if there never was one
    QDateTime lastSync;
};

/// Merges what the server counted for `channel` into the local files. Does
/// nothing without remote.json or while a sync is already running. Must be
/// called from the GUI thread.
void syncFromServer(const QString &channel);

SyncStatus syncStatus(const QString &channel);

/// Fires in the GUI thread after each sync attempt
pajlada::Signals::NoArgSignal &syncStatusChanged();

}  // namespace chatterino::modactivity
