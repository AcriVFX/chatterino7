// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/HighlightCategories.hpp"

#include "Test.hpp"

using namespace chatterino::highlightcategories;

namespace {

Rule rule(const QString &pattern, QColor color = QColor(0xff, 0x80, 0x00, 0x80))
{
    Rule r;
    r.pattern = pattern;
    r.color = color;
    return r;
}

QStringList patterns(const std::vector<Rule> &rules)
{
    QStringList out;
    for (const auto &r : rules)
    {
        out.append(r.pattern);
    }
    return out;
}

// A cut-down copy of the generated rules (build_highlights.py)
std::vector<Rule> sample()
{
    return {
        rule(R"((?#ME: Mention)(?:\b|\s|^)(?:acrivfx|acris|acri)(?:\b|\s|$))"),
        rule(
            R"((?#INSULT: Insults 1)(?:\b|\s|^)(?:fick\ dich|simp|hs)(?:\b|\s|$))"),
        rule(R"((?#INSULT: Insults 2)\bbastard\w*)"),
        rule(R"((?#BAN: Dead project)\bt[oö]te?[sr]?\W*projekt\w*)"),
        rule(R"((?#SPAM: Spam 1)⣿)"),
        rule(R"((?#SPAM: Spam 2)\b[lL]+\b)"),
        rule(
            R"((?#STREAMER NAMES [NAME])(?:\b|\s|^)(?:knossi|eli)(?:\b|\s|$))"),
    };
}

}  // namespace

TEST(HighlightCategories, Labels)
{
    EXPECT_EQ(labelOf("(?#INSULT: Insults 2)\\bx"), "INSULT: Insults 2");
    EXPECT_EQ(bodyOf("(?#INSULT: Insults 2)\\bx"), "\\bx");
    EXPECT_EQ(labelOf("egg"), "");
    EXPECT_EQ(bodyOf("egg"), "egg");
    EXPECT_EQ(categoryName("INSULT: Insults 2"), "INSULT: Insults");
    EXPECT_EQ(categoryName("SPOILER: RDR2"), "SPOILER: RDR2");
    EXPECT_EQ(categoryName("POLITICS 1"), "POLITICS");
    EXPECT_EQ(tagOf("LINKS [LINK]: Links & phone numbers"), "LINK");
    EXPECT_EQ(tagOf("INSULT: Insults"), "INSULT");
}

TEST(HighlightCategories, WithTag)
{
    EXPECT_EQ(withTag("SCAM: Fake giveaways", "scam"), "SCAM: Fake giveaways");
    EXPECT_EQ(withTag("SCAM: Fake giveaways", "FAKE"),
              "SCAM [FAKE]: Fake giveaways");
    EXPECT_EQ(withTag("LINKS [LINK]: Links", "URL"), "LINKS [URL]: Links");
    EXPECT_EQ(withTag("LINKS [LINK]: Links", ""), "LINKS [LINK]: Links");
    EXPECT_EQ(withTag("SCAM: Fake", "q\"z"), "SCAM [QZ]: Fake");
    EXPECT_EQ(withTag("SCAM: Fake", "\"\""), "SCAM: Fake");
}

TEST(HighlightCategories, WordListRoundTrip)
{
    // as written by Python's re.escape in the generator
    QString body =
        R"((?:\b|\s|^)(?:wenn\ du\ meine\ arbeit|n!\*\*3r|ne99a|piç|🦅)(?:\b|\s|$))";
    auto words = parseWordList(body);
    ASSERT_TRUE(words.has_value());
    EXPECT_EQ(*words, QStringList({"wenn du meine arbeit", "n!**3r", "ne99a",
                                   "piç", "🦅"}));

    EXPECT_EQ(buildWordList({"hs", "Fick dich", "simp", "hs"}),
              R"((?:\b|\s|^)(?:fick\ dich|simp|hs)(?:\b|\s|$))");

    // hand-written regexes are not word lists
    EXPECT_FALSE(parseWordList(R"((?:\b|\s|^)(?:a.b|c)(?:\b|\s|$))"));
    EXPECT_FALSE(parseWordList(R"((?:\b|\s|^)(?:)(?:\b|\s|$))"));
    EXPECT_FALSE(parseWordList(R"(\bbastard\w*)"));
    EXPECT_EQ(buildWordList({" ", ""}), PLACEHOLDER_BODY);
}

TEST(HighlightCategories, CaseSensitiveIsNotAWordList)
{
    auto r = rule(R"((?#SPAM: Caps)(?:\b|\s|^)(?:LOL|W)(?:\b|\s|$))");
    r.caseSensitive = true;
    auto cats = categories({r});
    EXPECT_EQ(cats[0].wordRule, -1);
    EXPECT_EQ(cats[0].advancedCount, 1);
    // so adding a word makes a separate word list and leaves LOL|W alone
    auto rules = addWord({r}, "SPAM: Caps", "xd");
    ASSERT_EQ(rules.size(), 2U);
    EXPECT_EQ(rules[1].pattern,
              R"((?#SPAM: Caps 2)(?:\b|\s|^)(?:LOL|W)(?:\b|\s|$))");
    EXPECT_TRUE(rules[1].caseSensitive);
    EXPECT_FALSE(rules[0].caseSensitive);
}

TEST(HighlightCategories, Categories)
{
    auto rules = sample();
    rules.push_back(rule("egg"));
    auto cats = categories(rules);
    ASSERT_EQ(cats.size(), 5U);
    EXPECT_EQ(cats[1].name, "INSULT: Insults");
    EXPECT_EQ(cats[1].tag, "INSULT");
    EXPECT_EQ(cats[1].wordRule, 1);
    EXPECT_EQ(cats[1].words, QStringList({"fick dich", "simp", "hs"}));
    EXPECT_EQ(cats[1].advancedCount, 1);
    EXPECT_EQ(cats[2].wordRule, -1);
    EXPECT_EQ(cats[4].tag, "NAME");
    EXPECT_EQ(unsortedRules(rules), std::vector<int>({7}));
}

TEST(HighlightCategories, AddWord)
{
    auto rules = addWord(sample(), "INSULT: Insults", " Arschloch ");
    EXPECT_EQ(
        rules[1].pattern,
        R"((?#INSULT: Insults 1)(?:\b|\s|^)(?:arschloch|fick\ dich|simp|hs)(?:\b|\s|$))");
    EXPECT_EQ(rules.size(), sample().size());

    // a category without a word list gets one in front, labels renumbered
    rules = addWord(sample(), "BAN: Dead project", "dead project");
    EXPECT_EQ(rules.size(), sample().size() + 1);
    EXPECT_EQ(
        rules[3].pattern,
        R"((?#BAN: Dead project 1)(?:\b|\s|^)(?:dead\ project)(?:\b|\s|$))");
    EXPECT_EQ(rules[4].pattern,
              R"((?#BAN: Dead project 2)\bt[oö]te?[sr]?\W*projekt\w*)");
    EXPECT_EQ(rules[3].color, rules[4].color);
    EXPECT_TRUE(rules[3].regex);
}

TEST(HighlightCategories, RemoveWords)
{
    auto rules = removeWords(sample(), "INSULT: Insults", {"simp"});
    EXPECT_EQ(
        rules[1].pattern,
        R"((?#INSULT: Insults 1)(?:\b|\s|^)(?:fick\ dich|hs)(?:\b|\s|$))");

    // removing every word drops the word rule if patterns remain
    rules =
        removeWords(sample(), "INSULT: Insults", {"simp", "hs", "fick dich"});
    EXPECT_EQ(rules.size(), sample().size() - 1);
    EXPECT_EQ(rules[1].pattern, R"((?#INSULT: Insults)\bbastard\w*)");

    // ...and leaves a placeholder that never matches otherwise
    rules = removeWords(sample(), "ME: Mention", {"acrivfx", "acris", "acri"});
    EXPECT_EQ(rules[0].pattern, "(?#ME: Mention)(?!)");
    EXPECT_EQ(categories(rules)[0].words.size(), 0U);
    rules = addWord(rules, "ME: Mention", "phil");
    EXPECT_EQ(rules[0].pattern,
              R"((?#ME: Mention)(?:\b|\s|^)(?:phil)(?:\b|\s|$))");
    EXPECT_EQ(rules.size(), sample().size());
}

TEST(HighlightCategories, Rename)
{
    auto rules =
        renameCategory(sample(), "INSULT: Insults", "INSULT [TO]: Rude");
    EXPECT_EQ(labelOf(rules[1].pattern), "INSULT [TO]: Rude 1");
    EXPECT_EQ(labelOf(rules[2].pattern), "INSULT [TO]: Rude 2");
    EXPECT_EQ(categories(rules)[1].tag, "TO");

    // names that would merge or break the label are refused
    EXPECT_EQ(renameCategory(sample(), "INSULT: Insults", "SPAM: Spam"),
              sample());
    EXPECT_EQ(renameCategory(sample(), "INSULT: Insults", "INSULT 2"),
              sample());
    EXPECT_EQ(
        labelOf(renameCategory(sample(), "ME: Mention", "ME: (Me)")[0].pattern),
        "ME: (Me");
}

TEST(HighlightCategories, ColorAndFlags)
{
    QColor blue(0, 0, 255, 128);
    auto rules = setCategoryColor(sample(), "SPAM: Spam", blue);
    EXPECT_EQ(rules[4].color, blue);
    EXPECT_EQ(rules[5].color, blue);
    EXPECT_NE(rules[3].color, blue);

    // one flag changes, the others stay per rule
    rules = sample();
    rules[2].alert = true;
    rules =
        setCategoryFlag(rules, "INSULT: Insults", Flag::ShowInMentions, true);
    EXPECT_TRUE(rules[1].showInMentions && rules[2].showInMentions);
    EXPECT_FALSE(rules[1].alert);
    EXPECT_TRUE(rules[2].alert);
    EXPECT_FALSE(rules[0].showInMentions);
}

TEST(HighlightCategories, Move)
{
    auto rules = sample();
    rules.insert(rules.begin() + 4, rule("egg"));
    auto moved = moveCategory(rules, "SPAM: Spam", -1);
    // SPAM jumps over the unsorted "egg", which stays in place
    EXPECT_EQ(patterns(moved).mid(3, 4),
              QStringList({rules[5].pattern, rules[6].pattern, rules[3].pattern,
                           "egg"}));
    EXPECT_EQ(moveCategory(rules, "ME: Mention", -1), rules);
    EXPECT_EQ(moveCategory(moveCategory(rules, "INSULT: Insults", 1),
                           "INSULT: Insults", -1),
              rules);
}

TEST(HighlightCategories, MoveKeepsOtherCategoriesInOrder)
{
    std::vector<Rule> rules{rule("(?#BAN 1)a"), rule("(?#INSULT)b"),
                            rule("(?#BAN 2)c"), rule("(?#SPAM)d")};
    // SPAM goes above INSULT; BAN 2 stays below INSULT
    EXPECT_EQ(
        patterns(moveCategory(rules, "SPAM", -1)),
        QStringList({"(?#BAN 1)a", "(?#SPAM)d", "(?#INSULT)b", "(?#BAN 2)c"}));
    EXPECT_EQ(
        patterns(moveCategory(rules, "INSULT", 1)),
        QStringList({"(?#BAN 1)a", "(?#BAN 2)c", "(?#SPAM)d", "(?#INSULT)b"}));
}

TEST(HighlightCategories, Create)
{
    QColor pink(0xff, 0x4f, 0xa0, 0x5a);
    auto rules =
        createCategory(sample(), "SCAM: Fake giveaways", pink, "SPAM: Spam");
    EXPECT_EQ(rules[4].pattern, "(?#SCAM: Fake giveaways)(?!)");
    EXPECT_EQ(rules[4].color, pink);
    EXPECT_EQ(categories(rules)[3].name, "SCAM: Fake giveaways");

    rules = createCategory(sample(), "SCAM", pink, "");
    EXPECT_EQ(rules.back().pattern, "(?#SCAM)(?!)");
    EXPECT_EQ(createCategory(sample(), "SPAM: Spam", pink, ""), sample());
}

TEST(HighlightCategories, MoveUnsorted)
{
    auto rules = sample();
    auto egg = rule("egg");
    egg.regex = false;
    egg.alert = true;
    rules.push_back(egg);
    rules.push_back(rule(R"(\bscam\w*)"));

    auto moved = moveUnsortedRule(rules, 7, "STREAMER NAMES [NAME]");
    EXPECT_EQ(moved.size(), rules.size() - 1);
    EXPECT_EQ(
        moved[6].pattern,
        R"((?#STREAMER NAMES [NAME])(?:\b|\s|^)(?:knossi|egg|eli)(?:\b|\s|$))");

    moved = moveUnsortedRule(rules, 8, "INSULT: Insults");
    EXPECT_EQ(moved.size(), rules.size());
    EXPECT_EQ(moved[3].pattern, R"((?#INSULT: Insults 3)\bscam\w*)");
    EXPECT_EQ(moved[3].color, rules[1].color);
    EXPECT_EQ(moved[8].pattern, "egg");

    // labeled rules can't be "moved"
    EXPECT_EQ(moveUnsortedRule(rules, 0, "INSULT: Insults"), rules);
}

TEST(HighlightCategories, LaneFilters)
{
    QString fyi =
        R"({"LINK", "REQUEST", "NAME"} contains message.highlight_tag || (flags.first_message && !flags.highlighted) && !(author.name == "fossabot"))";
    auto tags = laneTags(fyi);
    ASSERT_TRUE(tags.has_value());
    EXPECT_EQ(*tags, QStringList({"LINK", "REQUEST", "NAME"}));
    EXPECT_FALSE(laneTags(R"(author.name == "fossabot")"));
    // only filters that start with the tag set are lanes
    EXPECT_FALSE(
        laneTags(R"(!({"BAN", "INSULT"} contains message.highlight_tag))"));
    EXPECT_FALSE(laneTags(
        R"(flags.highlighted && {"BAN"} contains message.highlight_tag)"));

    auto added = withLaneTag(fyi, "SCAM", true);
    EXPECT_EQ(*laneTags(added),
              QStringList({"LINK", "REQUEST", "NAME", "SCAM"}));
    EXPECT_TRUE(added.endsWith(
        R"( contains message.highlight_tag || (flags.first_message && !flags.highlighted) && !(author.name == "fossabot"))"));
    EXPECT_EQ(withLaneTag(added, "SCAM", true), added);

    auto removed = withLaneTag(fyi, "REQUEST", false);
    EXPECT_EQ(*laneTags(removed), QStringList({"LINK", "NAME"}));

    QString one = R"({"ME"} contains message.highlight_tag)";
    EXPECT_EQ(withLaneTag(one, "ME", false),
              R"({"NONE"} contains message.highlight_tag)");
    EXPECT_EQ(withLaneTag(withLaneTag(one, "ME", false), "BAN", true),
              R"({"BAN"} contains message.highlight_tag)");

    // lane filters as the settings have them, wrapped in parens
    QString urgent =
        R"(({"BAN", "INSULT", "HATE", "ME"} contains message.highlight_tag) && !(author.name == "fossabot") && !(author.name == "TitleChange_Bot"))";
    EXPECT_EQ(*laneTags(urgent), QStringList({"BAN", "INSULT", "HATE", "ME"}));
    EXPECT_EQ(
        withLaneTag(urgent, "SPAM", true),
        R"(({"BAN", "INSULT", "HATE", "ME", "SPAM"} contains message.highlight_tag) && !(author.name == "fossabot") && !(author.name == "TitleChange_Bot"))");
    QString wrappedFyi =
        R"(( {"LINK", "SPAM"} contains message.highlight_tag || (flags.first_message && !flags.highlighted)) && !(author.name == "fossabot"))";
    EXPECT_EQ(*laneTags(wrappedFyi), QStringList({"LINK", "SPAM"}));
    EXPECT_EQ(
        withLaneTag(wrappedFyi, "SPAM", false),
        R"(( {"LINK"} contains message.highlight_tag || (flags.first_message && !flags.highlighted)) && !(author.name == "fossabot"))");
    EXPECT_FALSE(laneTags(
        R"((!({"BAN"} contains message.highlight_tag)) && flags.highlighted)"));
}
