// Chatterino9: see EmoteStats.hpp

#include "util/EmoteStats.hpp"

#include "Application.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Paths.hpp"
#include "util/ModActivity.hpp"
#include "util/PostToThread.hpp"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QTextStream>
#include <QThreadPool>
#include <QTimeZone>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace chatterino::emotestats {

namespace {

constexpr qint64 HOUR_MS = 60LL * 60 * 1000;
/// Counts in memory are written to disk at most this often
constexpr qint64 FLUSH_MSECS = 30LL * 1000;
/// Message ids remembered to count each message once
constexpr size_t RECENT_IDS = 5000;
/// Days kept in memory after they were written (today and yesterday)
constexpr size_t DAYS_IN_MEMORY = 2;

constexpr QStringView TOTAL_KEY = u"*";

QString dataDir(const QString &channel, const QString &part)
{
    static const QRegularExpression validName(QStringLiteral("^[a-z0-9_]+$"));
    const auto &misc = getApp()->getPaths().miscDirectory;
    if (misc.isEmpty() || !validName.match(channel).hasMatch())
    {
        return {};
    }
    return QDir(misc).filePath(
        QStringLiteral("EmoteStats/%1/%2").arg(channel, part));
}

QString channelRoot(const QString &channel)
{
    return dataDir(channel, {});
}

/// UTC day number (days since 1970-01-01) of `msecs`
qint64 dayOf(qint64 msecs)
{
    auto hours = msecs / HOUR_MS;
    if (msecs < 0 && msecs % HOUR_MS != 0)
    {
        hours--;
    }
    auto days = hours / 24;
    if (hours < 0 && hours % 24 != 0)
    {
        days--;
    }
    return days;
}

QString dayName(qint64 day)
{
    return QDate(1970, 1, 1).addDays(day).toString(Qt::ISODate);
}

std::optional<qint64> dayFromName(QStringView name)
{
    auto date = QDate::fromString(name.toString(), Qt::ISODate);
    if (!date.isValid())
    {
        return std::nullopt;
    }
    return QDate(1970, 1, 1).daysTo(date);
}

QString bucketKey(int hour, const QString &source, const QString &name)
{
    return QString::number(hour).rightJustified(2, u'0') + u'\t' + source +
           u'\t' + name;
}

struct Bucket {
    qint64 count = 0;
    QSet<QString> users;
};

using Day = QHash<QString, Bucket>;

/// Reads one day file into `day`, adding to what is there
void readDay(const QString &path, Day &day)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return;
    }
    QTextStream in(&file);
    QString line;
    while (in.readLineInto(&line))
    {
        const auto parts = QStringView(line).split(u'\t');
        if (parts.size() != 5)
        {
            continue;
        }
        bool ok = false;
        const auto count = parts[3].toLongLong(&ok);
        if (!ok)
        {
            continue;
        }
        auto &bucket = day[parts[0].toString() + u'\t' + parts[1].toString() +
                           u'\t' + parts[2].toString()];
        bucket.count = std::max(bucket.count, count);
        if (!parts[4].isEmpty())
        {
            const auto users = parts[4].split(u',');
            for (auto user : users)
            {
                bucket.users.insert(user.toString());
            }
        }
    }
}

bool writeDay(const QString &path, const Day &day)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return false;
    }
    QTextStream out(&file);
    for (auto it = day.cbegin(); it != day.cend(); ++it)
    {
        out << it.key() << u'\t' << it.value().count << u'\t';
        bool first = true;
        for (const auto &user : it.value().users)
        {
            if (!first)
            {
                out << u',';
            }
            first = false;
            out << user;
        }
        out << u'\n';
    }
    out.flush();
    return file.commit();
}

// ----------------------------------------------------------- live store ---

struct LiveDay {
    Day buckets;
    bool dirty = false;
};

struct LiveChannel {
    std::map<qint64, LiveDay> days;
    std::deque<QString> recentIds;
    QSet<QString> recentIdSet;
};

struct Store {
    std::mutex mutex;
    std::unordered_map<QString, LiveChannel> channels;
    qint64 lastFlush = 0;
};

Store &store()
{
    // never destroyed: the log import thread may still use it at exit
    static auto *instance = new Store;
    return *instance;
}

/// Writes the day files one after the other, off the GUI thread. One thread
/// keeps them in order, so an older copy never overwrites a newer one.
QThreadPool &writerPool()
{
    static auto *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(1);
        return p;
    }();
    return *pool;
}

void flushLocked(Store &st)
{
    st.lastFlush = QDateTime::currentMSecsSinceEpoch();
    // Day is implicitly shared: the copies are cheap, and the next count
    // only copies the hour bucket it changes
    std::vector<std::tuple<QString, qint64, QString, Day>> todo;
    for (auto &[channel, live] : st.channels)
    {
        const auto dir = dataDir(channel, QStringLiteral("local"));
        if (dir.isEmpty() || !QDir().mkpath(dir))
        {
            continue;
        }
        std::optional<qint64> oldestQueued;
        for (auto &[day, data] : live.days)
        {
            if (data.dirty)
            {
                todo.emplace_back(channel, day,
                                  QDir(dir).filePath(dayName(day) + ".tsv"),
                                  data.buckets);
                data.dirty = false;
                if (!oldestQueued)
                {
                    oldestQueued = day;
                }
            }
        }
        // keep only the newest days in memory, once they are on disk. A day
        // queued just now stays until the next flush, so a failed write can
        // still mark it dirty again
        while (live.days.size() > DAYS_IN_MEMORY &&
               !live.days.begin()->second.dirty &&
               live.days.begin()->first != oldestQueued)
        {
            live.days.erase(live.days.begin());
        }
    }
    if (todo.empty())
    {
        return;
    }
    writerPool().start([todo = std::move(todo)] {
        for (const auto &[channel, day, path, buckets] : todo)
        {
            if (!writeDay(path, buckets))
            {
                // try again with the next flush
                auto &st = store();
                std::scoped_lock lock(st.mutex);
                auto ch = st.channels.find(channel);
                if (ch != st.channels.end())
                {
                    auto d = ch->second.days.find(day);
                    if (d != ch->second.days.end())
                    {
                        d->second.dirty = true;
                    }
                }
            }
        }
    });
}

// ------------------------------------------------------------ log import ---

struct ImportState {
    std::mutex mutex;
    QSet<QString> running;
};

ImportState &importState()
{
    // never destroyed: the import thread may still run at exit
    static auto *instance = new ImportState;
    return *instance;
}

std::optional<Source> classifyName(const ImportNames &names,
                                   const QString &word)
{
    if (names.twitch.contains(word))
    {
        return Source::Twitch;
    }
    if (names.ffzChannel.contains(word))
    {
        return Source::Ffz;
    }
    if (names.bttvChannel.contains(word))
    {
        return Source::Bttv;
    }
    if (names.seventvChannel.contains(word))
    {
        return Source::SevenTv;
    }
    if (names.ffzGlobal.contains(word))
    {
        return Source::Ffz;
    }
    if (names.bttvGlobal.contains(word))
    {
        return Source::Bttv;
    }
    if (names.seventvGlobal.contains(word))
    {
        return Source::SevenTv;
    }
    return std::nullopt;
}

void countInto(Day &day, int hour, const QString &login,
               const std::vector<Use> &uses)
{
    auto &total =
        day[bucketKey(hour, TOTAL_KEY.toString(), TOTAL_KEY.toString())];
    total.count++;

    QSet<QString> seen;
    for (const auto &use : uses)
    {
        const auto source = sourceKey(use.source);
        auto key = bucketKey(hour, source, use.name);
        if (use.name.isEmpty() || seen.contains(key))
        {
            continue;
        }
        seen.insert(key);
        auto &bucket = day[key];
        bucket.count++;
        if (!login.isEmpty())
        {
            bucket.users.insert(login);
        }
    }
}

void runImport(const QString &channel, const QString &logDirectory,
               const QString &outDir, const QString &doneFile,
               const ImportNames &names)
{
    // "[12:34:56] login: text" or "[12:34:56] Display login: text"
    // not static: a static would be destroyed at exit while this thread
    // may still run
    const QRegularExpression lineRegex(QStringLiteral(
        R"(^\[(\d\d):(\d\d):(\d\d)\] (?:[^\s:]+ )?([a-z0-9_]+): (.*)$)"));
    const QRegularExpression fileRegex(
        QStringLiteral("^%1-(\\d{4}-\\d{2}-\\d{2})\\.log$")
            .arg(QRegularExpression::escape(channel)));

    QDir().mkpath(outDir);
    auto files = QDir(logDirectory)
                     .entryList({channel + "-*.log"}, QDir::Files, QDir::Name);

    std::map<qint64, Day> pending;
    auto writeDaysBefore = [&](qint64 limit) {
        while (!pending.empty() && pending.begin()->first < limit)
        {
            writeDay(
                QDir(outDir).filePath(dayName(pending.begin()->first) + ".tsv"),
                pending.begin()->second);
            pending.erase(pending.begin());
        }
    };

    for (const auto &fileName : std::as_const(files))
    {
        auto fileMatch = fileRegex.match(fileName);
        if (!fileMatch.hasMatch())
        {
            continue;
        }
        const auto date = QDate::fromString(fileMatch.captured(1), Qt::ISODate);
        if (!date.isValid())
        {
            continue;
        }
        // a local day spans two UTC days, everything before is complete
        writeDaysBefore(
            dayOf(QDateTime(date, QTime(0, 0)).toMSecsSinceEpoch()) - 1);

        QFile file(QDir(logDirectory).filePath(fileName));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            continue;
        }
        QTextStream in(&file);
        QString line;
        std::vector<Use> uses;
        while (in.readLineInto(&line))
        {
            auto m = lineRegex.match(line);
            if (!m.hasMatch())
            {
                continue;
            }
            const QDateTime time(
                date, QTime(m.captured(1).toInt(), m.captured(2).toInt(),
                            m.captured(3).toInt()));
            if (!time.isValid())
            {
                continue;
            }
            const auto msecs = time.toMSecsSinceEpoch();
            uses.clear();
            const auto words = m.capturedView(5).split(u' ');
            for (auto word : words)
            {
                if (word.isEmpty())
                {
                    continue;
                }
                const auto text = word.toString();
                if (auto source = classifyName(names, text))
                {
                    uses.push_back({*source, text});
                }
            }
            const auto hours = msecs / HOUR_MS;
            countInto(pending[dayOf(msecs)], static_cast<int>(hours % 24),
                      m.captured(4), uses);
        }
    }
    writeDaysBefore(std::numeric_limits<qint64>::max());

    QSaveFile done(doneFile);
    if (done.open(QIODevice::WriteOnly))
    {
        done.write(QJsonDocument(
                       QJsonObject{
                           {"importedAt", QDateTime::currentMSecsSinceEpoch()},
                           {"files", static_cast<int>(files.size())},
                       })
                       .toJson(QJsonDocument::Compact));
        done.commit();
    }
}

// ----------------------------------------------------------- server sync ---

constexpr int SYNC_TIMEOUT_MSECS = 30000;

/// Only used from the GUI thread
struct SyncState {
    bool loaded = false;
    bool running = false;
    bool ok = false;
    bool notCounting = false;
    bool connected = false;
    qint64 lastSync = 0;
    /// Server modification time of each day downloaded
    QHash<QString, qint64> days;
};

QString syncFile(const QString &channel)
{
    const auto root = channelRoot(channel);
    return root.isEmpty()
               ? QString()
               : QDir(root).filePath(QStringLiteral("server-sync.json"));
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
            state.lastSync = obj.value(u"lastSync").toInteger();
            state.ok = state.lastSync > 0;
            const auto days = obj.value(u"days").toObject();
            for (auto it = days.begin(); it != days.end(); ++it)
            {
                state.days.insert(it.key(), it.value().toInteger());
            }
        }
    }
    return state;
}

void saveSyncState(const QString &channel, const SyncState &state)
{
    const auto root = channelRoot(channel);
    if (root.isEmpty() || !QDir().mkpath(root))
    {
        return;
    }
    QJsonObject days;
    for (auto it = state.days.cbegin(); it != state.days.cend(); ++it)
    {
        days.insert(it.key(), it.value());
    }
    QSaveFile file(syncFile(channel));
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(QJsonObject{
                                     {"lastSync", state.lastSync},
                                     {"days", days},
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
    }
    saveSyncState(channel, state);
    updated().invoke();
}

/// Downloads `todo` one after the other, then finishes the sync
void fetchDays(const QString &channel, QStringList todo,
               const QHash<QString, qint64> &mtimes)
{
    if (todo.isEmpty())
    {
        finishSync(channel, true);
        return;
    }
    const auto day = todo.takeFirst();
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("d"), day);
    auto request = modactivity::remoteRequest(
        QStringLiteral("/v1/%1/emotes/day").arg(channel), query);
    if (!request)
    {
        finishSync(channel, false);
        return;
    }
    std::move(*request)
        .timeout(SYNC_TIMEOUT_MSECS)
        .onSuccess([channel, day, todo, mtimes](const NetworkResult &result) {
            if (result.status() != 200)
            {
                finishSync(channel, false);
                return;
            }
            const auto dir = dataDir(channel, QStringLiteral("server"));
            if (dir.isEmpty() || !QDir().mkpath(dir))
            {
                finishSync(channel, false);
                return;
            }
            QSaveFile file(QDir(dir).filePath(day + ".tsv"));
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(result.getData()) < 0 || !file.commit())
            {
                finishSync(channel, false);
                return;
            }
            syncState(channel).days.insert(day, mtimes.value(day));
            fetchDays(channel, todo, mtimes);
        })
        .onError([channel](const NetworkResult &) {
            finishSync(channel, false);
        })
        .execute();
}

void fetchDayList(const QString &channel)
{
    auto request = modactivity::remoteRequest(
        QStringLiteral("/v1/%1/emotes/days").arg(channel));
    if (!request)
    {
        finishSync(channel, false);
        return;
    }
    std::move(*request)
        .onSuccess([channel](const NetworkResult &result) {
            auto &state = syncState(channel);
            if (result.status() == 404)
            {
                // a server from before emote counting
                state.notCounting = true;
                finishSync(channel, true);
                return;
            }
            if (result.status() != 200)
            {
                finishSync(channel, false);
                return;
            }
            state.notCounting = false;
            QStringList todo;
            QHash<QString, qint64> mtimes;
            const auto lines = QString::fromUtf8(result.getData()).split(u'\n');
            for (const auto &line : lines)
            {
                const auto parts = QStringView(line).split(u'\t');
                if (parts.size() != 2 || !dayFromName(parts[0]))
                {
                    continue;
                }
                const auto day = parts[0].toString();
                const auto mtime = parts[1].toLongLong();
                mtimes.insert(day, mtime);
                if (state.days.value(day, -1) != mtime)
                {
                    todo.append(day);
                }
            }
            fetchDays(channel, todo, mtimes);
        })
        .onError([channel](const NetworkResult &result) {
            auto &state = syncState(channel);
            if (result.status() == 404)
            {
                state.notCounting = true;
                finishSync(channel, true);
                return;
            }
            finishSync(channel, false);
        })
        .execute();
}

}  // namespace

QString sourceKey(Source source)
{
    switch (source)
    {
        case Source::Twitch:
            return QStringLiteral("twitch");
        case Source::Bttv:
            return QStringLiteral("bttv");
        case Source::Ffz:
            return QStringLiteral("ffz");
        case Source::SevenTv:
            return QStringLiteral("7tv");
    }
    return {};
}

std::optional<Source> sourceFromKey(QStringView key)
{
    if (key == u"twitch")
    {
        return Source::Twitch;
    }
    if (key == u"bttv")
    {
        return Source::Bttv;
    }
    if (key == u"ffz")
    {
        return Source::Ffz;
    }
    if (key == u"7tv")
    {
        return Source::SevenTv;
    }
    return std::nullopt;
}

QString sourceName(Source source)
{
    switch (source)
    {
        case Source::Twitch:
            return QStringLiteral("Twitch");
        case Source::Bttv:
            return QStringLiteral("BTTV");
        case Source::Ffz:
            return QStringLiteral("FFZ");
        case Source::SevenTv:
            return QStringLiteral("7TV");
    }
    return {};
}

std::optional<Source> classify(const TwitchChannel &channel,
                               const QString &name)
{
    const EmoteName emoteName{name};
    if (channel.ffzEmote(emoteName))
    {
        return Source::Ffz;
    }
    if (channel.bttvEmote(emoteName))
    {
        return Source::Bttv;
    }
    if (channel.seventvEmote(emoteName))
    {
        return Source::SevenTv;
    }
    if (getApp()->getFfzEmotes()->emote(emoteName))
    {
        return Source::Ffz;
    }
    if (getApp()->getBttvEmotes()->emote(emoteName))
    {
        return Source::Bttv;
    }
    if (getApp()->getSeventvEmotes()->globalEmote(emoteName))
    {
        return Source::SevenTv;
    }
    return std::nullopt;
}

bool countsChannel(const QString &channel)
{
    return channel.compare(u"papaplatte", Qt::CaseInsensitive) == 0;
}

void recordMessage(const QString &channel, const QString &messageId,
                   const QString &login, const QDateTime &time,
                   const std::vector<Use> &uses)
{
    if (channel.isEmpty() || !time.isValid())
    {
        return;
    }
    const auto key = channel.toLower();
    const auto msecs = time.toMSecsSinceEpoch();
    const auto day = dayOf(msecs);
    const auto hour = static_cast<int>((msecs / HOUR_MS) % 24);

    auto &st = store();
    std::scoped_lock lock(st.mutex);
    auto &live = st.channels[key];

    if (!messageId.isEmpty())
    {
        if (live.recentIdSet.contains(messageId))
        {
            return;
        }
        live.recentIdSet.insert(messageId);
        live.recentIds.push_back(messageId);
        if (live.recentIds.size() > RECENT_IDS)
        {
            live.recentIdSet.remove(live.recentIds.front());
            live.recentIds.pop_front();
        }
    }

    auto it = live.days.find(day);
    if (it == live.days.end())
    {
        // continue the day saved before a restart
        it = live.days.emplace(day, LiveDay{}).first;
        const auto dir = dataDir(key, QStringLiteral("local"));
        if (!dir.isEmpty())
        {
            readDay(QDir(dir).filePath(dayName(day) + ".tsv"),
                    it->second.buckets);
        }
    }
    countInto(it->second.buckets, hour, login.toLower(), uses);
    it->second.dirty = true;

    if (QDateTime::currentMSecsSinceEpoch() - st.lastFlush >= FLUSH_MSECS)
    {
        flushLocked(st);
    }
}

void flush()
{
    auto &st = store();
    std::scoped_lock lock(st.mutex);
    flushLocked(st);
}

void flushAndWait()
{
    flush();
    writerPool().waitForDone();
}

Stats load(const QString &channel, qint64 from, qint64 to)
{
    // the counts flush() handed to the writer are on disk before reading
    writerPool().waitForDone();

    Stats stats;
    const auto key = channel.toLower();
    const std::array<QString, 3> dirs{
        dataDir(key, QStringLiteral("local")),
        dataDir(key, QStringLiteral("server")),
        dataDir(key, QStringLiteral("logs")),
    };

    // every day with data in any source
    std::map<qint64, std::vector<QString>> days;
    for (const auto &dir : dirs)
    {
        if (dir.isEmpty())
        {
            continue;
        }
        const auto names =
            QDir(dir).entryList({QStringLiteral("*.tsv")}, QDir::Files);
        for (const auto &name : names)
        {
            if (auto day = dayFromName(QStringView(name).left(10)))
            {
                days[*day].push_back(QDir(dir).filePath(name));
            }
        }
    }
    if (!days.empty())
    {
        stats.firstMsecs = days.begin()->first * 24 * HOUR_MS;
    }

    const auto fromHour = from / HOUR_MS;
    struct Total {
        qint64 messages = 0;
        QSet<int> users;
    };
    QHash<QString, Total> totals;
    QHash<QString, int> userIds;

    for (const auto &[day, files] : days)
    {
        const auto dayStart = day * 24 * HOUR_MS;
        if (dayStart + (24 * HOUR_MS) <= fromHour * HOUR_MS || dayStart >= to)
        {
            continue;
        }
        Day merged;
        for (const auto &file : files)
        {
            readDay(file, merged);
        }
        for (auto it = merged.cbegin(); it != merged.cend(); ++it)
        {
            const auto parts = QStringView(it.key()).split(u'\t');
            if (parts.size() != 3)
            {
                continue;
            }
            const auto hourStart = dayStart + (parts[0].toInt() * HOUR_MS);
            if (hourStart < fromHour * HOUR_MS || hourStart >= to)
            {
                continue;
            }
            if (parts[1] == TOTAL_KEY)
            {
                stats.totalMessages += it.value().count;
                continue;
            }
            auto &total =
                totals[parts[1].toString() + u'\t' + parts[2].toString()];
            total.messages += it.value().count;
            for (const auto &user : it.value().users)
            {
                auto id = userIds.find(user);
                if (id == userIds.end())
                {
                    id = userIds.insert(user, static_cast<int>(userIds.size()));
                }
                total.users.insert(*id);
            }
        }
    }

    stats.rows.reserve(static_cast<size_t>(totals.size()));
    for (auto it = totals.cbegin(); it != totals.cend(); ++it)
    {
        const auto tab = it.key().indexOf(u'\t');
        auto source = sourceFromKey(QStringView(it.key()).left(tab));
        if (!source)
        {
            continue;
        }
        stats.rows.push_back({
            .source = *source,
            .name = it.key().mid(tab + 1),
            .messages = it.value().messages,
            .chatters = it.value().users.size(),
        });
    }
    return stats;
}

bool logsImported(const QString &channel)
{
    const auto root = channelRoot(channel.toLower());
    return !root.isEmpty() &&
           QFile::exists(QDir(root).filePath(QStringLiteral("import.json")));
}

void importLogs(const QString &channel, const QString &logDirectory,
                ImportNames names)
{
    const auto key = channel.toLower();
    const auto root = channelRoot(key);
    if (root.isEmpty())
    {
        return;
    }
    {
        auto &state = importState();
        std::scoped_lock lock(state.mutex);
        if (state.running.contains(key))
        {
            return;
        }
        state.running.insert(key);
    }
    const auto outDir = dataDir(key, QStringLiteral("logs"));
    const auto doneFile = QDir(root).filePath(QStringLiteral("import.json"));

    std::thread([key, logDirectory, outDir, doneFile,
                 names = std::move(names)] {
        runImport(key, logDirectory, outDir, doneFile, names);
        {
            auto &state = importState();
            std::scoped_lock lock(state.mutex);
            state.running.remove(key);
        }
        runInGuiThread([] {
            updated().invoke();
        });
    }).detach();
}

bool importRunning(const QString &channel)
{
    auto &state = importState();
    std::scoped_lock lock(state.mutex);
    return state.running.contains(channel.toLower());
}

void syncFromServer(const QString &channel)
{
    const auto key = channel.toLower();
    auto &state = syncState(key);
    if (state.running || modactivity::remoteHost().isEmpty())
    {
        return;
    }
    state.running = true;

    auto request = modactivity::remoteRequest(QStringLiteral("/v1/status"));
    if (!request)
    {
        state.running = false;
        return;
    }
    std::move(*request)
        .onSuccess([key](const NetworkResult &result) {
            const auto status = result.parseJson();
            // a server counting another channel has nothing for this one
            if (result.status() != 200 ||
                status.value(u"channel").toString() != key)
            {
                finishSync(key, false);
                return;
            }
            syncState(key).connected =
                status.value(u"emotes").toObject().value(u"connected").toBool();
            fetchDayList(key);
        })
        .onError([key](const NetworkResult &) {
            finishSync(key, false);
        })
        .execute();
}

SyncStatus syncStatus(const QString &channel)
{
    const auto key = channel.toLower();
    const auto host = modactivity::remoteHost();
    if (host.isEmpty())
    {
        return {};
    }
    const auto &state = syncState(key);
    return {
        .configured = true,
        .ok = state.ok,
        .notCounting = state.notCounting,
        .connected = state.connected,
        .host = host,
        .lastSync = state.lastSync > 0
                        ? QDateTime::fromMSecsSinceEpoch(state.lastSync)
                        : QDateTime(),
    };
}

pajlada::Signals::NoArgSignal &updated()
{
    static pajlada::Signals::NoArgSignal signal;
    return signal;
}

}  // namespace chatterino::emotestats
