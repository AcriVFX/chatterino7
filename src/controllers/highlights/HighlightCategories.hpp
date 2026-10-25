// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

/// Highlight categories: message highlight rules that share a `(?#LABEL)`
/// comment (minus a trailing " N") form one category. Its tag (the `[TAG]` in
/// the label, else its first word) is what the lane filters match on.
///
/// A category's word list is the rule whose body is
/// `(?:\b|\s|^)(?:word|other word)(?:\b|\s|$)`, the same format the
/// highlight generator script writes.
///
/// Everything here works on plain values so the settings page only has to
/// convert from and to HighlightPhrase and FilterRecord.
namespace chatterino::highlightcategories {

struct Rule {
    QString pattern;
    bool showInMentions = false;
    bool alert = false;
    bool sound = false;
    bool regex = true;
    bool caseSensitive = false;
    QString soundUrl;
    QColor color;

    bool operator==(const Rule &other) const = default;
};

struct Category {
    QString name;
    QString tag;
    /// Indices into the rule list, in order
    std::vector<int> rules;
    /// Index of the word list rule, -1 if the category has none
    int wordRule = -1;
    /// Words as stored (longest first)
    QStringList words;
    /// Rules that are neither the word list nor an empty placeholder
    int advancedCount = 0;
};

/// Body of a new category without words yet, it never matches
inline const QString PLACEHOLDER_BODY = QStringLiteral("(?!)");

/// The text inside the leading (?#...), empty if there is none
QString labelOf(const QString &pattern);
/// The pattern without the leading (?#...)
QString bodyOf(const QString &pattern);
/// Strips the " N" a generated label gets when a category has several rules
QString categoryName(const QString &label);
/// Same rule as highlightTagFromLabel: [TAG] in the name, else the first word
QString tagOf(const QString &name);
/// Returns name changed so tagOf() gives tag
QString withTag(const QString &name, const QString &tag);
/// Removes characters a (?#...) label can't hold
QString cleanName(const QString &name);

QString normalizeWord(const QString &word);
/// Escapes like Python's re.escape so the generator script reads it back
QString escapeWord(const QString &word);
/// The words of a word list body, nullopt if body is anything else
std::optional<QStringList> parseWordList(const QString &body);
QString buildWordList(QStringList words);

/// Categories in the order they first appear
std::vector<Category> categories(const std::vector<Rule> &rules);
/// Indices of rules without a label
std::vector<int> unsortedRules(const std::vector<Rule> &rules);

std::vector<Rule> addWord(std::vector<Rule> rules, const QString &name,
                          const QString &word);
std::vector<Rule> removeWords(std::vector<Rule> rules, const QString &name,
                              const QStringList &words);
std::vector<Rule> renameCategory(std::vector<Rule> rules, const QString &name,
                                 const QString &newName);
std::vector<Rule> setCategoryColor(std::vector<Rule> rules, const QString &name,
                                   const QColor &color);
std::vector<Rule> setCategoryFlags(std::vector<Rule> rules, const QString &name,
                                   bool showInMentions, bool alert, bool sound);
/// Swaps the category with the previous (-1) or next (+1) category
std::vector<Rule> moveCategory(std::vector<Rule> rules, const QString &name,
                               int direction);
/// Adds an empty category above the category named before (at the bottom
/// if before is empty or unknown)
std::vector<Rule> createCategory(std::vector<Rule> rules, const QString &name,
                                 const QColor &color, const QString &before);
/// Moves a rule without a label into a category. A plain (non-regex) rule
/// joins the word list, a regex rule becomes one of the category's patterns.
std::vector<Rule> moveUnsortedRule(std::vector<Rule> rules, int index,
                                   const QString &name);

/// Tags of a lane filter (`{"A", "B"} contains message.highlight_tag`),
/// nullopt if the filter isn't one
std::optional<QStringList> laneTags(const QString &filter);
/// The filter with tag added to (present) or removed from its tag set
QString withLaneTag(const QString &filter, const QString &tag, bool present);

}  // namespace chatterino::highlightcategories
