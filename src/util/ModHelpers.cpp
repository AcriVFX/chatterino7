// Chatterino9: helpers for the usercard (account age, timeout history) and
// the repeat/spam highlight (see MessageBuilder.cpp).

#include "util/ModHelpers.hpp"

#include "common/Literals.hpp"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>

namespace chatterino::modhelpers {

using namespace literals;

QString repeatKey(const QStringList &textWords)
{
    QString key;
    int words = 0;
    for (const auto &word : textWords)
    {
        if (word.startsWith('@'))
        {
            continue;
        }
        QString cleaned;
        for (QChar c : word)
        {
            if (c.isLetterOrNumber())
            {
                cleaned.append(c.toLower());
            }
        }
        if (cleaned.isEmpty())
        {
            continue;
        }
        words++;
        key.append(cleaned);
    }

    if (words < 3 || key.size() < 12)
    {
        return {};
    }
    return key;
}

QString shortSpamKey(const QStringList &textWords)
{
    QString key;
    for (const auto &word : textWords)
    {
        if (word.startsWith('@'))
        {
            continue;
        }
        for (QChar c : word)
        {
            if (!c.isLetterOrNumber())
            {
                continue;
            }
            c = c.toLower();
            if (key.isEmpty() || key.back() != c)
            {
                key.append(c);
            }
        }
    }
    if (key.size() > 15)
    {
        return {};
    }
    return key;
}

bool isSameRepeatKey(const QString &a, const QString &b)
{
    if (a == b)
    {
        return true;
    }
    if (a.isEmpty() || b.isEmpty() || a.size() > 300 || b.size() > 300)
    {
        return false;
    }
    const auto allowed =
        std::max<qsizetype>(1, std::min(a.size(), b.size()) / 10);
    if (std::abs(a.size() - b.size()) > allowed)
    {
        return false;
    }

    // Levenshtein distance, one row at a time
    std::vector<qsizetype> row(b.size() + 1);
    for (qsizetype j = 0; j <= b.size(); j++)
    {
        row[j] = j;
    }
    for (qsizetype i = 1; i <= a.size(); i++)
    {
        qsizetype diagonal = row[0];
        row[0] = i;
        qsizetype best = row[0];
        for (qsizetype j = 1; j <= b.size(); j++)
        {
            qsizetype up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1,
                               diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = up;
            best = std::min(best, row[j]);
        }
        if (best > allowed)
        {
            return false;
        }
    }
    return row[b.size()] <= allowed;
}

QStringList variantTokens(const QStringList &textWords)
{
    QStringList tokens;
    for (const auto &word : textWords)
    {
        if (word.startsWith('@'))
        {
            continue;
        }
        QString token;
        for (QChar c : word)
        {
            if (!c.isLetterOrNumber())
            {
                continue;
            }
            c = c.toLower();
            if (token.isEmpty() || token.back() != c)
            {
                token.append(c);
            }
        }
        if (token.size() >= 3 && !tokens.contains(token))
        {
            tokens.append(token);
        }
    }
    return tokens;
}

bool isVariantOf(const QStringList &a, const QStringList &b)
{
    if (a.isEmpty() || b.isEmpty())
    {
        return false;
    }
    const auto &shorter = a.size() <= b.size() ? a : b;
    const auto &longer = a.size() <= b.size() ? b : a;

    qsizetype matched = 0;
    bool strong = false;
    for (const auto &token : shorter)
    {
        bool found = std::ranges::any_of(longer, [&](const QString &other) {
            return token == other || (token.size() >= 6 && other.size() >= 6 &&
                                      isSameRepeatKey(token, other));
        });
        if (found)
        {
            matched++;
            strong = strong || token.size() >= 4;
        }
    }
    return strong && matched * 2 >= shorter.size();
}

qint64 parseLogDuration(const QString &text)
{
    static const QRegularExpression part(R"((\d+)\s*([smhdw]))");

    qint64 total = 0;
    auto it = part.globalMatch(text);
    while (it.hasNext())
    {
        auto m = it.next();
        qint64 n = m.captured(1).toLongLong();
        switch (m.captured(2).at(0).toLatin1())
        {
            case 's':
                total += n;
                break;
            case 'm':
                total += n * 60;
                break;
            case 'h':
                total += n * 3600;
                break;
            case 'd':
                total += n * 86400;
                break;
            case 'w':
                total += n * 7 * 86400;
                break;
            default:
                break;
        }
    }
    return total;
}

void parseLogTimeouts(const QString &content, const QDate &date,
                      const QString &login, std::vector<LogTimeout> &out)
{
    static const QRegularExpression lineRe(
        R"(^\[(\d\d):(\d\d):(\d\d)\] (.*)$)");
    static const QRegularExpression vanishRe(
        R"(^!(?:v|vanish)\b)", QRegularExpression::CaseInsensitiveOption);

    const QString escaped = QRegularExpression::escape(login);
    const QRegularExpression timeoutRe(
        u"^"_s + escaped + uR"( has been timed out for (.+?)\.\s*$)"_s,
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression banRe(
        u"^"_s + escaped + uR"( has been permanently banned)"_s,
        QRegularExpression::CaseInsensitiveOption);
    // "login: text" or "Localized login: text"
    // "[time] mod: mod unbanned login."
    const QRegularExpression unbanRe(
        uR"(^\S+: \S+ unbanned )"_s + escaped + uR"(\.)"_s,
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression messageRe(
        uR"(^(?:\S+ )?)"_s + escaped + uR"(: (.*)$)"_s,
        QRegularExpression::CaseInsensitiveOption);

    QDateTime lastMessageTime;
    QString lastMessage;

    for (const auto &rawLine : content.split('\n'))
    {
        QString line = rawLine;
        if (line.endsWith('\r'))
        {
            line.chop(1);
        }
        auto m = lineRe.match(line);
        if (!m.hasMatch())
        {
            continue;
        }
        QDateTime time(date, QTime(m.captured(1).toInt(), m.captured(2).toInt(),
                                   m.captured(3).toInt()));
        QString rest = m.captured(4);

        if (unbanRe.match(rest).hasMatch())
        {
            if (!out.empty() && out.back().isBan() &&
                out.back().time.date() == date)
            {
                out.pop_back();
            }
            continue;
        }

        if (auto mm = messageRe.match(rest); mm.hasMatch())
        {
            lastMessageTime = time;
            lastMessage = mm.captured(1);
            continue;
        }

        LogTimeout timeout;
        if (auto mm = timeoutRe.match(rest); mm.hasMatch())
        {
            timeout.duration = mm.captured(1);
            timeout.seconds = parseLogDuration(timeout.duration);
        }
        else if (banRe.match(rest).hasMatch())
        {
            timeout.duration = u"ban"_s;
            timeout.seconds = -1;
        }
        else
        {
            continue;
        }

        timeout.time = time;
        // the same timeout shown twice (e.g. by two clients) without a message in between
        if (!lastMessageTime.isValid() && !out.empty() &&
            out.back().seconds == timeout.seconds &&
            out.back().time.secsTo(time) <= 5)
        {
            continue;
        }
        if (lastMessageTime.isValid())
        {
            auto since = lastMessageTime.secsTo(time);
            timeout.automatic = since >= 0 && since <= 2;
            timeout.vanish = since >= 0 && since <= 90 &&
                             vanishRe.match(lastMessage).hasMatch();
        }
        // bans are always shown, even if a bot gave them
        if (timeout.isBan())
        {
            timeout.automatic = false;
            timeout.vanish = false;
        }
        out.push_back(timeout);
        lastMessageTime = {};
        lastMessage.clear();
    }
}

std::vector<LogTimeout> readLogTimeouts(const QString &channelLogDirectory,
                                        const QString &channelName,
                                        const QString &login,
                                        const QDateTime &now, int days)
{
    std::vector<LogTimeout> out;
    if (login.isEmpty())
    {
        return out;
    }

    QDir dir(channelLogDirectory);
    for (int i = days; i >= 0; i--)
    {
        QDate date = now.date().addDays(-i);
        QFile file(dir.filePath(channelName + u'-' +
                                date.toString(u"yyyy-MM-dd"_s) + u".log"_s));
        if (!file.open(QIODevice::ReadOnly))
        {
            continue;
        }
        parseLogTimeouts(QString::fromUtf8(file.readAll()), date, login, out);
    }

    const auto cutoff = now.addDays(-days);
    std::erase_if(out, [&](const LogTimeout &t) {
        return t.time < cutoff || t.time > now.addSecs(60);
    });
    return out;
}

QString formatAgo(const QDateTime &from, const QDateTime &now)
{
    auto secs = std::max<qint64>(0, from.secsTo(now));
    if (secs < 60)
    {
        return u"just now"_s;
    }
    if (secs < 3600)
    {
        return QString::number(secs / 60) + u" min ago"_s;
    }
    if (secs < 86400)
    {
        return QString::number(secs / 3600) + u" h ago"_s;
    }
    auto d = secs / 86400;
    return QString::number(d) + (d == 1 ? u" day ago"_s : u" days ago"_s);
}

bool isTextArt(const QString &text)
{
    qsizetype art = 0;
    qsizetype visible = 0;
    for (const QChar c : text)
    {
        const auto u = c.unicode();
        if (c.isSpace() || u == 0x2800)
        {
            continue;
        }
        visible++;
        if ((u > 0x2800 && u <= 0x28FF) || (u >= 0x2500 && u <= 0x259F))
        {
            art++;
        }
    }
    return art >= 40 && art * 2 >= visible;
}

}  // namespace chatterino::modhelpers
