// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "BrowserExtension.hpp"

#include "singletons/NativeMessaging.hpp"
#include "util/RenameThread.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>
#include <memory>
#include <thread>

#ifdef Q_OS_WIN
#    include <fcntl.h>
#    include <io.h>

#    include <cstdio>

#endif

namespace {

using namespace chatterino;

void initFileMode()
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

// TODO(Qt6): Use QUtf8String
void sendToBrowser(QLatin1String str)
{
    auto len = static_cast<uint32_t>(str.size());
    std::cout.write(reinterpret_cast<const char *>(&len), sizeof(len));
    std::cout.write(str.data(), str.size());
    std::cout.flush();
}

void sendToBrowser(const QByteArray &json)
{
    auto len = static_cast<uint32_t>(json.size());
    std::cout.write(reinterpret_cast<const char *>(&len), sizeof(len));
    std::cout.write(json.data(), json.size());
    std::cout.flush();
}

/// Chatterino9's whisper extension waits for an answer to each whisper it
/// passes on, so it can keep the ones the app didn't get (app closed).
void acknowledge(const QByteArray &buffer, bool delivered)
{
    if (!buffer.contains("chatterino9-"))
    {
        return;
    }
    const auto root = QJsonDocument::fromJson(buffer).object();
    const auto id = root["id"].toString();
    if (!root["action"].toString().startsWith("chatterino9-") || id.isEmpty())
    {
        return;
    }
    QJsonObject ack{
        {"type", "ack"},
        {"id", id},
        {"ok", delivered},
    };
    if (!delivered)
    {
        ack.insert("reason",
                   static_cast<size_t>(buffer.size()) > nm::MAX_MESSAGE_SIZE
                       ? "too-long"
                       : "not-running");
    }
    sendToBrowser(QJsonDocument(ack).toJson(QJsonDocument::Compact));
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

void runLoop()
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

    while (true)
    {
        auto buffer = receiveFromBrowser();
        if (buffer.isNull())
        {
            break;
        }

        receivedMessage->store(true);

        bool delivered =
            static_cast<size_t>(buffer.size()) <= nm::MAX_MESSAGE_SIZE &&
            nm::client::sendMessage(buffer);
        acknowledge(buffer, delivered);
    }

    sendToBrowser(QLatin1String{
        R"({"type":"status","status":"exiting-host","reason":"received EOF"})"});
    _Exit(0);
}
}  // namespace

namespace chatterino {

void runBrowserExtensionHost()
{
    initFileMode();

    runLoop();
}

}  // namespace chatterino
