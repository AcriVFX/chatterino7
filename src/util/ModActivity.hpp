// Chatterino9: counts what each moderator does in a channel (timeouts, bans,
// unbans, deleted messages, warnings, AutoMod) for the Mod activity window.
// Everything comes from live EventSub moderation events, so only what the
// app sees while it is open gets counted. Kept with no time limit in
// Misc/ModActivity/<channel>/.

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

}  // namespace chatterino::modactivity
