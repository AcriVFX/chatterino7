// Chatterino9: Whispers window. All whisper conversations of the current
// account in one window that can be maximized or made full screen: the list
// of people on the left, the open conversation with an input on the right.
// History comes from util/WhisperHistory.

#pragma once

#include "widgets/BaseWindow.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QString>

#include <memory>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace chatterino {

class Channel;
class ChannelView;
using ChannelPtr = std::shared_ptr<Channel>;

class WhispersWindow : public BaseWindow
{
public:
    /// Shows the window (one for the whole app) with the conversation with
    /// `partner` open, or the newest one if `partner` is empty
    static void showConversation(const QString &partner = {});

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    WhispersWindow();

    void initUi();
    void reloadList();
    void openPartner(const QString &partner);
    void loadConversation();
    void sendInput();
    void openUsercard();
    void markOpenRead();

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
    /// When a whisper was last sent from here, to show its errors
    qint64 lastSendMsecs_ = 0;

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
