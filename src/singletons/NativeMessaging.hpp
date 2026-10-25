// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Atomic.hpp"
#include "util/Expected.hpp"

#include <QString>
#include <QThread>

#include <optional>
#include <vector>

namespace chatterino::nm::detail {

enum class WriteManifestError : std::uint8_t {
    FailedToCreateDirectory,
    FailedToCreateFile,
};

Expected<void, WriteManifestError> writeManifestTo(QString directory,
                                                   const QString &nmDirectory,
                                                   const QString &filename,
                                                   const QJsonDocument &json);

}  // namespace chatterino::nm::detail

namespace chatterino {

class Application;
class Paths;
class Channel;

using ChannelPtr = std::shared_ptr<Channel>;

void registerNmHost(const Paths &paths);
std::string &getNmQueueName(const Paths &paths);

Atomic<std::optional<QString>> &nmIpcError();

namespace nm {

/// Largest message the browser host can pass on to the app
inline constexpr size_t MAX_MESSAGE_SIZE = 4096;

}  // namespace nm

namespace nm::client {

/// Returns false if the app isn't running (or its queue is full)
bool sendMessage(const QByteArray &array);
void writeToCout(const QByteArray &array);

}  // namespace nm::client

class NativeMessagingServer final
{
public:
    NativeMessagingServer();
    NativeMessagingServer(const NativeMessagingServer &) = delete;
    NativeMessagingServer(NativeMessagingServer &&) = delete;
    NativeMessagingServer &operator=(const NativeMessagingServer &) = delete;
    NativeMessagingServer &operator=(NativeMessagingServer &&) = delete;
    ~NativeMessagingServer();

    void start();

private:
    class ReceiverThread : public QThread
    {
    public:
        ReceiverThread(NativeMessagingServer &parent);

        void run() override;

    private:
        void handleMessage(const QJsonObject &root);
        void handleSelect(const QJsonObject &root);
        void handleDetach(const QJsonObject &root);
        void handleSync(const QJsonObject &root);
        void handleWhisperSent(const QJsonObject &root);

        NativeMessagingServer &parent_;
    };

    void syncChannels(const QJsonArray &twitchChannels);
    /// Saves a whisper sent on twitch.tv to the Whispers window history
    static void recordWhisperSentElsewhere(const QString &recipientId,
                                           const QString &text, qint64 sentAt,
                                           int attempt);

    ReceiverThread *thread;

    /// This vector contains all channels that are open the user's browser.
    /// These channels are joined to be able to switch channels more quickly.
    std::vector<ChannelPtr> channelWarmer_;

    friend ReceiverThread;
};

}  // namespace chatterino
