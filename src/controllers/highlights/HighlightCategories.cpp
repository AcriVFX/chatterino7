// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightCategories.hpp"

#include <QRegularExpression>

#include <algorithm>

namespace chatterino::highlightcategories {

namespace {

const QString WORDS_START = QStringLiteral(R"((?:\b|\s|^)(?:)");
const QString WORDS_END = QStringLiteral(R"()(?:\b|\s|$))");
// Characters Python's re.escape escapes (3.7+)
const QString SPECIAL = QStringLiteral("()[]{}?*+-|^$\\.&~# \t\n\r\v\f");

QString makePattern(const QString &label, const QString &body)
{
    return QStringLiteral("(?#") + label + QStringLiteral(")") + body;
}

const Category *find(const std::vector<Category> &cats, const QString &name)
{
    auto it = std::find_if(cats.begin(), cats.end(), [&](const auto &c) {
        return c.name == name;
    });
    return it == cats.end() ? nullptr : &*it;
}

/// Renumbers the labels of a category: "NAME" for one rule, "NAME 1".. else
void relabel(std::vector<Rule> &rules, const QString &name)
{
    std::vector<int> indices;
    for (int i = 0; i < static_cast<int>(rules.size()); i++)
    {
        auto label = labelOf(rules[i].pattern);
        if (!label.isEmpty() && categoryName(label) == name)
        {
            indices.push_back(i);
        }
    }
    for (size_t n = 0; n < indices.size(); n++)
    {
        auto &rule = rules[indices[n]];
        auto label = indices.size() == 1
                         ? name
                         : QStringLiteral("%1 %2").arg(name).arg(n + 1);
        rule.pattern = makePattern(label, bodyOf(rule.pattern));
    }
}

/// A new rule of the category, with the flags and color of its first rule
Rule ruleLike(const Rule &first, const QString &name, const QString &body)
{
    Rule rule = first;
    rule.pattern = makePattern(name, body);
    rule.regex = true;
    rule.caseSensitive = false;
    return rule;
}

void erasePlaceholder(std::vector<Rule> &rules, const QString &name)
{
    for (int i = static_cast<int>(rules.size()) - 1; i >= 0; i--)
    {
        auto label = labelOf(rules[i].pattern);
        if (!label.isEmpty() && categoryName(label) == name &&
            bodyOf(rules[i].pattern) == PLACEHOLDER_BODY)
        {
            rules.erase(rules.begin() + i);
        }
    }
}

}  // namespace

QString labelOf(const QString &pattern)
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

QString bodyOf(const QString &pattern)
{
    if (!pattern.startsWith(u"(?#"))
    {
        return pattern;
    }
    auto end = pattern.indexOf(u')');
    if (end < 0)
    {
        return pattern;
    }
    return pattern.mid(end + 1);
}

QString categoryName(const QString &label)
{
    static const QRegularExpression number(R"(\s+\d+$)");
    auto name = label;
    name.remove(number);
    return name.trimmed();
}

QString tagOf(const QString &name)
{
    static const QRegularExpression bracketTag(R"(\[([^\]]+)\])");
    static const QRegularExpression firstWord(R"([A-Za-z0-9]+)");

    auto match = bracketTag.match(name);
    if (match.hasMatch())
    {
        return match.captured(1).trimmed().toUpper();
    }
    match = firstWord.match(name);
    if (match.hasMatch())
    {
        return match.captured(0).toUpper();
    }
    return {};
}

QString withTag(const QString &name, const QString &tag)
{
    static const QRegularExpression bracketTag(R"(\[([^\]]+)\])");
    static const QRegularExpression firstWord(R"([A-Za-z0-9]+)");

    auto cleanTag = tag.trimmed().toUpper();
    cleanTag.remove(QRegularExpression(R"([\[\]\)])"));
    if (cleanTag.isEmpty() || tagOf(name) == cleanTag)
    {
        return name;
    }

    auto bracket = bracketTag.match(name);
    if (bracket.hasMatch())
    {
        auto out = name;
        out.replace(bracket.capturedStart(1), bracket.capturedLength(1),
                    cleanTag);
        return out;
    }
    auto word = firstWord.match(name);
    if (word.hasMatch() && word.captured(0).toUpper() == cleanTag)
    {
        return name;
    }
    if (!word.hasMatch())
    {
        return QStringLiteral("%1 [%2]").arg(name.trimmed(), cleanTag);
    }
    auto out = name;
    out.insert(word.capturedEnd(0), QStringLiteral(" [%1]").arg(cleanTag));
    return out;
}

QString cleanName(const QString &name)
{
    auto out = name;
    out.remove(u')');
    out.replace(u'\n', u' ');
    return out.simplified();
}

QString normalizeWord(const QString &word)
{
    return word.simplified().toLower();
}

QString escapeWord(const QString &word)
{
    QString out;
    out.reserve(word.size() * 2);
    for (auto c : word)
    {
        if (SPECIAL.contains(c))
        {
            out += u'\\';
        }
        out += c;
    }
    return out;
}

std::optional<QStringList> parseWordList(const QString &body)
{
    if (!body.startsWith(WORDS_START) || !body.endsWith(WORDS_END) ||
        body.size() <= WORDS_START.size() + WORDS_END.size())
    {
        return std::nullopt;
    }
    auto inner = body.mid(WORDS_START.size(),
                          body.size() - WORDS_START.size() - WORDS_END.size());

    QStringList words;
    QString current;
    for (int i = 0; i < inner.size(); i++)
    {
        auto c = inner[i];
        if (c == u'\\')
        {
            if (i + 1 >= inner.size())
            {
                return std::nullopt;
            }
            current += inner[++i];
        }
        else if (c == u'|')
        {
            words.append(current);
            current.clear();
        }
        else if (SPECIAL.contains(c))
        {
            // an unescaped regex character: not a plain word list
            return std::nullopt;
        }
        else
        {
            current += c;
        }
    }
    words.append(current);

    // Only accept what we would write ourselves, so hand-made regexes that
    // happen to look similar stay untouched
    QStringList escaped;
    for (const auto &w : words)
    {
        if (w.isEmpty())
        {
            return std::nullopt;
        }
        escaped.append(escapeWord(w));
    }
    if (escaped.join(u'|') != inner)
    {
        return std::nullopt;
    }
    return words;
}

QString buildWordList(QStringList words)
{
    for (auto &w : words)
    {
        w = normalizeWord(w);
    }
    words.removeAll(QString());
    words.removeDuplicates();
    // longest first, like the generator script
    std::sort(words.begin(), words.end(), [](const auto &a, const auto &b) {
        if (a.size() != b.size())
        {
            return a.size() > b.size();
        }
        return a < b;
    });
    QStringList escaped;
    for (const auto &w : words)
    {
        escaped.append(escapeWord(w));
    }
    return WORDS_START + escaped.join(u'|') + WORDS_END;
}

std::vector<Category> categories(const std::vector<Rule> &rules)
{
    std::vector<Category> out;
    for (int i = 0; i < static_cast<int>(rules.size()); i++)
    {
        auto label = labelOf(rules[i].pattern);
        if (label.isEmpty())
        {
            continue;
        }
        auto name = categoryName(label);
        auto it = std::find_if(out.begin(), out.end(), [&](const auto &c) {
            return c.name == name;
        });
        if (it == out.end())
        {
            out.push_back(Category{name, tagOf(name), {}, -1, {}, 0});
            it = out.end() - 1;
        }
        it->rules.push_back(i);

        auto body = bodyOf(rules[i].pattern);
        if (body == PLACEHOLDER_BODY)
        {
            continue;
        }
        auto words = rules[i].regex ? parseWordList(body) : std::nullopt;
        if (words && it->wordRule < 0)
        {
            it->wordRule = i;
            it->words = *words;
        }
        else
        {
            it->advancedCount++;
        }
    }
    return out;
}

std::vector<int> unsortedRules(const std::vector<Rule> &rules)
{
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(rules.size()); i++)
    {
        if (labelOf(rules[i].pattern).isEmpty())
        {
            out.push_back(i);
        }
    }
    return out;
}

std::vector<Rule> addWord(std::vector<Rule> rules, const QString &name,
                          const QString &word)
{
    auto w = normalizeWord(word);
    auto cats = categories(rules);
    const auto *cat = find(cats, name);
    if (w.isEmpty() || cat == nullptr)
    {
        return rules;
    }

    if (cat->wordRule >= 0)
    {
        auto words = cat->words;
        if (words.contains(w))
        {
            return rules;
        }
        words.append(w);
        auto &rule = rules[cat->wordRule];
        rule.pattern = makePattern(labelOf(rule.pattern), buildWordList(words));
        return rules;
    }

    auto first = cat->rules.front();
    rules.insert(rules.begin() + first,
                 ruleLike(rules[first], name, buildWordList({w})));
    erasePlaceholder(rules, name);
    relabel(rules, name);
    return rules;
}

std::vector<Rule> removeWords(std::vector<Rule> rules, const QString &name,
                              const QStringList &words)
{
    auto cats = categories(rules);
    const auto *cat = find(cats, name);
    if (cat == nullptr || cat->wordRule < 0)
    {
        return rules;
    }

    auto remaining = cat->words;
    for (const auto &w : words)
    {
        remaining.removeAll(w);
    }
    auto &rule = rules[cat->wordRule];
    if (!remaining.isEmpty())
    {
        rule.pattern =
            makePattern(labelOf(rule.pattern), buildWordList(remaining));
        return rules;
    }

    if (cat->rules.size() > 1)
    {
        rules.erase(rules.begin() + cat->wordRule);
    }
    else
    {
        // keep the category, an empty word list would match everything
        rule.pattern = makePattern(name, PLACEHOLDER_BODY);
    }
    relabel(rules, name);
    return rules;
}

std::vector<Rule> renameCategory(std::vector<Rule> rules, const QString &name,
                                 const QString &newName)
{
    auto clean = cleanName(newName);
    auto cats = categories(rules);
    const auto *cat = find(cats, name);
    if (cat == nullptr || clean.isEmpty() || clean == name ||
        categoryName(clean) != clean || find(cats, clean) != nullptr)
    {
        return rules;
    }
    for (auto i : cat->rules)
    {
        // temporary label, relabel() numbers them
        rules[i].pattern = makePattern(clean, bodyOf(rules[i].pattern));
    }
    relabel(rules, clean);
    return rules;
}

std::vector<Rule> setCategoryColor(std::vector<Rule> rules, const QString &name,
                                   const QColor &color)
{
    auto cats = categories(rules);
    if (const auto *cat = find(cats, name))
    {
        for (auto i : cat->rules)
        {
            rules[i].color = color;
        }
    }
    return rules;
}

std::vector<Rule> setCategoryFlags(std::vector<Rule> rules, const QString &name,
                                   bool showInMentions, bool alert, bool sound)
{
    auto cats = categories(rules);
    if (const auto *cat = find(cats, name))
    {
        for (auto i : cat->rules)
        {
            rules[i].showInMentions = showInMentions;
            rules[i].alert = alert;
            rules[i].sound = sound;
        }
    }
    return rules;
}

std::vector<Rule> moveCategory(std::vector<Rule> rules, const QString &name,
                               int direction)
{
    // Units in list order: each category (its rules gathered where it first
    // appears) or a single unsorted rule. Unsorted rules keep their place.
    struct Unit {
        QString name;
        std::vector<int> rules;
    };
    std::vector<Unit> units;
    for (int i = 0; i < static_cast<int>(rules.size()); i++)
    {
        auto label = labelOf(rules[i].pattern);
        auto cat = label.isEmpty() ? QString() : categoryName(label);
        auto it = cat.isEmpty() ? units.end()
                                : std::find_if(units.begin(), units.end(),
                                               [&](const auto &u) {
                                                   return u.name == cat;
                                               });
        if (it == units.end())
        {
            units.push_back(Unit{cat, {i}});
        }
        else
        {
            it->rules.push_back(i);
        }
    }

    auto self = std::find_if(units.begin(), units.end(), [&](const auto &u) {
        return u.name == name;
    });
    if (self == units.end() || name.isEmpty())
    {
        return rules;
    }
    auto pos = static_cast<int>(self - units.begin());
    auto other = pos + direction;
    while (other >= 0 && other < static_cast<int>(units.size()) &&
           units[other].name.isEmpty())
    {
        other += direction;
    }
    if (other < 0 || other >= static_cast<int>(units.size()))
    {
        return rules;
    }
    std::swap(units[pos], units[other]);

    std::vector<Rule> out;
    out.reserve(rules.size());
    for (const auto &u : units)
    {
        for (auto i : u.rules)
        {
            out.push_back(rules[i]);
        }
    }
    return out;
}

std::vector<Rule> createCategory(std::vector<Rule> rules, const QString &name,
                                 const QColor &color, const QString &before)
{
    auto clean = cleanName(name);
    auto cats = categories(rules);
    if (clean.isEmpty() || categoryName(clean) != clean ||
        find(cats, clean) != nullptr)
    {
        return rules;
    }

    Rule rule;
    rule.pattern = makePattern(clean, PLACEHOLDER_BODY);
    rule.color = color;

    const auto *at = find(cats, before);
    auto index =
        at != nullptr ? at->rules.front() : static_cast<int>(rules.size());
    rules.insert(rules.begin() + index, rule);
    return rules;
}

std::vector<Rule> moveUnsortedRule(std::vector<Rule> rules, int index,
                                   const QString &name)
{
    if (index < 0 || index >= static_cast<int>(rules.size()) ||
        !labelOf(rules[index].pattern).isEmpty())
    {
        return rules;
    }
    auto cats = categories(rules);
    const auto *cat = find(cats, name);
    if (cat == nullptr)
    {
        return rules;
    }

    auto moved = rules[index];
    if (!moved.regex)
    {
        rules.erase(rules.begin() + index);
        return addWord(std::move(rules), name, moved.pattern);
    }

    auto rule = ruleLike(rules[cat->rules.front()], name, moved.pattern);
    auto after = cat->rules.back() + 1;
    rules.insert(rules.begin() + after, rule);
    rules.erase(rules.begin() + (index < after ? index : index + 1));
    erasePlaceholder(rules, name);
    relabel(rules, name);
    return rules;
}

namespace {

const QRegularExpression &laneRegex()
{
    static const QRegularExpression re(
        R"((\{[^{}]*\})(\s*contains\s+message\.highlight_tag\b))");
    return re;
}

QStringList tagsIn(const QString &set)
{
    static const QRegularExpression item(R"x("((?:[^"\\]|\\.)*)")x");
    QStringList tags;
    auto it = item.globalMatch(set);
    while (it.hasNext())
    {
        tags.append(it.next().captured(1));
    }
    return tags;
}

}  // namespace

std::optional<QStringList> laneTags(const QString &filter)
{
    auto match = laneRegex().match(filter);
    if (!match.hasMatch())
    {
        return std::nullopt;
    }
    return tagsIn(match.captured(1));
}

QString withLaneTag(const QString &filter, const QString &tag, bool present)
{
    auto match = laneRegex().match(filter);
    if (!match.hasMatch() || tag.isEmpty())
    {
        return filter;
    }
    auto tags = tagsIn(match.captured(1));
    if (tags.contains(tag) == present)
    {
        return filter;
    }
    if (present)
    {
        tags.removeAll(QStringLiteral("NONE"));
        tags.append(tag);
    }
    else
    {
        tags.removeAll(tag);
        if (tags.isEmpty())
        {
            // an empty set may not parse, and "" would match every
            // unhighlighted message
            tags.append(QStringLiteral("NONE"));
        }
    }
    QStringList quoted;
    for (const auto &t : tags)
    {
        quoted.append(QStringLiteral("\"%1\"").arg(t));
    }
    auto out = filter;
    out.replace(match.capturedStart(1), match.capturedLength(1),
                QStringLiteral("{") + quoted.join(QStringLiteral(", ")) +
                    QStringLiteral("}"));
    return out;
}

}  // namespace chatterino::highlightcategories
