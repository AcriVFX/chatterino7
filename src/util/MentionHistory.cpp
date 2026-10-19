#include "util/MentionHistory.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrc.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/IrcHelpers.hpp"
#include "util/PostToThread.hpp"
#include "util/VectorMessageSink.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <IrcMessage>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QtConcurrent>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

namespace {

using namespace chatterino;

/// Newest lines kept per channel file
constexpr qsizetype MAX_LINES = 1500;

/// An @ to someone counts as an answer to their mention up to this long after
/// it. Replies always count.
constexpr qint64 ANSWER_WINDOW_MS = 60LL * 60 * 1000;

/// Saved read state is written this long after the last change
constexpr int READ_SAVE_DELAY_MS = 3000;

/// After start, saved mentions of channels that are not open come back then
constexpr int SWEEP_DELAY_MS = 20000;

/// Answer rows use this before the id of the message they show, so they never
/// clash with a mention of the same message
const QString ANSWER_PREFIX = QStringLiteral("answer:");

struct Store {
    /// Account whose history is loaded, empty if none yet
    QString account;
    QString dir;
    /// Ids saved per channel, read from its file when first needed
    std::map<QString, QSet<QString>> savedIds;
    /// msecs of the newest mention that was seen
    qint64 lastRead = 0;
    /// msecs the "New" line goes under
    qint64 lineMark = 0;
    /// lastRead changed and is not written yet
    bool readStatePending = false;
    /// Channels whose saved history was brought back (or is loading)
    std::set<QString> restored;
};

Store &store()
{
    static Store instance;
    return instance;
}

/// Guards reading, trimming and appending the channel files, which happens
/// in the GUI thread and in the thread that loads saved history
std::mutex &fileMutex()
{
    static std::mutex mutex;
    return mutex;
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

bool isAnswerRow(const Message &message)
{
    return message.id.startsWith(ANSWER_PREFIX);
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
///   M <tab> msecs <tab> id <tab> IRC line                     (a mention)
///   A <tab> msecs <tab> id <tab> color <tab> mention id <tab> text
///                                                     (your answer to it)
struct Line {
    bool answer = false;
    qint64 msecs = 0;
    QString id;
    /// mention: the IRC line; answer: the text
    QString payload;
    QColor color;
    /// answer: id of the mention it answers
    QString anchor;
};

bool parseLine(const QString &text, Line &line)
{
    const bool answer = text.startsWith(u"A\t");
    if (!answer && !text.startsWith(u"M\t"))
    {
        return false;
    }
    const auto parts = text.split(u'\t');
    if (parts.size() < (answer ? 6 : 4))
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
        line.anchor = parts[4];
        line.payload = unescapeField(parts.mid(5).join(u'\t'));
    }
    else
    {
        // a raw IRC line may hold tabs itself
        line.payload = parts.mid(3).join(u'\t');
    }
    return true;
}

/// Id of a saved line, empty if it is not one
QString lineId(const QString &text)
{
    if (!text.startsWith(u"M\t") && !text.startsWith(u"A\t"))
    {
        return {};
    }
    return text.section(u'\t', 2, 2);
}

QString channelFile(const QString &dir, const QString &channel)
{
    return QDir(dir).filePath(channel + QStringLiteral(".txt"));
}

/// Hold fileMutex() while calling this
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
    std::lock_guard lock(fileMutex());
    if (!QDir().mkpath(dir))
    {
        return;
    }
    const auto path = channelFile(dir, channel);

    // a line cut off by a crash must not swallow this one
    bool startOnNewLine = false;
    {
        QFile tail(path);
        if (tail.size() > 0 && tail.open(QIODevice::ReadOnly) &&
            tail.seek(tail.size() - 1))
        {
            char last = '\n';
            startOnNewLine = tail.getChar(&last) && last != '\n';
        }
    }

    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text))
    {
        QTextStream out(&file);
        if (startOnNewLine)
        {
            out << '\n';
        }
        out << line << '\n';
    }
}

/// Ids saved for `channel`. Reads its file the first time.
QSet<QString> &savedIdsOf(const QString &channel)
{
    auto &st = store();
    auto it = st.savedIds.find(channel);
    if (it != st.savedIds.end())
    {
        return it->second;
    }
    QSet<QString> ids;
    if (!st.dir.isEmpty())
    {
        std::lock_guard lock(fileMutex());
        for (const auto &text : readLines(channelFile(st.dir, channel)))
        {
            const auto id = lineId(text);
            if (!id.isEmpty())
            {
                ids.insert(id);
            }
        }
    }
    return st.savedIds.emplace(channel, std::move(ids)).first->second;
}

void writeReadState()
{
    auto &st = store();
    st.readStatePending = false;
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

/// Writes the read state now if a change is waiting
void flushReadState()
{
    if (store().readStatePending)
    {
        writeReadState();
    }
}

/// Writes the read state a moment later, so a busy /mentions doesn't write
/// on every message
void saveReadStateSoon()
{
    store().readStatePending = true;
    static QTimer *timer = [] {
        // lives as long as the app
        auto *t = new QTimer();
        t->setSingleShot(true);
        t->setInterval(READ_SAVE_DELAY_MS);
        QObject::connect(t, &QTimer::timeout, [] {
            flushReadState();
        });
        return t;
    }();
    if (!timer->isActive())
    {
        timer->start();
    }
}

ChannelPtr mentionsChannel()
{
    return getApp()->getTwitch()->getMentionsChannel();
}

void sweep(bool includeOpen);

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

    const bool switched = !st.account.isEmpty();
    flushReadState();
    st = Store{};
    st.account = account;
    st.dir = accountDir(account);

    if (switched)
    {
        // the rows of the last account go, the saved ones of this one come
        QTimer::singleShot(0, [] {
            mentionsChannel()->clearMessages();
            sweep(true);
        });
    }

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
        writeReadState();
    }
    st.lineMark = st.lastRead;
    return st.dir;
}

/// msecs of the newest mention in /mentions that is not your own message
qint64 newestMention()
{
    const auto self = store().account;
    qint64 newest = 0;
    for (const auto &msg : mentionsChannel()->getMessageSnapshot())
    {
        if (msg->flags.has(MessageFlag::Highlighted) &&
            msg->loginName.compare(self, Qt::CaseInsensitive) != 0)
        {
            newest = std::max(newest, msecsOf(*msg));
        }
    }
    return newest;
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
        saveReadStateSoon();
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
        flushReadState();
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
    QObject::connect(QCoreApplication::instance(),
                     &QCoreApplication::aboutToQuit, [] {
                         flushReadState();
                     });

    // open channels bring their saved mentions back once their history is
    // loaded; the rest come back a little later
    QTimer::singleShot(SWEEP_DELAY_MS, [] {
        if (!ensureLoaded().isEmpty())
        {
            sweep(false);
        }
    });
}

/// The small indented line that shows your answer under the mention at
/// `anchorMsecs`
MessagePtr buildAnswer(const QString &channel, const QString &id, qint64 msecs,
                       const QColor &color, const QString &text,
                       qint64 anchorMsecs)
{
    const auto time = QDateTime::fromMSecsSinceEpoch(msecs);

    MessageBuilder b;
    // two em spaces indent the line
    b.emplace<TextElement>(QStringLiteral("  "), MessageElementFlag::Text,
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

    b->id = ANSWER_PREFIX + id;
    b->flags.set(MessageFlag::DoNotLog, MessageFlag::DoNotTriggerNotification);
    b->channelName = channel;
    b->loginName = store().account;
    b->displayName = store().account;
    b->messageText = text;
    b->searchText = QStringLiteral("You: ") + text;
    // sorts right under the mention; the line shows the real time
    b->serverReceivedTime = QDateTime::fromMSecsSinceEpoch(anchorMsecs + 1);
    b->parseTime = time.time();
    return b.release();
}

/// The mention in /mentions that `message` answers: the message it replies
/// to, or else the newest mention of someone it @s from the hour before.
/// Null if `message` is not yours or answers nobody there.
MessagePtr answeredMention(const Message &message)
{
    const auto self = store().account;
    if (self.isEmpty() ||
        message.loginName.compare(self, Qt::CaseInsensitive) != 0 ||
        message.id.isEmpty() || message.flags.has(MessageFlag::System))
    {
        return nullptr;
    }

    const auto snapshot = mentionsChannel()->getMessageSnapshot();
    const auto isMention = [&](const Message &m) {
        return m.flags.has(MessageFlag::Highlighted) && !isAnswerRow(m) &&
               m.channelName == message.channelName &&
               m.loginName.compare(self, Qt::CaseInsensitive) != 0;
    };

    if (message.replyParent)
    {
        for (auto m = snapshot.rbegin(); m != snapshot.rend(); ++m)
        {
            if ((*m)->id == message.replyParent->id && isMention(**m))
            {
                return *m;
            }
        }
    }

    QSet<QString> candidates;
    if (!message.replyParentLogin.isEmpty())
    {
        candidates.insert(message.replyParentLogin.toLower());
    }
    static const QRegularExpression atRe(
        QStringLiteral("(?:^|\\s)@([A-Za-z0-9_]{1,25})"));
    auto it = atRe.globalMatch(message.messageText);
    while (it.hasNext())
    {
        const auto login = it.next().captured(1).toLower();
        if (login != self && isLogin(login))
        {
            candidates.insert(login);
        }
    }
    if (candidates.isEmpty())
    {
        return nullptr;
    }

    const auto msecs = msecsOf(message) > 0
                           ? msecsOf(message)
                           : QDateTime::currentMSecsSinceEpoch();
    for (auto m = snapshot.rbegin(); m != snapshot.rend(); ++m)
    {
        const auto &msg = **m;
        const auto age = msecs - msecsOf(msg);
        if (isMention(msg) && age >= 0 && age <= ANSWER_WINDOW_MS &&
            candidates.contains(msg.loginName.toLower()))
        {
            return *m;
        }
    }
    return nullptr;
}

/// Builds and saves the answer line for `message`, null if it answers nobody
MessagePtr makeAnswer(const Message &message)
{
    const auto anchor = answeredMention(message);
    if (!anchor)
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
    const auto &st = store();
    if (!st.dir.isEmpty())
    {
        auto &ids = savedIdsOf(message.channelName);
        if (!ids.contains(message.id))
        {
            ids.insert(message.id);
            appendLine(st.dir, message.channelName,
                       QStringLiteral("A\t") + QString::number(msecs) + u'\t' +
                           message.id + u'\t' +
                           (message.usernameColor.isValid()
                                ? message.usernameColor.name()
                                : QString()) +
                           u'\t' + anchor->id + u'\t' + escapeField(text));
        }
    }
    return buildAnswer(message.channelName, message.id, msecs,
                       message.usernameColor, text, msecsOf(*anchor));
}

void recordMention(const MessagePtr &message, const QByteArray &rawIrc)
{
    const auto dir = ensureLoaded();
    const auto raw = QString::fromUtf8(rawIrc).trimmed();
    // your own messages are never saved: bringing them back would replay
    // your old badges onto the channel
    if (dir.isEmpty() || message->id.isEmpty() ||
        message->channelName.isEmpty() || raw.isEmpty() ||
        raw.contains(u'\n') || raw.contains(u'\r') ||
        message->loginName.compare(store().account, Qt::CaseInsensitive) == 0)
    {
        return;
    }
    auto &ids = savedIdsOf(message->channelName);
    if (ids.contains(message->id))
    {
        return;
    }
    ids.insert(message->id);
    appendLine(dir, message->channelName,
               QStringLiteral("M\t") + QString::number(msecsOf(*message)) +
                   u'\t' + message->id + u'\t' + raw);
}

/// A saved mention of a channel that is not open: name and text without the
/// channel's emotes and badges. Any thread.
MessagePtr buildPlain(Communi::IrcMessage *irc, const QString &channelName)
{
    auto *priv = dynamic_cast<Communi::IrcPrivateMessage *>(irc);
    if (priv == nullptr)
    {
        return nullptr;
    }
    const auto tags = irc->tags();
    const auto login = irc->nick();
    auto display = tags.value(QStringLiteral("display-name")).toString();
    if (display.isEmpty())
    {
        display = login;
    }
    const auto text = unescapeZeroWidthJoiner(priv->content());
    const auto time = calculateMessageTime(irc);

    MessageBuilder b;
    b->id = tags.value(QStringLiteral("id")).toString();
    b->channelName = channelName;
    b->loginName = login;
    b->displayName = display;
    b->messageText = text;
    b->searchText = display + QStringLiteral(": ") + text;
    b->serverReceivedTime = time;
    b->parseTime = time.time();
    b->flags.set(MessageFlag::RecentMessage, MessageFlag::Highlighted,
                 MessageFlag::ShowInMentions);

    b.emplace<TimestampElement>(time.time());
    b.emplace<TextElement>(u'#' + channelName, MessageElementFlag::ChannelName,
                           MessageColor::System)
        ->setLink({Link::JumpToChannel, channelName});
    const QColor color(tags.value(QStringLiteral("color")).toString());
    b.emplace<TextElement>(
         display + u':', MessageElementFlag::Username,
         color.isValid() ? MessageColor(color) : MessageColor::Text,
         FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, login});
    b.emplace<TextElement>(text, MessageElementFlag::Text, MessageColor::Text);

    const auto [highlighted, result] = getApp()->getHighlights()->check(
        {}, parseBadgeTag(tags), login, text, b->flags);
    if (highlighted && result.color)
    {
        b->highlightColor = result.color;
        b->highlightLabel = result.label;
    }
    else
    {
        b->highlightColor =
            ColorProvider::instance().color(ColorType::SelfHighlight);
    }
    return b.release();
}

/// What was read from a channel's file
struct Loaded {
    QSet<QString> ids;
    std::vector<MessagePtr> mentions;
    std::vector<Line> answers;
    /// Kept so the channel is released in the GUI thread
    std::shared_ptr<TwitchChannel> channel;
};

/// Reads the saved mentions and answers of a channel, building the newest
/// `cap` of them like chat history (or plainly when `weak` is gone). Skips
/// the ids in `present` and your own messages. Runs in a worker thread.
Loaded loadSaved(const QString &dir, const QString &name,
                 const std::weak_ptr<TwitchChannel> &weak,
                 QSet<QString> present, qsizetype cap, const QString &account)
{
    Loaded out;
    out.channel = weak.lock();

    QStringList lines;
    {
        std::lock_guard lock(fileMutex());
        const auto path = channelFile(dir, name);
        lines = readLines(path);
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
    }

    for (const auto &text : lines)
    {
        const auto id = lineId(text);
        if (!id.isEmpty())
        {
            out.ids.insert(id);
        }
    }

    for (auto i = std::max<qsizetype>(0, lines.size() - cap); i < lines.size();
         i++)
    {
        Line line;
        if (!parseLine(lines[i], line))
        {
            continue;
        }
        const auto rowId = line.answer ? ANSWER_PREFIX + line.id : line.id;
        if (present.contains(rowId))
        {
            continue;
        }
        present.insert(rowId);
        if (line.answer)
        {
            out.answers.push_back(std::move(line));
            continue;
        }

        std::unique_ptr<Communi::IrcMessage> irc(
            Communi::IrcMessage::fromData(line.payload.toUtf8(), nullptr));
        if (!irc || irc->nick().compare(account, Qt::CaseInsensitive) == 0)
        {
            continue;
        }
        // like chat history: no sounds, alerts or emote counting
        auto tags = irc->tags();
        tags.remove(QStringLiteral("rm-received-ts"));
        tags.insert(QStringLiteral("historical"), QStringLiteral("1"));
        irc->setTags(tags);

        if (!out.channel)
        {
            if (auto msg = buildPlain(irc.get(), name))
            {
                out.mentions.push_back(std::move(msg));
            }
            continue;
        }
        VectorMessageSink sink({}, MessageFlag::RecentMessage);
        IrcMessageHandler::parseMessageInto(irc.get(), sink, out.channel.get());
        for (const auto &msg : sink.messages())
        {
            if (msg->id == line.id)
            {
                out.mentions.push_back(msg);
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

/// Adds your answers found in `history` to /mentions
void addAnswers(const std::vector<MessagePtr> &history)
{
    std::vector<MessagePtr> answers;
    for (const auto &msg : history)
    {
        if (auto answer = makeAnswer(*msg))
        {
            answers.push_back(std::move(answer));
        }
    }
    fillIn(std::move(answers));
}

/// Puts what was loaded for `name` into /mentions. GUI thread.
void finishRestore(const QString &name, Loaded loaded,
                   const std::vector<MessagePtr> &history)
{
    savedIdsOf(name).unite(loaded.ids);

    // answers sit right under the mention they answer
    QHash<QString, qint64> mentionTimes;
    for (const auto &msg : mentionsChannel()->getMessageSnapshot())
    {
        mentionTimes.insert(msg->id, msecsOf(*msg));
    }
    for (const auto &msg : loaded.mentions)
    {
        mentionTimes.insert(msg->id, msecsOf(*msg));
    }

    auto batch = std::move(loaded.mentions);
    for (const auto &line : loaded.answers)
    {
        // without its mention an answer means nothing
        const auto it = mentionTimes.constFind(line.anchor);
        if (it != mentionTimes.constEnd())
        {
            batch.push_back(buildAnswer(name, line.id, line.msecs, line.color,
                                        line.payload, it.value()));
        }
    }
    fillIn(std::move(batch));

    // answers in the history, now that the mentions they answer are there
    addAnswers(history);
}

/// Brings back the saved mentions of `name`, parsed with `channel` or
/// plainly without it, then adds the answers in `history`. GUI thread.
void restoreOne(const QString &name,
                const std::shared_ptr<TwitchChannel> &channel,
                std::vector<MessagePtr> history)
{
    auto &st = store();
    st.restored.insert(name);

    QSet<QString> present;
    for (const auto &msg : mentionsChannel()->getMessageSnapshot())
    {
        present.insert(msg->id);
    }
    const auto cap = static_cast<qsizetype>(
        std::max(100, getSettings()->scrollbackSplitLimit.getValue()));
    std::ignore =
        QtConcurrent::run([dir = st.dir, account = st.account, name,
                           weak = std::weak_ptr<TwitchChannel>(channel),
                           present = std::move(present), cap,
                           history = std::move(history)]() mutable {
            auto loaded =
                loadSaved(dir, name, weak, std::move(present), cap, account);
            postToThread([account, name, loaded = std::move(loaded),
                          history = std::move(history)]() mutable {
                if (isAppAboutToQuit() || store().account != account)
                {
                    return;
                }
                finishRestore(name, std::move(loaded), history);
            });
        });
}

/// Brings back the saved mentions of every channel that has a file and was
/// not brought back yet. Open channels only with `includeOpen`.
void sweep(bool includeOpen)
{
    const auto &st = store();
    if (st.dir.isEmpty())
    {
        return;
    }
    const auto files =
        QDir(st.dir).entryList({QStringLiteral("*.txt")}, QDir::Files);
    for (const auto &file : files)
    {
        const auto name = file.chopped(4);
        if (store().restored.contains(name))
        {
            continue;
        }
        auto channel = std::dynamic_pointer_cast<TwitchChannel>(
            getApp()->getTwitch()->getChannelOrEmpty(name));
        if (channel && !includeOpen)
        {
            // comes back when its history is loaded
            continue;
        }
        restoreOne(name, channel, {});
    }
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
                fillIn({std::move(answer)});
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

    const auto name = channel.getName();
    if (!store().restored.contains(name))
    {
        restoreOne(
            name,
            std::static_pointer_cast<TwitchChannel>(channel.shared_from_this()),
            messages);
        return;
    }
    addAnswers(messages);
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
    flushReadState();
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
    // an answer row jumps to your message
    auto id = message->id;
    if (id.startsWith(ANSWER_PREFIX))
    {
        id = id.mid(ANSWER_PREFIX.size());
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
                if (split->getChannelView().scrollToMessageId(id))
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
