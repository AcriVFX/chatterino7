// Chatterino9: keeps every whisper the current account sends or receives, so
// the Whispers window can show whole conversations after a restart. Twitch
// has no API for old whispers, so history starts with what the app saw, plus
// whatever the Whispers chat logs held when this was first used.
// Kept per account in Misc/Whispers/<account>/<partner>.tsv.

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QColor>
#include <QString>

#include <memory>
#include <vector>

namespace chatterino {

struct Message;
using MessagePtr = std::shared_ptr<const Message>;

}  // namespace chatterino

namespace chatterino::whisperhistory {

struct Entry {
    qint64 msecs = 0;
    /// Sent by the current account
    bool outgoing = false;
    /// Display name of whoever sent it
    QString name;
    /// Name color of whoever sent it, invalid if unknown
    QColor color;
    QString text;
};

struct Conversation {
    /// Login of the other person
    QString partner;
    /// Their display name as last seen
    QString displayName;
    qint64 lastMsecs = 0;
    QString lastText;
    bool lastOutgoing = false;
    /// Received whispers newer than the last time this was read
    int unread = 0;
};

/// Saves a whisper the current account received
void recordIncoming(const MessagePtr &message);

/// Saves a whisper the current account sent to `partner`. `msecs` is when it
/// was sent, 0 for now. Whispers sent outside the app (twitch.tv) pass their
/// time; the same whisper saved twice with the same time is kept once.
void recordOutgoing(const QString &partner, const QString &text,
                    qint64 msecs = 0);

/// Conversations of the current account, newest first
std::vector<Conversation> conversations();

/// All whispers with `partner`, oldest first
std::vector<Entry> entries(const QString &partner);

void markRead(const QString &partner);
void markAllRead();

/// Login of the person a whisper message is with, empty for other messages
QString partnerOf(const Message &message);

/// Fires in the GUI thread after a whisper with the given partner was saved
/// or read state changed (empty partner: several changed)
pajlada::Signals::Signal<QString> &updated();

/// Fires in the GUI thread after a whisper sent outside the app was saved
/// with the given partner. It never passed through the whispers channel, so
/// open views of that conversation have to reload it.
pajlada::Signals::Signal<QString> &copiedIn();

}  // namespace chatterino::whisperhistory
