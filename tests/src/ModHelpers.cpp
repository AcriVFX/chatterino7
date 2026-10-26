#include "util/ModHelpers.hpp"

#include "common/Literals.hpp"
#include "Test.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace chatterino;
using namespace literals;
using modhelpers::countingNumber;
using modhelpers::readLogTimeouts;

namespace {

void writeLog(const QTemporaryDir &dir, const QString &day,
              const QByteArray &content)
{
    QFile file(QDir(dir.path()).filePath("chan-" + day + ".log"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(content);
}

QDateTime now()
{
    return {QDate(2026, 10, 18), QTime(12, 0)};
}

}  // namespace

TEST(ModHelpers, UnbanOnTheNextDayDropsTheBan)
{
    QTemporaryDir dir;
    writeLog(dir, "2026-10-17",
             "[23:50:00] spammer has been permanently banned.\r\n");
    writeLog(dir, "2026-10-18", "[09:00:00] modx: modx unbanned spammer.\r\n");

    auto out = readLogTimeouts(dir.path(), "chan", "spammer", now(), 7);
    ASSERT_TRUE(out.empty());
}

TEST(ModHelpers, UnbanDropsTheBanBeforeALaterTimeout)
{
    QTemporaryDir dir;
    writeLog(dir, "2026-10-18",
             "[08:00:00] spammer has been permanently banned.\r\n"
             "[08:00:01] spammer has been timed out for 1s.\r\n"
             "[09:00:00] modx: modx unbanned spammer.\r\n");

    auto out = readLogTimeouts(dir.path(), "chan", "spammer", now(), 7);
    ASSERT_EQ(out.size(), 1U);
    ASSERT_FALSE(out[0].isBan());
}

TEST(ModHelpers, BotTimeoutAfterMidnight)
{
    QTemporaryDir dir;
    writeLog(dir, "2026-10-17", "[23:59:59] spammer: buy followers\r\n");
    writeLog(dir, "2026-10-18",
             "[00:00:01] spammer has been timed out for 10m.\r\n");

    auto out = readLogTimeouts(dir.path(), "chan", "spammer", now(), 7);
    ASSERT_EQ(out.size(), 1U);
    ASSERT_TRUE(out[0].automatic);
    ASSERT_FALSE(out[0].countsAsModAction());
}

TEST(ModHelpers, EventSubLines)
{
    QTemporaryDir dir;
    writeLog(dir, "2026-10-18",
             "[10:00:00] spammer: hi\r\n"
             "[10:05:00] modx: modx timed out spammer for 10m: be nice\r\n"
             // the IRC line of the same timeout
             "[10:05:01] spammer has been timed out for 10m.\r\n"
             // shared chat, another channel
             "[10:20:00] modx: modx timed out spammer for 10m in other: x\r\n"
             // a chat message can't fake one
             "[10:30:00] troll: modx: modx timed out spammer for 1d\r\n"
             "[11:00:00] modx: modx banned spammer: bye\r\n");

    auto out = readLogTimeouts(dir.path(), "chan", "spammer", now(), 7);
    ASSERT_EQ(out.size(), 2U);
    ASSERT_EQ(out[0].seconds, 600);
    ASSERT_TRUE(out[0].countsAsModAction());
    ASSERT_TRUE(out[1].isBan());
}

TEST(ModHelpers, LocalizedModUnban)
{
    QTemporaryDir dir;
    writeLog(dir, "2026-10-18",
             "[08:00:00] spammer has been permanently banned.\r\n"
             "[09:00:00] 모드 somemod: somemod unbanned spammer.\r\n");

    auto out = readLogTimeouts(dir.path(), "chan", "spammer", now(), 7);
    ASSERT_TRUE(out.empty());
}

TEST(ModHelpers, CountingNumber)
{
    EXPECT_EQ(countingNumber(u"3"_s), 3);
    EXPECT_EQ(countingNumber(u" 14 "_s), 14);
    EXPECT_EQ(countingNumber(u"12."_s), 12);
    EXPECT_EQ(countingNumber(u"#3"_s), 3);
    // Chatterino's duplicate bypass character
    EXPECT_EQ(countingNumber(u"7 \U000E0000"_s), 7);
    EXPECT_FALSE(countingNumber(u"8,5"_s));
    EXPECT_FALSE(countingNumber(u"3/10"_s));
    EXPECT_FALSE(countingNumber(u"3 Kappa"_s));
    EXPECT_FALSE(countingNumber(u"12345"_s));
    EXPECT_FALSE(countingNumber(u"W"_s));
}
