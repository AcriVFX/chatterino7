// Chatterino9: Whispers window. All whisper conversations of the current
// account in one window that can be maximized or made full screen: the list
// of people on the left, the open conversation with an input on the right.
// History comes from util/WhisperHistory. The same panel fills /whispers
// splits.

#pragma once

#include "widgets/BaseWidget.hpp"
#include "widgets/BaseWindow.hpp"

#include <boost/signals2/connection.hpp>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QString>

#include <memory>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace chatterino {

class Channel;
class ChannelView;
using ChannelPtr = std::shared_ptr<Channel>;

/// The conversation list and the open conversation, used by the Whispers
/// window and by /whispers splits
class WhispersPanel : public BaseWidget
{
public:
    explicit WhispersPanel(QWidget *parent = nullptr);

    /// Opens the conversation with `partner`, or the newest one if `partner`
    /// is empty and nothing is open yet
    void showPartner(const QString &partner);

    QLineEdit *input() const;

    /// Display name of the open conversation, empty if none
    QString openDisplayName() const;

    /// Invoked when another conversation was opened
    pajlada::Signals::NoArgSignal partnerChanged;
    /// Invoked when the panel gets or loses keyboard focus
    pajlada::Signals::NoArgSignal focused;
    pajlada::Signals::NoArgSignal focusLost;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void initUi();
    void reloadList();
    void openPartner(const QString &partner);
    void loadConversation();
    void sendInput();
    void openUsercard();
    void markOpenRead();
    /// Whether the user is looking at this panel right now
    bool isSeen() const;

    QLineEdit *search_ = nullptr;
    QListWidget *list_ = nullptr;
    QLabel *nameLabel_ = nullptr;
    QPushButton *usercardButton_ = nullptr;
    QPushButton *markReadButton_ = nullptr;
    ChannelView *view_ = nullptr;
    ChannelPtr viewChannel_;
    QLineEdit *input_ = nullptr;
    QLabel *counter_ = nullptr;
    QLabel *empty_ = nullptr;

    /// Login of the open conversation
    QString partner_;
    QString displayName_;
    /// When a whisper was last sent from here, to show its errors
    qint64 lastSendMsecs_ = 0;
    /// A reload for whispers copied in from twitch.tv is on its way
    bool reloadQueued_ = false;

    pajlada::Signals::SignalHolder signalHolder_;
    std::vector<boost::signals2::scoped_connection> bSignals_;
};

class WhispersWindow : public BaseWindow
{
public:
    /// Opens the conversation with `partner` (the newest one if `partner` is
    /// empty) in the /whispers tab of the main window if there is one,
    /// otherwise in the Whispers window
    static void showConversation(const QString &partner = {});

    /// Like showConversation, but always in the Whispers window
    static void showWindow(const QString &partner = {});

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    WhispersWindow();

    void updateTitle();

    WhispersPanel *panel_ = nullptr;

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
