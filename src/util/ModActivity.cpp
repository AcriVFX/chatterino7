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
#include <QJsonValue>
#include <QSaveFile>
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

struct TrackingData {
    bool loaded = false;
    Counting counting = Counting::On;
    qint64 until = 0;
    qint64 since = 0;
    bool pending = false;
    bool outdated = false;
    /// Goes up on every change, so a late server answer to an older change
    /// can't mark a newer one as taken
    int revision = 0;
};

struct Store {
    std::mutex mutex;
    std::unordered_map<QString, ChannelData> channels;
    std::unordered_map<QString, LiveStream> live;
    std::unordered_map<QString, TrackingData> tracking;
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

// ------------------------------------------------------------- tracking

constexpr std::array<std::pair<Counting, QStringView>, 3> COUNTING_KEYS{{
    {Counting::On, u"on"},
    {Counting::Paused, u"paused"},
    {Counting::Stopped, u"stopped"},
}};

QString countingKey(Counting counting)
{
    for (const auto &[c, key] : COUNTING_KEYS)
    {
        if (c == counting)
        {
            return key.toString();
        }
    }
    return {};
}

std::optional<Counting> countingFromKey(const QString &key)
{
    for (const auto &[c, name] : COUNTING_KEYS)
    {
        if (key == name)
        {
            return c;
        }
    }
    return std::nullopt;
}

/// Misc/ModActivity/<channel>/tracking.json
void saveTrackingLocked(const QString &channel, const TrackingData &t)
{
    const auto dir = channelDir(channel);
    if (!ensureDir(dir))
    {
        return;
    }
    // QSaveFile: a crash while writing must not leave an empty file, which
    // would turn a stop back into counting
    QSaveFile file(QDir(dir).filePath(QStringLiteral("tracking.json")));
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(QJsonObject{
                                     {"state", countingKey(t.counting)},
                                     {"until", t.until},
                                     {"since", t.since},
                                     {"pending", t.pending},
                                 })
                       .toJson(QJsonDocument::Compact));
        file.commit();
    }
}

/// The tracking state of `channel`; a pause that ran out turns into On
TrackingData &trackingLocked(Store &st, const QString &channel, qint64 now)
{
    auto &t = st.tracking[channel];
    if (!t.loaded)
    {
        t.loaded = true;
        const auto dir = channelDir(channel);
        QFile file(dir.isEmpty()
                       ? QString()
                       : QDir(dir).filePath(QStringLiteral("tracking.json")));
        if (!dir.isEmpty() && file.open(QIODevice::ReadOnly))
        {
            const auto obj = QJsonDocument::fromJson(file.readAll()).object();
            if (auto counting = countingFromKey(obj.value(u"state").toString()))
            {
                t.counting = *counting;
                t.until = obj.value(u"until").toInteger();
                t.since = obj.value(u"since").toInteger();
                t.pending = obj.value(u"pending").toBool();
            }
        }
    }
    if (t.counting == Counting::Paused && t.until <= now)
    {
        // the server's pause runs out at the same time
        t.counting = Counting::On;
        t.since = t.until;
        t.until = 0;
        t.pending = false;
        ++t.revision;
        saveTrackingLocked(channel, t);
    }
    return t;
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
    QMultiHash<QString, qint64> saved;
    saved.reserve(static_cast<qsizetype>(data.actions.size()));
    for (const auto &action : data.actions)
    {
        saved.insert(sameActionKey(action), action.msecs);
    }

    // the server may not have taken a pause or stop yet (or is too old for
    // it): what it counted since then stays out, like record() does here
    const auto &tracking =
        trackingLocked(st, channel, QDateTime::currentMSecsSinceEpoch());
    const qint64 offSince =
        tracking.counting == Counting::On ? -1 : tracking.since;

    std::vector<Action> fresh;
    // new lines per month file, so each file is opened once
    std::unordered_map<QString, QString> lines;
    const auto rawLines = body.split('\n');
    for (const auto &raw : rawLines)
    {
        Action action;
        if (!parseAction(QString::fromUtf8(raw), action))
        {
            continue;
        }
        newest = std::max(newest, action.msecs);
        if (offSince >= 0 && action.msecs >= offSince)
        {
            continue;
        }
        auto [first, last] = saved.equal_range(sameActionKey(action));
        auto match = std::find_if(first, last, [&](qint64 msecs) {
            return std::abs(msecs - action.msecs) <= SAME_ACTION_MSECS;
        });
        if (match != last)
        {
            // each saved action stands for one server action only: three
            // deletes of the same user in two seconds stay three
            saved.erase(match);
            continue;
        }

        auto &chunk = lines[actionFile(action.msecs)];
        chunk += formatAction(action);
        chunk += u'\n';
        action.moderator = intern(st, action.moderator);
        action.target = intern(st, action.target);
        fresh.push_back(std::move(action));
    }

    for (const auto &[name, chunk] : lines)
    {
        QFile file(QDir(dir).filePath(name));
        if (file.open(QIODevice::Append | QIODevice::Text))
        {
            QTextStream out(&file);
            out << chunk;
        }
    }

    // one sort instead of an insert in the middle for every action
    const auto added = static_cast<int>(fresh.size());
    if (added > 0)
    {
        const auto oldSize = data.actions.size();
        std::ranges::move(fresh, std::back_inserter(data.actions));
        std::ranges::stable_sort(
            data.actions.begin() + static_cast<std::ptrdiff_t>(oldSize),
            data.actions.end(), {}, &Action::msecs);
        std::ranges::inplace_merge(
            data.actions,
            data.actions.begin() + static_cast<std::ptrdiff_t>(oldSize), {},
            &Action::msecs);
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
    const auto lines = body.split('\n');
    for (const auto &raw : lines)
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

/// The channel the mod tracker server counts, learned from /v1/status.
/// The server serves only that channel, so the others act as if there was
/// no server (no "needs setup.ps1 update" for them). Only used from the GUI
/// thread.
QString &serverChannel()
{
    static QString channel;
    return channel;
}

/// remoteConfig(), unless the server is known to count another channel
std::optional<RemoteConfig> remoteConfigFor(const QString &channel)
{
    auto config = remoteConfig();
    if (config && !serverChannel().isEmpty() && serverChannel() != channel)
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
    /// Goes up when the counts are deleted, so a sync that was already
    /// running doesn't bring the deleted actions back
    int epoch = 0;
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
    // a lost cursor would download the whole history again
    QSaveFile file(syncFile(channel));
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(QJsonObject{
                                     {"cursor", state.cursor},
                                     {"lastSync", state.lastSync},
                                 })
                       .toJson(QJsonDocument::Compact));
        file.commit();
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
                             const QUrlQuery &query = {},
                             NetworkRequestType type = NetworkRequestType::Get)
{
    auto url = config.url;
    url.setPath(path);
    url.setQuery(query);
    // Cloudflare Access answers a missing key with a redirect to its login
    // page, which must count as a failure
    auto request = NetworkRequest(url, type)
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

/// Sends the tracking state of `channel` to the server if it hasn't taken it
/// yet. Returns whether it was sent.
bool pushTracking(const QString &channel, const RemoteConfig &config)
{
    QJsonObject body;
    int revision = 0;
    {
        auto &st = store();
        std::scoped_lock lock(st.mutex);
        const auto &t =
            trackingLocked(st, channel, QDateTime::currentMSecsSinceEpoch());
        if (!t.pending)
        {
            return false;
        }
        body = {
            {"state", countingKey(t.counting)},
            {"until", t.until},
        };
        revision = t.revision;
    }

    serverRequest(config, QStringLiteral("/v1/%1/tracking").arg(channel), {},
                  NetworkRequestType::Post)
        .json(body)
        .onSuccess([channel, revision](const NetworkResult &result) {
            if (!isOk(result))
            {
                return;
            }
            {
                auto &st = store();
                std::scoped_lock lock(st.mutex);
                auto &t = trackingLocked(st, channel,
                                         QDateTime::currentMSecsSinceEpoch());
                t.outdated = false;
                if (t.revision == revision)
                {
                    t.pending = false;
                    saveTrackingLocked(channel, t);
                }
            }
            syncStatusChanged().invoke();
        })
        .onError([channel](const NetworkResult &result) {
            // a server from before the tracking controls
            if (result.status() == 404)
            {
                auto &st = store();
                std::scoped_lock lock(st.mutex);
                trackingLocked(st, channel, QDateTime::currentMSecsSinceEpoch())
                    .outdated = true;
            }
            syncStatusChanged().invoke();
        })
        .execute();
    return true;
}

/// Takes the server's tracking state when the app has nothing to send
void adoptServerTracking(const QString &channel, const QJsonObject &obj)
{
    const auto counting = countingFromKey(obj.value(u"state").toString());
    if (!counting)
    {
        // a server from before the tracking controls
        return;
    }
    const auto until =
        *counting == Counting::Paused ? obj.value(u"until").toInteger() : 0;

    auto &st = store();
    std::scoped_lock lock(st.mutex);
    auto &t = trackingLocked(st, channel, QDateTime::currentMSecsSinceEpoch());
    t.outdated = false;
    if (t.pending || (t.counting == *counting && t.until == until))
    {
        return;
    }
    t.counting = *counting;
    t.until = until;
    t.since = obj.value(u"since").toInteger();
    ++t.revision;
    saveTrackingLocked(channel, t);
}

/// A sync that started before the counts were deleted stops here
bool deletedSince(const QString &channel, int epoch)
{
    auto &state = syncState(channel);
    if (state.epoch == epoch)
    {
        return false;
    }
    state.running = false;
    return true;
}

void fetchStreams(const QString &channel, const RemoteConfig &config,
                  bool anyAdded, int epoch)
{
    serverRequest(config, QStringLiteral("/v1/%1/streams").arg(channel))
        .onSuccess([channel, anyAdded, epoch](const NetworkResult &result) {
            if (deletedSince(channel, epoch))
            {
                return;
            }
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

void fetchActions(const QString &channel, const RemoteConfig &config, int epoch)
{
    const auto &state = syncState(channel);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("since"),
                       QString::number(std::max<qint64>(
                           0, state.cursor - SYNC_OVERLAP_MSECS)));

    serverRequest(config, QStringLiteral("/v1/%1/actions").arg(channel), query)
        .onSuccess([channel, config, epoch](const NetworkResult &result) {
            if (deletedSince(channel, epoch))
            {
                return;
            }
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
            fetchStreams(channel, config, added > 0, epoch);
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

        if (trackingLocked(st, key, QDateTime::currentMSecsSinceEpoch())
                .counting != Counting::On)
        {
            return;
        }

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
        .onSuccess([key, config = *config,
                    epoch = state.epoch](const NetworkResult &result) {
            if (deletedSince(key, epoch))
            {
                return;
            }
            const auto status = result.parseJson();
            if (isOk(result) && !status.value(u"channel").toString().isEmpty())
            {
                serverChannel() = status.value(u"channel").toString();
            }
            // a server counting another channel has nothing for this one
            if (!isOk(result) || status.value(u"channel").toString() != key)
            {
                finishSync(key, false);
                return;
            }
            syncState(key).needsSignIn = status.value(u"needsSignIn").toBool();
            if (!pushTracking(key, config))
            {
                adoptServerTracking(key, status.value(u"tracking").toObject());
            }
            fetchActions(key, config, epoch);
        })
        .onError([key](const NetworkResult &) {
            finishSync(key, false);
        })
        .execute();
}

SyncStatus syncStatus(const QString &channel)
{
    const auto key = channel.toLower();
    const auto config = remoteConfigFor(key);
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

std::optional<NetworkRequest> remoteRequest(const QString &path,
                                            const QUrlQuery &query)
{
    const auto config = remoteConfig();
    if (!config)
    {
        return std::nullopt;
    }
    return serverRequest(*config, path, query);
}

QString remoteHost()
{
    const auto config = remoteConfig();
    return config ? config->url.host() : QString();
}

Tracking tracking(const QString &channel)
{
    const auto key = channel.toLower();
    auto &st = store();
    std::scoped_lock lock(st.mutex);
    const auto &t =
        trackingLocked(st, key, QDateTime::currentMSecsSinceEpoch());
    return {
        .counting = t.counting,
        .until = t.counting == Counting::Paused
                     ? QDateTime::fromMSecsSinceEpoch(t.until)
                     : QDateTime(),
        .since =
            t.since > 0 ? QDateTime::fromMSecsSinceEpoch(t.since) : QDateTime(),
        .serverPending = t.pending,
        .serverOutdated = t.outdated,
    };
}

void setCounting(const QString &channel, Counting counting,
                 const QDateTime &until)
{
    const auto key = channel.toLower();
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (counting == Counting::Paused &&
        (!until.isValid() || until.toMSecsSinceEpoch() <= now))
    {
        return;
    }
    {
        auto &st = store();
        std::scoped_lock lock(st.mutex);
        auto &t = trackingLocked(st, key, now);
        t.counting = counting;
        t.until = counting == Counting::Paused ? until.toMSecsSinceEpoch() : 0;
        t.since = now;
        // also without a server, so one set up later gets it
        t.pending = true;
        ++t.revision;
        saveTrackingLocked(key, t);
    }
    if (const auto config = remoteConfigFor(key))
    {
        pushTracking(key, *config);
    }
    syncStatusChanged().invoke();
}

namespace {

void deleteLocal(const QString &channel)
{
    {
        auto &st = store();
        std::scoped_lock lock(st.mutex);
        const auto dir = channelDir(channel);
        if (!dir.isEmpty())
        {
            QDir qdir(dir);
            const auto files = qdir.entryList({QStringLiteral("actions-*.tsv"),
                                               QStringLiteral("streams.tsv")},
                                              QDir::Files);
            for (const auto &name : files)
            {
                QFile::remove(qdir.filePath(name));
            }
        }
        auto &data = st.channels[channel];
        data = {};
        data.loaded = true;
        // the running stream gets its row again on the next status refresh
        auto live = st.live.find(channel);
        if (live != st.live.end())
        {
            live->second.lastWritten = 0;
        }
    }

    // whatever the server has now was counted after the delete
    auto &state = syncState(channel);
    ++state.epoch;
    state.cursor = 0;
    saveSyncState(channel, state);
    updated().invoke();
}

}  // namespace

void deleteAll(const QString &channel,
               std::function<void(const QString &error)> done)
{
    const auto key = channel.toLower();
    const auto config = remoteConfigFor(key);
    if (!config)
    {
        deleteLocal(key);
        done({});
        return;
    }

    const auto host = config->url.host();
    if (serverChannel().isEmpty())
    {
        // find out first which channel the server counts, it only knows
        // that one
        serverRequest(*config, QStringLiteral("/v1/status"))
            .onSuccess([channel, host, done](const NetworkResult &result) {
                const auto name =
                    result.parseJson().value(u"channel").toString();
                if (!isOk(result) || name.isEmpty())
                {
                    done(QString("%1 could not be reached, so nothing was "
                                 "deleted.")
                             .arg(host));
                    return;
                }
                serverChannel() = name;
                deleteAll(channel, done);
            })
            .onError([host, done](const NetworkResult &) {
                done(QString("%1 could not be reached, so nothing was "
                             "deleted.")
                         .arg(host));
            })
            .execute();
        return;
    }

    serverRequest(*config, QStringLiteral("/v1/%1/delete").arg(key), {},
                  NetworkRequestType::Post)
        .json(QJsonObject{{"confirm", key}})
        .onSuccess([key, host, done](const NetworkResult &result) {
            if (!isOk(result))
            {
                done(QString("%1 did not delete the counts, so nothing was "
                             "deleted.")
                         .arg(host));
                return;
            }
            deleteLocal(key);
            done({});
            syncFromServer(key);
        })
        .onError([host, done](const NetworkResult &result) {
            const auto status = result.status();
            if (!status)
            {
                done(QString("%1 could not be reached, so nothing was "
                             "deleted.")
                         .arg(host));
            }
            else if (*status == 404)
            {
                // a server from before the tracking controls
                done(QString("%1 needs setup.ps1 update before it can "
                             "delete, so nothing was deleted.")
                         .arg(host));
            }
            else
            {
                done(QString("%1 did not delete the counts (error %2), so "
                             "nothing was deleted.")
                         .arg(host)
                         .arg(*status));
            }
        })
        .execute();
}

}  // namespace chatterino::modactivity
