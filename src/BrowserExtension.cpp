// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "BrowserExtension.hpp"

#include "singletons/NativeMessaging.hpp"
#include "util/RenameThread.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#ifdef Q_OS_WIN
#    include <fcntl.h>
#    include <io.h>

#    include <cstdio>

#endif

namespace {

using namespace chatterino;

/// The ping check and the whisper receipts write from other threads
std::mutex &outputMutex()
{
    static std::mutex mutex;
    return mutex;
}

void initFileMode()
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

void writeToBrowser(const char *data, uint32_t len)
{
    std::lock_guard lock(outputMutex());
    std::cout.write(reinterpret_cast<const char *>(&len), sizeof(len));
    std::cout.write(data, len);
    std::cout.flush();
}

// TODO(Qt6): Use QUtf8String
void sendToBrowser(QLatin1String str)
{
    writeToBrowser(str.data(), static_cast<uint32_t>(str.size()));
}

void sendToBrowser(const QByteArray &json)
{
    writeToBrowser(json.data(), static_cast<uint32_t>(json.size()));
}

QByteArray receiveFromBrowser()
{
    uint32_t size = 0;
    std::cin.read(reinterpret_cast<char *>(&size), sizeof(size));

    if (std::cin.eof())
    {
        return {};
    }

    QByteArray buffer{static_cast<QByteArray::size_type>(size),
                      Qt::Uninitialized};
    std::cin.read(buffer.data(), size);

    return buffer;
}

void sendAck(const QString &id, bool ok, const char *reason = nullptr)
{
    QJsonObject ack{
        {"type", "ack"},
        {"id", id},
        {"ok", ok},
    };
    if (reason != nullptr)
    {
        ack.insert("reason", reason);
    }
    sendToBrowser(QJsonDocument(ack).toJson(QJsonDocument::Compact));
}

/// Chatterino9's whisper extension waits for an answer to each whisper it
/// passes on, so it keeps every whisper the app hasn't saved yet. The app
/// notes each saved (or refused) whisper in a receipts file, which this
/// checks for a few seconds after passing whispers on.
class WhisperReceipts
{
public:
    explicit WhisperReceipts(const Paths &paths)
        : paths_(paths)
    {
    }

    void wait(const QString &id)
    {
        std::lock_guard lock(this->mutex_);
        this->waiting_.emplace(id, QDateTime::currentMSecsSinceEpoch());
        if (!this->started_)
        {
            this->started_ = true;
            auto thread = std::thread([this] {
                this->run();
            });
            renameThread(thread, "WhisperReceipts");
            thread.detach();
        }
    }

private:
    void run()
    {
        using namespace std::chrono_literals;
        while (true)
        {
            std::this_thread::sleep_for(150ms);
            std::map<QString, qint64> waiting;
            {
                std::lock_guard lock(this->mutex_);
                waiting = this->waiting_;
            }
            if (waiting.empty())
            {
                continue;
            }
            const auto receipts = nm::whispers::readReceipts(this->paths_);
            const auto now = QDateTime::currentMSecsSinceEpoch();
            for (const auto &[id, since] : waiting)
            {
                auto receipt = receipts.find(id);
                if (receipt != receipts.end())
                {
                    if (receipt->second)
                    {
                        sendAck(id, true);
                    }
                    else
                    {
                        sendAck(id, false, "rejected");
                    }
                }
                else if (now - since > 5000)
                {
                    // e.g. the app is still looking up who it went to:
                    // the extension sends it again later
                    sendAck(id, false, "pending");
                }
                else
                {
                    continue;
                }
                std::lock_guard lock(this->mutex_);
                this->waiting_.erase(id);
            }
        }
    }

    const Paths &paths_;
    std::mutex mutex_;
    std::map<QString, qint64> waiting_;
    bool started_ = false;
};

/// Handles a message from Chatterino9's whisper extension. Returns false if
/// it isn't one.
bool handleWhisperMessage(const QByteArray &buffer, const QString &origin,
                          WhisperReceipts &receipts)
{
    if (!buffer.contains("chatterino9-"))
    {
        return false;
    }
    const auto root = QJsonDocument::fromJson(buffer).object();
    if (!root["action"].toString().startsWith("chatterino9-"))
    {
        return false;
    }
    const auto id = root["id"].toString();
    // only the whisper extension itself may add whispers
    if (origin != nm::whispers::EXTENSION_ORIGIN || id.isEmpty() ||
        id.size() > 100)
    {
        return true;
    }
    if (static_cast<size_t>(buffer.size()) > nm::MAX_MESSAGE_SIZE)
    {
        sendAck(id, false, "too-long");
        return true;
    }
    if (!nm::client::sendMessage(buffer))
    {
        sendAck(id, false, "not-running");
        return true;
    }
    receipts.wait(id);
    return true;
}

void runLoop(const Paths &paths, const QString &origin)
{
    auto receivedMessage = std::make_shared<std::atomic_bool>(true);

    auto thread = std::thread([=]() {
        while (true)
        {
            using namespace std::chrono_literals;
            if (!receivedMessage->exchange(false))
            {
                sendToBrowser(QLatin1String{
                    R"({"type":"status","status":"exiting-host","reason":"no message was received in 10s"})"});
                _Exit(1);
            }
            std::this_thread::sleep_for(10s);
        }
    });
    renameThread(thread, "BrowserPingCheck");

    // lives until _Exit, its thread keeps using it
    auto *receipts = new WhisperReceipts(paths);

    while (true)
    {
        auto buffer = receiveFromBrowser();
        if (buffer.isNull())
        {
            break;
        }

        receivedMessage->store(true);

        if (!handleWhisperMessage(buffer, origin, *receipts))
        {
            nm::client::sendMessage(buffer);
        }
    }

    sendToBrowser(QLatin1String{
        R"({"type":"status","status":"exiting-host","reason":"received EOF"})"});
    _Exit(0);
}
}  // namespace

namespace chatterino {

void runBrowserExtensionHost(const Paths &paths, const QString &origin)
{
    initFileMode();

    runLoop(paths, origin);
}

}  // namespace chatterino
