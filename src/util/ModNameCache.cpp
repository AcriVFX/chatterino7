// Chatterino9: see ModNameCache.hpp

#include "util/ModNameCache.hpp"

#include "Application.hpp"
#include "singletons/Paths.hpp"

#include <QDir>
#include <QFile>
#include <QTextStream>

#include <cstdlib>
#include <mutex>
#include <vector>

namespace chatterino::modnames {

namespace {

struct Entry {
    qint64 msecs;
    QString user;
    QString moderator;
};

constexpr qint64 KEEP_MSECS = 3LL * 24 * 60 * 60 * 1000;
constexpr qint64 MATCH_MSECS = 15 * 1000;

std::mutex mutex;
bool loaded = false;
std::vector<Entry> entries;

QString filePath()
{
    return QDir(getApp()->getPaths().miscDirectory)
        .filePath(QStringLiteral("moderator-names.tsv"));
}

/// Reads the file once and rewrites it without entries older than 3 days
void loadLocked()
{
    if (loaded)
    {
        return;
    }
    loaded = true;

    const auto cutoff = QDateTime::currentMSecsSinceEpoch() - KEEP_MSECS;
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return;
    }
    bool dropped = false;
    QTextStream in(&file);
    while (!in.atEnd())
    {
        const auto parts = in.readLine().split(u'\t');
        bool ok = false;
        const auto msecs = parts.value(0).toLongLong(&ok);
        if (parts.size() != 3 || !ok || msecs < cutoff)
        {
            dropped = true;
            continue;
        }
        entries.push_back({msecs, parts[1], parts[2]});
    }
    file.close();

    if (dropped &&
        file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
        QTextStream out(&file);
        for (const auto &e : entries)
        {
            out << e.msecs << '\t' << e.user << '\t' << e.moderator << '\n';
        }
    }
}

}  // namespace

void remember(const QString &user, const QDateTime &time,
              const QString &moderator)
{
    if (user.isEmpty() || moderator.isEmpty() || !time.isValid())
    {
        return;
    }
    std::lock_guard lock(mutex);
    loadLocked();

    const auto msecs = time.toMSecsSinceEpoch();
    entries.push_back({msecs, user, moderator});

    QFile file(filePath());
    if (file.open(QIODevice::Append | QIODevice::Text))
    {
        QTextStream out(&file);
        out << msecs << '\t' << user << '\t' << moderator << '\n';
    }
}

QString lookup(const QString &user, const QDateTime &time)
{
    if (user.isEmpty() || !time.isValid())
    {
        return {};
    }
    std::lock_guard lock(mutex);
    loadLocked();

    const auto msecs = time.toMSecsSinceEpoch();
    QString best;
    qint64 bestDiff = MATCH_MSECS + 1;
    for (const auto &e : entries)
    {
        const auto diff = std::abs(e.msecs - msecs);
        if (diff < bestDiff && e.user.compare(user, Qt::CaseInsensitive) == 0)
        {
            best = e.moderator;
            bestDiff = diff;
        }
    }
    return best;
}

}  // namespace chatterino::modnames
