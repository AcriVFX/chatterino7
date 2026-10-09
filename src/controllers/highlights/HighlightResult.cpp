// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightResult.hpp"

#include <QRegularExpression>

namespace chatterino {

HighlightResult::HighlightResult(bool _alert, bool _playSound,
                                 std::optional<QUrl> _customSoundUrl,
                                 std::shared_ptr<QColor> _color,
                                 bool _showInMentions)
    : alert(_alert)
    , playSound(_playSound)
    , customSoundUrl(std::move(_customSoundUrl))
    , color(std::move(_color))
    , showInMentions(_showInMentions)
{
}

HighlightResult HighlightResult::emptyResult()
{
    return {
        false, false, std::nullopt, nullptr, false,
    };
}

bool HighlightResult::operator==(const HighlightResult &other) const
{
    if (this->alert != other.alert)
    {
        return false;
    }
    if (this->playSound != other.playSound)
    {
        return false;
    }
    if (this->customSoundUrl != other.customSoundUrl)
    {
        return false;
    }

    if (this->color && other.color)
    {
        if (*this->color != *other.color)
        {
            return false;
        }
    }

    if (this->showInMentions != other.showInMentions)
    {
        return false;
    }

    return true;
}

bool HighlightResult::operator!=(const HighlightResult &other) const
{
    return !(*this == other);
}

bool HighlightResult::empty() const
{
    return !this->alert && !this->playSound &&
           !this->customSoundUrl.has_value() && !this->color &&
           !this->showInMentions;
}

bool HighlightResult::full() const
{
    return this->alert && this->playSound && this->customSoundUrl.has_value() &&
           this->color && this->showInMentions;
}

std::ostream &operator<<(std::ostream &os, const HighlightResult &result)
{
    os << "Alert: " << (result.alert ? "Yes" : "No") << ", "
       << "Play sound: " << (result.playSound ? "Yes" : "No") << " ("
       << (result.customSoundUrl
               ? result.customSoundUrl->toString().toStdString()
               : "")
       << ")" << ", "
       << "Color: " << (result.color ? result.color->name().toStdString() : "")
       << ", "
       << "Show in mentions: " << (result.showInMentions ? "Yes" : "No");
    return os;
}

QString highlightLabelFromPattern(const QString &pattern)
{
    if (!pattern.startsWith(u"(?#"))
    {
        return {};
    }
    auto end = pattern.indexOf(u')');
    if (end < 0)
    {
        return {};
    }
    return pattern.mid(3, end - 3).trimmed();
}

QString highlightTagFromLabel(const QString &label)
{
    static const QRegularExpression bracketTag(R"(\[([^\]]+)\])");
    static const QRegularExpression firstWord(R"([A-Za-z0-9]+)");

    auto match = bracketTag.match(label);
    if (match.hasMatch())
    {
        return match.captured(1).trimmed().toUpper();
    }
    match = firstWord.match(label);
    if (match.hasMatch())
    {
        return match.captured(0).toUpper();
    }
    return {};
}

}  // namespace chatterino
