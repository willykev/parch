#include "cslengine.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <functional>

namespace {

// ---------------------------------------------------------------------------
// The output tree. Rendering builds it; text case and period stripping
// change its leaves; flattening turns it into CslText.

enum class Tag : quint8 { None, Names, NameList, Name, Date, YearSuffix, CitationNumber, Locator };

struct Out {
    QString text;
    std::vector<Out> kids;
    bool leaf = false;
    QString prefix;
    QString suffix;
    QString delimiter;
    // -1 unset, 0 off, 1 on, 2 toggled (the flip-flop of markup in data).
    qint8 italic = -1;
    qint8 bold = -1;
    qint8 smallCaps = -1;
    qint8 underline = -1;
    // -1 unset, 0 baseline, 1 superscript, 2 subscript.
    qint8 verticalAlign = -1;
    bool quotes = false;
    bool nocase = false;
    bool nodecor = false;
    CslDisplay display = CslDisplay::None;
    QString link;
    Tag tag = Tag::None;
    // For names: what identifies them, for grouping and substitution.
    QString key;
    // A choose's branch, whose elements join those around it.
    bool splice = false;
    // Quotes that punctuation never moves into, as a cite's own affixes have.
    bool literalQuotes = false;

    static Out of(const QString &text)
    {
        Out o;
        o.leaf = true;
        o.text = text;
        return o;
    }
    static Out seq(std::vector<Out> kids, const QString &delimiter = {})
    {
        Out o;
        o.kids = std::move(kids);
        o.delimiter = delimiter;
        return o;
    }
    bool isEmpty() const
    {
        if (leaf) return text.isEmpty();
        for (const Out &kid : kids) {
            if (!kid.isEmpty()) return false;
        }
        return true;
    }
};

// The text of an output, without affixes of the outermost node.
void plainInto(const Out &o, QString &text, bool outer = true)
{
    if (o.isEmpty()) return;
    if (!outer) text += o.prefix;
    if (o.leaf) {
        text += o.text;
    } else {
        bool first = true;
        for (const Out &kid : o.kids) {
            if (kid.isEmpty()) continue;
            if (!first) text += o.delimiter;
            plainInto(kid, text, false);
            first = false;
        }
    }
    if (!outer) text += o.suffix;
}

QString plainOf(const Out &o)
{
    QString text;
    plainInto(o, text);
    return text;
}

Out *findTag(Out &o, Tag tag)
{
    if (o.tag == tag && !o.isEmpty()) return &o;
    for (Out &kid : o.kids) {
        if (Out *found = findTag(kid, tag)) return found;
    }
    return nullptr;
}

void findTags(Out &o, Tag tag, std::vector<Out *> &found)
{
    if (o.tag == tag && !o.isEmpty()) {
        found.push_back(&o);
        return;
    }
    for (Out &kid : o.kids) findTags(kid, tag, found);
}

void applyFormatting(Out &o, const CslFormatting &f)
{
    o.prefix = f.prefix + o.prefix;
    o.suffix += f.suffix;
    if (f.italic >= 0) o.italic = f.italic;
    if (f.bold >= 0) o.bold = f.bold;
    if (f.smallCaps >= 0) o.smallCaps = f.smallCaps;
    if (f.underline >= 0) o.underline = f.underline;
    if (f.verticalAlign >= 0) o.verticalAlign = f.verticalAlign;
    if (f.quotes) o.quotes = true;
    if (f.display != CslDisplay::None) o.display = f.display;
}

// Wraps an output so that new formatting applies around what it has.
Out wrap(Out inner)
{
    if (inner.prefix.isEmpty() && inner.suffix.isEmpty() && inner.italic < 0 && inner.bold < 0 && inner.smallCaps < 0
        && inner.underline < 0 && inner.verticalAlign < 0 && !inner.quotes && inner.display == CslDisplay::None
        && inner.link.isEmpty() && inner.tag == Tag::None && !inner.nocase && !inner.nodecor)
        return inner;
    std::vector<Out> kids;
    kids.push_back(std::move(inner));
    return Out::seq(std::move(kids));
}

// ---------------------------------------------------------------------------
// Text case and periods.

struct Leaf {
    QString *text;
    bool protectedCase;
};

void collectLeaves(Out &o, bool protectedCase, std::vector<Leaf> &leaves)
{
    const bool p = protectedCase || o.nocase;
    if (o.leaf) {
        leaves.push_back({&o.text, p});
        return;
    }
    for (Out &kid : o.kids) collectLeaves(kid, p, leaves);
}

// Words title case leaves lowercase: articles, conjunctions and
// prepositions, as Chicago and MLA lowercase them, and name particles.
const QSet<QString> &stopWords()
{
    static const QSet<QString> words = [] {
        QSet<QString> set;
        const char *const list[] = {
            "a", "according", "across", "afore", "after", "against", "ahead", "along", "alongside", "amid",
            "amidst", "among", "amongst", "an", "and", "anenst", "apart", "apropos", "apud", "around", "as",
            "aside", "astride", "at", "athwart", "atop", "barring", "before", "behind", "below", "beneath",
            "beside", "besides", "between", "beyond", "but", "by", "circa", "despite", "down", "during", "et",
            "except", "for", "forenenst", "from", "given", "in", "inside", "into", "lest", "like", "modulo",
            "near", "next", "nor", "notwithstanding", "of", "off", "on", "onto", "or", "out", "over", "per",
            "plus", "pro", "qua", "sans", "since", "so", "than", "the", "through", "thru", "throughout",
            "thruout", "till", "to", "toward", "towards", "under", "underneath", "until", "unto", "up", "upon",
            "versus", "vs", "v", "via", "with", "within", "without", "yet", "de", "d'", "d’", "von", "van",
            "ca", "about"};
        for (const char *word : list) set.insert(QString::fromUtf8(word));
        return set;
    }();
    return words;
}

void applyTextCase(Out &o, CslTextCase textCase, bool english)
{
    if (textCase == CslTextCase::None) return;
    if (textCase == CslTextCase::Title && !english) return;
    std::vector<Leaf> leaves;
    collectLeaves(o, false, leaves);
    // The whole text, with each character's leaf and protection.
    QString all;
    std::vector<bool> locked;
    for (const Leaf &leaf : leaves) {
        all += *leaf.text;
        locked.insert(locked.end(), size_t(leaf.text->size()), leaf.protectedCase);
    }
    if (all.isEmpty()) return;
    QString result = all;
    auto upperAt = [&](qsizetype i) {
        if (!locked[size_t(i)]) result[i] = result[i].toUpper();
    };
    auto lowerAt = [&](qsizetype i) {
        if (!locked[size_t(i)]) result[i] = result[i].toLower();
    };
    // Words: runs between spaces, and for title case also between hyphens and slashes.
    struct Word {
        qsizetype start;
        qsizetype end;
    };
    auto words = [&](bool splitHyphens) {
        std::vector<Word> list;
        qsizetype i = 0;
        while (i < all.size()) {
            auto separator = [&](QChar c) {
                return c.isSpace() || (splitHyphens && (c == u'-' || c == u'/' || c == u'–' || c == u'—'));
            };
            while (i < all.size() && separator(all[i])) ++i;
            const qsizetype start = i;
            while (i < all.size() && !separator(all[i])) ++i;
            if (i > start) list.push_back({start, i});
        }
        return list;
    };
    auto letters = [&](const Word &w, bool wantLower) {
        bool any = false;
        for (qsizetype i = w.start; i < w.end; ++i) {
            const QChar c = all[i];
            if (!c.isLetter()) continue;
            any = true;
            if (wantLower ? !c.isLower() : !c.isUpper()) return false;
        }
        return any;
    };
    auto firstLetter = [&](const Word &w) -> qsizetype {
        for (qsizetype i = w.start; i < w.end; ++i) {
            if (all[i].isLetter()) return i;
        }
        return -1;
    };
    switch (textCase) {
    case CslTextCase::Lowercase:
        for (qsizetype i = 0; i < all.size(); ++i) lowerAt(i);
        break;
    case CslTextCase::Uppercase:
        for (qsizetype i = 0; i < all.size(); ++i) upperAt(i);
        break;
    case CslTextCase::CapitalizeFirst: {
        const std::vector<Word> list = words(false);
        if (!list.empty()) {
            const qsizetype at = firstLetter(list.front());
            if (at >= 0) upperAt(at);
        }
        break;
    }
    case CslTextCase::CapitalizeAll:
        for (const Word &w : words(false)) {
            const qsizetype at = firstLetter(w);
            if (at >= 0 && letters(w, true)) upperAt(at);
        }
        break;
    case CslTextCase::Sentence: {
        // As citeproc-js does: lowercase but for nocase text, then the
        // first letter capitalised.
        for (qsizetype i = 0; i < all.size(); ++i) lowerAt(i);
        const std::vector<Word> list = words(false);
        if (!list.empty()) {
            const qsizetype at = firstLetter(list.front());
            if (at >= 0) upperAt(at);
        }
        break;
    }
    case CslTextCase::Title: {
        const std::vector<Word> list = words(true);
        for (size_t n = 0; n < list.size(); ++n) {
            const Word &w = list[n];
            const qsizetype at = firstLetter(w);
            if (at < 0) continue;
            // The word itself, without punctuation around it, for the stop-word test.
            QString bare;
            for (qsizetype i = w.start; i < w.end; ++i)
                if (all[i].isLetter() || all[i] == u'\'' || all[i] == u'’') bare += all[i].toLower();
            // A word after a colon or a sentence's end starts anew; a word
            // before a hyphen begins a compound ("Pro-Environmental").
            bool afterColon = false;
            for (qsizetype i = w.start - 1; i >= 0; --i) {
                if (all[i].isSpace()) continue;
                afterColon = (all[i] == u':' || all[i] == u'?' || all[i] == u'!' || all[i] == u'.')
                             && i + 1 < w.start && all[i + 1].isSpace();
                break;
            }
            const bool beforeHyphen = w.end < all.size() && all[w.end] == u'-'
                                      && !(w.start > 0 && all[w.start - 1] == u'-');
            const bool edge = n == 0 || n + 1 == list.size() || afterColon || beforeHyphen;
            const bool stop = stopWords().contains(bare) && !edge;
            // Words in capitals ("UK", "NASA") keep them.
            if (stop) {
                if (letters(w, true) || (all[at].isUpper() && [&] {
                        for (qsizetype i = at + 1; i < w.end; ++i)
                            if (all[i].isLetter() && !all[i].isLower()) return false;
                        return true;
                    }()))
                    for (qsizetype i = w.start; i < w.end; ++i) lowerAt(i);
            } else if (letters(w, true)) {
                upperAt(at);
            }
        }
        break;
    }
    case CslTextCase::None:
        break;
    }
    qsizetype offset = 0;
    for (const Leaf &leaf : leaves) {
        const qsizetype size = leaf.text->size();
        *leaf.text = result.mid(offset, size);
        offset += size;
    }
}

void stripPeriods(Out &o)
{
    if (o.leaf) {
        o.text.remove(u'.');
        return;
    }
    for (Out &kid : o.kids) stripPeriods(kid);
}

void finish(Out &o, const CslFormatting &f, bool english)
{
    if (f.stripPeriods) stripPeriods(o);
    applyTextCase(o, f.textCase, english);
    applyFormatting(o, f);
}

// ---------------------------------------------------------------------------
// Markup in data: <i>, <b>, <sup>, <sub>, small caps, nocase and nodecor
// spans, and quotation marks, which become quotes that alternate with the
// quotes around them.

Out richText(const QString &value, bool quotes = true)
{
    Out root;
    std::vector<Out *> stack{&root};
    // Which entries of the stack are quotes, and with which mark they opened.
    std::vector<QChar> opened{QChar()};
    std::vector<QString> names{QString()};
    auto push = [&](Out node, QChar quote, const QString &name) {
        stack.back()->kids.push_back(std::move(node));
        stack.push_back(&stack.back()->kids.back());
        opened.push_back(quote);
        names.push_back(name);
    };
    auto pop = [&] {
        if (stack.size() > 1) {
            stack.pop_back();
            opened.pop_back();
            names.pop_back();
        }
    };
    auto addText = [&](const QString &text) {
        if (text.isEmpty()) return;
        std::vector<Out> &kids = stack.back()->kids;
        if (!kids.empty() && kids.back().leaf && kids.back().tag == Tag::None && kids.back().prefix.isEmpty())
            kids.back().text += text;
        else
            kids.push_back(Out::of(text));
    };
    auto quoteOpenAt = [&](QChar mark) -> qsizetype {
        for (qsizetype i = qsizetype(opened.size()) - 1; i > 0; --i) {
            if (opened[size_t(i)] == mark) return i;
        }
        return -1;
    };
    for (const CslMarkup::Token &token : CslMarkup::tokenize(value)) {
        if (token.type == CslMarkup::Token::Type::Open) {
            if (token.selfClosing) continue;
            Out node;
            const QString &name = token.text;
            if (name == u"i" || name == u"em")
                node.italic = 2;
            else if (name == u"b" || name == u"strong")
                node.bold = 2;
            else if (name == u"sup")
                node.verticalAlign = 1;
            else if (name == u"sub")
                node.verticalAlign = 2;
            else if (name == u"sc")
                node.smallCaps = 1;
            else if (name == u"u")
                node.underline = 1;
            else if (name == u"span") {
                const QStringList classes = token.cssClass.split(u' ', Qt::SkipEmptyParts);
                if (classes.contains(QStringLiteral("nocase"))) node.nocase = true;
                if (classes.contains(QStringLiteral("nodecor"))) node.nodecor = true;
                const QString style = token.style.toLower().remove(u' ');
                if (style.contains(u"font-variant:small-caps")) node.smallCaps = 1;
                if (style.contains(u"font-style:italic")) node.italic = 2;
                if (style.contains(u"font-weight:bold")) node.bold = 2;
                if (style.contains(u"vertical-align:sup")) node.verticalAlign = 1;
                if (style.contains(u"vertical-align:sub")) node.verticalAlign = 2;
            } else if (name == u"a") {
                node.link = token.href;
            }
            // Small capitals, superscripts and subscripts keep their case.
            if (node.smallCaps == 1 || node.verticalAlign > 0) node.nocase = true;
            push(std::move(node), QChar(), name);
            continue;
        }
        if (token.type == CslMarkup::Token::Type::Close) {
            bool matched = false;
            for (qsizetype i = qsizetype(names.size()) - 1; i > 0; --i) {
                if (names[size_t(i)] == token.text) {
                    while (qsizetype(stack.size()) > i) pop();
                    matched = true;
                    break;
                }
            }
            // A closing tag that closes nothing is text.
            if (!matched) addText(QStringLiteral("</%1>").arg(token.text));
            continue;
        }
        const QString &text = token.text;
        QString run;
        for (qsizetype i = 0; i < text.size(); ++i) {
            const QChar c = text[i];
            const QChar before = i > 0 ? text[i - 1] : QChar(u' ');
            const QChar after = i + 1 < text.size() ? text[i + 1] : QChar(u' ');
            // Quotation marks become quotes that alternate with those around
            // them. A cite's own prefix and suffix keep typographic ones as typed.
            if (!quotes && (c == u'“' || c == u'”' || c == u'‘' || c == u'’')) {
                run += c;
                continue;
            }
            if (c == u'"' || c == u'“' || c == u'”' || c == u'‘') {
                const QChar mark = c == u'”' ? QChar(u'“') : c;
                const qsizetype open = c == u'“' || c == u'‘' ? -1 : quoteOpenAt(mark);
                if (open > 0 && !(c == u'"' && before.isSpace() && !after.isSpace() && !after.isPunct())) {
                    addText(run);
                    run.clear();
                    while (qsizetype(stack.size()) > open) pop();
                    continue;
                }
                if (c == u'”') {
                    run += c;
                    continue;
                }
                addText(run);
                run.clear();
                Out quote;
                quote.quotes = true;
                quote.literalQuotes = !quotes;
                push(std::move(quote), c, QString());
                continue;
            }
            if (c == u'’' && quoteOpenAt(u'‘') > 0 && !after.isLetter()) {
                addText(run);
                run.clear();
                const qsizetype open = quoteOpenAt(u'‘');
                while (qsizetype(stack.size()) > open) pop();
                continue;
            }
            if (c == u'\'') {
                // A single quotation mark at the start of a word opens a
                // quotation; one that ends a word closes it. Any other is
                // an apostrophe.
                const bool opensWord = (before.isSpace() || before == u'(' || before == u'[' || before == u'"'
                                        || before == u'“')
                                       && after.isLetterOrNumber();
                const qsizetype open = quoteOpenAt(u'\'');
                if (open > 0 && !before.isSpace() && !after.isLetterOrNumber()) {
                    addText(run);
                    run.clear();
                    while (qsizetype(stack.size()) > open) pop();
                    continue;
                }
                if (opensWord && text.indexOf(u'\'', i + 1) > i) {
                    addText(run);
                    run.clear();
                    Out quote;
                    quote.quotes = true;
                    quote.literalQuotes = !quotes;
                    push(std::move(quote), u'\'', QString());
                    continue;
                }
                run += QChar(u'’');
                continue;
            }
            run += c;
        }
        addText(run);
    }
    if (root.kids.size() == 1 && root.kids.front().leaf) return Out::of(root.kids.front().text);
    return root;
}

// ---------------------------------------------------------------------------
// Flattening into CslText, with CSL's punctuation rules where pieces meet.

bool isMergeable(QChar c)
{
    return c == u'.' || c == u',' || c == u';' || c == u':' || c == u'!' || c == u'?';
}

enum class Merge { KeepBoth, KeepFirst, KeepSecond };

// What two punctuation marks become where text that ends with the first
// meets text that starts with the second, as citeproc-js joins them: "!"
// then "." is "!", ";" then "!" is "!", "." then "," stays ".,".
Merge merge(QChar first, QChar second)
{
    if (first == second) return Merge::KeepFirst;
    switch (second.unicode()) {
    case u':':
        return first == u'.' || first == u',' ? Merge::KeepBoth : Merge::KeepFirst;
    case u'.':
        return first == u',' ? Merge::KeepBoth : Merge::KeepFirst;
    case u'!':
    case u'?':
        return first == u':' || first == u';' ? Merge::KeepSecond : Merge::KeepBoth;
    default:
        return Merge::KeepBoth;
    }
}

class Flattener {
public:
    explicit Flattener(const CslLocale &locale) : m_locale(locale) {}

    void add(const Out &o, const CslFormat &base = {}) { walk(o, base, 0); }

    CslText result() const
    {
        CslText text;
        qsizetype start = 0;
        qsizetype end = m_text.size();
        while (start < end && m_text[start] == u' ') ++start;
        while (end > start && m_text[end - 1] == u' ') --end;
        qsizetype runStart = start;
        for (qsizetype i = start; i <= end; ++i) {
            if (i == end || m_format[i] != m_format[runStart]) {
                if (i > runStart) text.append(m_text.mid(runStart, i - runStart), m_formats[m_format[runStart]]);
                runStart = i;
            }
        }
        return text;
    }

private:
    int formatIndex(const CslFormat &format)
    {
        for (qsizetype i = 0; i < m_formats.size(); ++i) {
            if (m_formats[i] == format) return int(i);
        }
        m_formats.append(format);
        return int(m_formats.size() - 1);
    }

    QString quoteTerm(bool open, int level) const
    {
        const bool inner = level % 2 == 1;
        const QString name = open ? (inner ? QStringLiteral("open-inner-quote") : QStringLiteral("open-quote"))
                                  : (inner ? QStringLiteral("close-inner-quote") : QStringLiteral("close-quote"));
        QString text = m_locale.termText(name);
        if (text.isEmpty()) text = open ? (inner ? QStringLiteral("‘") : QStringLiteral("“"))
                                        : (inner ? QStringLiteral("’") : QStringLiteral("”"));
        return text;
    }

    void walk(const Out &o, const CslFormat &inherited, int level)
    {
        if (o.isEmpty()) return;
        CslFormat outer = inherited;
        switch (o.display) {
        case CslDisplay::Block:
            outer.display = CslFormat::Display::Block;
            break;
        case CslDisplay::LeftMargin:
            outer.display = CslFormat::Display::LeftMargin;
            break;
        case CslDisplay::RightInline:
            outer.display = CslFormat::Display::RightInline;
            break;
        case CslDisplay::Indent:
            outer.display = CslFormat::Display::Indent;
            break;
        case CslDisplay::None:
            break;
        }
        put(o.prefix, outer);
        CslFormat inner = outer;
        if (o.nodecor) {
            inner.italic = inner.bold = inner.smallCaps = inner.underline = false;
            inner.position = CslFormat::Position::Baseline;
        }
        auto apply = [](qint8 op, bool &value) {
            if (op == 0) value = false;
            if (op == 1) value = true;
            if (op == 2) value = !value;
        };
        apply(o.italic, inner.italic);
        apply(o.bold, inner.bold);
        apply(o.smallCaps, inner.smallCaps);
        apply(o.underline, inner.underline);
        if (o.verticalAlign == 0) inner.position = CslFormat::Position::Baseline;
        if (o.verticalAlign == 1) inner.position = CslFormat::Position::Superscript;
        if (o.verticalAlign == 2) inner.position = CslFormat::Position::Subscript;
        if (!o.link.isEmpty()) inner.link = o.link;
        int quoteLevel = level;
        if (o.quotes) {
            put(quoteTerm(true, level), inner, !o.literalQuotes);
            ++quoteLevel;
        }
        if (o.leaf) {
            put(o.text, inner);
        } else {
            bool first = true;
            for (const Out &kid : o.kids) {
                if (kid.isEmpty()) continue;
                if (!first) put(o.delimiter, inner);
                walk(kid, inner, quoteLevel);
                first = false;
            }
        }
        if (o.quotes) put(quoteTerm(false, level), inner, !o.literalQuotes);
        put(o.suffix, outer);
    }

    void insertAt(qsizetype at, QChar c, int format)
    {
        m_text.insert(at, c);
        m_format.insert(at, format);
        m_quoteMark.insert(at, false);
    }

    void removeAt(qsizetype at)
    {
        m_text.remove(at, 1);
        m_format.remove(at);
        m_quoteMark.remove(at);
    }

    void put(QString s, const CslFormat &format, bool quoteMark = false)
    {
        if (s.isEmpty()) return;
        // One space where two pieces meet with spaces.
        while (!s.isEmpty() && s[0] == u' ' && (m_text.isEmpty() || m_text.back().isSpace())) s.remove(0, 1);
        if (s.isEmpty()) return;
        if (!quoteMark && isMergeable(s[0]) && !m_text.isEmpty()) {
            qsizetype t = m_text.size() - 1;
            const qsizetype last = t;
            while (t >= 0 && m_quoteMark[t]) --t;
            const bool afterQuote = t < last;
            if (t >= 0 && isMergeable(m_text[t])) {
                switch (merge(m_text[t], s[0])) {
                case Merge::KeepFirst:
                    s.remove(0, 1);
                    break;
                case Merge::KeepSecond:
                    removeAt(t);
                    --t;
                    break;
                case Merge::KeepBoth:
                    break;
                }
            }
            if (!s.isEmpty() && afterQuote && m_locale.punctuationInQuote() && (s[0] == u'.' || s[0] == u',')) {
                // American usage puts periods and commas inside closing quotes.
                insertAt(t + 1, s[0], m_format[t + 1]);
                s.remove(0, 1);
            }
            if (s.isEmpty()) return;
        }
        const int index = formatIndex(format);
        m_text += s;
        for (qsizetype i = 0; i < s.size(); ++i) {
            m_format.append(index);
            m_quoteMark.append(quoteMark);
        }
    }

    const CslLocale &m_locale;
    QString m_text;
    QList<int> m_format;
    QList<bool> m_quoteMark;
    QList<CslFormat> m_formats;
};

// ---------------------------------------------------------------------------
// Numbers, page ranges and locators.

const QRegularExpression &numericPattern()
{
    static const QRegularExpression pattern(QStringLiteral(
        "^\\s*[A-Za-z]{0,3}\\d+[A-Za-z]{0,3}\\s*(?:(?:[-–—,&]|\\band\\b)\\s*[A-Za-z]{0,3}\\d+[A-Za-z]{0,3}\\s*)*$"));
    return pattern;
}

bool isNumeric(const QString &value)
{
    return !value.isEmpty() && numericPattern().match(value).hasMatch();
}

// Numbers joined as a range or a list, in any notation: "1-3", "213, and
// 235", "i–ix"; not "1, fol. 186", where the second number has a label.
bool isPluralValue(const QString &value)
{
    static const QRegularExpression joined(QStringLiteral(
        "^\\s*(?:[A-Za-z]{0,3}\\d+|[ivxlcdm]+\\b)[A-Za-z]{0,3}(?:\\s*(?:[-–—,&]|\\band\\b|\\bet\\b)\\s*)+"
        "(?:[A-Za-z]{0,3}\\d|[ivxlcdm]+\\b)"),
                                             QRegularExpression::CaseInsensitiveOption);
    return joined.match(value).hasMatch();
}

QString roman(int number)
{
    if (number <= 0 || number >= 4000) return QString::number(number);
    static const std::pair<int, const char *> table[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"},
                                                         {100, "c"},  {90, "xc"},  {50, "l"},  {40, "xl"},
                                                         {10, "x"},   {9, "ix"},   {5, "v"},   {4, "iv"},
                                                         {1, "i"}};
    QString text;
    for (const auto &[value, letters] : table) {
        while (number >= value) {
            text += QLatin1String(letters);
            number -= value;
        }
    }
    return text;
}

// The second number of a range, written out in full.
QString expandSecond(const QString &first, const QString &second)
{
    if (second.size() >= first.size()) return second;
    return first.left(first.size() - second.size()) + second;
}

QString minimalSecond(const QString &first, const QString &second, int keep)
{
    const QString full = expandSecond(first, second);
    if (full.size() != first.size()) return full;
    qsizetype same = 0;
    while (same < first.size() && first[same] == full[same]) ++same;
    qsizetype start = qMin(same, full.size() - 1);
    start = qMin(start, full.size() - keep);
    return full.mid(qMax<qsizetype>(0, start));
}

QString formatRangeSecond(const QString &first, const QString &second, const QString &format)
{
    const QString full = expandSecond(first, second);
    if (format == u"expanded") return full;
    if (format == u"minimal") return minimalSecond(first, second, 1);
    if (format == u"minimal-two") return minimalSecond(first, second, 2);
    if (format.startsWith(u"chicago")) {
        bool ok = false;
        const int start = first.toInt(&ok);
        if (!ok) return full;
        if (start < 100 || start % 100 == 0) return full;
        if (start % 100 < 10) return minimalSecond(first, second, 1);
        if (format != u"chicago-16" && first.size() == 4) {
            int changed = 0;
            for (int i = 0; i < 4; ++i)
                if (first[i] != full[i]) ++changed;
            if (changed >= 3) return full;
        }
        return minimalSecond(first, second, 2);
    }
    return second;
}

// Writes ranges with the locale's delimiter (an en dash) and, given a
// format, as page-range-format asks.
QString formatRanges(const QString &value, const QString &format, const QString &delimiter)
{
    // A range of two numbers with the same prefix, if any ("S213-S235",
    // "8n11564-8n11568"), takes the delimiter and the format; a shortened
    // second number drops its prefix. Other composites ("123N110-N5",
    // "N110-5") stay as written, without spaces around their hyphen.
    static const QRegularExpression range(QStringLiteral(
        "(?<![\\w])((?:[A-Za-z0-9]*[A-Za-z])?)(\\d+)\\s*[-–—]+\\s*((?:[A-Za-z0-9]*[A-Za-z])?)(\\d+)(?![\\w])"));
    static const QRegularExpression spaced(QStringLiteral("\\s*-\\s*"));
    QString out;
    qsizetype last = 0;
    auto it = range.globalMatch(value);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString firstPrefix = m.captured(1);
        const QString first = m.captured(2);
        const QString secondPrefix = m.captured(3);
        QString second = m.captured(4);
        out += value.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        if (secondPrefix != firstPrefix) {
            out += m.captured(0).replace(spaced, QStringLiteral("-"));
            continue;
        }
        QString shownPrefix = secondPrefix;
        if (!format.isEmpty()) {
            second = formatRangeSecond(first, second, format);
            if (second.size() < first.size()) shownPrefix.clear();
        }
        out += firstPrefix + first + delimiter + shownPrefix + second;
    }
    out += value.mid(last);
    // Ranges of roman numerals take the delimiter too.
    static const QRegularExpression roman(
        QStringLiteral("(?<![\\w])([ivxlcdm]+)\\s*[-–—]+\\s*([ivxlcdm]+)(?![\\w])"), QRegularExpression::CaseInsensitiveOption);
    out.replace(roman, QStringLiteral("\\1") + delimiter + QStringLiteral("\\2"));
    return out;
}

// ---------------------------------------------------------------------------
// Sorting.

QString foldForSort(const QString &text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString folded;
    folded.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing) continue;
        if (c.isPunct() && c != u'\x01' && c != u'\x02') continue;
        folded += c.toCaseFolded();
    }
    return folded.simplified();
}

struct SortValue {
    bool empty = true;
    bool numeric = false;
    double number = 0;
    QString text;
};

int compareValues(const SortValue &a, const SortValue &b, bool descending)
{
    if (a.empty && b.empty) return 0;
    // Empty values go last in either direction.
    if (a.empty) return 1;
    if (b.empty) return -1;
    int result = 0;
    if (a.numeric && b.numeric)
        result = a.number < b.number ? -1 : a.number > b.number ? 1 : 0;
    else
        result = QString::compare(a.text, b.text);
    return descending ? -result : result;
}

QString dateSortKey(const CslDatePoint &point)
{
    if (point.parts == 0) return {};
    return QStringLiteral("%1%2%3")
        .arg(point.year + 50000, 6, 10, QLatin1Char('0'))
        .arg(point.parts >= 2 ? point.month : 0, 2, 10, QLatin1Char('0'))
        .arg(point.parts >= 3 ? point.day : 0, 2, 10, QLatin1Char('0'));
}

// ---------------------------------------------------------------------------
// Rendering.

enum Position { First = 0, Subsequent = 1, Ibid = 2, IbidWithLocator = 3 };

struct ItemState {
    // Names shown after adding names to disambiguate, or -1.
    int namesShown = -1;
    // Given-name expansion per name position: 1 initials, 2 full.
    QList<int> given;
    bool disambiguate = false;
    // Shows given names to this level, while names are added to disambiguate.
    int expandLevel = 0;
    QString yearSuffix;
    QString citationNumber;
};

struct CiteInfo {
    int position = First;
    bool nearNote = false;
    int firstNote = 0;
};

// A name as cites show it, with how it reads at each expansion level, for
// disambiguating given names across the document.
struct ShownName {
    QString person;
    int index = 0;
    QString forms[3];
};

struct Context {
    const CslReference *reference = nullptr;
    const CslCite *cite = nullptr;
    bool bibliography = false;
    CiteInfo info;
    const ItemState *state = nullptr;
    const QHash<QString, QString> *section = nullptr;
    bool english = true;
    bool sortMode = false;
    const CslStyle::SortKey *sortKey = nullptr;
    int called = 0;
    int rendered = 0;
    QSet<QString> suppressed;
    QSet<QString> *used = nullptr;
    bool yearSuffixDone = false;
    bool suppressNames = false;
    bool namesSuppressed = false;
    const CslNode *parentNames = nullptr;
    std::vector<ShownName> *shownNames = nullptr;
};

class Processor {
public:
    Processor(const CslStyle &style, const CslLocale &locale) : m_style(style), m_locale(locale) {}

    QHash<QString, int> globalGiven;
    bool globalPrimaryOnly = false;

    Out render(const CslNode &node, Context &ctx);
    Out renderChildren(const std::vector<CslNode> &children, Context &ctx, const QString &delimiter = {})
    {
        std::vector<Out> kids;
        kids.reserve(children.size());
        for (const CslNode &child : children) {
            Out o = render(child, ctx);
            if (o.splice) {
                for (Out &kid : o.kids) kids.push_back(std::move(kid));
            } else {
                kids.push_back(std::move(o));
            }
        }
        return Out::seq(std::move(kids), delimiter);
    }

    // A section's layout for one reference, without the layout's own
    // affixes and formatting.
    Out layoutBody(const CslStyle::Section &section, Context &ctx) { return renderChildren(section.layout.children, ctx); }

    QString nameSortKey(const CslName &name) const;
    QString variableText(const QString &variable, const Context &ctx) const;
    static QString citationLabel(const CslReference &reference);
    SortValue sortValue(const CslStyle::SortKey &key, Context ctx);
    QString option(const QString &name, const Context &ctx, const CslNode *node = nullptr,
                   const QString &nodeAttribute = {}) const;

private:
    Out text(const CslNode &node, Context &ctx);
    Out number(const CslNode &node, Context &ctx);
    Out date(const CslNode &node, Context &ctx);
    Out label(const CslNode &node, Context &ctx);
    Out names(const CslNode &node, Context &ctx);
    Out group(const CslNode &node, Context &ctx);
    Out choose(const CslNode &node, Context &ctx);
    bool test(const CslNode &node, Context &ctx);
    bool testOne(const QString &condition, const QString &value, Context &ctx);
    Out nameList(const QList<CslName> &list, const QString &variable, const CslNode *nameNode,
                 const CslNode *etAlNode, Context &ctx, int *shownCount);
    Out personName(const CslName &name, bool inverted, QString form, int level, const CslNode *nameNode,
                   const Context &ctx);
    Out datePart(const CslNode &spec, const CslDatePoint &point, const CslDate &date, bool yearSuffixHere,
                 bool stripSuffix, Context &ctx);
    QString termFor(const QString &variable) const;
    void noteVariable(Context &ctx, const QString &variable, bool nonEmpty) const
    {
        ++ctx.called;
        if (nonEmpty) {
            ++ctx.rendered;
            // A variable that stands in for names is shown once, even
            // within what stands in.
            if (ctx.used) {
                ctx.used->insert(variable);
                if (CslReference::isNameVariable(variable)) ctx.suppressed.insert(variable);
            }
        }
    }

    const CslStyle &m_style;
    const CslLocale &m_locale;
};

QString Processor::option(const QString &name, const Context &ctx, const CslNode *node, const QString &nodeAttribute) const
{
    if (node) {
        const QString attribute = nodeAttribute.isEmpty() ? name : nodeAttribute;
        if (node->has(attribute)) return node->attribute(attribute);
    }
    if (ctx.section && ctx.section->contains(name)) return ctx.section->value(name);
    return m_style.options().value(name);
}

Out Processor::render(const CslNode &node, Context &ctx)
{
    switch (node.element) {
    case CslElement::Text:
        return text(node, ctx);
    case CslElement::Number:
        return number(node, ctx);
    case CslElement::Date:
        return date(node, ctx);
    case CslElement::Label:
        return label(node, ctx);
    case CslElement::Names:
        return names(node, ctx);
    case CslElement::Group:
        return group(node, ctx);
    case CslElement::Choose:
        return choose(node, ctx);
    default:
        return {};
    }
}

QString Processor::variableText(const QString &variable, const Context &ctx) const
{
    if (variable == u"locator") return ctx.cite ? ctx.cite->locator : QString();
    if (variable == u"citation-number") return ctx.state ? ctx.state->citationNumber : QString();
    if (variable == u"year-suffix") return ctx.state ? ctx.state->yearSuffix : QString();
    if (variable == u"first-reference-note-number")
        return ctx.cite && ctx.info.position >= Subsequent && ctx.info.firstNote > 0 ? QString::number(ctx.info.firstNote)
                                                                                      : QString();
    if (!ctx.reference) return {};
    if (variable == u"citation-label" && ctx.reference->field(variable).isEmpty()) return citationLabel(*ctx.reference);
    if (variable == u"page-first" && ctx.reference->field(variable).isEmpty()) {
        static const QRegularExpression first(QStringLiteral("^\\s*([^\\s,&–—-]+)"));
        return first.match(ctx.reference->field(QStringLiteral("page"))).captured(1);
    }
    return ctx.reference->field(variable);
}

// A label from the names and the year, in CSL's default trigraph form,
// "Aaaa00:AaAa00:AaAA00:AAAA00": "Doe06" for one author, "DoRo06" for two.
QString Processor::citationLabel(const CslReference &reference)
{
    QList<CslName> names;
    for (const char *variable : {"author", "editor", "translator"}) {
        names = reference.names.value(QLatin1String(variable));
        if (!names.isEmpty()) break;
    }
    QString label;
    auto letters = [](const CslName &name, int count) {
        const QString family = name.literal.isEmpty() ? name.family : name.literal;
        QString text;
        for (const QChar c : family) {
            if (!c.isLetter()) continue;
            text += c;
            if (text.size() == count) break;
        }
        return text;
    };
    if (names.size() == 1)
        label = letters(names[0], 4);
    else if (names.size() == 2)
        label = letters(names[0], 2) + letters(names[1], 2);
    else if (names.size() == 3)
        label = letters(names[0], 2) + letters(names[1], 1) + letters(names[2], 1);
    else
        for (int i = 0; i < names.size() && i < 4; ++i) label += letters(names[i], 1);
    if (label.isEmpty()) return {};
    const CslDate issued = reference.dates.value(QStringLiteral("issued"));
    if (issued.start.parts > 0) label += QStringLiteral("%1").arg(qAbs(issued.start.year) % 100, 2, 10, QLatin1Char('0'));
    return label;
}

Out Processor::text(const CslNode &node, Context &ctx)
{
    Out o;
    if (node.has(QStringLiteral("variable"))) {
        const QString variable = node.attribute(QStringLiteral("variable"));
        if (ctx.suppressed.contains(variable)) {
            noteVariable(ctx, variable, false);
            return {};
        }
        QString value = variableText(variable, ctx);
        if (node.attribute(QStringLiteral("form")) == u"short" && ctx.reference) {
            QString shortValue = ctx.reference->field(variable + QStringLiteral("-short"));
            if (shortValue.isEmpty() && variable == u"title") shortValue = ctx.reference->field(QStringLiteral("shortTitle"));
            if (shortValue.isEmpty() && variable == u"container-title")
                shortValue = ctx.reference->field(QStringLiteral("journalAbbreviation"));
            if (!shortValue.isEmpty()) value = shortValue;
        }
        noteVariable(ctx, variable, !value.isEmpty());
        if (value.isEmpty()) return {};
        if (ctx.sortMode && CslReference::isNumberVariable(variable) && isNumeric(value)) {
            static const QRegularExpression digits(QStringLiteral("\\d+"));
            const QString first = digits.match(value).captured(0);
            return Out::of(QStringLiteral("%1").arg(first.toLongLong(), 10, 10, QLatin1Char('0')));
        }
        const QString rangeDelimiter = [this] {
            const QString term = m_locale.termText(QStringLiteral("page-range-delimiter"));
            return term.isEmpty() ? QStringLiteral("–") : term;
        }();
        if (variable == u"page") {
            o = Out::of(formatRanges(value, m_style.options().value(QStringLiteral("page-range-format")), rangeDelimiter));
        } else if (variable == u"locator") {
            const QString label = ctx.cite && !ctx.cite->label.isEmpty() ? ctx.cite->label : QStringLiteral("page");
            o = Out::of(formatRanges(value,
                                     label == u"page" ? m_style.options().value(QStringLiteral("page-range-format")) : QString(),
                                     rangeDelimiter));
            o = wrap(std::move(o));
            o.tag = Tag::Locator;
        } else if (variable == u"citation-number") {
            o = wrap(Out::of(value));
            o.tag = Tag::CitationNumber;
        } else if (variable == u"year-suffix") {
            o = wrap(Out::of(value));
            o.tag = Tag::YearSuffix;
        } else if (variable == u"citation-label" && !m_style.rendersYearSuffix() && !ctx.yearSuffixDone && ctx.state
                   && !ctx.state->yearSuffix.isEmpty()) {
            // A label tells cites apart with the year-suffix, as a year would.
            Out suffix = wrap(Out::of(ctx.state->yearSuffix));
            suffix.tag = Tag::YearSuffix;
            o = Out::seq({richText(value), std::move(suffix)});
            ctx.yearSuffixDone = true;
        } else if (variable == u"URL" || variable == u"DOI") {
            o = Out::of(value);
            QString link = value;
            if (variable == u"DOI" && !value.startsWith(u"http")) {
                link = QStringLiteral("https://doi.org/") + value;
                // A style that writes the resolver as a prefix links it with the DOI.
                const QString prefix = node.formatting.prefix;
                const qsizetype at = prefix.indexOf(QStringLiteral("doi.org/"));
                if (at >= 0) {
                    qsizetype start = prefix.lastIndexOf(u' ', at);
                    start = start < 0 ? 0 : start + 1;
                    const QString resolver = prefix.mid(start);
                    CslFormatting f = node.formatting;
                    f.prefix = prefix.left(start);
                    Out linked = Out::seq({Out::of(resolver), Out::of(value)});
                    linked.link = resolver + value;
                    finish(linked, CslFormatting{}, ctx.english);
                    Out outer = wrap(std::move(linked));
                    finish(outer, f, ctx.english);
                    return outer;
                }
            }
            o.link = link;
            o = wrap(std::move(o));
        } else {
            o = richText(value);
        }
    } else if (node.has(QStringLiteral("macro"))) {
        const CslNode *macro = m_style.macro(node.attribute(QStringLiteral("macro")));
        if (!macro) return {};
        // A macro is left out, like a group, when it calls variables and
        // all of them are empty, as citeproc-js does and styles expect.
        const int called = ctx.called;
        const int rendered = ctx.rendered;
        o = renderChildren(macro->children, ctx);
        if (ctx.called > called && ctx.rendered == rendered) return {};
        if (o.isEmpty()) return {};
        // What the macro shows counts as shown content around it.
        if (ctx.rendered == rendered) ++ctx.rendered;
    } else if (node.has(QStringLiteral("term"))) {
        if (ctx.sortMode) return {};
        QString form = node.attribute(QStringLiteral("form"));
        if (form.isEmpty()) form = QStringLiteral("long");
        o = Out::of(m_locale.termText(node.attribute(QStringLiteral("term")), form,
                                      node.attribute(QStringLiteral("plural")) == u"true"));
    } else {
        o = richText(node.attribute(QStringLiteral("value")));
    }
    if (o.isEmpty()) return {};
    o = wrap(std::move(o));
    finish(o, node.formatting, ctx.english);
    return o;
}

QString Processor::termFor(const QString &variable) const
{
    if (m_locale.term(variable)) return variable;
    if (variable == u"number-of-pages") return QStringLiteral("page");
    if (variable == u"number-of-volumes") return QStringLiteral("volume");
    if (variable == u"chapter-number") return QStringLiteral("chapter");
    if (variable == u"collection-number") return QStringLiteral("number");
    return variable;
}

Out Processor::number(const CslNode &node, Context &ctx)
{
    const QString variable = node.attribute(QStringLiteral("variable"));
    if (ctx.suppressed.contains(variable)) {
        noteVariable(ctx, variable, false);
        return {};
    }
    const QString value = variableText(variable, ctx);
    noteVariable(ctx, variable, !value.isEmpty());
    if (value.isEmpty()) return {};
    static const QRegularExpression digits(QStringLiteral("\\d+"));
    if (ctx.sortMode && isNumeric(value))
        return Out::of(QStringLiteral("%1").arg(digits.match(value).captured(0).toLongLong(), 10, 10, QLatin1Char('0')));
    Out o;
    if (isNumeric(value)) {
        const QString form = node.attribute(QStringLiteral("form"));
        const std::optional<CslLocale::Term> term = m_locale.term(termFor(variable));
        const QString gender = term ? term->gender : QString();
        QString text;
        qsizetype last = 0;
        auto it = digits.globalMatch(value);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            QString between = value.mid(last, m.capturedStart() - last);
            between.replace(QRegularExpression(QStringLiteral("\\s*[-–—]\\s*")), QStringLiteral("–"));
            text += between;
            const int n = m.captured(0).toInt();
            if (form == u"ordinal")
                text += m_locale.ordinal(n, gender);
            else if (form == u"long-ordinal")
                text += m_locale.longOrdinal(n, gender);
            else if (form == u"roman")
                text += roman(n);
            else
                text += m.captured(0);
            last = m.capturedEnd();
        }
        text += value.mid(last);
        // A number with a letter suffix keeps it only in numeric form.
        if (form == u"ordinal" || form == u"long-ordinal") {
            static const QRegularExpression letterAfter(QStringLiteral("(\\d)(?:st|nd|rd|th)([a-z]+)"));
            text.replace(letterAfter, QStringLiteral("\\1\\2"));
        }
        o = Out::of(text);
    } else {
        o = richText(value);
    }
    o = wrap(std::move(o));
    finish(o, node.formatting, ctx.english);
    return o;
}

Out Processor::label(const CslNode &node, Context &ctx)
{
    if (ctx.sortMode) return {};
    const QString variable = node.attribute(QStringLiteral("variable"));
    QString value;
    QString term;
    if (variable == u"locator") {
        if (!ctx.cite || ctx.cite->locator.isEmpty()) return {};
        value = ctx.cite->locator;
        term = ctx.cite->label.isEmpty() ? QStringLiteral("page") : ctx.cite->label;
        if (term == u"sub verbo") term = QStringLiteral("sub-verbo");
        if (!m_locale.term(term) && term == u"sub-verbo") term = QStringLiteral("sub verbo");
    } else {
        if (ctx.suppressed.contains(variable)) return {};
        value = variableText(variable, ctx);
        if (value.isEmpty()) return {};
        term = termFor(variable);
    }
    const QString pluralMode = node.attribute(QStringLiteral("plural"));
    bool plural = false;
    if (pluralMode == u"always")
        plural = true;
    else if (pluralMode != u"never") {
        if (variable == u"number-of-pages" || variable == u"number-of-volumes") {
            static const QRegularExpression digits(QStringLiteral("\\d+"));
            plural = digits.match(value).captured(0).toInt() > 1;
        } else {
            plural = isPluralValue(value);
        }
    }
    QString form = node.attribute(QStringLiteral("form"));
    if (form.isEmpty()) form = QStringLiteral("long");
    Out o = Out::of(m_locale.termText(term, form, plural));
    if (o.isEmpty()) return {};
    o = wrap(std::move(o));
    finish(o, node.formatting, ctx.english);
    return o;
}

Out Processor::group(const CslNode &node, Context &ctx)
{
    const int called = ctx.called;
    const int rendered = ctx.rendered;
    Out o = renderChildren(node.children, ctx, node.delimiter);
    if (ctx.called > called && ctx.rendered == rendered) return {};
    if (o.isEmpty()) return {};
    if (ctx.rendered == rendered) ++ctx.rendered;
    o = wrap(std::move(o));
    finish(o, node.formatting, ctx.english);
    return o;
}

Out Processor::choose(const CslNode &node, Context &ctx)
{
    for (const CslNode &branch : node.children) {
        if (branch.element == CslElement::Else || test(branch, ctx)) {
            // The branch stands in for the choose: a group around it sets
            // its delimiter between the branch's own elements.
            Out o = renderChildren(branch.children, ctx);
            o.splice = true;
            return o;
        }
    }
    return {};
}

bool Processor::test(const CslNode &node, Context &ctx)
{
    QString match = node.attribute(QStringLiteral("match"));
    if (match.isEmpty()) match = QStringLiteral("all");
    static const QStringList conditions{QStringLiteral("type"),       QStringLiteral("variable"),
                                        QStringLiteral("is-numeric"), QStringLiteral("is-uncertain-date"),
                                        QStringLiteral("locator"),    QStringLiteral("position"),
                                        QStringLiteral("disambiguate")};
    bool all = true;
    bool any = false;
    for (const QString &condition : conditions) {
        if (!node.has(condition)) continue;
        for (const QString &value : node.attribute(condition).split(u' ', Qt::SkipEmptyParts)) {
            const bool result = testOne(condition, value, ctx);
            all = all && result;
            any = any || result;
        }
    }
    if (match == u"any") return any;
    if (match == u"none") return !any;
    return all;
}

bool Processor::testOne(const QString &condition, const QString &value, Context &ctx)
{
    if (condition == u"disambiguate") return (value == u"true") == (ctx.state && ctx.state->disambiguate && !ctx.bibliography);
    if (condition == u"type") return ctx.reference && ctx.reference->type == value;
    if (condition == u"variable") {
        if (value == u"locator" || value == u"citation-number" || value == u"year-suffix"
            || value == u"first-reference-note-number")
            return !variableText(value, ctx).isEmpty();
        return ctx.reference && ctx.reference->hasVariable(value) && !ctx.suppressed.contains(value);
    }
    if (condition == u"is-numeric") return isNumeric(CslText::fromHtml(variableText(value, ctx)).toPlainText());
    if (condition == u"is-uncertain-date")
        return ctx.reference && ctx.reference->dates.value(value).circa;
    if (condition == u"locator") {
        if (!ctx.cite || ctx.cite->locator.isEmpty()) return false;
        QString label = ctx.cite->label.isEmpty() ? QStringLiteral("page") : ctx.cite->label;
        if (label == u"sub verbo") label = QStringLiteral("sub-verbo");
        return label == (value == u"sub verbo" ? QStringLiteral("sub-verbo") : value);
    }
    if (condition == u"position") {
        if (ctx.bibliography || !ctx.cite) return false;
        const int p = ctx.info.position;
        if (value == u"first") return p == First;
        if (value == u"subsequent") return p >= Subsequent;
        if (value == u"ibid") return p >= Ibid;
        if (value == u"ibid-with-locator") return p == IbidWithLocator;
        if (value == u"near-note") return p >= Subsequent && ctx.info.nearNote;
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Dates.

Out Processor::datePart(const CslNode &spec, const CslDatePoint &point, const CslDate &date, bool yearSuffixHere,
                        bool stripSuffix, Context &ctx)
{
    const QString name = spec.attribute(QStringLiteral("name"));
    const QString form = spec.attribute(QStringLiteral("form"));
    QString text;
    if (name == u"year") {
        if (point.parts < 1) return {};
        if (form == u"short") {
            text = QStringLiteral("%1").arg(qAbs(point.year) % 100, 2, 10, QLatin1Char('0'));
        } else {
            text = QString::number(qAbs(point.year));
            if (point.year < 0)
                text += m_locale.termText(QStringLiteral("bc"));
            else if (point.year > 0 && point.year < 1000)
                text += m_locale.termText(QStringLiteral("ad"));
        }
    } else if (name == u"month") {
        if (point.parts < 2) {
            if (date.season.isEmpty() || point.parts < 1) return {};
            bool ok = false;
            const int season = date.season.toInt(&ok);
            text = ok && season >= 1 && season <= 4
                       ? m_locale.termText(QStringLiteral("season-%1").arg(season, 2, 10, QLatin1Char('0')))
                       : date.season;
        } else if (point.month > 12) {
            text = m_locale.termText(QStringLiteral("season-%1").arg(point.month - 12, 2, 10, QLatin1Char('0')));
        } else if (form == u"numeric") {
            text = QString::number(point.month);
        } else if (form == u"numeric-leading-zeros") {
            text = QStringLiteral("%1").arg(point.month, 2, 10, QLatin1Char('0'));
        } else {
            text = m_locale.termText(QStringLiteral("month-%1").arg(point.month, 2, 10, QLatin1Char('0')),
                                     form == u"short" ? QStringLiteral("short") : QStringLiteral("long"));
        }
    } else if (name == u"day") {
        if (point.parts < 3) return {};
        if (form == u"numeric-leading-zeros") {
            text = QStringLiteral("%1").arg(point.day, 2, 10, QLatin1Char('0'));
        } else if (form == u"ordinal") {
            if (m_locale.limitDayOrdinalsToDay1() && point.day != 1) {
                text = QString::number(point.day);
            } else {
                const std::optional<CslLocale::Term> month =
                    m_locale.term(QStringLiteral("month-%1").arg(point.month, 2, 10, QLatin1Char('0')));
                text = m_locale.ordinal(point.day, month ? month->gender : QString());
            }
        } else {
            text = QString::number(point.day);
        }
    }
    if (text.isEmpty()) return {};
    Out o = Out::of(text);
    if (yearSuffixHere && name == u"year" && ctx.state && !ctx.state->yearSuffix.isEmpty()) {
        Out suffix = wrap(Out::of(ctx.state->yearSuffix));
        suffix.tag = Tag::YearSuffix;
        o = Out::seq({std::move(o), std::move(suffix)});
        ctx.yearSuffixDone = true;
    }
    o = wrap(std::move(o));
    CslFormatting f = spec.formatting;
    if (stripSuffix) f.suffix.clear();
    finish(o, f, ctx.english);
    return o;
}

Out Processor::date(const CslNode &node, Context &ctx)
{
    const QString variable = node.attribute(QStringLiteral("variable"));
    if (ctx.suppressed.contains(variable) || !ctx.reference) {
        noteVariable(ctx, variable, false);
        return {};
    }
    const CslDate date = ctx.reference->dates.value(variable);
    if (date.isEmpty()) {
        noteVariable(ctx, variable, false);
        return {};
    }

    // The date-parts to render, with their attributes.
    std::vector<CslNode> specs;
    QString delimiter = node.delimiter;
    if (node.has(QStringLiteral("form"))) {
        const CslNode *format = m_locale.dateFormat(node.attribute(QStringLiteral("form")));
        QString parts = node.attribute(QStringLiteral("date-parts"));
        if (parts.isEmpty()) parts = QStringLiteral("year-month-day");
        if (format) {
            delimiter = format->delimiter;
            for (const CslNode &part : format->children) {
                const QString name = part.attribute(QStringLiteral("name"));
                if (name == u"month" && parts == u"year") continue;
                if (name == u"day" && parts != u"year-month-day") continue;
                CslNode spec = part;
                // The style's date-parts change the locale's, all but their affixes.
                for (const CslNode &own : node.children) {
                    if (own.attribute(QStringLiteral("name")) != name) continue;
                    for (auto it = own.attributes.constBegin(); it != own.attributes.constEnd(); ++it) {
                        if (it.key() != u"prefix" && it.key() != u"suffix") spec.attributes.insert(it.key(), it.value());
                    }
                }
                spec.formatting = cslFormatting(spec.attributes);
                specs.push_back(std::move(spec));
            }
        } else {
            CslNode year;
            year.element = CslElement::DatePart;
            year.attributes.insert(QStringLiteral("name"), QStringLiteral("year"));
            specs.push_back(year);
        }
    } else {
        for (const CslNode &part : node.children) specs.push_back(part);
    }

    if (ctx.sortMode) {
        // Only the parts the macro shows take part in sorting.
        bool month = false;
        bool day = false;
        for (const CslNode &spec : specs) {
            month = month || spec.attribute(QStringLiteral("name")) == u"month";
            day = day || spec.attribute(QStringLiteral("name")) == u"day";
        }
        auto key = [&](CslDatePoint p) {
            if (!day && p.parts > 2) p.parts = 2;
            if (!month && p.parts > 1) p.parts = 1;
            return dateSortKey(p);
        };
        noteVariable(ctx, variable, true);
        if (!date.literal.isEmpty()) return Out::of(date.literal);
        return Out::of(key(date.start) + (date.isRange ? QStringLiteral("/") + key(date.end) : QString()));
    }

    Out o;
    o.tag = Tag::Date;
    if (!date.literal.isEmpty()) {
        o.kids.push_back(Out::of(date.literal));
    } else {
        const bool implicitSuffix = !m_style.rendersYearSuffix() && variable == u"issued" && !ctx.yearSuffixDone;
        auto partIn = [&specs](const QString &name) {
            for (size_t i = 0; i < specs.size(); ++i)
                if (specs[i].attribute(QStringLiteral("name")) == name) return int(i);
            return -1;
        };
        const CslDatePoint &start = date.start;
        const CslDatePoint &end = date.end;
        // The largest unit that differs between the ends of a range.
        QString largest;
        if (date.isRange && end.parts > 0) {
            if (start.year != end.year)
                largest = QStringLiteral("year");
            else if (start.parts >= 2 && end.parts >= 2 && start.month != end.month)
                largest = QStringLiteral("month");
            else if (start.parts >= 3 && end.parts >= 3 && start.day != end.day)
                largest = QStringLiteral("day");
        }
        const bool openRange = date.isRange && end.parts == 0;
        std::vector<Out> pieces;
        if (largest.isEmpty()) {
            for (const CslNode &spec : specs) pieces.push_back(datePart(spec, start, date, implicitSuffix, false, ctx));
            if (openRange) {
                // "2000–": an open range ends with its delimiter.
                QString rangeDelimiter = QStringLiteral("–");
                const int yearIndex = partIn(QStringLiteral("year"));
                if (yearIndex >= 0 && specs[size_t(yearIndex)].has(QStringLiteral("range-delimiter")))
                    rangeDelimiter = specs[size_t(yearIndex)].attribute(QStringLiteral("range-delimiter"));
                Out seq = Out::seq(std::move(pieces), delimiter);
                pieces.clear();
                pieces.push_back(Out::seq({std::move(seq), Out::of(rangeDelimiter)}));
                delimiter.clear();
            }
        } else {
            QSet<QString> differing{QStringLiteral("day")};
            if (largest != u"day") differing.insert(QStringLiteral("month"));
            if (largest == u"year") differing.insert(QStringLiteral("year"));
            int firstDiff = -1;
            int lastDiff = -1;
            auto present = [&](const QString &name) {
                const int needed = name == u"day" ? 3 : name == u"month" ? 2 : 1;
                return start.parts >= needed || end.parts >= needed;
            };
            for (size_t i = 0; i < specs.size(); ++i) {
                const QString name = specs[i].attribute(QStringLiteral("name"));
                if (differing.contains(name) && present(name)) {
                    if (firstDiff < 0) firstDiff = int(i);
                    lastDiff = int(i);
                }
            }
            QString rangeDelimiter = QStringLiteral("–");
            const int largestIndex = partIn(largest);
            if (largestIndex >= 0 && specs[size_t(largestIndex)].has(QStringLiteral("range-delimiter")))
                rangeDelimiter = specs[size_t(largestIndex)].attribute(QStringLiteral("range-delimiter"));
            const bool yearShared = !differing.contains(QStringLiteral("year"));
            for (int i = 0; i < firstDiff; ++i)
                pieces.push_back(datePart(specs[size_t(i)], start, date, implicitSuffix && yearShared, false, ctx));
            std::vector<Out> startRun;
            std::vector<Out> endRun;
            for (int i = firstDiff; i <= lastDiff && firstDiff >= 0; ++i) {
                startRun.push_back(datePart(specs[size_t(i)], start, date, false, i == lastDiff, ctx));
                endRun.push_back(datePart(specs[size_t(i)], end, date, implicitSuffix && !yearShared, false, ctx));
            }
            if (firstDiff >= 0) {
                Out range = Out::seq({Out::seq(std::move(startRun), delimiter), Out::of(rangeDelimiter),
                                      Out::seq(std::move(endRun), delimiter)});
                pieces.push_back(std::move(range));
            }
            for (int i = lastDiff + 1; i < int(specs.size()); ++i)
                pieces.push_back(datePart(specs[size_t(i)], start, date, implicitSuffix && yearShared, false, ctx));
        }
        o.kids.push_back(Out::seq(std::move(pieces), delimiter));
    }
    // A date counts as shown only if its parts showed something.
    noteVariable(ctx, variable, !o.isEmpty());
    if (o.isEmpty()) return {};
    finish(o, node.formatting, ctx.english);
    return o;
}

// ---------------------------------------------------------------------------
// Names.

bool isRomanesque(const CslName &name)
{
    for (const QString &part : {name.family, name.given}) {
        for (const QChar c : part) {
            if (!c.isLetter()) continue;
            const QChar::Script script = c.script();
            if (script == QChar::Script_Han || script == QChar::Script_Hiragana || script == QChar::Script_Katakana
                || script == QChar::Script_Hangul)
                return false;
        }
    }
    return true;
}

// Initials of given names: "John Michael" with ". " is "J. M.", and with
// hyphens kept, "Jean-Paul" is "J.-P.". Abbreviations ("Ph.", "Ts.") and
// single letters take the initial form's punctuation; lowercase words
// ("de") stay whole. Without initialize, other words stay whole too.
QString initials(const QString &given, const QString &with, bool initialize, bool hyphen)
{
    QString out;
    auto addInitial = [&out](const QString &text) { out += text; };
    auto addWord = [&out](const QString &word) {
        if (!out.isEmpty() && !out.endsWith(u' ')) out += u' ';
        out += word + u' ';
    };
    for (const QString &word : given.split(u' ', Qt::SkipEmptyParts)) {
        // Pieces between periods, each with whether a period ended it.
        QStringList pieces;
        QList<bool> dotted;
        QString current;
        for (const QChar c : word) {
            if (c == u'.') {
                if (!current.isEmpty()) {
                    pieces.append(current);
                    dotted.append(true);
                }
                current.clear();
            } else {
                current += c;
            }
        }
        if (!current.isEmpty()) {
            pieces.append(current);
            dotted.append(false);
        }
        for (qsizetype p = 0; p < pieces.size(); ++p) {
            const QString &piece = pieces[p];
            if (piece.contains(u'-') && piece.size() > 1) {
                const QStringList parts = piece.split(u'-', Qt::SkipEmptyParts);
                bool allWords = true;
                for (const QString &part : parts) allWords = allWords && part.size() > 1;
                if (!initialize && allWords) {
                    addWord(piece);
                    continue;
                }
                QStringList rendered;
                for (const QString &part : parts) {
                    // A lowercase part, as in "Guo-ping", has no initial of its own.
                    if (!rendered.isEmpty() && part[0].isLower()) continue;
                    rendered.append(QString(part[0]) + with);
                }
                if (hyphen) {
                    for (qsizetype i = 0; i + 1 < rendered.size(); ++i) rendered[i] = rendered[i].trimmed();
                    addInitial(rendered.join(u'-'));
                } else {
                    addInitial(rendered.join(QString()));
                }
            } else if (piece.size() == 1 && piece[0].isLetter()) {
                addInitial(piece.toUpper() + with);
            } else if (dotted[p]) {
                addInitial(piece + with);
            } else if (piece[0].isLower() || !initialize) {
                addWord(piece);
            } else if (piece.size() >= 3 && piece[1].isUpper() && piece[2].isLower()) {
                // A name that starts with a digraph, as "TSerendorjiin" does, keeps both letters.
                addInitial(QString(piece[0]) + piece[1].toLower() + with);
            } else {
                addInitial(QString(piece[0]) + with);
            }
        }
    }
    return out.trimmed();
}

QString joinParticle(const QString &particle, const QString &family)
{
    if (particle.isEmpty()) return family;
    if (family.isEmpty()) return particle;
    const QChar last = particle.back();
    if (last == u'\'' || last == u'’' || last == u'-') return particle + family;
    return particle + u' ' + family;
}

QString Processor::nameSortKey(const CslName &name) const
{
    if (!name.literal.isEmpty()) return name.literal;
    const QString demote = m_style.options().value(QStringLiteral("demote-non-dropping-particle"));
    QStringList parts;
    if (demote == u"never") {
        parts << joinParticle(name.nonDroppingParticle, name.family) << name.droppingParticle << name.given
              << name.suffix;
    } else {
        parts << name.family
              << (name.droppingParticle + u' ' + name.nonDroppingParticle).trimmed() << name.given << name.suffix;
    }
    return parts.join(u'\x01');
}

Out Processor::personName(const CslName &original, bool inverted, QString form, int level, const CslNode *nameNode,
                          const Context &ctx)
{
    // Names show typographic apostrophes, as in "O’Malley".
    CslName name = original;
    for (QString *part : {&name.family, &name.given, &name.droppingParticle, &name.nonDroppingParticle, &name.suffix,
                          &name.literal})
        part->replace(u'\'', u'’');
    const CslNode *givenPart = nullptr;
    const CslNode *familyPart = nullptr;
    if (nameNode) {
        for (const CslNode &part : nameNode->children) {
            if (part.element != CslElement::NamePart) continue;
            if (part.attribute(QStringLiteral("name")) == u"given") givenPart = &part;
            if (part.attribute(QStringLiteral("name")) == u"family") familyPart = &part;
        }
    }
    auto withPart = [&ctx](Out o, const CslNode *part) {
        if (!part || o.isEmpty()) return o;
        o = wrap(std::move(o));
        finish(o, part->formatting, ctx.english);
        return o;
    };
    Out result;
    if (!name.literal.isEmpty()) {
        result = withPart(richText(name.literal), familyPart);
    } else {
        QString initializeWith;
        bool hasInitializeWith = false;
        if (nameNode && nameNode->has(QStringLiteral("initialize-with"))) {
            hasInitializeWith = true;
            initializeWith = nameNode->attribute(QStringLiteral("initialize-with"));
        } else if (ctx.section && ctx.section->contains(QStringLiteral("initialize-with"))) {
            hasInitializeWith = true;
            initializeWith = ctx.section->value(QStringLiteral("initialize-with"));
        } else if (m_style.options().contains(QStringLiteral("initialize-with"))) {
            hasInitializeWith = true;
            initializeWith = m_style.options().value(QStringLiteral("initialize-with"));
        }
        const bool initialize = option(QStringLiteral("initialize"), ctx, nameNode) != u"false";
        // Given-name disambiguation shows more of a name.
        if (form == u"short" && level >= 1) {
            form = QStringLiteral("long");
            if (level >= 2) hasInitializeWith = false;
        } else if (form != u"short" && level >= 1) {
            hasInitializeWith = false;
        }
        const bool romanesque = isRomanesque(name);
        QString given = name.given;
        // A name with only a given name, such as "Banksy", is never initialised.
        if (hasInitializeWith && romanesque && !given.isEmpty() && !name.family.isEmpty())
            given = initials(given, initializeWith,
                             initialize, m_style.options().value(QStringLiteral("initialize-with-hyphen")) != u"false");
        const QString demote = m_style.options().value(QStringLiteral("demote-non-dropping-particle"));
        QString sortSeparator = option(QStringLiteral("sort-separator"), ctx, nameNode);
        if (!nameNode || !nameNode->has(QStringLiteral("sort-separator"))) {
            if (!(ctx.section && ctx.section->contains(QStringLiteral("sort-separator")))
                && !m_style.options().contains(QStringLiteral("sort-separator")))
                sortSeparator = QStringLiteral(", ");
        }
        // Each part with the formatting of its name-part: the given name's
        // for the given name and the dropping particle, the family name's
        // for the family name and the non-dropping particle. Affixes are
        // added around the blocks below.
        auto styled = [&ctx](const QString &text, const CslNode *part) {
            Out o = Out::of(text);
            if (!part || text.isEmpty()) return o;
            CslFormatting f = part->formatting;
            f.prefix.clear();
            f.suffix.clear();
            o = wrap(std::move(o));
            finish(o, f, ctx.english);
            return o;
        };
        auto affixed = [](Out o, const CslNode *part) {
            if (!part || o.isEmpty()) return o;
            o = wrap(std::move(o));
            o.prefix = part->formatting.prefix + o.prefix;
            o.suffix += part->formatting.suffix;
            return o;
        };
        // Joins parts with spaces, but none after a particle ending in an
        // apostrophe or a hyphen ("d’Alembert", "al-One").
        auto spaced = [](const std::vector<std::pair<QString, Out>> &parts) {
            Out o;
            QString previous;
            for (const auto &[text, part] : parts) {
                if (text.isEmpty()) continue;
                if (!previous.isEmpty()) {
                    const QChar last = previous.back();
                    if (last != u'’' && last != u'-') o.kids.push_back(Out::of(QStringLiteral(" ")));
                }
                o.kids.push_back(part);
                previous = text;
            }
            return o;
        };
        const QString &dp = name.droppingParticle;
        const QString &ndp = name.nonDroppingParticle;
        if (!romanesque) {
            QString text = name.family;
            if (form != u"short") text += given;
            result = affixed(styled(text, familyPart), familyPart);
        } else if (form == u"short") {
            result = affixed(spaced({{ndp, styled(ndp, familyPart)}, {name.family, styled(name.family, familyPart)}}),
                             familyPart);
        } else if (name.staticOrdering) {
            result = Out::seq({affixed(styled(joinParticle(ndp, name.family), familyPart), familyPart),
                               affixed(styled(given, givenPart), givenPart)},
                              QStringLiteral(" "));
        } else if (!inverted) {
            // "Given dp ndp Family Suffix": the family block holds the particles and the suffix.
            std::vector<std::pair<QString, Out>> familyParts{{dp, styled(dp, givenPart)},
                                                             {ndp, styled(ndp, familyPart)},
                                                             {name.family, styled(name.family, familyPart)}};
            Out family = spaced(familyParts);
            if (!name.suffix.isEmpty())
                family = Out::seq({std::move(family), Out::of(name.suffix)},
                                  name.commaSuffix ? QStringLiteral(", ") : QStringLiteral(" "));
            family = affixed(std::move(family), familyPart);
            Out givenOut = affixed(styled(given, givenPart), givenPart);
            result = givenOut.isEmpty() ? std::move(family)
                                        : Out::seq({std::move(givenOut), std::move(family)}, QStringLiteral(" "));
        } else {
            // "Family, Given dp ndp, Suffix", or with particles kept in front,
            // "ndp Family, Given dp, Suffix".
            // Sorting demotes the particle unless the style never does.
            const bool keepParticle = demote == u"never" || (demote == u"sort-only" && !ctx.sortMode);
            Out family = affixed(keepParticle ? spaced({{ndp, styled(ndp, familyPart)},
                                                         {name.family, styled(name.family, familyPart)}})
                                              : styled(name.family, familyPart),
                                 familyPart);
            std::vector<std::pair<QString, Out>> givenParts{{given, styled(given, givenPart)},
                                                            {dp, styled(dp, givenPart)}};
            if (!keepParticle) givenParts.push_back({ndp, styled(ndp, familyPart)});
            Out givenOut = affixed(spaced(givenParts), givenPart);
            std::vector<Out> parts;
            parts.push_back(std::move(family));
            parts.push_back(std::move(givenOut));
            if (!name.suffix.isEmpty()) parts.push_back(Out::of(name.suffix));
            result = Out::seq(std::move(parts), sortSeparator);
        }
    }
    result = wrap(std::move(result));
    result.tag = Tag::Name;
    result.key = nameSortKey(original);
    return result;
}

Out Processor::nameList(const QList<CslName> &list, const QString &variable, const CslNode *nameNode,
                        const CslNode *etAlNode, Context &ctx, int *shownCount)
{
    Q_UNUSED(variable)
    auto intOption = [&](const QString &name, int fallback) {
        bool ok = false;
        const int value = option(name, ctx, nameNode).toInt(&ok);
        return ok ? value : fallback;
    };
    int etAlMin = intOption(QStringLiteral("et-al-min"), 0);
    int etAlUseFirst = intOption(QStringLiteral("et-al-use-first"), 0);
    bool etAlUseLast = option(QStringLiteral("et-al-use-last"), ctx, nameNode) == u"true";
    if (!ctx.bibliography && ctx.cite && ctx.info.position >= Subsequent) {
        const QString subsequentMin = option(QStringLiteral("et-al-subsequent-min"), ctx, nameNode);
        const QString subsequentFirst = option(QStringLiteral("et-al-subsequent-use-first"), ctx, nameNode);
        if (!subsequentMin.isEmpty()) etAlMin = subsequentMin.toInt();
        if (!subsequentFirst.isEmpty()) etAlUseFirst = subsequentFirst.toInt();
    }
    if (ctx.sortMode && ctx.sortKey) {
        if (ctx.sortKey->namesMin >= 0) etAlMin = ctx.sortKey->namesMin;
        if (ctx.sortKey->namesUseFirst >= 0) etAlUseFirst = ctx.sortKey->namesUseFirst;
        if (ctx.sortKey->namesUseLast >= 0) etAlUseLast = ctx.sortKey->namesUseLast == 1;
    }
    const int total = int(list.size());
    int shown = total;
    if (etAlMin > 0 && etAlUseFirst > 0 && total >= etAlMin && etAlUseFirst < total) shown = etAlUseFirst;
    if (!ctx.bibliography && ctx.state && ctx.state->namesShown > shown) shown = qMin(total, ctx.state->namesShown);
    const bool truncated = shown < total;
    const bool useLast = etAlUseLast && truncated && shown + 2 <= total;
    *shownCount = shown;

    QString form = option(QStringLiteral("name-form"), ctx, nameNode, QStringLiteral("form"));
    if (form.isEmpty()) form = QStringLiteral("long");
    if (form == u"count") return Out::of(QString::number(shown));

    if (ctx.sortMode) {
        // A sort key names a variable: the full names, family first. A
        // macro sorts by its names as it shows them, inverted, so that its
        // initials and forms count.
        const bool asShown = ctx.sortKey && !ctx.sortKey->macro.isEmpty();
        auto key = [&](const CslName &name) {
            return asShown ? plainOf(personName(name, true, form, 0, nameNode, ctx)) : nameSortKey(name);
        };
        QStringList keys;
        for (int i = 0; i < shown; ++i) keys.append(key(list[i]));
        if (useLast) keys.append(key(list.last()));
        return Out::of(keys.join(u'\x02'));
    }

    const QString asSortOrder = option(QStringLiteral("name-as-sort-order"), ctx, nameNode);
    QString delimiter = option(QStringLiteral("name-delimiter"), ctx, nameNode, QStringLiteral("delimiter"));
    if (!(nameNode && nameNode->has(QStringLiteral("delimiter"))) && !(ctx.section && ctx.section->contains(QStringLiteral("name-delimiter")))
        && !m_style.options().contains(QStringLiteral("name-delimiter")))
        delimiter = QStringLiteral(", ");
    const QString andOption = option(QStringLiteral("and"), ctx, nameNode);
    const QString andText = andOption == u"text" ? m_locale.termText(QStringLiteral("and"))
                            : andOption == u"symbol" ? QStringLiteral("&")
                                                     : QString();
    QString precedesLast = option(QStringLiteral("delimiter-precedes-last"), ctx, nameNode);
    if (precedesLast.isEmpty()) precedesLast = QStringLiteral("contextual");
    QString precedesEtAl = option(QStringLiteral("delimiter-precedes-et-al"), ctx, nameNode);
    if (precedesEtAl.isEmpty()) precedesEtAl = QStringLiteral("contextual");

    auto levelFor = [&](int index, const CslName &name) {
        if (ctx.bibliography) return 0;
        int level = 0;
        if (ctx.state && index < ctx.state->given.size()) level = ctx.state->given[index];
        if (ctx.state && ctx.state->expandLevel > 0 && (!globalPrimaryOnly || index == 0))
            level = qMax(level, ctx.state->expandLevel);
        if (!globalPrimaryOnly || index == 0) level = qMax(level, globalGiven.value(nameSortKey(name)));
        return level;
    };
    auto inverted = [&](int index) {
        if (index < list.size() && (!list[index].literal.isEmpty() || !isRomanesque(list[index]))) return false;
        return asSortOrder == u"all" || (asSortOrder == u"first" && index == 0);
    };

    std::vector<Out> names;
    for (int i = 0; i < shown; ++i) {
        const CslName &name = list[i];
        names.push_back(personName(name, inverted(i), form, levelFor(i, name), nameNode, ctx));
        if (ctx.shownNames) {
            ShownName record;
            record.person = nameSortKey(name);
            record.index = i;
            for (int level = 0; level < 3; ++level)
                record.forms[level] = plainOf(personName(name, false, form, level, nameNode, ctx));
            ctx.shownNames->push_back(record);
        }
    }
    // Joins the names with the delimiter and "and", as the options ask.
    Out joined;
    for (int i = 0; i < int(names.size()); ++i) {
        if (i > 0) {
            const bool last = i == int(names.size()) - 1 && !truncated;
            QString join = delimiter;
            if (last && !andText.isEmpty()) {
                bool precede = false;
                if (precedesLast == u"always")
                    precede = true;
                else if (precedesLast == u"after-inverted-name")
                    precede = inverted(i - 1);
                else if (precedesLast == u"contextual")
                    precede = shown >= 3;
                join = precede ? delimiter + andText + u' ' : QStringLiteral(" ") + andText + u' ';
            }
            joined.kids.push_back(Out::of(join));
        }
        joined.kids.push_back(std::move(names[size_t(i)]));
    }
    if (nameNode) {
        CslFormatting fonts = nameNode->formatting;
        fonts.prefix.clear();
        fonts.suffix.clear();
        joined = wrap(std::move(joined));
        finish(joined, fonts, ctx.english);
    }
    std::vector<Out> all;
    all.push_back(std::move(joined));
    if (useLast) {
        all.push_back(Out::of(delimiter + QStringLiteral("… ")));
        Out last = personName(list.last(), inverted(total - 1), form, levelFor(total - 1, list.last()), nameNode, ctx);
        if (nameNode) {
            CslFormatting fonts = nameNode->formatting;
            fonts.prefix.clear();
            fonts.suffix.clear();
            finish(last, fonts, ctx.english);
        }
        all.push_back(std::move(last));
    } else if (truncated) {
        QString term = QStringLiteral("et-al");
        if (etAlNode && etAlNode->has(QStringLiteral("term"))) term = etAlNode->attribute(QStringLiteral("term"));
        Out etAl = Out::of(m_locale.termText(term));
        if (!etAl.isEmpty()) {
            if (etAlNode) {
                etAl = wrap(std::move(etAl));
                finish(etAl, etAlNode->formatting, ctx.english);
            }
            bool precede = false;
            if (precedesEtAl == u"always")
                precede = true;
            else if (precedesEtAl == u"after-inverted-name")
                precede = inverted(shown - 1);
            else if (precedesEtAl == u"contextual")
                precede = shown >= 2;
            all.push_back(Out::of(precede ? delimiter : QStringLiteral(" ")));
            all.push_back(std::move(etAl));
        }
    }
    Out o = Out::seq(std::move(all));
    if (nameNode) {
        o.prefix = nameNode->formatting.prefix;
        o.suffix = nameNode->formatting.suffix;
    }
    // The list itself, apart from its label, is what a subsequent author
    // substitute replaces.
    o = wrap(std::move(o));
    o.tag = Tag::NameList;
    return o;
}

Out Processor::names(const CslNode &node, Context &ctx)
{
    const CslNode *nameNode = nullptr;
    const CslNode *etAlNode = nullptr;
    const CslNode *labelNode = nullptr;
    const CslNode *substitute = nullptr;
    bool labelBefore = false;
    auto scan = [&](const CslNode &n, bool withSubstitute) {
        for (const CslNode &c : n.children) {
            if (c.element == CslElement::Name) nameNode = &c;
            if (c.element == CslElement::EtAl) etAlNode = &c;
            if (c.element == CslElement::Label) {
                labelNode = &c;
                labelBefore = !nameNode;
            }
            if (c.element == CslElement::Substitute && withSubstitute) substitute = &c;
        }
        // Without a name element, the implicit name comes first.
        if (!nameNode) labelBefore = false;
    };
    scan(node, true);
    // A <names> inside <substitute> without children of its own takes the
    // parent's name, et-al and label.
    if (!nameNode && !etAlNode && !labelNode && ctx.parentNames) scan(*ctx.parentNames, false);

    struct Part {
        QString variable;
        QList<CslName> names;
        QString term;
    };
    QList<Part> parts;
    for (const QString &variable : node.attribute(QStringLiteral("variable")).split(u' ', Qt::SkipEmptyParts)) {
        if (ctx.suppressed.contains(variable) || !ctx.reference) {
            noteVariable(ctx, variable, false);
            continue;
        }
        const QList<CslName> list = ctx.reference->names.value(variable);
        noteVariable(ctx, variable, !list.isEmpty());
        if (!list.isEmpty()) parts.append({variable, list, variable});
    }
    // An editor who is also the translator is named once.
    qsizetype editor = -1;
    qsizetype translator = -1;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (parts[i].variable == u"editor") editor = i;
        if (parts[i].variable == u"translator") translator = i;
    }
    const QString labelForm = labelNode && labelNode->has(QStringLiteral("form")) ? labelNode->attribute(QStringLiteral("form"))
                                                                                  : QStringLiteral("long");
    if (editor >= 0 && translator >= 0 && parts[editor].names == parts[translator].names
        && !m_locale.termText(QStringLiteral("editortranslator"), labelForm).isEmpty()) {
        parts[qMin(editor, translator)].term = QStringLiteral("editortranslator");
        parts.removeAt(qMax(editor, translator));
    }

    Out o;
    if (parts.isEmpty()) {
        if (!substitute) return {};
        for (const CslNode &child : substitute->children) {
            QSet<QString> used;
            QSet<QString> *outerUsed = ctx.used;
            const CslNode *outerParent = ctx.parentNames;
            ctx.used = &used;
            ctx.parentNames = &node;
            Out candidate = render(child, ctx);
            ctx.used = outerUsed;
            ctx.parentNames = outerParent;
            if (outerUsed) outerUsed->unite(used);
            if (!candidate.isEmpty()) {
                // What stands in for the names is not repeated later in the cite.
                for (const QString &variable : std::as_const(used)) ctx.suppressed.insert(variable);
                // Names that stand in are identified by the people, not their label.
                const QString key = candidate.tag == Tag::Names ? candidate.key : QString();
                o = wrap(std::move(candidate));
                if (o.tag == Tag::Names) o = wrap(Out::seq({std::move(o)}));
                o.key = key;
                break;
            }
        }
        if (o.isEmpty()) return {};
        if (o.key.isEmpty()) o.key = plainOf(o);
    } else {
        QString namesDelimiter = node.hasDelimiter ? node.delimiter : option(QStringLiteral("names-delimiter"), ctx);
        int total = 0;
        std::vector<Out> lists;
        QStringList keys;
        for (const Part &part : parts) {
            int shown = 0;
            Out list = nameList(part.names, part.variable, nameNode, etAlNode, ctx, &shown);
            total += shown;
            for (int i = 0; i < shown && i < part.names.size(); ++i) keys.append(nameSortKey(part.names[i]));
            if (labelNode && !ctx.sortMode) {
                QString form = labelNode->attribute(QStringLiteral("form"));
                if (form.isEmpty()) form = QStringLiteral("long");
                const QString pluralMode = labelNode->attribute(QStringLiteral("plural"));
                const bool plural = pluralMode == u"always" || (pluralMode != u"never" && part.names.size() > 1);
                Out label = Out::of(m_locale.termText(part.term, form, plural));
                if (!label.isEmpty()) {
                    label = wrap(std::move(label));
                    finish(label, labelNode->formatting, ctx.english);
                    if (labelBefore)
                        list = Out::seq({std::move(label), std::move(list)});
                    else
                        list = Out::seq({std::move(list), std::move(label)});
                }
            }
            lists.push_back(std::move(list));
        }
        const QString form = option(QStringLiteral("name-form"), ctx, nameNode, QStringLiteral("form"));
        if (form == u"count") {
            if (ctx.sortMode) return Out::of(QStringLiteral("%1").arg(total, 10, 10, QLatin1Char('0')));
            o = Out::of(QString::number(total));
        } else {
            o = Out::seq(std::move(lists), ctx.sortMode ? QStringLiteral("\x02") : namesDelimiter);
        }
        if (ctx.sortMode) return o;
        o = wrap(std::move(o));
        o.key = keys.join(u'\x02');
    }
    // The author left out of a cite is the outermost names element that
    // shows something, with whatever stands in for it.
    if (ctx.suppressNames && !ctx.namesSuppressed && !ctx.parentNames) {
        ctx.namesSuppressed = true;
        return {};
    }
    const QString key = o.key;
    finish(o, node.formatting, ctx.english);
    o = wrap(std::move(o));
    o.tag = Tag::Names;
    o.key = key.isEmpty() ? plainOf(o) : key;
    return o;
}

SortValue Processor::sortValue(const CslStyle::SortKey &key, Context ctx)
{
    SortValue value;
    ctx.sortMode = true;
    ctx.sortKey = &key;
    if (!key.macro.isEmpty()) {
        const CslNode *macro = m_style.macro(key.macro);
        if (!macro) return value;
        const Out o = renderChildren(macro->children, ctx);
        value.text = foldForSort(CslText::fromHtml(plainOf(o)).toPlainText());
        value.empty = value.text.isEmpty();
        return value;
    }
    const QString &variable = key.variable;
    if (CslReference::isNameVariable(variable)) {
        const QList<CslName> list = ctx.reference ? ctx.reference->names.value(variable) : QList<CslName>();
        if (list.isEmpty()) return value;
        int shown = 0;
        const Out o = nameList(list, variable, nullptr, nullptr, ctx, &shown);
        value.text = foldForSort(plainOf(o));
        value.empty = value.text.isEmpty();
        return value;
    }
    if (CslReference::isDateVariable(variable)) {
        const CslDate date = ctx.reference ? ctx.reference->dates.value(variable) : CslDate();
        if (date.isEmpty()) return value;
        value.empty = false;
        value.text = date.literal.isEmpty()
                         ? dateSortKey(date.start) + (date.isRange ? QStringLiteral("/") + dateSortKey(date.end) : QString())
                         : foldForSort(date.literal);
        return value;
    }
    const QString text = CslText::fromHtml(variableText(variable, ctx)).toPlainText();
    if (text.isEmpty()) return value;
    value.empty = false;
    if (CslReference::isNumberVariable(variable) && isNumeric(text)) {
        static const QRegularExpression digits(QStringLiteral("\\d+"));
        value.numeric = true;
        value.number = digits.match(text).captured(0).toDouble();
    }
    value.text = foldForSort(text);
    return value;
}

// ---------------------------------------------------------------------------
// Citations and the bibliography.

class Run {
public:
    Run(const CslStyle &style, const CslLocale &locale) : m_style(style), m_locale(locale), m_processor(style, locale) {}

    CslEngine::Result process(const QList<CslCitation> &input, const QList<const CslReference *> &uncited);

private:
    struct Item {
        CslCite cite;
        CiteInfo info;
        int reference = -1;
    };

    Context context(int reference, const CslCite *cite, const CiteInfo &info, bool bibliography) const
    {
        Context ctx;
        ctx.reference = reference >= 0 ? m_references[size_t(reference)] : nullptr;
        ctx.cite = cite;
        ctx.info = info;
        ctx.bibliography = bibliography;
        ctx.state = reference >= 0 ? &m_states[size_t(reference)] : nullptr;
        ctx.section = bibliography && m_style.bibliography() ? &m_style.bibliography()->options
                                                             : &m_style.citation().options;
        const QString language = ctx.reference ? ctx.reference->field(QStringLiteral("language")) : QString();
        ctx.english = language.isEmpty() || language.toLower().startsWith(u"en");
        return ctx;
    }

    Out renderCite(const Item &item, bool suppressNames = false, std::vector<ShownName> *shownNames = nullptr);
    QString citeKey(int reference);
    std::vector<std::vector<int>> ambiguousGroups();
    void disambiguate();
    std::vector<SortValue> keysFor(const std::vector<CslStyle::SortKey> &keys, int reference, const CslCite *cite,
                                   bool bibliography);
    CslText renderCitation(std::vector<Item> items);
    CslText flatten(const Out &o) const
    {
        Flattener flattener(m_locale);
        flattener.add(o);
        return flattener.result();
    }

    const CslStyle &m_style;
    const CslLocale &m_locale;
    Processor m_processor;
    std::vector<const CslReference *> m_references;
    std::vector<ItemState> m_states;
    // Each reference's place in the bibliography.
    std::vector<int> m_bibliographyRank;
    std::vector<std::vector<Item>> m_citations;
    // The references cited, as opposed to those only listed.
    QSet<int> m_cited;
};

std::vector<SortValue> Run::keysFor(const std::vector<CslStyle::SortKey> &keys, int reference, const CslCite *cite,
                                    bool bibliography)
{
    std::vector<SortValue> values;
    const Context ctx = context(reference, cite, {}, bibliography);
    for (const CslStyle::SortKey &key : keys) values.push_back(m_processor.sortValue(key, ctx));
    return values;
}

Out Run::renderCite(const Item &item, bool suppressNames, std::vector<ShownName> *shownNames)
{
    if (item.reference < 0) {
        Out missing = Out::of(item.cite.missingKey + u'?');
        missing.bold = 1;
        return wrap(std::move(missing));
    }
    Context ctx = context(item.reference, &item.cite, item.info, false);
    ctx.suppressNames = suppressNames;
    ctx.shownNames = shownNames;
    Out o = m_processor.layoutBody(m_style.citation(), ctx);
    return o;
}

QString Run::citeKey(int reference)
{
    Item item;
    item.reference = reference;
    item.info.position = m_style.isNoteStyle() ? Subsequent : First;
    return flatten(renderCite(item)).toPlainText();
}

std::vector<std::vector<int>> Run::ambiguousGroups()
{
    QHash<QString, std::vector<int>> byText;
    QStringList order;
    for (int i = 0; i < int(m_references.size()); ++i) {
        if (!m_cited.contains(i)) continue;
        const QString key = citeKey(i);
        if (!byText.contains(key)) order.append(key);
        byText[key].push_back(i);
    }
    std::vector<std::vector<int>> groups;
    for (const QString &key : order) {
        if (byText[key].size() > 1) groups.push_back(byText[key]);
    }
    return groups;
}

} // namespace

namespace {

// "Ibid." starts a note, or a sentence after a cite's prefix, with a capital.
void capitalizeIbid(CslText &text, const QString &ibid)
{
    if (ibid.isEmpty()) return;
    const QString plain = text.toPlainText();
    QList<qsizetype> at;
    qsizetype from = 0;
    for (;;) {
        const qsizetype found = plain.indexOf(ibid, from, Qt::CaseInsensitive);
        if (found < 0) break;
        qsizetype before = found - 1;
        while (before >= 0 && (plain[before].isSpace() || plain[before] == u'”' || plain[before] == u'’')) --before;
        if (before < 0 || plain[before] == u'.' || plain[before] == u'?' || plain[before] == u'!') at.append(found);
        from = found + ibid.size();
    }
    if (at.isEmpty()) return;
    CslText result;
    qsizetype offset = 0;
    for (CslSpan span : text.spans()) {
        for (const qsizetype position : at) {
            if (position >= offset && position < offset + span.text.size())
                span.text[position - offset] = span.text[position - offset].toUpper();
        }
        offset += span.text.size();
        result.append(span.text, span.format);
    }
    text = result;
}

QString yearSuffixLetters(int index)
{
    QString letters;
    int n = index;
    do {
        letters.prepend(QChar(u'a' + n % 26));
        n = n / 26 - 1;
    } while (n >= 0);
    return letters;
}

} // namespace

void Run::disambiguate()
{
    const QHash<QString, QString> &options = m_style.citation().options;
    const bool addNames = options.value(QStringLiteral("disambiguate-add-names")) == u"true";
    const bool addGiven = options.value(QStringLiteral("disambiguate-add-givenname")) == u"true";
    const bool addYear = options.value(QStringLiteral("disambiguate-add-year-suffix")) == u"true";
    QString rule = options.value(QStringLiteral("givenname-disambiguation-rule"));
    if (rule.isEmpty()) rule = QStringLiteral("by-cite");

    // Names that read alike for different people are expanded wherever
    // they appear, whether or not their cites are ambiguous.
    const bool global = addGiven && rule != u"by-cite";
    const bool primary = rule.startsWith(u"primary-name");
    const int maxLevel = rule.endsWith(u"with-initials") ? 1 : 2;
    auto expandNamesGlobally = [&] {
        std::vector<ShownName> shown;
        for (const int i : std::as_const(m_cited)) {
            Item item;
            item.reference = i;
            renderCite(item, false, &shown);
        }
        QHash<QString, ShownName> persons;
        for (const ShownName &name : shown) {
            if (primary && name.index != 0) continue;
            if (!persons.contains(name.person)) persons.insert(name.person, name);
        }
        QHash<QString, int> levels;
        for (auto p = persons.constBegin(); p != persons.constEnd(); ++p) {
            // The first level at which no other person reads the same; a
            // name that no allowed level tells apart stays as it is.
            int level = 0;
            for (; level <= maxLevel; ++level) {
                bool clash = false;
                for (auto q = persons.constBegin(); q != persons.constEnd() && !clash; ++q)
                    clash = q.key() != p.key() && q->forms[level] == p->forms[level];
                if (!clash) break;
            }
            if (level > 0 && level <= maxLevel) levels.insert(p.key(), level);
        }
        m_processor.globalGiven = levels;
        m_processor.globalPrimaryOnly = primary;
    };
    if (global) expandNamesGlobally();

    std::vector<std::vector<int>> groups = ambiguousGroups();
    if (groups.empty()) return;
    auto distinct = [this](const std::vector<int> &group) {
        QSet<QString> texts;
        for (const int i : group) texts.insert(citeKey(i));
        return int(texts.size());
    };
    auto mostNames = [this](const std::vector<int> &group) {
        int most = 0;
        for (const int i : group)
            for (const QList<CslName> &list : std::as_const(m_references[size_t(i)]->names))
                most = qMax(most, int(list.size()));
        return most;
    };

    if (addNames) {
        for (const std::vector<int> &group : groups) {
            std::vector<int> original;
            for (const int i : group) original.push_back(m_states[size_t(i)].namesShown);
            // With given names to come, names are added as if every given
            // name were shown, so that a name differing only in its given
            // name counts as telling cites apart.
            for (const int i : group) m_states[size_t(i)].expandLevel = addGiven ? maxLevel : 0;
            int best = distinct(group);
            int bestCount = -1;
            const int most = mostNames(group);
            for (int n = 1; n <= most && best < int(group.size()); ++n) {
                for (const int i : group) m_states[size_t(i)].namesShown = n;
                const int count = distinct(group);
                if (count > best) {
                    best = count;
                    bestCount = n;
                }
            }
            for (size_t k = 0; k < group.size(); ++k) {
                m_states[size_t(group[k])].expandLevel = 0;
                m_states[size_t(group[k])].namesShown = bestCount < 0 ? original[k] : bestCount;
            }
        }
        // Names shown now may need their given names to tell people apart.
        if (global) expandNamesGlobally();
        groups = ambiguousGroups();
    }

    // By cite, names expand only where cites are still ambiguous; the other
    // rules have expanded what they allow above.
    if (addGiven && rule == u"by-cite" && !groups.empty()) {
        for (const std::vector<int> &group : groups) {
            int current = distinct(group);
            const int most = mostNames(group);
            for (int index = 0; index < most && current < int(group.size()); ++index) {
                for (int level = 1; level <= 2 && current < int(group.size()); ++level) {
                    std::vector<QList<int>> saved;
                    for (const int i : group) {
                        ItemState &state = m_states[size_t(i)];
                        saved.push_back(state.given);
                        while (state.given.size() <= index) state.given.append(0);
                        state.given[index] = qMax(state.given[index], level);
                    }
                    const int count = distinct(group);
                    if (count > current) {
                        current = count;
                    } else {
                        for (size_t k = 0; k < group.size(); ++k) m_states[size_t(group[k])].given = saved[k];
                    }
                }
            }
        }
        groups = ambiguousGroups();
    }

    if (!groups.empty()) {
        // Styles can show more for cites that are still ambiguous.
        for (const std::vector<int> &group : groups)
            for (const int i : group) m_states[size_t(i)].disambiguate = true;
        groups = ambiguousGroups();
    }

    if (addYear) {
        for (std::vector<int> group : groups) {
            std::stable_sort(group.begin(), group.end(), [this](int a, int b) {
                return m_bibliographyRank[size_t(a)] < m_bibliographyRank[size_t(b)];
            });
            for (size_t k = 0; k < group.size(); ++k) m_states[size_t(group[k])].yearSuffix = yearSuffixLetters(int(k));
        }
    }
}

CslText Run::renderCitation(std::vector<Item> items)
{
    const CslStyle::Section &section = m_style.citation();
    const QString collapse = section.options.value(QStringLiteral("collapse"));
    const QString layoutDelimiter = section.layout.delimiter;
    const QString citeGroupDelimiter = section.options.contains(QStringLiteral("cite-group-delimiter"))
                                           ? section.options.value(QStringLiteral("cite-group-delimiter"))
                                           : QStringLiteral(", ");
    const QString yearSuffixDelimiter = section.options.contains(QStringLiteral("year-suffix-delimiter"))
                                            ? section.options.value(QStringLiteral("year-suffix-delimiter"))
                                        : section.options.contains(QStringLiteral("cite-group-delimiter"))
                                            ? citeGroupDelimiter
                                            : layoutDelimiter;
    const QString afterCollapseDelimiter = section.options.contains(QStringLiteral("after-collapse-delimiter"))
                                               ? section.options.value(QStringLiteral("after-collapse-delimiter"))
                                               : layoutDelimiter;

    auto affixed = [](Out o, const CslCite &cite) {
        if (cite.prefix.isEmpty() && cite.suffix.isEmpty()) return o;
        std::vector<Out> kids;
        if (!cite.prefix.isEmpty()) {
            QString prefix = cite.prefix;
            const QChar last = prefix.back();
            if (!last.isSpace() && last != u'(' && last != u'[' && last != u'“' && last != u'‘') prefix += u' ';
            kids.push_back(richText(prefix, false));
        }
        kids.push_back(std::move(o));
        if (!cite.suffix.isEmpty()) {
            QString suffix = cite.suffix;
            if (suffix.front().isLetterOrNumber()) suffix.prepend(u' ');
            kids.push_back(richText(suffix, false));
        }
        return Out::seq(std::move(kids));
    };

    // A narrative citation: the author outside, the rest as usual.
    Out narrative;
    if (!items.empty() && items.front().cite.mode == CslCite::Mode::AuthorInText && items.front().reference >= 0) {
        Out full = renderCite(items.front());
        if (Out *names = findTag(full, Tag::Names)) {
            narrative = *names;
            narrative.prefix.clear();
            narrative.suffix.clear();
        }
        items.front().cite.mode = CslCite::Mode::SuppressAuthor;
    }
    bool onlyAuthors = !items.empty();
    for (const Item &item : items) onlyAuthors = onlyAuthors && item.cite.mode == CslCite::Mode::AuthorOnly;
    if (onlyAuthors) {
        std::vector<Out> authors;
        for (const Item &item : items) {
            Out full = renderCite(item);
            Out *names = findTag(full, Tag::Names);
            authors.push_back(names ? *names : full);
        }
        return flatten(Out::seq(std::move(authors), layoutDelimiter));
    }

    std::vector<Out> rendered;
    for (const Item &item : items) rendered.push_back(renderCite(item, item.cite.mode == CslCite::Mode::SuppressAuthor));
    auto plain = [](const CslCite &cite) {
        return cite.locator.isEmpty() && cite.prefix.isEmpty() && cite.suffix.isEmpty()
               && cite.mode == CslCite::Mode::Normal;
    };

    // Units joined by delimiters: each is one cite or a collapsed run.
    std::vector<Out> units;
    std::vector<QString> before;
    // The first and last cite of each unit, whose prefix and suffix can
    // stand in for the delimiter.
    std::vector<const CslCite *> unitFirst;
    std::vector<const CslCite *> unitLast;
    auto addUnit = [&](Out o, const QString &delimiter, int first, int last) {
        before.push_back(units.empty() ? QString() : delimiter);
        units.push_back(std::move(o));
        unitFirst.push_back(&items[size_t(first)].cite);
        unitLast.push_back(&items[size_t(last)].cite);
    };
    const int n = int(items.size());
    if (collapse == u"citation-number") {
        auto number = [&](int k) {
            const int reference = items[size_t(k)].reference;
            return reference < 0 ? -10 : m_states[size_t(reference)].citationNumber.toInt();
        };
        QString next = layoutDelimiter;
        int i = 0;
        while (i < n) {
            int j = i;
            while (j + 1 < n && plain(items[size_t(j)].cite) && plain(items[size_t(j + 1)].cite)
                   && items[size_t(j)].reference >= 0 && number(j + 1) == number(j) + 1)
                ++j;
            if (j - i >= 2) {
                Out range = Out::seq({rendered[size_t(i)], Out::of(QStringLiteral("–")), rendered[size_t(j)]});
                addUnit(std::move(range), next, i, j);
                next = afterCollapseDelimiter;
            } else {
                for (int k = i; k <= j; ++k) {
                    addUnit(affixed(rendered[size_t(k)], items[size_t(k)].cite), next, k, k);
                    next = layoutDelimiter;
                }
            }
            i = j + 1;
        }
    } else if (collapse.startsWith(u"year") || section.options.contains(QStringLiteral("cite-group-delimiter"))) {
        const bool collapsing = collapse.startsWith(u"year");
        // Cites with the same names form a group. A sorted citation brings
        // them together, in the order their names first appear; in a
        // citation the style does not sort, only adjacent cites group, as
        // the CSL test suite expects.
        const bool regroup = !section.sort.empty();
        QStringList order;
        QHash<QString, std::vector<int>> groups;
        QString lastKey;
        for (int k = 0; k < n; ++k) {
            QString key = QStringLiteral("\x03%1").arg(k);
            const Item &item = items[size_t(k)];
            const bool groupable = item.cite.mode == CslCite::Mode::Normal
                                   || item.cite.mode == CslCite::Mode::SuppressAuthor;
            if (item.reference >= 0 && groupable) {
                // A cite without its author groups by the author it leaves out.
                Out full = item.cite.mode == CslCite::Mode::Normal ? rendered[size_t(k)] : renderCite(item);
                if (Out *names = findTag(full, Tag::Names)) key = names->key + u'\x04' + plainOf(*names);
            }
            if (regroup) {
                if (!groups.contains(key)) order.append(key);
                groups[key].push_back(k);
            } else if (k > 0 && key == lastKey) {
                groups[order.last()].push_back(k);
            } else {
                order.append(QString::number(k));
                groups[order.last()].push_back(k);
            }
            lastKey = key;
        }
        auto dateKey = [&](int k) {
            Out full = rendered[size_t(k)];
            Out *date = findTag(full, Tag::Date);
            if (!date) return QString();
            Out copy = *date;
            std::vector<Out *> suffixes;
            findTags(copy, Tag::YearSuffix, suffixes);
            for (Out *suffix : suffixes) *suffix = Out();
            return plainOf(copy);
        };
        bool previousCollapsed = false;
        for (const QString &key : std::as_const(order)) {
            const std::vector<int> &group = groups[key];
            std::vector<Out> parts;
            // How each part joins the one before it; a suffix-only part joins with a mark.
            std::vector<QString> joins;
            std::vector<bool> suffixJoins;
            for (size_t g = 0; g < group.size(); ++g) {
                const int k = group[g];
                const Item &item = items[size_t(k)];
                if (g == 0 || !collapsing) {
                    parts.push_back(affixed(rendered[size_t(k)], item.cite));
                    joins.push_back(citeGroupDelimiter);
                    suffixJoins.push_back(false);
                    continue;
                }
                const Item &previous = items[size_t(group[g - 1])];
                const bool suffixOnly = (collapse == u"year-suffix" || collapse == u"year-suffix-ranged")
                                        && item.reference >= 0 && previous.reference >= 0
                                        && !m_states[size_t(item.reference)].yearSuffix.isEmpty()
                                        && !m_states[size_t(previous.reference)].yearSuffix.isEmpty()
                                        && plain(item.cite) && previous.cite.locator.isEmpty()
                                        && dateKey(k) == dateKey(group[g - 1]);
                if (suffixOnly) {
                    Out full = rendered[size_t(k)];
                    Out *suffix = findTag(full, Tag::YearSuffix);
                    Out only = suffix ? *suffix : Out::of(m_states[size_t(item.reference)].yearSuffix);
                    only.prefix.clear();
                    only.suffix.clear();
                    parts.push_back(std::move(only));
                    joins.push_back(yearSuffixDelimiter);
                    suffixJoins.push_back(true);
                } else {
                    parts.push_back(affixed(renderCite(item, true), item.cite));
                    // A cite with a locator ends a run of collapsed years.
                    joins.push_back(previous.cite.locator.isEmpty() ? citeGroupDelimiter : afterCollapseDelimiter);
                    suffixJoins.push_back(false);
                }
            }
            // year-suffix-ranged: three or more consecutive suffixes in a
            // run, counting the one of the cite that starts it, become a
            // range ("2000a–c"); others stay apart ("2000a, c").
            auto suffixOf = [&](size_t p) {
                const int reference = items[size_t(group[p])].reference;
                return reference < 0 ? QString() : m_states[size_t(reference)].yearSuffix;
            };
            auto follows = [&](size_t p) {
                const QString a = suffixOf(p - 1);
                const QString b = suffixOf(p);
                return a.size() == 1 && b.size() == 1 && b[0].unicode() == a[0].unicode() + 1;
            };
            Out unit;
            for (size_t p = 0; p < parts.size(); ++p) {
                if (parts[p].isEmpty()) continue;
                if (collapse == u"year-suffix-ranged" && p + 1 < parts.size() && suffixJoins[p + 1] && follows(p + 1)) {
                    size_t q = p + 1;
                    while (q + 1 < parts.size() && suffixJoins[q + 1] && follows(q + 1)) ++q;
                    if (q - p >= 2) {
                        if (!unit.kids.empty()) unit.kids.push_back(Out::of(joins[p]));
                        unit.kids.push_back(parts[p]);
                        unit.kids.push_back(Out::of(QStringLiteral("–")));
                        unit.kids.push_back(parts[q]);
                        p = q;
                        continue;
                    }
                }
                if (!unit.kids.empty()) {
                    // A year after collapsed year-suffixes follows the after-collapse delimiter.
                    const bool afterSuffixes = p > 0 && !suffixJoins[p] && suffixJoins[p - 1];
                    unit.kids.push_back(Out::of(afterSuffixes ? afterCollapseDelimiter : joins[p]));
                }
                unit.kids.push_back(parts[p]);
            }
            addUnit(std::move(unit), previousCollapsed ? afterCollapseDelimiter : layoutDelimiter, group.front(),
                    group.back());
            previousCollapsed = collapsing && group.size() > 1;
        }
    } else {
        for (int k = 0; k < n; ++k)
            addUnit(affixed(rendered[size_t(k)], items[size_t(k)].cite), layoutDelimiter, k, k);
    }

    // Punctuation that a cite's own prefix starts with, or that the cite
    // before ends its suffix with, replaces the delimiter.
    auto punctuation = [](QChar c) { return c == u',' || c == u'.' || c == u';' || c == u':'; };
    Out body;
    size_t previousUnit = 0;
    for (size_t u = 0; u < units.size(); ++u) {
        // A cite that shows nothing takes no delimiter.
        if (units[u].isEmpty()) continue;
        if (!body.kids.empty()) {
            QString delimiter = before[u];
            const QString prefix = unitFirst[u]->prefix.trimmed();
            const QString &suffix = unitLast[previousUnit]->suffix;
            if (!prefix.isEmpty() && punctuation(prefix.front()))
                delimiter.clear();
            else if (!suffix.trimmed().isEmpty() && punctuation(suffix.trimmed().back()))
                delimiter = suffix.back().isSpace() ? QString() : QStringLiteral(" ");
            // A suffix that ends a sentence is followed directly by the next cite.
            if (!suffix.isEmpty() && suffix.back() == u'.') delimiter.clear();
            body.kids.push_back(Out::of(delimiter));
        }
        body.kids.push_back(std::move(units[u]));
        previousUnit = u;
    }
    // The layout's fonts take in its affixes too, as citeproc-js sets them.
    Out layout = wrap(std::move(body));
    layout.prefix = section.layout.formatting.prefix;
    layout.suffix = section.layout.formatting.suffix;
    layout = wrap(std::move(layout));
    CslFormatting fonts = section.layout.formatting;
    fonts.prefix.clear();
    fonts.suffix.clear();
    finish(layout, fonts, true);
    if (!narrative.isEmpty()) return flatten(Out::seq({std::move(narrative), std::move(layout)}, QStringLiteral(" ")));
    return flatten(layout);
}

CslEngine::Result Run::process(const QList<CslCitation> &input, const QList<const CslReference *> &uncited)
{
    QHash<const CslReference *, int> index;
    auto add = [&](const CslReference *reference) {
        if (!reference || index.contains(reference)) return;
        index.insert(reference, int(m_references.size()));
        m_references.push_back(reference);
    };
    for (const CslCitation &citation : input)
        for (const CslCite &cite : citation.cites) add(cite.reference);
    for (const CslReference *reference : uncited) add(reference);
    // Every listed reference takes part in disambiguation, so that the
    // bibliography tells apart even entries no citation names.
    for (int i = 0; i < int(m_references.size()); ++i) m_cited.insert(i);
    const int count = int(m_references.size());
    m_states.assign(size_t(count), ItemState());
    m_bibliographyRank.assign(size_t(count), 0);

    // The bibliography's order, which numbers the references.
    std::vector<int> order(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        order[size_t(i)] = i;
        m_states[size_t(i)].citationNumber = QString::number(i + 1);
    }
    const CslStyle::Section *bibliography = m_style.bibliography();
    bool numbersFollowOrder = true;
    if (bibliography && !bibliography->sort.empty()) {
        std::vector<std::vector<SortValue>> keys(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) keys[size_t(i)] = keysFor(bibliography->sort, i, nullptr, true);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            for (size_t k = 0; k < bibliography->sort.size(); ++k) {
                const int c = compareValues(keys[size_t(a)][k], keys[size_t(b)][k], bibliography->sort[k].descending);
                if (c != 0) return c < 0;
            }
            return false;
        });
        for (const CslStyle::SortKey &key : bibliography->sort)
            if (key.variable == u"citation-number") numbersFollowOrder = false;
    }
    for (int k = 0; k < count; ++k) {
        m_bibliographyRank[size_t(order[size_t(k)])] = k;
        if (numbersFollowOrder) m_states[size_t(order[size_t(k)])].citationNumber = QString::number(k + 1);
    }

    // Cites in each citation, sorted as the style asks.
    const std::vector<CslStyle::SortKey> &citationSort = m_style.citation().sort;
    for (const CslCitation &citation : input) {
        std::vector<Item> items;
        for (const CslCite &cite : citation.cites) {
            Item item;
            item.cite = cite;
            item.cite.locator = cite.locator.trimmed();
            // A locator without a type that starts with a label ("vol. 1,
            // fol. 186") takes its type from it.
            if (item.cite.label.isEmpty() && !item.cite.locator.isEmpty() && !item.cite.locator[0].isDigit()) {
                const CslEngine::Locator parsed = CslEngine::parseLocator(item.cite.locator, m_locale);
                if (!parsed.label.isEmpty() && !parsed.locator.isEmpty()) {
                    item.cite.label = parsed.label;
                    item.cite.locator = parsed.locator + parsed.suffix;
                }
            }
            item.reference = cite.reference ? index.value(cite.reference, -1) : -1;
            items.push_back(item);
        }
        if (!citationSort.empty() && items.size() > 1) {
            std::vector<std::vector<SortValue>> keys;
            for (const Item &item : items)
                keys.push_back(item.reference < 0 ? std::vector<SortValue>(citationSort.size())
                                                  : keysFor(citationSort, item.reference, &item.cite, false));
            std::vector<int> positions(items.size());
            for (size_t k = 0; k < items.size(); ++k) positions[k] = int(k);
            std::stable_sort(positions.begin(), positions.end(), [&](int a, int b) {
                for (size_t k = 0; k < citationSort.size(); ++k) {
                    const int c = compareValues(keys[size_t(a)][k], keys[size_t(b)][k], citationSort[k].descending);
                    if (c != 0) return c < 0;
                }
                return false;
            });
            std::vector<Item> sorted;
            for (const int p : positions) sorted.push_back(items[size_t(p)]);
            items = sorted;
        }
        m_citations.push_back(items);
    }

    // Positions: first, subsequent, ibid and near-note.
    bool ok = false;
    int nearNoteDistance = m_style.citation().options.value(QStringLiteral("near-note-distance")).toInt(&ok);
    if (!ok) nearNoteDistance = 5;
    QHash<int, int> firstNote;
    QHash<int, int> lastNote;
    // What a citation's first cite follows: the cite before it in the same
    // note, or else the cites of the note (or text citation) before.
    std::vector<const Item *> previousGroup;
    std::vector<const Item *> currentGroup;
    int currentNote = -1;
    for (size_t c = 0; c < m_citations.size(); ++c) {
        std::vector<Item> &items = m_citations[c];
        const int note = input[qsizetype(c)].noteIndex;
        const bool sameNote = note > 0 && note == currentNote;
        if (!sameNote) {
            if (c > 0) previousGroup = currentGroup;
            currentGroup.clear();
        }
        currentNote = note;
        for (size_t k = 0; k < items.size(); ++k) {
            Item &item = items[k];
            if (item.reference < 0) continue;
            if (!firstNote.contains(item.reference)) {
                item.info.position = First;
                firstNote.insert(item.reference, note);
            } else {
                const Item *prior = nullptr;
                if (k > 0)
                    prior = &items[k - 1];
                else if (sameNote && !currentGroup.empty())
                    prior = currentGroup.back();
                else if (!sameNote && previousGroup.size() == 1)
                    prior = previousGroup.front();
                if (prior && prior->reference == item.reference) {
                    const QString &before = prior->cite.locator;
                    const QString &now = item.cite.locator;
                    if (before.isEmpty() && now.isEmpty())
                        item.info.position = Ibid;
                    else if (now.isEmpty())
                        item.info.position = Subsequent;
                    else if (before == now && prior->cite.label == item.cite.label)
                        item.info.position = Ibid;
                    else
                        item.info.position = IbidWithLocator;
                } else {
                    item.info.position = Subsequent;
                }
                item.info.nearNote = note - lastNote.value(item.reference) <= nearNoteDistance;
            }
            item.info.firstNote = firstNote.value(item.reference);
            lastNote.insert(item.reference, note);
        }
        for (const Item &item : items) currentGroup.push_back(&item);
    }

    disambiguate();

    CslEngine::Result result;
    const QString ibid = m_locale.termText(QStringLiteral("ibid"));
    for (const std::vector<Item> &items : m_citations) {
        CslText text = renderCitation(items);
        if (m_style.isNoteStyle()) capitalizeIbid(text, ibid);
        result.citations.append(text);
    }

    if (bibliography) {
        const QHash<QString, QString> &options = bibliography->options;
        result.layout.hangingIndent = options.value(QStringLiteral("hanging-indent")) == u"true";
        result.layout.secondFieldAlign = options.value(QStringLiteral("second-field-align"));
        bool numberOk = false;
        const double lineSpacing = options.value(QStringLiteral("line-spacing")).toDouble(&numberOk);
        if (numberOk && lineSpacing > 0) result.layout.lineSpacing = lineSpacing;
        const double entrySpacing = options.value(QStringLiteral("entry-spacing")).toDouble(&numberOk);
        if (numberOk && entrySpacing >= 0) result.layout.entrySpacing = entrySpacing;
        const bool substitute = options.contains(QStringLiteral("subsequent-author-substitute"));
        const QString substituteText = options.value(QStringLiteral("subsequent-author-substitute"));
        QString rule = options.value(QStringLiteral("subsequent-author-substitute-rule"));
        if (rule.isEmpty()) rule = QStringLiteral("complete-all");
        QStringList previousNames;
        QString previousKey;
        for (const int i : order) {
            Context ctx = context(i, nullptr, {}, true);
            std::vector<Out> kids;
            for (const CslNode &child : bibliography->layout.children) kids.push_back(m_processor.render(child, ctx));
            CslFormatting layoutFormat = bibliography->layout.formatting;
            if (!result.layout.secondFieldAlign.isEmpty()) {
                size_t firstField = 0;
                while (firstField < kids.size() && kids[firstField].isEmpty()) ++firstField;
                if (firstField < kids.size()) {
                    Out margin = wrap(std::move(kids[firstField]));
                    margin.display = CslDisplay::LeftMargin;
                    std::vector<Out> rest;
                    for (size_t k = firstField + 1; k < kids.size(); ++k) rest.push_back(std::move(kids[k]));
                    Out right = Out::seq(std::move(rest));
                    right.display = CslDisplay::RightInline;
                    right.suffix = layoutFormat.suffix;
                    layoutFormat.suffix.clear();
                    kids.clear();
                    kids.push_back(std::move(margin));
                    kids.push_back(std::move(right));
                }
            }
            Out entry = wrap(Out::seq(std::move(kids)));
            finish(entry, layoutFormat, ctx.english);
            if (substitute) {
                Out *names = findTag(entry, Tag::Names);
                QStringList keys;
                QString key;
                if (names) {
                    std::vector<Out *> people;
                    findTags(*names, Tag::Name, people);
                    for (Out *person : people) keys.append(person->key);
                    key = names->key;
                    const bool same = !key.isEmpty() && key == previousKey;
                    if (rule == u"complete-all" && same) {
                        // The names give way to the substitute; a label stays.
                        Out *list = findTag(*names, Tag::NameList);
                        Out *replaced = list ? list : names;
                        replaced->kids.clear();
                        replaced->leaf = false;
                        replaced->delimiter.clear();
                        replaced->kids.push_back(Out::of(substituteText));
                    } else if (rule == u"complete-each" && same) {
                        for (Out *person : people) *person = Out::of(substituteText);
                    } else if (rule == u"partial-each" || rule == u"partial-first") {
                        for (int p = 0; p < int(people.size()) && p < previousNames.size(); ++p) {
                            if (keys[p] != previousNames[p]) break;
                            *people[size_t(p)] = Out::of(substituteText);
                            if (rule == u"partial-first") break;
                        }
                    }
                }
                previousNames = keys;
                previousKey = key;
            }
            // A reference the layout shows nothing for has no entry.
            const CslText text = flatten(entry);
            if (!text.isEmpty()) result.bibliography.append({m_references[size_t(i)]->id, text, m_references[size_t(i)]});
        }
    }
    return result;
}

CslEngine::CslEngine(std::shared_ptr<const CslStyle> style, const CslLocale &locale)
    : m_style(std::move(style)), m_locale(locale)
{
    // The style's locale elements: those for every language, then those for
    // the language, then those for its dialect.
    const QString language = m_locale.language();
    const QString primary = language.section(u'-', 0, 0);
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto &[lang, overrides] : m_style->locales()) {
            const bool matches = pass == 0   ? lang.isEmpty()
                                 : pass == 1 ? !lang.isEmpty() && lang == primary && lang != language
                                             : !lang.isEmpty() && lang == language;
            if (matches) m_locale.merge(overrides);
        }
    }
}

CslEngine::Result CslEngine::process(const QList<CslCitation> &citations, const QList<const CslReference *> &uncited) const
{
    Run run(*m_style, m_locale);
    return run.process(citations, uncited);
}

CslEngine::Locator CslEngine::parseLocator(const QString &input, const CslLocale &locale)
{
    Locator result;
    QString text = input.trimmed();
    if (text.startsWith(u',')) text = text.mid(1).trimmed();
    if (text.isEmpty()) return result;
    static const QStringList types{
        QStringLiteral("act"),       QStringLiteral("appendix"),   QStringLiteral("article-locator"),
        QStringLiteral("book"),      QStringLiteral("canon"),      QStringLiteral("chapter"),
        QStringLiteral("column"),    QStringLiteral("elocation"),  QStringLiteral("equation"),
        QStringLiteral("figure"),    QStringLiteral("folio"),      QStringLiteral("issue"),
        QStringLiteral("line"),      QStringLiteral("note"),       QStringLiteral("opus"),
        QStringLiteral("page"),      QStringLiteral("paragraph"),  QStringLiteral("part"),
        QStringLiteral("rule"),      QStringLiteral("scene"),      QStringLiteral("section"),
        QStringLiteral("sub-verbo"), QStringLiteral("supplement"), QStringLiteral("table"),
        QStringLiteral("timestamp"), QStringLiteral("title-locator"), QStringLiteral("verse"),
        QStringLiteral("volume"),
    };
    // Every spelling of every locator term, longest first, so that "pp."
    // is tried before "p.".
    QList<QPair<QString, QString>> spellings;
    for (const QString &type : types) {
        for (const char *form : {"long", "short", "symbol"}) {
            for (const bool plural : {false, true}) {
                const QString spelling = locale.termText(type, QLatin1String(form), plural);
                if (!spelling.isEmpty()) spellings.append({spelling.toLower(), type});
            }
        }
    }
    static const QList<QPair<QString, QString>> common{
        {QStringLiteral("p."), QStringLiteral("page")},          {QStringLiteral("pp."), QStringLiteral("page")},
        {QStringLiteral("p"), QStringLiteral("page")},           {QStringLiteral("pp"), QStringLiteral("page")},
        {QStringLiteral("pg."), QStringLiteral("page")},         {QStringLiteral("chap."), QStringLiteral("chapter")},
        {QStringLiteral("chaps."), QStringLiteral("chapter")},   {QStringLiteral("ch."), QStringLiteral("chapter")},
        {QStringLiteral("sec."), QStringLiteral("section")},     {QStringLiteral("secs."), QStringLiteral("section")},
        {QStringLiteral("§"), QStringLiteral("section")},        {QStringLiteral("§§"), QStringLiteral("section")},
        {QStringLiteral("vol."), QStringLiteral("volume")},      {QStringLiteral("vols."), QStringLiteral("volume")},
        {QStringLiteral("fig."), QStringLiteral("figure")},      {QStringLiteral("figs."), QStringLiteral("figure")},
        {QStringLiteral("para."), QStringLiteral("paragraph")},  {QStringLiteral("paras."), QStringLiteral("paragraph")},
        {QStringLiteral("¶"), QStringLiteral("paragraph")},      {QStringLiteral("l."), QStringLiteral("line")},
        {QStringLiteral("ll."), QStringLiteral("line")},         {QStringLiteral("n."), QStringLiteral("note")},
        {QStringLiteral("nn."), QStringLiteral("note")},         {QStringLiteral("no."), QStringLiteral("issue")},
        {QStringLiteral("s.v."), QStringLiteral("sub-verbo")},   {QStringLiteral("s.vv."), QStringLiteral("sub-verbo")},
    };
    spellings.append(common);
    std::stable_sort(spellings.begin(), spellings.end(),
                     [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
    QString rest = text;
    const QString lower = text.toLower();
    for (const auto &[spelling, type] : std::as_const(spellings)) {
        if (!lower.startsWith(spelling)) continue;
        const QString after = text.mid(spelling.size());
        // A word label is followed by a space; a symbol or an abbreviation
        // may be followed by the number itself.
        if (!after.isEmpty() && !after[0].isSpace() && !after[0].isDigit() && after[0] != u'{'
            && spelling.back().isLetter())
            continue;
        if (!after.isEmpty() && after[0].isDigit() && spelling.back().isLetter() && spelling.size() > 2) continue;
        if (after.trimmed().isEmpty()) continue;
        result.label = type;
        rest = after.trimmed();
        break;
    }
    if (rest.startsWith(u'{')) {
        const qsizetype close = rest.indexOf(u'}');
        if (close > 0) {
            result.locator = rest.mid(1, close - 1);
            result.suffix = rest.mid(close + 1);
            if (result.label.isEmpty()) result.label = QStringLiteral("page");
            return result;
        }
    }
    // The locator runs while it reads as numbers, numerals and ranges.
    static const QRegularExpression locator(QStringLiteral(
        "^((?:[A-Za-z]{0,3}\\d+[A-Za-z]{0,3}|[ivxlcdmIVXLCDM]+\\b)"
        "(?:\\s*(?:[-–—]+|&|,|\\band\\b)\\s*(?:[A-Za-z]{0,3}\\d+[A-Za-z]{0,3}|[ivxlcdmIVXLCDM]+\\b))*)"));
    const QRegularExpressionMatch match = locator.match(rest);
    if (match.hasMatch() && (!result.label.isEmpty() || rest[0].isDigit())) {
        result.locator = match.captured(1).trimmed();
        result.suffix = rest.mid(match.capturedLength());
        if (result.label.isEmpty()) result.label = QStringLiteral("page");
        return result;
    }
    if (!result.label.isEmpty()) {
        // A label followed by words: the words up to a comma are the locator.
        const qsizetype comma = rest.indexOf(u',');
        result.locator = comma < 0 ? rest : rest.left(comma).trimmed();
        result.suffix = comma < 0 ? QString() : rest.mid(comma);
        return result;
    }
    result.suffix = input;
    return result;
}
