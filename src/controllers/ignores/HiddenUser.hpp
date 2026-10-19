// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/RapidjsonHelpers.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <pajlada/serialize.hpp>
#include <QString>

#include <utility>

namespace chatterino {

/// A user whose chat messages are hidden in every split (see
/// isHiddenMessage). Local to Chatterino, nothing is sent to Twitch.
class HiddenUser
{
public:
    HiddenUser(const QString &name, QString since)
        // "@Name" typed in the settings table matches too
        : name_(name.trimmed().startsWith(u'@') ? name.trimmed().mid(1)
                                                : name.trimmed())
        , since_(std::move(since))
    {
    }

    bool operator==(const HiddenUser &other) const
    {
        return std::tie(this->name_, this->since_) ==
               std::tie(other.name_, other.since_);
    }

    const QString &getName() const
    {
        return this->name_;
    }

    /// Date the user was hidden, as yyyy-MM-dd
    const QString &getSince() const
    {
        return this->since_;
    }

    bool isMatch(const QString &loginName) const
    {
        return this->name_.compare(loginName, Qt::CaseInsensitive) == 0;
    }

private:
    QString name_;
    QString since_;
};

}  // namespace chatterino

namespace pajlada {

template <>
struct Serialize<chatterino::HiddenUser> {
    static rapidjson::Value get(const chatterino::HiddenUser &value,
                                rapidjson::Document::AllocatorType &a)
    {
        rapidjson::Value ret(rapidjson::kObjectType);

        chatterino::rj::set(ret, "name", value.getName(), a);
        chatterino::rj::set(ret, "since", value.getSince(), a);

        return ret;
    }
};

template <>
struct Deserialize<chatterino::HiddenUser> {
    static chatterino::HiddenUser get(const rapidjson::Value &value,
                                      bool *error = nullptr)
    {
        QString name;
        QString since;

        if (!value.IsObject())
        {
            PAJLADA_REPORT_ERROR(error)
            return {name, since};
        }

        chatterino::rj::getSafe(value, "name", name);
        chatterino::rj::getSafe(value, "since", since);

        return {name, since};
    }
};

}  // namespace pajlada
