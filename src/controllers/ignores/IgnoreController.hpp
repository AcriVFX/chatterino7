// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <vector>

namespace chatterino {

class IgnorePhrase;
struct Message;
struct TwitchEmoteOccurrence;

enum class ShowIgnoredUsersMessages { Never, IfModerator, IfBroadcaster };

struct IgnoredMessageParameters {
    QString message;

    QString twitchUserID;
    QString twitchUserLogin;
    bool isMod;
    bool isBroadcaster;
};

bool isIgnoredMessage(IgnoredMessageParameters &&params);

/// Whether `text` contains `login` as a whole word (case-insensitive), with
/// or without a leading @. "acrivfx" is mentioned in "@AcriVFX hi" but not in
/// "acrivfx_fan hi".
bool mentionsLogin(const QString &text, const QString &login);

/// Whether a split should leave this message out because its sender is on
/// the hidden users list. Messages that mention the current user or reply to
/// them stay (if showHiddenUsersMentions is on), and so do system, moderation
/// and AutoMod messages and whispers.
bool isHiddenMessage(const Message &message);

/// @brief Processes replacement ignore-phrases for a message
///
/// @param phrases A list of IgnorePhrases to process. Block phrases as well as
/// 	           invalid phrases are ignored.
/// @param content The message text. This gets altered by replacements.
/// @param twitchEmotes A list of emotes present in the message. Occurrences
///                     that have been removed from the message will also be
///                     removed in this list. Similarly, if new emotes are added
///                     from a replacement, this list gets updated as well.
void processIgnorePhrases(const std::vector<IgnorePhrase> &phrases,
                          QString &content,
                          std::vector<TwitchEmoteOccurrence> &twitchEmotes);

}  // namespace chatterino
