#include "util/WhisperHistory.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#include <algorithm>
#include <map>

namespace {

using namespace chatterino;
using namespace chatterino::whisperhistory;

struct Store {
    /// Account whose whispers are loaded, empty if none yet
    QString account;
    QHash<QString, std::vector<Entry>> byPartner;
    /// partner -> display name from their last whisper
    QHash<QString, QString> displayNames;
    /// partner -> msecs of the newest whisper the user has read
    QHash<QString, qint64> lastRead;
};

Store &store()
{
    static Store instance;
    return instance;
}

bool isLogin(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9_]{1,25}$"));
    return re.match(name).hasMatch();
}

/// Login of the signed-in account, empty when anonymous
QString currentAccount()
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        return {};
    }
    return account->getUserName().toLower();
}

/// Empty when the app has no misc directory (tests)
QString accountDir(const QString &account)
{
    const auto &misc = getApp()->getPaths().miscDirectory;
    if (misc.isEmpty() || account.isEmpty())
    {
        return {};
    }
    return QDir(misc).filePath(QStringLiteral("Whispers/") + account);
}

QString escapeField(QString text)
{
    text.replace(u'\\', QStringLiteral("\\\\"));
    text.replace(u'\t', QStringLiteral("\\t"));
    text.replace(u'\n', QStringLiteral("\\n"));
    text.replace(u'\r', QStringLiteral("\\r"));
    return text;
}

QString unescapeField(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); i++)
    {
        auto c = text[i];
        if (c == u'\\' && i + 1 < text.size())
        {
            i++;
            switch (text[i].unicode())
            {
                case 't':
                    out += u'\t';
                    break;
                case 'n':
                    out += u'\n';
                    break;
                case 'r':
                    out += u'\r';
                    break;
                default:
                    out += text[i];
                    break;
            }
            continue;
        }
        out += c;
    }
    return out;
}

/// msecs, i/o, name, color, text
QString toLine(const Entry &entry)
{
    return QString::number(entry.msecs) + u'\t' +
           (entry.outgoing ? u'o' : u'i') + u'\t' + escapeField(entry.name) +
           u'\t' + (entry.color.isValid() ? entry.color.name() : QString()) +
           u'\t' + escapeField(entry.text);
}

bool fromLine(const QString &line, Entry &entry)
{
    const auto parts = line.split(u'\t');
    if (parts.size() != 5)
    {
        return false;
    }
    bool ok = false;
    entry.msecs = parts[0].toLongLong(&ok);
    if (!ok || (parts[1] != u"i" && parts[1] != u"o"))
    {
        return false;
    }
    entry.outgoing = parts[1] == u"o";
    entry.name = unescapeField(parts[2]);
    entry.color = parts[3].isEmpty() ? QColor() : QColor(parts[3]);
    entry.text = unescapeField(parts[4]);
    return true;
}

void writeEntries(const QString &dir, const QString &partner,
                  const std::vector<Entry> &entries, bool append)
{
    if (!QDir().mkpath(dir))
    {
        return;
    }
    QFile file(QDir(dir).filePath(partner + QStringLiteral(".tsv")));
    if (!file.open((append ? QIODevice::Append : QIODevice::WriteOnly) |
                   QIODevice::Text))
    {
        return;
    }
    QTextStream out(&file);
    for (const auto &entry : entries)
    {
        out << toLine(entry) << '\n';
    }
}

void saveReadState(const QString &dir)
{
    if (dir.isEmpty() || !QDir().mkpath(dir))
    {
        return;
    }
    QJsonObject obj;
    for (auto it = store().lastRead.cbegin(); it != store().lastRead.cend();
         ++it)
    {
        obj.insert(it.key(), QString::number(it.value()));
    }
    QSaveFile file(QDir(dir).filePath(QStringLiteral("read.json")));
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

/// Folder the Whispers chat logs are written to
QString whisperLogDir()
{
    QString base = getSettings()->logPath;
    if (base.isEmpty())
    {
        base = getApp()->getPaths().messageLogDirectory;
    }
    if (base.isEmpty())
    {
        return {};
    }
    return QDir(base).filePath(QStringLiteral("Twitch/Whispers"));
}

/// Copies whispers from the Whispers chat logs once per account, so the
/// window starts with what Chatterino logged before it existed. Received
/// whispers are logged as "[time] name: text"; sent ones carry no text in
/// the logs, so only received ones come back.
void importLogsOnce(const QString &account, const QString &dir)
{
    const auto flagPath = QDir(dir).filePath(QStringLiteral("imported-logs"));
    if (dir.isEmpty() || QFile::exists(flagPath) || !QDir().mkpath(dir))
    {
        return;
    }
    // The logs don't say which account got a whisper, so they belong to
    // the first account only. Others would get conversations they never had.
    {
        QDir parent(dir);
        parent.cdUp();
        const auto accounts =
            parent.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto &other : accounts)
        {
            if (QFile::exists(QDir(parent.filePath(other))
                                  .filePath(QStringLiteral("imported-logs"))))
            {
                QFile flag(flagPath);
                if (flag.open(QIODevice::WriteOnly))
                {
                    flag.write("imported by another account");
                }
                return;
            }
        }
    }

    static const QRegularExpression fileRe(
        QStringLiteral("^whispers-(\\d{4}-\\d{2}-\\d{2})\\.log$"));
    // [time] (localized name) login: text   or   [time] from -> to: text
    static const QRegularExpression lineRe(QStringLiteral(
        "^(?:\\[(\\d{1,2}):(\\d{2})(?::(\\d{2}))?\\] )?"
        "(?:([a-z0-9_]{1,25}) -> ([a-z0-9_]{1,25})|(?:\\S+ )?([a-z0-9_]{1,25}))"
        ": (.*)$"));

    std::map<QString, std::vector<Entry>> found;
    const QDir logDir(whisperLogDir());
    const auto files =
        logDir.entryList({QStringLiteral("whispers-*.log")}, QDir::Files);
    for (const auto &fileName : files)
    {
        auto fileMatch = fileRe.match(fileName);
        if (!fileMatch.hasMatch())
        {
            continue;
        }
        const auto date = QDate::fromString(fileMatch.captured(1), Qt::ISODate);
        QFile file(logDir.filePath(fileName));
        if (!date.isValid() ||
            !file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            continue;
        }
        QTextStream in(&file);
        while (!in.atEnd())
        {
            auto m = lineRe.match(in.readLine());
            if (!m.hasMatch() || m.captured(7).trimmed().isEmpty())
            {
                continue;
            }
            Entry entry;
            QString partner;
            if (m.hasCaptured(6))
            {
                partner = m.captured(6);
                entry.name = partner;
            }
            else
            {
                const auto from = m.captured(4);
                const auto to = m.captured(5);
                entry.outgoing = from == account;
                partner = entry.outgoing ? to : from;
                entry.name = from;
            }
            if (partner == account)
            {
                continue;
            }
            const QTime time(m.captured(1).toInt(), m.captured(2).toInt(),
                             m.captured(3).toInt());
            entry.msecs = QDateTime(date, time.isValid() ? time : QTime(0, 0))
                              .toMSecsSinceEpoch();
            entry.text = m.captured(7);
            found[partner].push_back(std::move(entry));
        }
    }

    for (auto &[partner, entries] : found)
    {
        std::ranges::stable_sort(entries, [](const Entry &a, const Entry &b) {
            return a.msecs < b.msecs;
        });
        writeEntries(dir, partner, entries, false);
        // old whispers count as read
        store().lastRead[partner] = entries.back().msecs;
    }
    if (!found.empty())
    {
        saveReadState(dir);
    }

    QFile flag(flagPath);
    if (flag.open(QIODevice::WriteOnly))
    {
        flag.write(QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8());
    }
}

/// Loads the whispers of the signed-in account. Returns its folder, empty
/// when nobody is signed in.
QString ensureLoaded()
{
    const auto account = currentAccount();
    auto &st = store();
    if (account == st.account)
    {
        return accountDir(account);
    }

    st = Store{};
    st.account = account;
    auto dir = accountDir(account);
    if (dir.isEmpty())
    {
        return {};
    }

    QFile readFile(QDir(dir).filePath(QStringLiteral("read.json")));
    if (readFile.open(QIODevice::ReadOnly))
    {
        const auto obj = QJsonDocument::fromJson(readFile.readAll()).object();
        for (auto it = obj.begin(); it != obj.end(); ++it)
        {
            st.lastRead[it.key()] = it.value().toString().toLongLong();
        }
    }

    importLogsOnce(account, dir);

    const QDir qdir(dir);
    const auto fileNames =
        qdir.entryList({QStringLiteral("*.tsv")}, QDir::Files);
    for (const auto &fileName : fileNames)
    {
        const auto partner = fileName.chopped(4);
        if (!isLogin(partner))
        {
            continue;
        }
        QFile file(qdir.filePath(fileName));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            continue;
        }
        std::vector<Entry> list;
        QTextStream in(&file);
        while (!in.atEnd())
        {
            Entry entry;
            if (fromLine(in.readLine(), entry))
            {
                if (!entry.outgoing && !entry.name.isEmpty())
                {
                    st.displayNames[partner] = entry.name;
                }
                list.push_back(std::move(entry));
            }
        }
        if (!list.empty())
        {
            st.byPartner.emplace(partner, std::move(list));
        }
    }
    return dir;
}

/// Returns false if nothing was saved
bool add(const QString &partner, Entry entry)
{
    const auto dir = ensureLoaded();
    if (dir.isEmpty() || !isLogin(partner) || partner == store().account)
    {
        return false;
    }
    auto &list = store().byPartner[partner];
    // the same whisper again (e.g. sent from twitch.tv and passed on twice)
    for (auto it = list.crbegin();
         it != list.crend() && it->msecs >= entry.msecs; ++it)
    {
        if (it->msecs == entry.msecs && it->outgoing == entry.outgoing &&
            it->text == entry.text)
        {
            return false;
        }
    }
    if (!entry.outgoing && !entry.name.isEmpty())
    {
        store().displayNames[partner] = entry.name;
    }
    if (list.empty() || list.back().msecs <= entry.msecs)
    {
        writeEntries(dir, partner, {entry}, true);
        list.push_back(std::move(entry));
    }
    else
    {
        // sent on twitch.tv while the app was closed: keep time order
        auto pos =
            std::ranges::upper_bound(list, entry.msecs, {}, &Entry::msecs);
        list.insert(pos, std::move(entry));
        writeEntries(dir, partner, list, false);
    }
    updated().invoke(partner);
    return true;
}

}  // namespace

namespace chatterino::whisperhistory {

void recordIncoming(const MessagePtr &message)
{
    if (!message)
    {
        return;
    }
    Entry entry;
    entry.msecs = message->serverReceivedTime.isValid()
                      ? message->serverReceivedTime.toMSecsSinceEpoch()
                      : QDateTime::currentMSecsSinceEpoch();
    entry.name = message->displayName.isEmpty() ? message->loginName
                                                : message->displayName;
    entry.color = message->usernameColor;
    entry.text = message->messageText;
    runInGuiThread([partner = message->loginName.toLower(),
                    entry = std::move(entry)]() mutable {
        add(partner, std::move(entry));
    });
}

void recordOutgoing(const QString &partner, const QString &text, qint64 msecs)
{
    runInGuiThread([partner = partner.toLower(), text, msecs] {
        auto account = getApp()->getAccounts()->twitch.getCurrent();
        Entry entry;
        entry.msecs = msecs != 0 ? msecs : QDateTime::currentMSecsSinceEpoch();
        entry.outgoing = true;
        entry.name = account ? account->getUserName() : QString();
        entry.color = account ? account->color() : QColor();
        entry.text = text;
        if (add(partner, std::move(entry)) && msecs != 0)
        {
            copiedIn().invoke(partner);
        }
    });
}

std::vector<Conversation> conversations()
{
    ensureLoaded();
    const auto &st = store();
    std::vector<Conversation> out;
    for (auto it = st.byPartner.cbegin(); it != st.byPartner.cend(); ++it)
    {
        const auto &list = it.value();
        if (list.empty())
        {
            continue;
        }
        Conversation conv;
        conv.partner = it.key();
        conv.displayName = st.displayNames.value(it.key(), it.key());
        conv.lastMsecs = list.back().msecs;
        conv.lastText = list.back().text;
        conv.lastOutgoing = list.back().outgoing;
        const auto readUpTo = st.lastRead.value(it.key(), 0);
        for (auto e = list.crbegin(); e != list.crend() && e->msecs > readUpTo;
             ++e)
        {
            if (!e->outgoing)
            {
                conv.unread++;
            }
        }
        out.push_back(std::move(conv));
    }
    std::ranges::sort(out, [](const Conversation &a, const Conversation &b) {
        return a.lastMsecs > b.lastMsecs;
    });
    return out;
}

std::vector<Entry> entries(const QString &partner)
{
    ensureLoaded();
    return store().byPartner.value(partner.toLower());
}

void markRead(const QString &partner)
{
    const auto dir = ensureLoaded();
    auto &st = store();
    const auto key = partner.toLower();
    auto it = st.byPartner.constFind(key);
    if (it == st.byPartner.cend() || it->empty() ||
        st.lastRead.value(key, 0) >= it->back().msecs)
    {
        return;
    }
    st.lastRead[key] = it->back().msecs;
    saveReadState(dir);
    updated().invoke(key);
}

void markAllRead()
{
    const auto dir = ensureLoaded();
    auto &st = store();
    bool changed = false;
    for (auto it = st.byPartner.cbegin(); it != st.byPartner.cend(); ++it)
    {
        if (!it->empty() && st.lastRead.value(it.key(), 0) < it->back().msecs)
        {
            st.lastRead[it.key()] = it->back().msecs;
            changed = true;
        }
    }
    if (changed)
    {
        saveReadState(dir);
        updated().invoke(QString());
    }
}

QString partnerOf(const Message &message)
{
    if (!message.whisperPartner.isEmpty())
    {
        return message.whisperPartner;
    }
    if (message.flags.has(MessageFlag::Whisper))
    {
        return message.loginName.toLower();
    }
    return {};
}

pajlada::Signals::Signal<QString> &updated()
{
    static pajlada::Signals::Signal<QString> signal;
    return signal;
}

pajlada::Signals::Signal<QString> &copiedIn()
{
    static pajlada::Signals::Signal<QString> signal;
    return signal;
}

}  // namespace chatterino::whisperhistory
