// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Atomic.hpp"
#include "util/Expected.hpp"

#include <QString>
#include <QThread>

#include <map>
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

/// Chatterino9's extension that copies whispers sent on twitch.tv
/// (tools/whisper-extension). It has its own native messaging host name, so
/// other Chatterino builds that rewrite the shared manifest can't lock it out.
namespace nm::whispers {

/// Its ID comes from the key in its manifest
inline constexpr const char *EXTENSION_ORIGIN =
    "chrome-extension://bcgnaflbiogmdloflolimldoepjipehp/";

/// Notes that the app saved (or refused for good) the whisper with `id`
void addReceipt(const Paths &paths, const QString &id, bool saved);

/// id -> saved, for the receipts of the last month
std::map<QString, bool> readReceipts(const Paths &paths);

}  // namespace nm::whispers

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

    ReceiverThread *thread;

    /// This vector contains all channels that are open the user's browser.
    /// These channels are joined to be able to switch channels more quickly.
    std::vector<ChannelPtr> channelWarmer_;

    friend ReceiverThread;
};

}  // namespace chatterino
