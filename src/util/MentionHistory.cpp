#include "util/MentionHistory.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"
#include "util/VectorMessageSink.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <IrcMessage>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <set>

namespace {

using namespace chatterino;

/// Newest lines kept per channel file
constexpr qsizetype MAX_LINES = 1500;

struct Store {
    /// Account whose history is loaded, empty if none yet
    QString account;
    QString dir;
    /// Ids of every saved mention and answer
    QSet<QString> savedIds;
    /// msecs of the newest mention that was seen
    qint64 lastRead = 0;
    /// msecs the "New" line goes under
    qint64 lineMark = 0;
    /// Channels whose saved history was brought back this session
    std::set<QString> restored;
};

Store &store()
{
    static Store instance;
    return instance;
}

struct Views {
    std::vector<QPointer<ChannelView>> list;
    /// A /mentions view is on screen in the active window
    bool active = false;
    QTimer *timer = nullptr;
    bool hooked = false;
};

Views &views()
{
    static Views instance;
    return instance;
}

bool isLogin(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9_]{1,25}$"));
    return re.match(name).hasMatch();
}

/// Login of the signed-in account, empty when anonymous
QString currentAccount()
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        return {};
    }
    return account->getUserName().toLower();
}

/// Empty when the app has no misc directory
QString accountDir(const QString &account)
{
    const auto &misc = getApp()->getPaths().miscDirectory;
    if (misc.isEmpty() || account.isEmpty())
    {
        return {};
    }
    return QDir(misc).filePath(QStringLiteral("Mentions/") + account);
}

qint64 msecsOf(const Message &message)
{
    return message.serverReceivedTime.isValid()
               ? message.serverReceivedTime.toMSecsSinceEpoch()
               : 0;
}

QString escapeField(QString text)
{
    text.replace(u'\\', QStringLiteral("\\\\"));
    text.replace(u'\t', QStringLiteral("\\t"));
    text.replace(u'\n', QStringLiteral("\\n"));
    text.replace(u'\r', QStringLiteral("\\r"));
    return text;
}

QString unescapeField(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); i++)
    {
        auto c = text[i];
        if (c == u'\\' && i + 1 < text.size())
        {
            i++;
            switch (text[i].unicode())
            {
                case 't':
                    out += u'\t';
                    break;
                case 'n':
                    out += u'\n';
                    break;
                case 'r':
                    out += u'\r';
                    break;
                default:
                    out += text[i];
                    break;
            }
            continue;
        }
        out += c;
    }
    return out;
}

/// One saved line:
///   M <tab> msecs <tab> id <tab> IRC line            (a mention)
///   A <tab> msecs <tab> id <tab> color <tab> text    (your answer)
struct Line {
    bool answer = false;
    qint64 msecs = 0;
    QString id;
    /// mention: the IRC line; answer: the text
    QString payload;
    QColor color;
};

bool parseLine(const QString &text, Line &line)
{
    const bool answer = text.startsWith(u"A\t");
    if (!answer && !text.startsWith(u"M\t"))
    {
        return false;
    }
    const auto parts = text.split(u'\t');
    if (parts.size() < (answer ? 5 : 4))
    {
        return false;
    }
    bool ok = false;
    line.answer = answer;
    line.msecs = parts[1].toLongLong(&ok);
    line.id = parts[2];
    if (!ok || line.id.isEmpty())
    {
        return false;
    }
    if (answer)
    {
        line.color = parts[3].isEmpty() ? QColor() : QColor(parts[3]);
        line.payload = unescapeField(parts.mid(4).join(u'\t'));
    }
    else
    {
        // a raw IRC line may hold tabs itself
        line.payload = parts.mid(3).join(u'\t');
    }
    return true;
}

QString channelFile(const QString &dir, const QString &channel)
{
    return QDir(dir).filePath(channel + QStringLiteral(".txt"));
}

QStringList readLines(const QString &path)
{
    QStringList lines;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return lines;
    }
    QTextStream in(&file);
    while (!in.atEnd())
    {
        auto line = in.readLine();
        if (!line.isEmpty())
        {
            lines.append(line);
        }
    }
    return lines;
}

void appendLine(const QString &dir, const QString &channel, const QString &line)
{
    if (!QDir().mkpath(dir))
    {
        return;
    }
    QFile file(channelFile(dir, channel));
    if (file.open(QIODevice::Append | QIODevice::Text))
    {
        QTextStream out(&file);
        out << line << '\n';
    }
}

void saveReadState()
{
    const auto &st = store();
    if (st.dir.isEmpty() || !QDir().mkpath(st.dir))
    {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("lastRead"), QString::number(st.lastRead));
    QSaveFile file(QDir(st.dir).filePath(QStringLiteral("read.json")));
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

/// Loads the history of the signed-in account. Returns its folder, empty
/// when nobody is signed in.
QString ensureLoaded()
{
    const auto account = currentAccount();
    auto &st = store();
    if (account == st.account)
    {
        return st.dir;
    }

    st = Store{};
    st.account = account;
    st.dir = accountDir(account);
    if (st.dir.isEmpty())
    {
        return {};
    }

    QFile readFile(QDir(st.dir).filePath(QStringLiteral("read.json")));
    if (readFile.open(QIODevice::ReadOnly))
    {
        const auto obj = QJsonDocument::fromJson(readFile.readAll()).object();
        st.lastRead =
            obj.value(QStringLiteral("lastRead")).toString().toLongLong();
    }
    else
    {
        // first start: what is already in chat counts as read
        st.lastRead = QDateTime::currentMSecsSinceEpoch();
        saveReadState();
    }
    st.lineMark = st.lastRead;

    const QDir qdir(st.dir);
    const auto fileNames =
        qdir.entryList({QStringLiteral("*.txt")}, QDir::Files);
    for (const auto &fileName : fileNames)
    {
        const auto path = qdir.filePath(fileName);
        auto lines = readLines(path);
        if (lines.size() > MAX_LINES)
        {
            lines = lines.mid(lines.size() - MAX_LINES);
            QSaveFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text))
            {
                file.write((lines.join(u'\n') + u'\n').toUtf8());
                file.commit();
            }
        }
        for (const auto &text : lines)
        {
            Line line;
            if (parseLine(text, line))
            {
                st.savedIds.insert(line.id);
            }
        }
    }
    return st.dir;
}

ChannelPtr mentionsChannel()
{
    return getApp()->getTwitch()->getMentionsChannel();
}

/// msecs of the newest mention in /mentions that is not your own message
qint64 newestMention()
{
    const auto self = store().account;
    const auto snapshot = mentionsChannel()->getMessageSnapshot();
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
    {
        const auto &msg = **it;
        if (msg.flags.has(MessageFlag::Highlighted) &&
            msg.loginName.compare(self, Qt::CaseInsensitive) != 0)
        {
            return msecsOf(msg);
        }
    }
    return 0;
}

void notifyChanged()
{
    for (const auto &view : views().list)
    {
        if (view)
        {
            view->update();
        }
    }
    mentionhistory::changed().invoke();
}

/// Everything in /mentions up to now counts as read
void markSeen()
{
    ensureLoaded();
    auto &st = store();
    const auto newest = newestMention();
    if (newest > st.lastRead)
    {
        st.lastRead = newest;
        saveReadState();
    }
}

void updateActive()
{
    auto &v = views();
    std::erase_if(v.list, [](const auto &view) {
        return view.isNull();
    });

    const auto mentions = mentionsChannel();
    bool nowActive = false;
    for (const auto &view : v.list)
    {
        if (view->isVisible() && view->window()->isActiveWindow() &&
            view->underlyingChannel() == mentions)
        {
            nowActive = true;
            break;
        }
    }

    if (nowActive == v.active)
    {
        return;
    }
    v.active = nowActive;
    if (nowActive)
    {
        // the "New" line stays where it was until /mentions is left
        markSeen();
    }
    else
    {
        store().lineMark = store().lastRead;
    }
    notifyChanged();
}

void onMentionsChanged()
{
    if (views().active)
    {
        markSeen();
    }
    notifyChanged();
}

void ensureHooked()
{
    auto &v = views();
    if (v.hooked)
    {
        return;
    }
    v.hooked = true;

    // The mentions channel lives as long as the app (see CHANNEL_LIFETIME)
    auto channel = mentionsChannel();
    std::ignore = channel->messageAppended.connect([](auto &&...) {
        onMentionsChanged();
    });
    std::ignore = channel->filledInMessages.connect([](auto &&...) {
        onMentionsChanged();
    });
    std::ignore =
        getApp()->getAccounts()->twitch.currentUserChanged.connect([] {
            ensureLoaded();
            notifyChanged();
        });
}

/// The small indented line that shows your answer under a mention
MessagePtr buildAnswer(const QString &channel, const QString &id, qint64 msecs,
                       const QColor &color, const QString &text)
{
    const auto time = QDateTime::fromMSecsSinceEpoch(msecs);

    MessageBuilder b;
    // two em spaces indent the line
    b.emplace<TextElement>(QStringLiteral("  "), MessageElementFlag::Text,
                           MessageColor::System, FontStyle::ChatMediumSmall);
    b.emplace<TextElement>(time.time().toString(getSettings()->timestampFormat),
                           MessageElementFlag::Timestamp, MessageColor::System,
                           FontStyle::ChatMediumSmall);
    b.emplace<TextElement>(
        QStringLiteral("You:"), MessageElementFlag::Text,
        color.isValid() ? MessageColor(color) : MessageColor::Text,
        FontStyle::ChatMediumSmall);
    b.emplace<SingleLineTextElement>(text, MessageElementFlag::Text,
                                     MessageColor::System,
                                     FontStyle::ChatMediumSmall);

    b->id = id;
    b->channelName = channel;
    b->loginName = store().account;
    b->displayName = store().account;
    b->messageText = text;
    b->searchText = QStringLiteral("You: ") + text;
    b->serverReceivedTime = time;
    b->parseTime = time.time();
    return b.release();
}

/// The person `message` answers, if it is yours and they are in /mentions
/// for the same channel. Empty otherwise.
QString answeredPerson(const Message &message)
{
    const auto self = store().account;
    if (self.isEmpty() ||
        message.loginName.compare(self, Qt::CaseInsensitive) != 0 ||
        message.id.isEmpty() || message.flags.has(MessageFlag::System))
    {
        return {};
    }

    QStringList candidates;
    if (!message.replyParentLogin.isEmpty())
    {
        candidates.append(message.replyParentLogin.toLower());
    }
    static const QRegularExpression atRe(
        QStringLiteral("(?:^|\\s)@([A-Za-z0-9_]{1,25})"));
    auto it = atRe.globalMatch(message.messageText);
    while (it.hasNext())
    {
        candidates.append(it.next().captured(1).toLower());
    }
    if (candidates.isEmpty())
    {
        return {};
    }

    const auto snapshot = mentionsChannel()->getMessageSnapshot();
    for (const auto &candidate : candidates)
    {
        if (candidate == self || !isLogin(candidate))
        {
            continue;
        }
        for (auto m = snapshot.rbegin(); m != snapshot.rend(); ++m)
        {
            const auto &msg = **m;
            if (msg.flags.has(MessageFlag::Highlighted) &&
                msg.channelName == message.channelName &&
                msg.loginName.compare(candidate, Qt::CaseInsensitive) == 0)
            {
                return candidate;
            }
        }
    }
    return {};
}

/// Builds and saves the answer line for `message`, null if it answers nobody
MessagePtr makeAnswer(const Message &message)
{
    const auto person = answeredPerson(message);
    if (person.isEmpty())
    {
        return nullptr;
    }

    // replies have the leading @name stripped from their text
    auto text = message.messageText;
    if (!message.replyParentLogin.isEmpty() &&
        !text.startsWith(u'@' + message.replyParentLogin, Qt::CaseInsensitive))
    {
        text = u'@' + message.replyParentLogin + u' ' + text;
    }

    const auto msecs = msecsOf(message) > 0
                           ? msecsOf(message)
                           : QDateTime::currentMSecsSinceEpoch();
    auto &st = store();
    if (!st.dir.isEmpty() && !st.savedIds.contains(message.id))
    {
        st.savedIds.insert(message.id);
        appendLine(
            st.dir, message.channelName,
            QStringLiteral("A\t") + QString::number(msecs) + u'\t' +
                message.id + u'\t' +
                (message.usernameColor.isValid() ? message.usernameColor.name()
                                                 : QString()) +
                u'\t' + escapeField(text));
    }
    return buildAnswer(message.channelName, message.id, msecs,
                       message.usernameColor, text);
}

void recordMention(const MessagePtr &message, const QByteArray &rawIrc)
{
    const auto dir = ensureLoaded();
    auto &st = store();
    const auto raw = QString::fromUtf8(rawIrc).trimmed();
    if (dir.isEmpty() || message->id.isEmpty() ||
        message->channelName.isEmpty() || raw.isEmpty() ||
        raw.contains(u'\n') || raw.contains(u'\r') ||
        st.savedIds.contains(message->id))
    {
        return;
    }
    st.savedIds.insert(message->id);
    appendLine(dir, message->channelName,
               QStringLiteral("M\t") + QString::number(msecsOf(*message)) +
                   u'\t' + message->id + u'\t' + raw);
}

/// The saved mentions and answers of `channel`, built like chat history
std::vector<MessagePtr> loadSaved(TwitchChannel &channel)
{
    const auto &st = store();
    std::vector<MessagePtr> out;
    const auto lines = readLines(channelFile(st.dir, channel.getName()));
    for (const auto &text : lines)
    {
        Line line;
        if (!parseLine(text, line))
        {
            continue;
        }
        if (line.answer)
        {
            out.push_back(buildAnswer(channel.getName(), line.id, line.msecs,
                                      line.color, line.payload));
            continue;
        }

        std::unique_ptr<Communi::IrcMessage> irc(
            Communi::IrcMessage::fromData(line.payload.toUtf8(), nullptr));
        if (!irc)
        {
            continue;
        }
        // like chat history: no sounds, alerts or emote counting
        auto tags = irc->tags();
        tags.remove(QStringLiteral("rm-received-ts"));
        tags.insert(QStringLiteral("historical"), QStringLiteral("1"));
        irc->setTags(tags);

        VectorMessageSink sink({}, MessageFlag::RecentMessage);
        IrcMessageHandler::parseMessageInto(irc.get(), sink, &channel);
        for (const auto &msg : sink.messages())
        {
            if (msg->id == line.id)
            {
                out.push_back(msg);
            }
        }
    }
    return out;
}

/// Adds messages to /mentions that are not in it yet, oldest first
void fillIn(std::vector<MessagePtr> messages)
{
    if (messages.empty())
    {
        return;
    }
    QSet<QString> ids;
    for (const auto &msg : mentionsChannel()->getMessageSnapshot())
    {
        ids.insert(msg->id);
    }
    std::erase_if(messages, [&ids](const MessagePtr &msg) {
        if (msg->id.isEmpty() || ids.contains(msg->id))
        {
            return true;
        }
        ids.insert(msg->id);
        return false;
    });
    std::ranges::stable_sort(messages, [](const auto &a, const auto &b) {
        return msecsOf(*a) < msecsOf(*b);
    });
    mentionsChannel()->fillInMissingMessages(messages);
}

}  // namespace

namespace chatterino::mentionhistory {

void onChatMessage(const MessagePtr &message, const QByteArray &rawIrc,
                   bool isMention, bool live)
{
    if (!message || getApp()->isTest())
    {
        return;
    }
    if (isMention)
    {
        runInGuiThread([message, rawIrc] {
            recordMention(message, rawIrc);
        });
    }
    else if (live && !message->loginName.isEmpty() &&
             message->loginName.compare(
                 getApp()->getAccounts()->twitch.getCurrent()->getUserName(),
                 Qt::CaseInsensitive) == 0)
    {
        runInGuiThread([message] {
            ensureLoaded();
            if (auto answer = makeAnswer(*message))
            {
                mentionsChannel()->addMessage(answer, MessageContext::Original);
            }
        });
    }
}

void restoreChannel(TwitchChannel &channel,
                    const std::vector<MessagePtr> &messages)
{
    if (getApp()->isTest() || ensureLoaded().isEmpty())
    {
        return;
    }
    ensureHooked();

    auto &st = store();
    if (st.restored.insert(channel.getName()).second)
    {
        fillIn(loadSaved(channel));
    }

    // answers in the history, now that the mentions they answer are there
    std::vector<MessagePtr> answers;
    for (const auto &msg : messages)
    {
        if (auto answer = makeAnswer(*msg))
        {
            answers.push_back(std::move(answer));
        }
    }
    fillIn(std::move(answers));
}

int unreadCount()
{
    if (ensureLoaded().isEmpty())
    {
        return 0;
    }
    ensureHooked();
    const auto &st = store();
    int count = 0;
    for (const auto &msg : mentionsChannel()->getMessageSnapshot())
    {
        if (msg->flags.has(MessageFlag::Highlighted) &&
            msecsOf(*msg) > st.lastRead &&
            msg->loginName.compare(st.account, Qt::CaseInsensitive) != 0)
        {
            count++;
        }
    }
    return count;
}

void markAllRead()
{
    markSeen();
    store().lineMark = store().lastRead;
    notifyChanged();
}

qint64 newLineMark()
{
    ensureLoaded();
    return store().lineMark;
}

void watchView(ChannelView *view)
{
    auto &v = views();
    if (std::ranges::any_of(v.list, [view](const auto &watched) {
            return watched.data() == view;
        }))
    {
        return;
    }
    ensureHooked();
    v.list.emplace_back(view);
    if (v.timer == nullptr)
    {
        // lives as long as the app
        v.timer = new QTimer();
        QObject::connect(v.timer, &QTimer::timeout, [] {
            updateActive();
        });
        v.timer->start(500);
    }
    // not right away: this runs while the view paints
    QTimer::singleShot(0, [] {
        updateActive();
    });
}

bool jumpTo(const MessagePtr &message)
{
    if (!message || message->id.isEmpty())
    {
        return false;
    }
    auto &notebook = getApp()->getWindows()->getMainWindow().getNotebook();
    // the full chat first, filtered splits (lanes) only if it isn't there
    for (const bool filtered : {false, true})
    {
        for (int i = 0; i < notebook.getPageCount(); i++)
        {
            auto *page = dynamic_cast<SplitContainer *>(notebook.getPageAt(i));
            if (page == nullptr)
            {
                continue;
            }
            for (auto *split : page->getSplits())
            {
                auto channel = split->getChannel();
                if (channel->getType() != Channel::Type::Twitch ||
                    channel->getName() != message->channelName ||
                    split->getFilters().isEmpty() == filtered)
                {
                    continue;
                }
                if (split->getChannelView().scrollToMessageId(message->id))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

pajlada::Signals::NoArgSignal &changed()
{
    static pajlada::Signals::NoArgSignal signal;
    return signal;
}

}  // namespace chatterino::mentionhistory
