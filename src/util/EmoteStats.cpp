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
#include <QFileInfo>
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
/// Name of the "*" line that counts messages with at least one emote
constexpr QStringView ANY_EMOTE_KEY = u"+";
/// Version of the log import. 2 counts per chatter; an import of an older
/// version is done again.
constexpr int IMPORT_VERSION = 2;

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
    /// Who sent the messages and how many; 0 means "at least one" (files
    /// from before chatters were counted)
    QHash<QString, qint64> users;
};

/// Adds "login" or "login:N" to `users`, the highest count wins
void addUser(QHash<QString, qint64> &users, QStringView entry)
{
    const auto colon = entry.indexOf(u':');
    qint64 count = 0;
    if (colon >= 0)
    {
        count = entry.mid(colon + 1).toLongLong();
        entry = entry.left(colon);
    }
    if (entry.isEmpty())
    {
        return;
    }
    auto &known = users[entry.toString()];
    known = std::max(known, count);
}

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
                addUser(bucket.users, user);
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
        const auto &users = it.value().users;
        for (auto user = users.cbegin(); user != users.cend(); ++user)
        {
            if (!first)
            {
                out << u',';
            }
            first = false;
            out << user.key();
            if (user.value() > 0)
            {
                out << u':' << user.value();
            }
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
    auto add = [&](const QString &key) {
        auto &bucket = day[key];
        bucket.count++;
        if (!login.isEmpty())
        {
            bucket.users[login]++;
        }
    };
    add(bucketKey(hour, TOTAL_KEY.toString(), TOTAL_KEY.toString()));

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
        add(key);
    }
    if (!seen.isEmpty())
    {
        add(bucketKey(hour, TOTAL_KEY.toString(), ANY_EMOTE_KEY.toString()));
    }
}

/// Source and name of every emote in the day files in `dir`, so an import
/// done again still finds emotes the channel removed since the last one
QHash<QString, Source> namesIn(const QString &dir)
{
    QHash<QString, Source> names;
    const auto files =
        QDir(dir).entryList({QStringLiteral("*.tsv")}, QDir::Files);
    for (const auto &name : files)
    {
        Day day;
        readDay(QDir(dir).filePath(name), day);
        for (auto it = day.cbegin(); it != day.cend(); ++it)
        {
            const auto parts = QStringView(it.key()).split(u'\t');
            if (parts.size() != 3)
            {
                continue;
            }
            if (auto source = sourceFromKey(parts[1]))
            {
                names.insert(parts[2].toString(), *source);
            }
        }
    }
    return names;
}

void runImport(const QString &channel, const QString &logDirectory,
               const QString &outDir, const QString &doneFile,
               const ImportNames &names)
{
    const auto earlierNames = namesIn(outDir);

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
                else if (auto earlier = earlierNames.constFind(text);
                         earlier != earlierNames.constEnd())
                {
                    uses.push_back({*earlier, text});
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
                           {"version", IMPORT_VERSION},
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

void flushAndWait()
{
    flush();
    writerPool().waitForDone();
}

void flush()
{
    auto &st = store();
    std::scoped_lock lock(st.mutex);
    flushLocked(st);
}

namespace {

struct Total {
    qint64 messages = 0;
    QSet<int> users;
};

/// Sums of a run of days
struct Sums {
    QHash<QString, Total> totals;
    QHash<QString, int> userIds;
    qint64 totalMessages = 0;
    qint64 emoteMessages = 0;
};

/// Adds the hours of `day` that start in [`fromHour` * HOUR_MS, `to`)
void addDay(Sums &sums, qint64 day, const std::vector<QString> &files,
            qint64 fromHour, qint64 to)
{
    const auto dayStart = day * 24 * HOUR_MS;
    if (dayStart + (24 * HOUR_MS) <= fromHour * HOUR_MS || dayStart >= to)
    {
        return;
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
            if (parts[2] == TOTAL_KEY)
            {
                sums.totalMessages += it.value().count;
            }
            else if (parts[2] == ANY_EMOTE_KEY)
            {
                sums.emoteMessages += it.value().count;
            }
            continue;
        }
        auto &total =
            sums.totals[parts[1].toString() + u'\t' + parts[2].toString()];
        total.messages += it.value().count;
        const auto &users = it.value().users;
        for (auto user = users.cbegin(); user != users.cend(); ++user)
        {
            auto id = sums.userIds.find(user.key());
            if (id == sums.userIds.end())
            {
                id = sums.userIds.insert(user.key(),
                                         static_cast<int>(sums.userIds.size()));
            }
            total.users.insert(*id);
        }
    }
}

}  // namespace

namespace {

/// Every day with data in any source, with its files
std::map<qint64, std::vector<QString>> dayFiles(const QString &channel)
{
    const std::array<QString, 3> dirs{
        dataDir(channel, QStringLiteral("local")),
        dataDir(channel, QStringLiteral("server")),
        dataDir(channel, QStringLiteral("logs")),
    };
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
    return days;
}

/// How many messages `login` has in the chatters field `users` ("a:3,b:1"),
/// 0 if none or not counted per chatter
qint64 userCount(QByteArrayView users, QByteArrayView login)
{
    qsizetype from = 0;
    while (from < users.size())
    {
        auto end = users.indexOf(',', from);
        if (end < 0)
        {
            end = users.size();
        }
        const auto entry = users.sliced(from, end - from);
        if (entry.startsWith(login) && entry.size() > login.size() + 1 &&
            entry.at(login.size()) == ':')
        {
            return entry.sliced(login.size() + 1).toLongLong();
        }
        from = end + 1;
    }
    return 0;
}

}  // namespace

struct LoadCache {
    /// Which files and hours `sums` was added up from
    QString key;
    Sums sums;
};

std::shared_ptr<LoadCache> makeLoadCache()
{
    return std::make_shared<LoadCache>();
}

Stats load(const QString &channel, qint64 from, qint64 to, LoadCache *cache)
{
    // the counts flush() handed to the writer are on disk before reading
    writerPool().waitForDone();

    Stats stats;
    const auto key = channel.toLower();
    const auto days = dayFiles(key);
    if (!days.empty())
    {
        stats.firstMsecs = days.begin()->first * 24 * HOUR_MS;
    }

    const auto fromHour = from / HOUR_MS;
    const auto today = dayOf(QDateTime::currentMSecsSinceEpoch());

    // the days before today rarely change: add them up again only when one
    // of their files did
    Sums sums;
    {
        // `to` only matters to the past days when it lies before today
        QString pastKey = QString("%1 %2 %3 %4")
                              .arg(key)
                              .arg(fromHour)
                              .arg(std::min(to, today * 24 * HOUR_MS))
                              .arg(today);
        for (const auto &[day, files] : days)
        {
            if (day >= today)
            {
                break;
            }
            for (const auto &file : files)
            {
                const QFileInfo info(file);
                pastKey += QString("|%1 %2 %3")
                               .arg(file)
                               .arg(info.lastModified().toMSecsSinceEpoch())
                               .arg(info.size());
            }
        }
        if (cache != nullptr && cache->key == pastKey)
        {
            sums = cache->sums;
        }
        else
        {
            for (const auto &[day, files] : days)
            {
                if (day >= today)
                {
                    break;
                }
                addDay(sums, day, files, fromHour, to);
            }
            if (cache != nullptr)
            {
                cache->key = pastKey;
                cache->sums = sums;
            }
        }
    }
    for (auto it = days.lower_bound(today); it != days.end(); ++it)
    {
        addDay(sums, it->first, it->second, fromHour, to);
    }

    stats.totalMessages = sums.totalMessages;
    stats.emoteMessages = sums.emoteMessages;
    stats.rows.reserve(static_cast<size_t>(sums.totals.size()));
    for (auto it = sums.totals.cbegin(); it != sums.totals.cend(); ++it)
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

Stats loadUser(const QString &channel, const QString &login, qint64 from,
               qint64 to)
{
    writerPool().waitForDone();

    Stats stats;
    const auto key = channel.toLower();
    const auto user = login.toLower().toUtf8();
    if (user.isEmpty())
    {
        return stats;
    }
    const auto days = dayFiles(key);
    if (!days.empty())
    {
        stats.firstMsecs = days.begin()->first * 24 * HOUR_MS;
    }
    const auto fromHour = from / HOUR_MS;

    QHash<QString, qint64> totals;
    for (const auto &[day, files] : days)
    {
        const auto dayStart = day * 24 * HOUR_MS;
        if (dayStart + (24 * HOUR_MS) <= fromHour * HOUR_MS || dayStart >= to)
        {
            continue;
        }
        // the sources count the same messages: per hour and emote the
        // highest count wins
        QHash<QByteArray, qint64> best;
        for (const auto &path : files)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
            {
                continue;
            }
            const auto data = file.readAll();
            qsizetype start = 0;
            while (start < data.size())
            {
                auto end = data.indexOf('\n', start);
                if (end < 0)
                {
                    end = data.size();
                }
                auto line = QByteArrayView(data).sliced(start, end - start);
                start = end + 1;
                // files written on Windows end their lines with \r\n
                if (line.endsWith('\r'))
                {
                    line.chop(1);
                }
                if (!line.contains(user))
                {
                    continue;
                }
                // "HH source name messages chatters"
                const auto usersAt = line.lastIndexOf('\t');
                const auto countAt = line.first(std::max<qsizetype>(usersAt, 0))
                                         .lastIndexOf('\t');
                if (usersAt < 0 || countAt < 0)
                {
                    continue;
                }
                const auto count = userCount(line.sliced(usersAt + 1), user);
                if (count <= 0)
                {
                    continue;
                }
                const auto hourStart =
                    dayStart + (line.first(2).toInt() * HOUR_MS);
                if (hourStart < fromHour * HOUR_MS || hourStart >= to)
                {
                    continue;
                }
                auto &known = best[line.first(countAt).toByteArray()];
                known = std::max(known, count);
            }
        }
        for (auto it = best.cbegin(); it != best.cend(); ++it)
        {
            const auto parts = QString::fromUtf8(it.key()).split(u'\t');
            if (parts.size() != 3)
            {
                continue;
            }
            if (parts[1] == TOTAL_KEY)
            {
                if (parts[2] == TOTAL_KEY)
                {
                    stats.totalMessages += it.value();
                }
                else if (parts[2] == ANY_EMOTE_KEY)
                {
                    stats.emoteMessages += it.value();
                }
                continue;
            }
            totals[parts[1] + u'\t' + parts[2]] += it.value();
        }
    }

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
            .messages = it.value(),
            .chatters = 1,
        });
    }
    return stats;
}

bool logsImported(const QString &channel)
{
    const auto root = channelRoot(channel.toLower());
    if (root.isEmpty())
    {
        return false;
    }
    QFile file(QDir(root).filePath(QStringLiteral("import.json")));
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }
    // an import from before chatters were counted is done again
    return QJsonDocument::fromJson(file.readAll())
               .object()
               .value(u"version")
               .toInt() >= IMPORT_VERSION;
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
