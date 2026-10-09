// Chatterino9: remembers which moderator timed out or banned whom, so the
// lane chips can still name the moderator after a restart. Twitch's chat
// history (CLEARCHAT) never says who did it, only EventSub does, live.

#pragma once

#include <QDateTime>
#include <QString>

namespace chatterino::modnames {

/// Stores `moderator` for the timeout/ban of `user` at `time`
/// (Misc/moderator-names.tsv, kept for 3 days)
void remember(const QString &user, const QDateTime &time,
              const QString &moderator);

/// The moderator of `user`'s timeout/ban within 15s of `time`, or empty
QString lookup(const QString &user, const QDateTime &time);

}  // namespace chatterino::modnames
