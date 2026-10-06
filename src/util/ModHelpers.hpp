#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <vector>

/// Small helpers for moderators: repeated message detection and a
/// timeout history that is read from Chatterino's own chat logs.
namespace chatterino::modhelpers {

/// A message counts as repeated when the same user sent the same text
/// (ignoring emotes, mentions, punctuation and case) this many times before
/// within REPEAT_WINDOW_SECONDS.
inline constexpr int REPEAT_PREVIOUS_NEEDED = 2;
inline constexpr qint64 REPEAT_WINDOW_SECONDS = 5 * 60;

/// Builds the comparison key for repeated message detection from the plain
/// text words of a message (emotes already removed).
/// Returns an empty string if the message is too short to compare
/// (fewer than 3 words or fewer than 12 letters/digits).
QString repeatKey(const QStringList &textWords);

/// Short spam ("W", "Ww", "Wwwwww", "1"): only counted when the same user
/// sends it SHORT_SPAM_PREVIOUS_NEEDED more times within
/// SHORT_SPAM_WINDOW_SECONDS and nobody else sent it within
/// SHORT_SPAM_OTHERS_SECONDS (so a whole-chat "W" spam is not flagged).
inline constexpr int SHORT_SPAM_PREVIOUS_NEEDED = 2;
inline constexpr qint64 SHORT_SPAM_WINDOW_SECONDS = 60;
inline constexpr qint64 SHORT_SPAM_OTHERS_SECONDS = 120;

/// Short spam is also counted, even while others spam it too, when the same
/// user sent it HEAVY_SPAM_PREVIOUS_NEEDED more times within
/// HEAVY_SPAM_WINDOW_SECONDS and at least as often as everyone else together
/// (one user sending "67" 15 times while a few others join in).
inline constexpr int HEAVY_SPAM_PREVIOUS_NEEDED = 4;
inline constexpr qint64 HEAVY_SPAM_WINDOW_SECONDS = 10 * 60;

/// Comparison key for short spam: letters/digits only, lowercase, repeated
/// characters collapsed ("Wwwww" -> "w"). Empty if longer than 15 characters.
QString shortSpamKey(const QStringList &textWords);

/// Two repeat keys count as the same message if they differ by at most one
/// typo per 10 characters ("hällst" / "hältst").
bool isSameRepeatKey(const QString &a, const QString &b);

struct LogTimeout {
    QDateTime time;
    /// -1 for a permanent ban
    qint64 seconds = 0;
    QString duration;
    /// Timeout came within 2 seconds of the user's last message
    /// (usually a bot like Fossabot)
    bool automatic = false;
    /// The user asked to be timed out with !v / !vanish
    bool vanish = false;

    bool isBan() const
    {
        return this->seconds < 0;
    }
    /// A timeout given by a human moderator (not a bot, not !vanish, not
    /// one of the short "purge" timeouts) or a ban.
    bool countsAsModAction() const
    {
        return this->isBan() ||
               (!this->automatic && !this->vanish && this->seconds >= 10);
    }
};

/// Parses "10m", "6m 40s", "1d", "14d" ... into seconds. Returns 0 if unknown.
qint64 parseLogDuration(const QString &text);

/// Parses the content of one log file (one day) and appends all timeouts and
/// bans of `login` to `out`.
void parseLogTimeouts(const QString &content, const QDate &date,
                      const QString &login, std::vector<LogTimeout> &out);

/// Reads the log files of `channelName` in `channelLogDirectory` for the last
/// `days` days (including today) and returns the timeouts and bans of `login`
/// that happened after now - days, oldest first.
std::vector<LogTimeout> readLogTimeouts(const QString &channelLogDirectory,
                                        const QString &channelName,
                                        const QString &login,
                                        const QDateTime &now, int days);

/// "31 min ago", "5 h ago", "2 days ago"
QString formatAgo(const QDateTime &from, const QDateTime &now);

}  // namespace chatterino::modhelpers
