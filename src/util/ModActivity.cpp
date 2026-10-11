// Chatterino9: see ModActivity.hpp

#include "util/ModActivity.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "singletons/Paths.hpp"
#include "util/PostToThread.hpp"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringView>
#include <QTextStream>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace chatterino::modactivity {

namespace {

/// A live stream gets its "last seen" time written at most this often
constexpr qint64 STREAM_WRITE_MSECS = 5LL * 60 * 1000;

/// A server action this close to a saved one with the same kind, mod and
/// target is the same action, seen by both the app and the server
constexpr qint64 SAME_ACTION_MSECS = 5000;

/// Each sync asks for the last few minutes again, in case the server saved
/// an action a little late
constexpr qint64 SYNC_OVERLAP_MSECS = 10LL * 60 * 1000;

constexpr int SYNC_TIMEOUT_MSECS = 20000;

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

QString formatAction(const Action &action)
{
    return QString::number(action.msecs) + '\t' + kindKey(action.kind) + '\t' +
           action.moderator + '\t' + action.target + '\t' +
           QString::number(action.seconds);
}

/// Parses one actions-*.tsv line, names are not interned yet
bool parseAction(const QString &line, Action &action)
{
    const auto parts = line.split(u'\t');
    if (parts.size() != 5)
    {
        return false;
    }
    bool okTime = false;
    bool okSeconds = false;
    action.msecs = parts[0].toLongLong(&okTime);
    action.seconds = parts[4].toInt(&okSeconds);
    if (!okTime || !okSeconds || parts[2].isEmpty() ||
        !kindFromKey(parts[1], action.kind))
    {
        return false;
    }
    action.moderator = parts[2];
    action.target = parts[3];
    return true;
}

QString actionFile(qint64 msecs)
{
    return QStringLiteral("actions-%1.tsv")
        .arg(QDateTime::fromMSecsSinceEpoch(msecs).toLocalTime().toString(
            QStringLiteral("yyyy-MM")));
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
            Action action;
            if (!parseAction(in.readLine(), action))
            {
                continue;
            }
            action.moderator = intern(st, action.moderator);
            action.target = intern(st, action.target);
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

QString sameActionKey(const Action &action)
{
    return kindKey(action.kind) + '\t' + action.moderator + '\t' +
           action.target;
}

/// Saves the server's actions that are not saved yet. Returns how many were
/// added, `newest` gets the time of the newest server action.
int mergeActionsLocked(Store &st, const QString &channel,
                       const QByteArray &body, qint64 &newest)
{
    const auto dir = channelDir(channel);
    if (!ensureDir(dir))
    {
        return 0;
    }
    auto &data = st.channels[channel];
    loadLocked(st, channel, data);

    // times of the actions saved before this sync, by kind, mod and target
    QHash<QString, std::vector<qint64>> saved;
    for (const auto &action : data.actions)
    {
        saved[sameActionKey(action)].push_back(action.msecs);
    }

    int added = 0;
    for (const auto &raw : body.split('\n'))
    {
        Action action;
        if (!parseAction(QString::fromUtf8(raw), action))
        {
            continue;
        }
        newest = std::max(newest, action.msecs);
        const auto it = saved.constFind(sameActionKey(action));
        if (it != saved.constEnd() &&
            std::ranges::any_of(*it, [&](qint64 msecs) {
                return std::abs(msecs - action.msecs) <= SAME_ACTION_MSECS;
            }))
        {
            continue;
        }

        appendLine(QDir(dir).filePath(actionFile(action.msecs)),
                   formatAction(action));
        action.moderator = intern(st, action.moderator);
        action.target = intern(st, action.target);
        auto pos = std::ranges::upper_bound(data.actions, action.msecs, {},
                                            &Action::msecs);
        data.actions.insert(pos, std::move(action));
        ++added;
    }
    return added;
}

/// Saves the server's streams and stream ends the app doesn't know yet.
/// Returns whether anything changed.
bool mergeStreamsLocked(Store &st, const QString &channel,
                        const QByteArray &body)
{
    const auto dir = channelDir(channel);
    if (!ensureDir(dir))
    {
        return false;
    }
    auto &data = st.channels[channel];
    loadLocked(st, channel, data);

    bool changed = false;
    for (const auto &raw : body.split('\n'))
    {
        const auto parts = QString::fromUtf8(raw).split(u'\t');
        bool okStart = false;
        bool okEnd = false;
        const auto start = parts.value(1).toLongLong(&okStart);
        const auto end = parts.value(2).toLongLong(&okEnd);
        if (parts.size() != 3 || parts[0].isEmpty() || !okStart || !okEnd)
        {
            continue;
        }
        auto s = std::ranges::find(data.streams, parts[0], &Stream::id);
        if (s != data.streams.end() && s->end >= end)
        {
            continue;
        }
        appendLine(QDir(dir).filePath(QStringLiteral("streams.tsv")),
                   parts[0] + '\t' + QString::number(start) + '\t' +
                       QString::number(end));
        if (s == data.streams.end())
        {
            data.streams.push_back({parts[0], start, std::max(start, end)});
            std::ranges::sort(data.streams, {}, &Stream::start);
        }
        else
        {
            s->end = end;
        }
        changed = true;
    }
    return changed;
}

// ------------------------------------------------------------------ sync

/// Misc/ModActivity/remote.json, written by the mod tracker's setup script
struct RemoteConfig {
    QUrl url;
    QByteArray apiKey;
    QByteArray accessClientId;
    QByteArray accessClientSecret;
};

std::optional<RemoteConfig> remoteConfig()
{
    const auto &misc = getApp()->getPaths().miscDirectory;
    if (misc.isEmpty())
    {
        return std::nullopt;
    }
    QFile file(QDir(misc).filePath(QStringLiteral("ModActivity/remote.json")));
    if (!file.open(QIODevice::ReadOnly))
    {
        return std::nullopt;
    }
    const auto obj = QJsonDocument::fromJson(file.readAll()).object();
    RemoteConfig config{
        .url = QUrl(obj.value(u"url").toString()),
        .apiKey = obj.value(u"apiKey").toString().toUtf8(),
        .accessClientId = obj.value(u"accessClientId").toString().toUtf8(),
        .accessClientSecret =
            obj.value(u"accessClientSecret").toString().toUtf8(),
    };
    // the keys only ever go over https
    if (!config.url.isValid() || config.url.scheme() != u"https" ||
        config.url.host().isEmpty() || config.apiKey.isEmpty())
    {
        return std::nullopt;
    }
    return config;
}

/// Only used from the GUI thread
struct SyncState {
    bool loaded = false;
    bool running = false;
    bool ok = false;
    bool needsSignIn = false;
    /// Time of the newest server action merged so far
    qint64 cursor = 0;
    qint64 lastSync = 0;
};

QString syncFile(const QString &channel)
{
    const auto dir = channelDir(channel);
    return dir.isEmpty()
               ? QString()
               : QDir(dir).filePath(QStringLiteral("server-sync.json"));
}

SyncState &syncState(const QString &channel)
{
    static std::unordered_map<QString, SyncState> states;
    auto &state = states[channel];
    if (!state.loaded)
    {
        state.loaded = true;
        QFile file(syncFile(channel));
        if (file.open(QIODevice::ReadOnly))
        {
            const auto obj = QJsonDocument::fromJson(file.readAll()).object();
            state.cursor = obj.value(u"cursor").toInteger();
            state.lastSync = obj.value(u"lastSync").toInteger();
            state.ok = state.lastSync > 0;
        }
    }
    return state;
}

void saveSyncState(const QString &channel, const SyncState &state)
{
    if (!ensureDir(channelDir(channel)))
    {
        return;
    }
    QFile file(syncFile(channel));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        file.write(QJsonDocument(QJsonObject{
                                     {"cursor", state.cursor},
                                     {"lastSync", state.lastSync},
                                 })
                       .toJson(QJsonDocument::Compact));
    }
}

void finishSync(const QString &channel, bool ok)
{
    auto &state = syncState(channel);
    state.running = false;
    state.ok = ok;
    if (ok)
    {
        state.lastSync = QDateTime::currentMSecsSinceEpoch();
        saveSyncState(channel, state);
    }
    syncStatusChanged().invoke();
}

NetworkRequest serverRequest(const RemoteConfig &config, const QString &path,
                             const QUrlQuery &query = {})
{
    auto url = config.url;
    url.setPath(path);
    url.setQuery(query);
    // Cloudflare Access answers a missing key with a redirect to its login
    // page, which must count as a failure
    auto request = NetworkRequest(url)
                       .followRedirects(false)
                       .timeout(SYNC_TIMEOUT_MSECS)
                       .header("X-Modtracker-Key", config.apiKey);
    if (!config.accessClientId.isEmpty())
    {
        request =
            std::move(request)
                .header("CF-Access-Client-Id", config.accessClientId)
                .header("CF-Access-Client-Secret", config.accessClientSecret);
    }
    return request;
}

bool isOk(const NetworkResult &result)
{
    return result.status() == 200;
}

void fetchStreams(const QString &channel, const RemoteConfig &config,
                  bool anyAdded)
{
    serverRequest(config, QStringLiteral("/v1/%1/streams").arg(channel))
        .onSuccess([channel, anyAdded](const NetworkResult &result) {
            if (!isOk(result))
            {
                finishSync(channel, false);
                return;
            }
            bool changed = anyAdded;
            {
                auto &st = store();
                std::scoped_lock lock(st.mutex);
                changed = mergeStreamsLocked(st, channel, result.getData()) ||
                          changed;
            }
            finishSync(channel, true);
            if (changed)
            {
                updated().invoke();
            }
        })
        .onError([channel](const NetworkResult &) {
            finishSync(channel, false);
        })
        .execute();
}

void fetchActions(const QString &channel, const RemoteConfig &config)
{
    const auto &state = syncState(channel);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("since"),
                       QString::number(std::max<qint64>(
                           0, state.cursor - SYNC_OVERLAP_MSECS)));

    serverRequest(config, QStringLiteral("/v1/%1/actions").arg(channel), query)
        .onSuccess([channel, config](const NetworkResult &result) {
            if (!isOk(result))
            {
                finishSync(channel, false);
                return;
            }
            auto &state = syncState(channel);
            qint64 newest = state.cursor;
            int added = 0;
            {
                auto &st = store();
                std::scoped_lock lock(st.mutex);
                added =
                    mergeActionsLocked(st, channel, result.getData(), newest);
            }
            state.cursor = newest;
            fetchStreams(channel, config, added > 0);
        })
        .onError([channel](const NetworkResult &) {
            finishSync(channel, false);
        })
        .execute();
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
            appendLine(QDir(dir).filePath(actionFile(action.msecs)),
                       formatAction(action));
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

void syncFromServer(const QString &channel)
{
    const auto key = channel.toLower();
    const auto config = remoteConfig();
    auto &state = syncState(key);
    if (!config || state.running)
    {
        return;
    }
    state.running = true;

    serverRequest(*config, QStringLiteral("/v1/status"))
        .onSuccess([key, config = *config](const NetworkResult &result) {
            const auto status = result.parseJson();
            // a server counting another channel has nothing for this one
            if (!isOk(result) || status.value(u"channel").toString() != key)
            {
                finishSync(key, false);
                return;
            }
            syncState(key).needsSignIn = status.value(u"needsSignIn").toBool();
            fetchActions(key, config);
        })
        .onError([key](const NetworkResult &) {
            finishSync(key, false);
        })
        .execute();
}

SyncStatus syncStatus(const QString &channel)
{
    const auto key = channel.toLower();
    const auto config = remoteConfig();
    if (!config)
    {
        return {};
    }
    const auto &state = syncState(key);
    return {
        .configured = true,
        .ok = state.ok,
        .needsSignIn = state.needsSignIn,
        .host = config->url.host(),
        .lastSync = state.lastSync > 0
                        ? QDateTime::fromMSecsSinceEpoch(state.lastSync)
                        : QDateTime(),
    };
}

pajlada::Signals::NoArgSignal &syncStatusChanged()
{
    static pajlada::Signals::NoArgSignal signal;
    return signal;
}

}  // namespace chatterino::modactivity
