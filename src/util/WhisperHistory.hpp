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

/// Saves a whisper the current account sent to `partner`
void recordOutgoing(const QString &partner, const QString &text);

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

}  // namespace chatterino::whisperhistory
