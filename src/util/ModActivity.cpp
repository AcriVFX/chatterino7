// Chatterino9: see ModActivity.hpp

#include "util/ModActivity.hpp"

#include "Application.hpp"
#include "singletons/Paths.hpp"
#include "util/PostToThread.hpp"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QStringView>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace chatterino::modactivity {

namespace {

/// A live stream gets its "last seen" time written at most this often
constexpr qint64 STREAM_WRITE_MSECS = 5LL * 60 * 1000;

constexpr std::array<std::pair<Kind, QStringView>, 8> KIND_KEYS{{
    {Kind::Timeout, u"timeout"},
    {Kind::Ban, u"ban"},
    {Kind::Unban, u"unban"},
    {Kind::Untimeout, u"untimeout"},
    {Kind::Delete, u"delete"},
    {Kind::Warn, u"warn"},
    {Kind::AutomodApprove, u"automod_approve"},
    {Kind::AutomodDeny, u"automod_deny"},
}};

QString kindKey(Kind kind)
{
    for (const auto &[k, key] : KIND_KEYS)
    {
        if (k == kind)
        {
            return key.toString();
        }
    }
    return {};
}

bool kindFromKey(const QString &key, Kind &out)
{
    for (const auto &[k, name] : KIND_KEYS)
    {
        if (key == name)
        {
            out = k;
            return true;
        }
    }
    return false;
}

struct LiveStream {
    QString id;
    qint64 start = 0;
    qint64 lastWritten = 0;
};

struct ChannelData {
    bool loaded = false;
    std::vector<Action> actions;
    std::vector<Stream> streams;
};

struct Store {
    std::mutex mutex;
    std::unordered_map<QString, ChannelData> channels;
    std::unordered_map<QString, LiveStream> live;
    /// Shares one copy of each name between all loaded actions
    QSet<QString> names;
};

Store &store()
{
    static Store instance;
    return instance;
}

QString intern(Store &st, const QString &name)
{
    auto it = st.names.constFind(name);
    if (it != st.names.constEnd())
    {
        return *it;
    }
    st.names.insert(name);
    return name;
}

/// Empty when the app has no misc directory (tests)
QString channelDir(const QString &channel)
{
    const auto &misc = getApp()->getPaths().miscDirectory;
    if (misc.isEmpty())
    {
        return {};
    }
    return QDir(misc).filePath(QStringLiteral("ModActivity/") +
                               channel.toLower());
}

bool ensureDir(const QString &dir)
{
    return !dir.isEmpty() && QDir().mkpath(dir);
}

void appendLine(const QString &path, const QString &line)
{
    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text))
    {
        QTextStream out(&file);
        out << line << '\n';
    }
}

void loadStreams(ChannelData &data, const QString &dir)
{
    QFile file(QDir(dir).filePath(QStringLiteral("streams.tsv")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return;
    }
    // id -> index in data.streams; a later row for the same id wins
    QHash<QString, size_t> byId;
    QTextStream in(&file);
    while (!in.atEnd())
    {
        const auto parts = in.readLine().split(u'\t');
        bool okStart = false;
        bool okEnd = false;
        const auto start = parts.value(1).toLongLong(&okStart);
        const auto end = parts.value(2).toLongLong(&okEnd);
        if (parts.size() != 3 || parts[0].isEmpty() || !okStart || !okEnd)
        {
            continue;
        }
        auto it = byId.constFind(parts[0]);
        if (it == byId.constEnd())
        {
            byId.insert(parts[0], data.streams.size());
            data.streams.push_back({parts[0], start, std::max(start, end)});
        }
        else
        {
            auto &s = data.streams[*it];
            s.end = std::max(s.end, end);
        }
    }
    std::ranges::sort(data.streams, {}, &Stream::start);
}

void loadLocked(Store &st, const QString &channel, ChannelData &data)
{
    if (data.loaded)
    {
        return;
    }
    data.loaded = true;

    const auto dir = channelDir(channel);
    if (dir.isEmpty())
    {
        return;
    }
    const auto files = QDir(dir).entryList({QStringLiteral("actions-*.tsv")},
                                           QDir::Files, QDir::Name);
    for (const auto &name : files)
    {
        QFile file(QDir(dir).filePath(name));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            continue;
        }
        QTextStream in(&file);
        while (!in.atEnd())
        {
            const auto parts = in.readLine().split(u'\t');
            if (parts.size() != 5)
            {
                continue;
            }
            bool okTime = false;
            bool okSeconds = false;
            Action action;
            action.msecs = parts[0].toLongLong(&okTime);
            action.seconds = parts[4].toInt(&okSeconds);
            if (!okTime || !okSeconds || parts[2].isEmpty() ||
                !kindFromKey(parts[1], action.kind))
            {
                continue;
            }
            action.moderator = intern(st, parts[2]);
            action.target = intern(st, parts[3]);
            data.actions.push_back(std::move(action));
        }
    }
    std::ranges::stable_sort(data.actions, {}, &Action::msecs);

    loadStreams(data, dir);
}

/// Writes the current live stream's row and updates loaded data
void writeStreamLocked(Store &st, const QString &channel,
                       const LiveStream &live, qint64 now)
{
    const auto dir = channelDir(channel);
    if (!ensureDir(dir))
    {
        return;
    }
    appendLine(QDir(dir).filePath(QStringLiteral("streams.tsv")),
               live.id + '\t' + QString::number(live.start) + '\t' +
                   QString::number(now));

    auto it = st.channels.find(channel);
    if (it == st.channels.end() || !it->second.loaded)
    {
        return;
    }
    auto &streams = it->second.streams;
    auto s = std::ranges::find(streams, live.id, &Stream::id);
    if (s == streams.end())
    {
        streams.push_back({live.id, live.start, now});
        std::ranges::sort(streams, {}, &Stream::start);
    }
    else
    {
        s->end = std::max(s->end, now);
    }
}

}  // namespace

void record(const QString &channel, const QDateTime &time, Kind kind,
            const QString &moderator, const QString &target, int seconds)
{
    if (channel.isEmpty() || moderator.isEmpty() || !time.isValid())
    {
        return;
    }
    const auto key = channel.toLower();
    Action action{
        .msecs = time.toMSecsSinceEpoch(),
        .kind = kind,
        .moderator = moderator.toLower(),
        .target = target.toLower(),
        .seconds = std::max(0, seconds),
    };

    {
        auto &st = store();
        std::scoped_lock lock(st.mutex);

        const auto dir = channelDir(key);
        if (ensureDir(dir))
        {
            const auto file = QStringLiteral("actions-%1.tsv")
                                  .arg(time.toLocalTime().toString(
                                      QStringLiteral("yyyy-MM")));
            appendLine(QDir(dir).filePath(file),
                       QString::number(action.msecs) + '\t' + kindKey(kind) +
                           '\t' + action.moderator + '\t' + action.target +
                           '\t' + QString::number(action.seconds));
        }

        auto it = st.channels.find(key);
        if (it != st.channels.end() && it->second.loaded)
        {
            action.moderator = intern(st, action.moderator);
            action.target = intern(st, action.target);
            auto &list = it->second.actions;
            // events can arrive slightly out of order
            auto pos = std::ranges::upper_bound(list, action.msecs, {},
                                                &Action::msecs);
            list.insert(pos, std::move(action));
        }
    }

    runInGuiThread([] {
        updated().invoke();
    });
}

void streamSeen(const QString &channel, const QString &streamId,
                const QDateTime &startedAt)
{
    if (channel.isEmpty() || streamId.isEmpty() || !startedAt.isValid())
    {
        return;
    }
    const auto key = channel.toLower();
    const auto now = QDateTime::currentMSecsSinceEpoch();

    auto &st = store();
    std::scoped_lock lock(st.mutex);
    auto &live = st.live[key];
    if (live.id != streamId)
    {
        live = {
            .id = streamId,
            .start = startedAt.toMSecsSinceEpoch(),
            .lastWritten = 0,
        };
    }
    if (now - live.lastWritten < STREAM_WRITE_MSECS)
    {
        return;
    }
    live.lastWritten = now;
    writeStreamLocked(st, key, live, now);
}

void streamEnded(const QString &channel)
{
    const auto key = channel.toLower();
    auto &st = store();
    std::scoped_lock lock(st.mutex);
    auto it = st.live.find(key);
    if (it == st.live.end())
    {
        return;
    }
    if (!it->second.id.isEmpty())
    {
        writeStreamLocked(st, key, it->second,
                          QDateTime::currentMSecsSinceEpoch());
    }
    st.live.erase(it);
}

std::vector<Action> actions(const QString &channel)
{
    const auto key = channel.toLower();
    auto &st = store();
    std::scoped_lock lock(st.mutex);
    auto &data = st.channels[key];
    loadLocked(st, key, data);
    return data.actions;
}

std::vector<Stream> streams(const QString &channel)
{
    const auto key = channel.toLower();
    auto &st = store();
    std::scoped_lock lock(st.mutex);
    auto &data = st.channels[key];
    loadLocked(st, key, data);
    return data.streams;
}

bool isBot(const QString &login)
{
    static const QSet<QString> bots{
        "fossabot",        "aecrobot",       "nightbot",
        "moobot",          "streamelements", "streamlabs",
        "wizebot",         "sery_bot",       "botrixoficial",
        "titlechange_bot", "soundalerts",    "pokemoncommunitygame",
        "commanderroot",   "deepbot",        "coebot",
        "vivbot",          "phantombot",     "ankhbot",
        "kofistreambot",   "blerp",
    };
    const auto name = login.toLower();
    return bots.contains(name) || name.endsWith(QStringLiteral("bot"));
}

pajlada::Signals::NoArgSignal &updated()
{
    static pajlada::Signals::NoArgSignal signal;
    return signal;
}

}  // namespace chatterino::modactivity
