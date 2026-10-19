// Chatterino9: keeps /mentions across restarts and shows the current account's
// answers in it. Mentions are saved as the IRC lines they came from, so they
// come back like the history Chatterino loads when joining a channel. Your
// answers (an @ or a reply to someone in /mentions) show as one small,
// indented line. Also tracks what you have read: the "New" line and the
// unread count on the /mentions tab.
// Kept per account in Misc/Mentions/<account>/<channel>.txt.

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QByteArray>

#include <memory>
#include <vector>

namespace chatterino {

struct Message;
using MessagePtr = std::shared_ptr<const Message>;
class ChannelView;
class TwitchChannel;

}  // namespace chatterino

namespace chatterino::mentionhistory {

/// Called for every chat message that was built. Saves it when it goes to
/// /mentions (`isMention`), and for `live` messages of the current account
/// adds them to /mentions when they answer someone there. Any thread.
void onChatMessage(const MessagePtr &message, const QByteArray &rawIrc,
                   bool isMention, bool live);

/// After `channel` loaded its recent messages: brings back the saved
/// mentions and answers of that channel once per session, and adds your
/// answers found in `messages`. GUI thread.
void restoreChannel(TwitchChannel &channel,
                    const std::vector<MessagePtr> &messages);

/// Mentions you have not seen yet
int unreadCount();

void markAllRead();

/// Time (msecs) of the newest mention that was read when /mentions was last
/// opened. The "New" line goes under it.
qint64 newLineMark();

/// Lets the /mentions view count as read while it is on screen. Repeated
/// calls are fine. GUI thread.
void watchView(ChannelView *view);

/// Scrolls the chat the message came from to it. Returns false if no open
/// split has it.
bool jumpTo(const MessagePtr &message);

/// Fires in the GUI thread when the unread count or the "New" line changed
pajlada::Signals::NoArgSignal &changed();

}  // namespace chatterino::mentionhistory
