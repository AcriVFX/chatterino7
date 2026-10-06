// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Updates.hpp"

#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/CombinePath.hpp"
#include "util/PostToThread.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QStringBuilder>
#include <QtConcurrent>
#include <semver/semver.hpp>

namespace {

using namespace chatterino;
using namespace literals;

}  // namespace

namespace chatterino {

Updates::Updates(const Paths &paths_, Settings &settings)
    : paths(paths_)
    , currentVersion_(CHATTERINO_VERSION %
                      QStringLiteral(" (build %1)").arg(CHATTERINO3_BUILD))
    , updateGuideLink_("https://github.com/AcriVFX/chatterino7/releases")
{
    qCDebug(chatterinoUpdate) << "init UpdateManager";

    settings.betaUpdates.connect(
        [this] {
            this->checkForUpdates();
        },
        this->managedConnections, false);
}

/// Checks if the online version is newer or older than the current version.
bool Updates::isDowngradeOf(const QString &online, const QString &current)
{
    semver::version onlineVersion;
    if (!onlineVersion.from_string_noexcept(online.toStdString()))
    {
        qCWarning(chatterinoUpdate) << "Unable to parse online version"
                                    << online << "into a proper semver string";
        return false;
    }

    semver::version currentVersion;
    if (!currentVersion.from_string_noexcept(current.toStdString()))
    {
        qCWarning(chatterinoUpdate) << "Unable to parse current version"
                                    << current << "into a proper semver string";
        return false;
    }

    // TODO: remove once chatterino7's major version switches from `7` to `2`
    if (currentVersion.major == 7 && onlineVersion.major == 2)
    {
        currentVersion = {2, currentVersion.minor, currentVersion.patch,
                          currentVersion.prerelease_type,
                          currentVersion.prerelease_number};
    }

    return onlineVersion < currentVersion;
}

void Updates::deleteOldFiles()
{
    std::ignore = QtConcurrent::run([dir{this->paths.miscDirectory}] {
        {
            auto path = combinePath(dir, "Update.exe");
            if (QFile::exists(path))
            {
                QFile::remove(path);
            }
        }
        {
            auto path = combinePath(dir, "update.zip");
            if (QFile::exists(path))
            {
                QFile::remove(path);
            }
        }
    });
}

const QString &Updates::getCurrentVersion() const
{
    return this->currentVersion_;
}

const QString &Updates::getOnlineVersion() const
{
    return this->onlineVersion_;
}

void Updates::installUpdates()
{
    if (this->status_ != UpdateAvailable)
    {
        assert(false);
        return;
    }

    if (Version::instance().isNightly())
    {
        // Since Nightly builds can be installed in many different ways, we ask the user to download the update manually.
        QDesktopServices::openUrl(
            QUrl("https://github.com/AcriVFX/chatterino7/releases"));
        return;
    }

#ifdef Q_OS_MACOS
    QMessageBox *box = new QMessageBox(
        QMessageBox::Information, "Chatterino Update",
        "A link will open in your browser. Download and install to update.");
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
    QDesktopServices::openUrl(this->updateExe_);
#elif defined Q_OS_LINUX
    QMessageBox *box =
        new QMessageBox(QMessageBox::Information, "Chatterino Update",
                        "Automatic updates are currently not available on "
                        "Linux. Please redownload the app to update.");
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
    QDesktopServices::openUrl(this->updateGuideLink_);
#elif defined Q_OS_WIN
    // Chatterino3 is always installed from a zip (never the installer),
    // so always update by downloading the release zip and letting the
    // bundled updater (updater.1/ChatterinoUpdater.exe) unpack it over the
    // install folder. Settings stay in %APPDATA%, so they are not touched.
    {
        QMessageBox *box =
            new QMessageBox(QMessageBox::Information, "Chatterino Update",
                            "Chatterino is downloading the update "
                            "in the background and will run the "
                            "updater once it is finished.");
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->show();

        NetworkRequest(this->updatePortable_)
            .timeout(600000)
            .followRedirects(true)
            .onError([this](NetworkResult) {
                this->setStatus_(DownloadFailed);

                postToThread([] {
                    QMessageBox *box = new QMessageBox(
                        QMessageBox::Information, "Chatterino Update",
                        "Failed while trying to download the update.");
                    box->setAttribute(Qt::WA_DeleteOnClose);
                    box->show();
                    box->raise();
                });
            })
            .onSuccess([this](auto result) {
                if (result.status() != 200)
                {
                    auto *box = new QMessageBox(
                        QMessageBox::Information, "Chatterino Update",
                        QStringLiteral("The update couldn't be downloaded "
                                       "(Error: %1).")
                            .arg(result.formatError()));
                    box->setAttribute(Qt::WA_DeleteOnClose);
                    box->exec();
                    return;
                }

                QByteArray object = result.getData();
                auto filename =
                    combinePath(this->paths.miscDirectory, "update.zip");

                QFile file(filename);
                if (!file.open(QIODevice::Truncate | QIODevice::WriteOnly))
                {
                    qCWarning(chatterinoUpdate)
                        << "Failed to save update.zip" << file.errorString();
                    this->setStatus_(WriteFileFailed);
                    return;
                }

                if (file.write(object) == -1)
                {
                    this->setStatus_(WriteFileFailed);
                    return;
                }
                file.flush();
                file.close();

                auto updaterPath = Updates::portableUpdaterPath();
                if (!QFile::exists(updaterPath))
                {
                    this->setStatus_(MissingPortableUpdater);
                    return;
                }
                bool ok =
                    QProcess::startDetached(updaterPath, {filename, "restart"});
                if (!ok)
                {
                    this->setStatus_(RunUpdaterFailed);
                    return;
                }

                QApplication::exit(0);
            })
            .execute();
        this->setStatus_(Downloading);
    }
#endif
}

void Updates::checkForUpdates()
{
#ifndef CHATTERINO_DISABLE_UPDATER
    // Chatterino3: updates come from this fork's own GitHub releases, not
    // from 7TV. 7TV's update would replace Chatterino3 with plain
    // Chatterino7. New Chatterino7 versions reach Chatterino3 as new builds
    // of this fork.
#    ifdef Q_OS_WIN
    if (this->status_ == Downloading || this->status_ == UpdateAvailable)
    {
        return;
    }

    auto onSuccess = [this](const NetworkResult &result) {
        const auto object = result.parseJson();

        // Tags look like "chatterino3-build-57", where 57 is the CI run
        // number the build was made from.
        static const QRegularExpression buildRegex(u"build-(\\d+)$"_s);
        auto tag = object["tag_name"_L1].toString();
        auto match = buildRegex.match(tag);
        if (!match.hasMatch())
        {
            qCDebug(chatterinoUpdate)
                << "error checking version - unexpected tag" << tag;
            this->setStatus_(NoUpdateAvailable);
            return;
        }
        auto onlineBuild = match.captured(1).toLongLong();

        QString zipUrl;
        const auto assets = object["assets"_L1].toArray();
        for (const auto &assetValue : assets)
        {
            auto asset = assetValue.toObject();
            auto name = asset.value("name"_L1).toString();
            if (name.endsWith(u".zip"_s, Qt::CaseInsensitive))
            {
                zipUrl = asset.value("browser_download_url"_L1).toString();
                break;
            }
        }
        if (zipUrl.isEmpty())
        {
            qCDebug(chatterinoUpdate)
                << "error checking version - release has no zip" << tag;
            this->setStatus_(NoUpdateAvailable);
            return;
        }

        this->updatePortable_ = zipUrl;
        this->onlineVersion_ = object["name"_L1].toString();
        if (this->onlineVersion_.isEmpty())
        {
            this->onlineVersion_ = tag;
        }

        if (onlineBuild > CHATTERINO3_BUILD)
        {
            this->setStatus_(UpdateAvailable);
        }
        else
        {
            this->setStatus_(NoUpdateAvailable);
        }
    };

    QString url =
        u"https://api.github.com/repos/AcriVFX/chatterino7/releases/latest"_s;
    qCDebug(chatterinoUpdate) << "Requesting updates from" << url;
    NetworkRequest(url)
        .timeout(60000)
        .followRedirects(true)
        .header("Accept", "application/vnd.github+json")
        .onSuccess(onSuccess)
        .onError([this](const NetworkResult &result) {
            // 404 = no release published yet. Network errors are not shown
            // either, the next check will try again.
            qCDebug(chatterinoUpdate)
                << "Update check failed:" << result.formatError();
            this->setStatus_(NoUpdateAvailable);
        })
        .execute();

    this->setStatus_(Searching);
#    endif
#endif
}

Updates::Status Updates::getStatus() const
{
    return this->status_;
}

QString Updates::portableUpdaterPath()
{
    return combinePath(QCoreApplication::applicationDirPath(),
                       "updater.1/ChatterinoUpdater.exe");
}

bool Updates::shouldShowUpdateButton() const
{
    switch (this->getStatus())
    {
        case UpdateAvailable:
        case SearchFailed:
        case Downloading:
        case DownloadFailed:
        case WriteFileFailed:
            return true;

        default:
            return false;
    }
}

bool Updates::isError() const
{
    switch (this->getStatus())
    {
        case SearchFailed:
        case DownloadFailed:
        case WriteFileFailed:
        case MissingPortableUpdater:
        case RunUpdaterFailed:
            return true;

        default:
            return false;
    }
}

bool Updates::isDowngrade() const
{
    return this->isDowngrade_;
}

QString Updates::buildUpdateAvailableText() const
{
    const auto &version = Version::instance();

    if (version.isNightly())
    {
        // Since Nightly builds can be installed in many different ways, we ask the user to download the update manually.
        if (this->isDowngrade())
        {
            return QString("The version online (%1) seems to be lower than the "
                           "current (%2).\nEither a version was reverted or "
                           "you are running a newer build.\n\nDo you want to "
                           "head to Chatterino.com to download it?")
                .arg(this->getOnlineVersion(), this->getCurrentVersion());
        }

        return QString("An update (%1) is available.\n\nDo you want to head to "
                       "Chatterino.com to download the new update?")
            .arg(this->getOnlineVersion());
    }

    if (this->isDowngrade())
    {
        return QString("The version online (%1) seems to be lower than the "
                       "current (%2).\nEither a version was reverted or "
                       "you are running a newer build.\n\nDo you want to "
                       "download and install it?")
            .arg(this->getOnlineVersion(), this->getCurrentVersion());
    }

    return QString("An update (%1) is available.\n\nDo you want to "
                   "download and install it?")
        .arg(this->getOnlineVersion());
}

void Updates::setStatus_(Status status)
{
    if (this->status_ != status)
    {
        this->status_ = status;
        postToThread([this, status] {
            this->statusUpdated.invoke(status);
        });
    }
}

}  // namespace chatterino
