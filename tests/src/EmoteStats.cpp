#include "util/EmoteStats.hpp"

#include "mocks/BaseApplication.hpp"
#include "Test.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTimeZone>

#include <algorithm>

using namespace chatterino;
using emotestats::Source;

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
    {
        this->paths_.miscDirectory =
            QDir(this->settingsDir.path()).filePath("Misc");
    }
};

qint64 at(int hour, int minute)
{
    return QDateTime(QDate(2026, 10, 13), QTime(hour, minute), QTimeZone::utc())
        .toMSecsSinceEpoch();
}

QDateTime timeAt(int hour, int minute)
{
    return QDateTime::fromMSecsSinceEpoch(at(hour, minute), QTimeZone::utc());
}

const emotestats::Row *findRow(const emotestats::Stats &stats, Source source,
                               const QString &name)
{
    auto it = std::ranges::find_if(stats.rows, [&](const auto &row) {
        return row.source == source && row.name == name;
    });
    return it == stats.rows.end() ? nullptr : &*it;
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(data);
}

}  // namespace

TEST(EmoteStats, CountsEachMessageOncePerEmote)
{
    MockApplication app;

    emotestats::recordMessage("statsone", "id1", "UserOne", timeAt(18, 0),
                              {
                                  {Source::SevenTv, "omE"},
                                  {Source::SevenTv, "omE"},
                                  {Source::Twitch, "Kappa"},
                              });
    // the same message again (e.g. built for another split)
    emotestats::recordMessage("statsone", "id1", "userone", timeAt(18, 0),
                              {{Source::SevenTv, "omE"}});
    emotestats::recordMessage("statsone", "id2", "usertwo", timeAt(18, 5),
                              {{Source::SevenTv, "omE"}});
    emotestats::recordMessage("statsone", "id3", "userone", timeAt(19, 5), {});
    emotestats::flush();

    auto stats = emotestats::load("statsone", at(0, 0), at(23, 0));
    EXPECT_EQ(stats.totalMessages, 3);
    ASSERT_EQ(stats.rows.size(), 2);
    const auto *ome = findRow(stats, Source::SevenTv, "omE");
    ASSERT_NE(ome, nullptr);
    EXPECT_EQ(ome->messages, 2);
    EXPECT_EQ(ome->chatters, 2);
    const auto *kappa = findRow(stats, Source::Twitch, "Kappa");
    ASSERT_NE(kappa, nullptr);
    EXPECT_EQ(kappa->messages, 1);
    EXPECT_EQ(kappa->chatters, 1);
    EXPECT_EQ(stats.firstMsecs, at(0, 0));

    // only the hours from 19:00
    stats = emotestats::load("statsone", at(19, 0), at(23, 0));
    EXPECT_EQ(stats.totalMessages, 1);
    EXPECT_TRUE(stats.rows.empty());
}

TEST(EmoteStats, MergesServerCountsWithLocalOnes)
{
    MockApplication app;

    emotestats::recordMessage("statstwo", "a", "userone", timeAt(18, 0),
                              {{Source::SevenTv, "omE"}});
    emotestats::recordMessage("statstwo", "b", "usertwo", timeAt(18, 1),
                              {{Source::SevenTv, "omE"}});
    emotestats::flush();

    // the server saw the same hour while this PC was off for a while
    writeFile(QDir(app.paths_.miscDirectory)
                  .filePath("EmoteStats/statstwo/server/2026-10-13.tsv"),
              "18\t*\t*\t9\t\n"
              "18\t7tv\tomE\t5\tuserthree,userone\n"
              "20\tbttv\tClap\t1\tuserfour\n");

    auto stats = emotestats::load("statstwo", at(0, 0), at(23, 0));
    EXPECT_EQ(stats.totalMessages, 9);
    const auto *ome = findRow(stats, Source::SevenTv, "omE");
    ASSERT_NE(ome, nullptr);
    EXPECT_EQ(ome->messages, 5);
    EXPECT_EQ(ome->chatters, 3);
    const auto *clap = findRow(stats, Source::Bttv, "Clap");
    ASSERT_NE(clap, nullptr);
    EXPECT_EQ(clap->messages, 1);
}

TEST(EmoteStats, ReusesPastDaysButRereadsToday)
{
    MockApplication app;

    const auto dir =
        QDir(app.paths_.miscDirectory).filePath("EmoteStats/statscache/server");
    const auto pastFile = QDir(dir).filePath("2026-10-13.tsv");
    const auto todayFile = QDir(dir).filePath(
        QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate) + ".tsv");
    writeFile(pastFile, "18\t7tv\tomE\t5\tuserone\n");
    writeFile(todayFile, "00\t7tv\tomE\t1\tusertwo\n");

    auto cache = emotestats::makeLoadCache();
    auto load = [&] {
        return emotestats::load(
            "statscache", 0,
            QDateTime::currentMSecsSinceEpoch() + (24LL * 3600000),
            cache.get());
    };
    auto stats = load();
    const auto *ome = findRow(stats, Source::SevenTv, "omE");
    ASSERT_NE(ome, nullptr);
    EXPECT_EQ(ome->messages, 6);
    EXPECT_EQ(ome->chatters, 2);

    // today is read again on every load
    writeFile(todayFile, "00\t7tv\tomE\t3\tusertwo,userthree\n");
    stats = load();
    ome = findRow(stats, Source::SevenTv, "omE");
    ASSERT_NE(ome, nullptr);
    EXPECT_EQ(ome->messages, 8);
    EXPECT_EQ(ome->chatters, 3);

    // a past day is read again once its file changed
    writeFile(pastFile, "18\t7tv\tomE\t7\tuserone,userfour\n");
    stats = load();
    ome = findRow(stats, Source::SevenTv, "omE");
    ASSERT_NE(ome, nullptr);
    EXPECT_EQ(ome->messages, 10);
    EXPECT_EQ(ome->chatters, 4);
}

TEST(EmoteStats, ImportsChatLogs)
{
    MockApplication app;

    const auto logDir =
        QDir(app.settingsDir.path()).filePath("Logs/Twitch/Channels/statslog");
    writeFile(QDir(logDir).filePath("statslog-2026-10-13.log"),
              "# Start logging at 2026-10-13 18:00:00 CEST\n"
              "[18:00:01] userone: omE omE hallo\n"
              "[18:00:02] Display usertwo: Kappa omE\n"
              "[18:00:03] usertwo has been timed out for 10s.\n"
              "[18:00:04] userthree: nothing here\n");
    // not a log of this channel
    writeFile(QDir(logDir).filePath("otherchannel-2026-10-13.log"),
              "[18:00:01] userone: omE\n");

    emotestats::ImportNames names;
    names.twitch = {"Kappa"};
    names.seventvChannel = {"omE"};

    ASSERT_FALSE(emotestats::logsImported("statslog"));
    emotestats::importLogs("statslog", logDir, names);

    QElapsedTimer timer;
    timer.start();
    while ((emotestats::importRunning("statslog") ||
            !emotestats::logsImported("statslog")) &&
           timer.elapsed() < 10000)
    {
        QCoreApplication::processEvents();
    }
    ASSERT_TRUE(emotestats::logsImported("statslog"));

    // the log's times are local, so read every hour
    auto stats =
        emotestats::load("statslog", 0, at(23, 0) + (7LL * 24 * 3600000));
    EXPECT_EQ(stats.totalMessages, 3);
    const auto *ome = findRow(stats, Source::SevenTv, "omE");
    ASSERT_NE(ome, nullptr);
    EXPECT_EQ(ome->messages, 2);
    EXPECT_EQ(ome->chatters, 2);
    const auto *kappa = findRow(stats, Source::Twitch, "Kappa");
    ASSERT_NE(kappa, nullptr);
    EXPECT_EQ(kappa->messages, 1);
}
