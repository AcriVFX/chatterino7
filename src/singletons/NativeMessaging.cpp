// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/NativeMessaging.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/Modes.hpp"
#include "common/QLogging.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/IpcQueue.hpp"
#include "util/PostToThread.hpp"
#include "util/WhisperHistory.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStringBuilder>
#include <QTextStream>

#include <deque>

#ifdef Q_OS_WIN
#    include "widgets/AttachedWindow.hpp"
#endif

namespace {

using namespace chatterino::nm::detail;
using namespace chatterino;
using namespace chatterino::literals;

const QString EXTENSION_ID = u"glknmaideaikkmemifbfkhnomoknepka"_s;

struct Config {
#ifdef Q_OS_WIN
    QString fileName;
    QString registryKey;
#else
    QString browserDirectory;
    QString nmDirectory;
#endif
};

const Config FIREFOX{
#ifdef Q_OS_WIN
    .fileName = u"native-messaging-manifest-firefox.json"_s,
    .registryKey =
        u"HKCU\\Software\\Mozilla\\NativeMessagingHosts\\com.chatterino.chatterino"_s,
#elif defined(Q_OS_MACOS)
    .browserDirectory = u"~/Library/Application Support/Mozilla"_s,
    .nmDirectory = u"NativeMessagingHosts"_s,
#else
    .browserDirectory = u"~/.mozilla"_s,
    .nmDirectory = u"native-messaging-hosts"_s,
#endif
};

const Config CHROME{
#ifdef Q_OS_WIN
    .fileName = u"native-messaging-manifest-chrome.json"_s,
    .registryKey =
        u"HKCU\\Software\\Google\\Chrome\\NativeMessagingHosts\\com.chatterino.chatterino"_s,
#elif defined(Q_OS_MACOS)
    .browserDirectory = u"~/Library/Application Support/Google/Chrome/"_s,
    .nmDirectory = u"NativeMessagingHosts"_s,
#else
    .browserDirectory = u"~/.config/google-chrome"_s,
    .nmDirectory = u"NativeMessagingHosts"_s,
#endif
};

#ifdef Q_OS_WIN
const Config WHISPERS_CHROME{
    .fileName = u"native-messaging-manifest-chatterino9-whispers.json"_s,
    .registryKey =
        u"HKCU\\Software\\Google\\Chrome\\NativeMessagingHosts\\com.chatterino9.whispers"_s,
};

const Config WHISPERS_EDGE{
    .fileName = u"native-messaging-manifest-chatterino9-whispers.json"_s,
    .registryKey =
        u"HKCU\\Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.chatterino9.whispers"_s,
};
#endif

void registerNmManifest([[maybe_unused]] const Paths &paths,
                        const Config &config, const QJsonDocument &document)
{
#ifdef Q_OS_WIN
    std::ignore =
        writeManifestTo(paths.miscDirectory, u"."_s, config.fileName, document);

    QSettings registry(config.registryKey, QSettings::NativeFormat);
    registry.setValue("Default",
                      QString(paths.miscDirectory % u'/' % config.fileName));
#else
    std::ignore =
        writeManifestTo(config.browserDirectory, config.nmDirectory,
                        u"com.chatterino.chatterino.json"_s, document);
#endif
}

QString receiptsPath(const Paths &paths)
{
    return paths.miscDirectory % u"/Whispers/browser-receipts.tsv";
}

constexpr qint64 RECEIPT_MAX_AGE = 31LL * 24 * 60 * 60 * 1000;

/// A whisper sent on twitch.tv, from the whisper extension
struct BrowserWhisper {
    QString id;
    QString recipientId;
    QString text;
    qint64 sentAt = 0;
};

/// Who whispers from the browser went to (they only carry a user id).
/// GUI thread only.
struct RecipientLookups {
    /// user id -> login
    QHash<QString, QString> logins;
    /// user id -> whispers waiting for its lookup
    QHash<QString, std::vector<BrowserWhisper>> waiting;
    /// user id -> when its last lookup failed
    QHash<QString, qint64> failedAt;
    QHash<QString, int> failures;
    /// when the lookups of the last minute started
    std::deque<qint64> recent;
};

RecipientLookups &lookups()
{
    static RecipientLookups instance;
    return instance;
}

void saveBrowserWhisper(const BrowserWhisper &whisper, const QString &login)
{
    if (whisperhistory::recordSentElsewhere(login, whisper.text,
                                            whisper.sentAt))
    {
        nm::whispers::addReceipt(getApp()->getPaths(), whisper.id, true);
    }
    // otherwise nobody is signed in: the extension sends it again later
}

/// Saves a whisper sent on twitch.tv once it's known who it went to. Anything
/// not saved gets no receipt, so the extension keeps it and sends it again.
void handleBrowserWhisper(const BrowserWhisper &whisper)
{
    assertInGuiThread();

    auto &st = lookups();
    const auto &recipientId = whisper.recipientId;
    auto known = st.logins.constFind(recipientId);
    if (known != st.logins.cend())
    {
        saveBrowserWhisper(whisper, *known);
        return;
    }
    auto pending = st.waiting.find(recipientId);
    if (pending != st.waiting.end())
    {
        if (pending->size() < 100)
        {
            pending->push_back(whisper);
        }
        return;
    }

    // Lookups use the account's Helix budget, which bans and timeouts need
    // too: don't retry a failing id for 10 minutes, at most 20 per minute.
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (st.failedAt.value(recipientId, 0) > now - 10 * 60 * 1000)
    {
        return;
    }
    while (!st.recent.empty() && st.recent.front() < now - 60 * 1000)
    {
        st.recent.pop_front();
    }
    if (st.recent.size() >= 20)
    {
        return;
    }
    st.recent.push_back(now);
    st.waiting[recipientId].push_back(whisper);

    getHelix()->getUserById(
        recipientId,
        [recipientId](const HelixUser &user) {
            auto &st = lookups();
            const auto login = user.login.toLower();
            const auto whispers = st.waiting.take(recipientId);
            st.logins.insert(recipientId, login);
            st.failures.remove(recipientId);
            for (const auto &w : whispers)
            {
                saveBrowserWhisper(w, login);
            }
        },
        [recipientId] {
            auto &st = lookups();
            const auto whispers = st.waiting.take(recipientId);
            st.failedAt[recipientId] = QDateTime::currentMSecsSinceEpoch();
            // failing for an hour: no such user (or never will be)
            if (++st.failures[recipientId] < 6)
            {
                return;
            }
            qCWarning(chatterinoNativeMessage)
                << "Couldn't look up whisper recipient" << recipientId;
            for (const auto &w : whispers)
            {
                nm::whispers::addReceipt(getApp()->getPaths(), w.id, false);
            }
        });
}

}  // namespace

namespace chatterino::nm::detail {

Expected<void, WriteManifestError> writeManifestTo(QString directory,
                                                   const QString &nmDirectory,
                                                   const QString &filename,
                                                   const QJsonDocument &json)
{
    if (directory.startsWith('~'))
    {
        directory = QDir::homePath() % QStringView{directory}.sliced(1);
    }

    QDir dir(directory);
    if (!dir.exists(nmDirectory) && !dir.mkdir(nmDirectory))
    {
        qCWarning(chatterinoNativeMessage)
            << "Failed to create" << nmDirectory << "in" << directory;
        return makeUnexpected(WriteManifestError::FailedToCreateDirectory);
    }
    dir.cd(nmDirectory);

    QFile file(dir.filePath(filename));
    if (!file.open(QFile::WriteOnly | QFile::Truncate))
    {
        qCWarning(chatterinoNativeMessage)
            << "Failed to open" << filename << "in" << directory;
        return makeUnexpected(WriteManifestError::FailedToCreateFile);
    }
    file.write(json.toJson());

    return {};
}

}  // namespace chatterino::nm::detail

namespace chatterino {

using namespace chatterino::nm::detail;
using namespace literals;

void registerNmHost(const Paths &paths)
{
    if (Modes::instance().isPortable)
    {
        return;
    }

    auto getBaseDocument = [] {
        return QJsonObject{
            {u"name"_s, "com.chatterino.chatterino"_L1},
            {u"description"_s, "Browser interaction with chatterino."_L1},
            {u"path"_s, QCoreApplication::applicationFilePath()},
            {u"type"_s, "stdio"_L1},
        };
    };

    QStringList extensionIDs =
        getSettings()->additionalExtensionIDs.getValue().split(
            ';', Qt::SkipEmptyParts);

    // chrome
    {
        auto obj = getBaseDocument();
        QJsonArray allowedOriginsArr = {
            u"chrome-extension://%1/"_s.arg(EXTENSION_ID)};

        for (const auto &id : extensionIDs)
        {
            QString trimmedID = id.trimmed();
            if (!trimmedID.isEmpty())
            {
                allowedOriginsArr.append(
                    u"chrome-extension://%1/"_s.arg(trimmedID));
            }
        }

        obj.insert("allowed_origins", allowedOriginsArr);

        registerNmManifest(paths, CHROME, QJsonDocument{obj});
    }

    // firefox
    {
        auto obj = getBaseDocument();
        QJsonArray allowedExtensions = {"chatterino_native@chatterino.com"};

        for (const auto &id : extensionIDs)
        {
            QString trimmedID = id.trimmed();
            if (!trimmedID.isEmpty())
            {
                allowedExtensions.append(trimmedID);
            }
        }

        obj.insert("allowed_extensions", allowedExtensions);

        registerNmManifest(paths, FIREFOX, QJsonDocument{obj});
    }

#ifdef Q_OS_WIN
    // Chatterino9's whisper extension (Edge, Chrome)
    {
        auto obj = getBaseDocument();
        obj.insert(u"name"_s, "com.chatterino9.whispers"_L1);
        obj.insert(u"description"_s,
                   "Whispers sent on twitch.tv, for Chatterino9."_L1);
        obj.insert(
            u"allowed_origins"_s,
            QJsonArray{QString::fromLatin1(nm::whispers::EXTENSION_ORIGIN)});

        registerNmManifest(paths, WHISPERS_CHROME, QJsonDocument{obj});
        registerNmManifest(paths, WHISPERS_EDGE, QJsonDocument{obj});
    }
#endif
}

std::string &getNmQueueName(const Paths &paths)
{
    static std::string name =
        "chatterino_gui" + paths.applicationFilePathHash.toStdString();
    return name;
}

namespace nm::whispers {

void addReceipt(const Paths &paths, const QString &id, bool saved)
{
    const auto path = receiptsPath(paths);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        return;
    }
    const auto now = QDateTime::currentMSecsSinceEpoch();

    // drop receipts older than a month now and then
    if (QFileInfo(path).size() > 64 * 1024)
    {
        QStringList kept;
        QFile old(path);
        if (old.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            QTextStream in(&old);
            while (!in.atEnd())
            {
                const auto line = in.readLine();
                const auto parts = line.split(u'\t');
                if (parts.size() == 3 &&
                    parts[2].toLongLong() > now - RECEIPT_MAX_AGE)
                {
                    kept.append(line);
                }
            }
        }
        old.close();
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream out(&file);
            for (const auto &line : kept)
            {
                out << line << '\n';
            }
            out.flush();
            file.commit();
        }
    }

    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text))
    {
        QTextStream out(&file);
        out << id << '\t' << (saved ? '1' : '0') << '\t' << now << '\n';
    }
}

std::map<QString, bool> readReceipts(const Paths &paths)
{
    std::map<QString, bool> receipts;
    QFile file(receiptsPath(paths));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return receipts;
    }
    const auto cutoff = QDateTime::currentMSecsSinceEpoch() - RECEIPT_MAX_AGE;
    QTextStream in(&file);
    while (!in.atEnd())
    {
        // a line still being written has fewer parts
        const auto parts = in.readLine().split(u'\t');
        if (parts.size() == 3 && parts[2].toLongLong() > cutoff)
        {
            receipts[parts[0]] = parts[1] == u"1";
        }
    }
    return receipts;
}

}  // namespace nm::whispers

// CLIENT

namespace nm::client {

bool sendMessage(const QByteArray &array)
{
    return ipc::sendMessage("chatterino_gui", array);
}

void writeToCout(const QByteArray &array)
{
    const auto *data = array.data();
    auto size = uint32_t(array.size());

    // We're writing the raw bytes to cout.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    std::cout.write(reinterpret_cast<char *>(&size), 4);
    std::cout.write(data, size);
    std::cout.flush();
}

}  // namespace nm::client

// SERVER
NativeMessagingServer::NativeMessagingServer()
    : thread(new ReceiverThread(*this))
{
    this->thread->setObjectName("C2NMReceiver");
}

NativeMessagingServer::~NativeMessagingServer()
{
    if (!ipc::IpcQueue::remove("chatterino_gui"))
    {
        qCWarning(chatterinoNativeMessage) << "Failed to remove message queue";
    }
    this->thread->requestInterruption();
    this->thread->quit();
    // Most likely, the receiver thread will still wait for a message
    if (!this->thread->wait(100))
    {
        this->thread->terminate();

        if (!this->thread->wait(100))
        {
            qCWarning(chatterinoNativeMessage)
                << "Failed to terminate thread cleanly";
        }
    }
}

void NativeMessagingServer::start()
{
    this->thread->start();
}

NativeMessagingServer::ReceiverThread::ReceiverThread(
    NativeMessagingServer &parent)
    : parent_(parent)
{
}

void NativeMessagingServer::ReceiverThread::run()
{
    auto [messageQueue, error] = ipc::IpcQueue::tryReplaceOrCreate(
        "chatterino_gui", 100, nm::MAX_MESSAGE_SIZE);

    if (!error.isEmpty())
    {
        qCDebug(chatterinoNativeMessage)
            << "Failed to create message queue:" << error;

        nmIpcError().set(error);
        return;
    }

    while (!this->isInterruptionRequested())
    {
        auto buf = messageQueue->receive();
        if (buf.isEmpty())
        {
            continue;
        }
        auto document = QJsonDocument::fromJson(buf);

        this->handleMessage(document.object());
    }
}

void NativeMessagingServer::ReceiverThread::handleMessage(
    const QJsonObject &root)
{
    QString action = root["action"_L1].toString();

    if (action == "select")
    {
        this->handleSelect(root);
        return;
    }
    if (action == "detach")
    {
        this->handleDetach(root);
        return;
    }
    if (action == "sync")
    {
        this->handleSync(root);
        return;
    }
    if (action == "chatterino9-whisper-sent")
    {
        this->handleWhisperSent(root);
        return;
    }

    qCDebug(chatterinoNativeMessage) << "NM unknown action" << action;
}

// NOLINTBEGIN(readability-convert-member-functions-to-static)
void NativeMessagingServer::ReceiverThread::handleSelect(
    const QJsonObject &root)
{
    QString type = root["type"_L1].toString();
    bool attach = root["attach"_L1].toBool();
    bool attachFullscreen = root["attach_fullscreen"_L1].toBool();
    QString name = root["name"_L1].toString();

#ifdef USEWINSDK
    const auto sizeObject = root["size"_L1].toObject();
    AttachedWindow::GetArgs args = {
        .winId = root["winId"_L1].toString(),
        .yOffset = root["yOffset"_L1].toInt(-1),
        .x = sizeObject["x"_L1].toDouble(-1.0),
        .pixelRatio = sizeObject["pixelRatio"_L1].toDouble(-1.0),
        .width = sizeObject["width"_L1].toInt(-1),
        .height = sizeObject["height"_L1].toInt(-1),
        .fullscreen = attachFullscreen,
    };

    qCDebug(chatterinoNativeMessage)
        << args.x << args.pixelRatio << args.width << args.height << args.winId;

    if (args.winId.isNull())
    {
        qCDebug(chatterinoNativeMessage) << "winId in select is missing";
        return;
    }
#endif

    if (type != u"twitch"_s)
    {
        qCDebug(chatterinoNativeMessage) << "NM unknown channel type";
        return;
    }

    postToThread([=] {
        if (!name.isEmpty())
        {
            auto channel = getApp()->getTwitch()->getOrAddChannel(name);
            if (getApp()->getTwitch()->getWatchingChannel().get() != channel)
            {
                getApp()->getTwitch()->setWatchingChannel(channel);
            }
        }

        if (attach || attachFullscreen)
        {
#ifdef USEWINSDK
            auto *window = AttachedWindow::getForeground(args);
            if (!name.isEmpty())
            {
                window->setChannel(
                    getApp()->getTwitch()->getOrAddChannel(name));
            }
#endif
        }
    });
}

void NativeMessagingServer::ReceiverThread::handleDetach(
    const QJsonObject &root)
{
    QString winId = root["winId"_L1].toString();

    if (winId.isNull())
    {
        qCDebug(chatterinoNativeMessage) << "NM winId missing";
        return;
    }

#ifdef USEWINSDK
    postToThread([winId] {
        qCDebug(chatterinoNativeMessage) << "NW detach";
        AttachedWindow::detach(winId);
    });
#endif
}
// NOLINTEND(readability-convert-member-functions-to-static)

void NativeMessagingServer::ReceiverThread::handleSync(const QJsonObject &root)
{
    // Structure:
    // { action: 'sync', twitchChannels?: string[] }
    postToThread([&parent = this->parent_,
                  twitch = root["twitchChannels"_L1].toArray()] {
        parent.syncChannels(twitch);
    });
}

void NativeMessagingServer::ReceiverThread::handleWhisperSent(
    const QJsonObject &root)
{
    // Structure (from tools/whisper-extension, through the browser host, which
    // only passes these on from that extension):
    // { action: 'chatterino9-whisper-sent', id: string, recipientId: string,
    //   message: string, sentAt: number (msecs) }
    static const QRegularExpression userIdRe(u"^[0-9]{1,20}$"_s);
    BrowserWhisper whisper{
        .id = root["id"_L1].toString(),
        .recipientId = root["recipientId"_L1].toString(),
        .text = root["message"_L1].toString().trimmed(),
    };
    if (whisper.id.isEmpty() || whisper.id.size() > 100)
    {
        return;
    }
    // nothing from before 2020 or from the future (NaN fails too)
    const double sentAt = root["sentAt"_L1].toDouble(-1);
    const auto now = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
    const bool valid = userIdRe.match(whisper.recipientId).hasMatch() &&
                       !whisper.text.isEmpty() && whisper.text.size() <= 1000 &&
                       sentAt >= 1577836800000.0 && sentAt <= now + 60000.0;
    if (valid)
    {
        whisper.sentAt = static_cast<qint64>(sentAt);
    }

    postToThread([whisper = std::move(whisper), valid] {
        if (valid)
        {
            handleBrowserWhisper(whisper);
        }
        else
        {
            qCDebug(chatterinoNativeMessage) << "NM whisper-sent is malformed";
            nm::whispers::addReceipt(getApp()->getPaths(), whisper.id, false);
        }
    });
}

void NativeMessagingServer::syncChannels(const QJsonArray &twitchChannels)
{
    assertInGuiThread();

    std::vector<ChannelPtr> updated;
    updated.reserve(twitchChannels.size());
    for (const auto &value : twitchChannels)
    {
        auto name = value.toString();
        if (name.isEmpty())
        {
            continue;
        }
        // the deduping is done on the extension side
        updated.emplace_back(getApp()->getTwitch()->getOrAddChannel(name));
    }

    // This will destroy channels that aren't used anymore.
    this->channelWarmer_ = std::move(updated);
}

Atomic<std::optional<QString>> &nmIpcError()
{
    static Atomic<std::optional<QString>> x;
    return x;
}

}  // namespace chatterino
