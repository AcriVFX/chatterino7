// Chatterino9: counts what each moderator does in a channel (timeouts, bans,
// unbans, deleted messages, warnings, AutoMod) for the Mod activity window.
// Everything comes from live EventSub moderation events: what the app sees
// while it is open, plus what the mod tracker server counted around the
// clock when Misc/ModActivity/remote.json names one. Kept with no time limit
// in Misc/ModActivity/<channel>/. Counting can be paused, stopped and
// started again, and the counts deleted, from the Mod activity window.

#pragma once

#include "common/network/NetworkRequest.hpp"

#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QString>
#include <QUrlQuery>

#include <cstdint>
#include <functional>
#include <optional>
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

/// A GET request to the mod tracker server named in remote.json, carrying
/// its keys, or nothing without remote.json. Used by the Emote stats sync.
std::optional<NetworkRequest> remoteRequest(const QString &path,
                                            const QUrlQuery &query = {});

/// Host name of the mod tracker server, empty without remote.json
QString remoteHost();

enum class Counting : std::uint8_t { On, Paused, Stopped };

struct Tracking {
    Counting counting = Counting::On;
    /// Paused: when counting starts again by itself
    QDateTime until;
    /// When the pause or stop began
    QDateTime since;
    /// The server has not taken this state yet. It is sent again on every
    /// sync until it has.
    bool serverPending = false;
    /// The server answered that it can't do this yet (needs setup.ps1
    /// update)
    bool serverOutdated = false;
};

/// Whether mod actions in `channel` are counted right now, on this PC and on
/// the mod tracker server. A pause that ran out reads as On.
Tracking tracking(const QString &channel);

/// Pauses counting in `channel` until `until`, stops it or starts it again:
/// on this PC right away and on the server named in remote.json as soon as it
/// can be reached. Fires syncStatusChanged(). Must be called from the GUI
/// thread.
void setCounting(const QString &channel, Counting counting,
                 const QDateTime &until = {});

/// Deletes every counted mod action and stream of `channel`: on the server
/// first when remote.json names one, then on this PC. `done` gets an empty
/// text when it worked, or what went wrong, and then nothing was deleted.
/// Must be called from the GUI thread.
void deleteAll(const QString &channel,
               std::function<void(const QString &error)> done);

}  // namespace chatterino::modactivity
