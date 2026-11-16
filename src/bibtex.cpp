#include "bibtex.h"

#include "cslrichtext.h"

#include <QHash>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>

#include <algorithm>

namespace {

using Severity = BibliographyDiagnostic::Severity;

// ---------------------------------------------------------------------------
// LaTeX to markup

QChar combiningAccent(QChar command)
{
    switch (command.unicode()) {
    case u'\'':
        return QChar(0x0301);
    case u'`':
        return QChar(0x0300);
    case u'^':
        return QChar(0x0302);
    case u'"':
        return QChar(0x0308);
    case u'~':
        return QChar(0x0303);
    case u'=':
        return QChar(0x0304);
    case u'.':
        return QChar(0x0307);
    case u'c':
        return QChar(0x0327);
    case u'v':
        return QChar(0x030C);
    case u'u':
        return QChar(0x0306);
    case u'H':
        return QChar(0x030B);
    case u'k':
        return QChar(0x0328);
    case u'r':
        return QChar(0x030A);
    case u'd':
        return QChar(0x0323);
    case u'b':
        return QChar(0x0331);
    case u't':
        return QChar(0x0361);
    default:
        return {};
    }
}

const QHash<QString, QString> &symbols()
{
    static const QHash<QString, QString> table{
        {QStringLiteral("ss"), QStringLiteral("ß")},
        {QStringLiteral("SS"), QStringLiteral("SS")},
        {QStringLiteral("ae"), QStringLiteral("æ")},
        {QStringLiteral("AE"), QStringLiteral("Æ")},
        {QStringLiteral("oe"), QStringLiteral("œ")},
        {QStringLiteral("OE"), QStringLiteral("Œ")},
        {QStringLiteral("o"), QStringLiteral("ø")},
        {QStringLiteral("O"), QStringLiteral("Ø")},
        {QStringLiteral("aa"), QStringLiteral("å")},
        {QStringLiteral("AA"), QStringLiteral("Å")},
        {QStringLiteral("l"), QStringLiteral("ł")},
        {QStringLiteral("L"), QStringLiteral("Ł")},
        {QStringLiteral("i"), QStringLiteral("ı")},
        {QStringLiteral("j"), QStringLiteral("ȷ")},
        {QStringLiteral("dh"), QStringLiteral("ð")},
        {QStringLiteral("DH"), QStringLiteral("Ð")},
        {QStringLiteral("th"), QStringLiteral("þ")},
        {QStringLiteral("TH"), QStringLiteral("Þ")},
        {QStringLiteral("ng"), QStringLiteral("ŋ")},
        {QStringLiteral("NG"), QStringLiteral("Ŋ")},
        {QStringLiteral("dj"), QStringLiteral("đ")},
        {QStringLiteral("DJ"), QStringLiteral("Đ")},
        {QStringLiteral("textendash"), QStringLiteral("–")},
        {QStringLiteral("textemdash"), QStringLiteral("—")},
        {QStringLiteral("ldots"), QStringLiteral("…")},
        {QStringLiteral("dots"), QStringLiteral("…")},
        {QStringLiteral("textellipsis"), QStringLiteral("…")},
        {QStringLiteral("textquoteleft"), QStringLiteral("‘")},
        {QStringLiteral("textquoteright"), QStringLiteral("’")},
        {QStringLiteral("textquotedblleft"), QStringLiteral("“")},
        {QStringLiteral("textquotedblright"), QStringLiteral("”")},
        {QStringLiteral("guillemotleft"), QStringLiteral("«")},
        {QStringLiteral("guillemotright"), QStringLiteral("»")},
        {QStringLiteral("S"), QStringLiteral("§")},
        {QStringLiteral("P"), QStringLiteral("¶")},
        {QStringLiteral("copyright"), QStringLiteral("©")},
        {QStringLiteral("textcopyright"), QStringLiteral("©")},
        {QStringLiteral("textregistered"), QStringLiteral("®")},
        {QStringLiteral("texttrademark"), QStringLiteral("™")},
        {QStringLiteral("pounds"), QStringLiteral("£")},
        {QStringLiteral("textsterling"), QStringLiteral("£")},
        {QStringLiteral("euro"), QStringLiteral("€")},
        {QStringLiteral("texteuro"), QStringLiteral("€")},
        {QStringLiteral("textdegree"), QStringLiteral("°")},
        {QStringLiteral("dag"), QStringLiteral("†")},
        {QStringLiteral("ddag"), QStringLiteral("‡")},
        {QStringLiteral("textbullet"), QStringLiteral("•")},
        {QStringLiteral("textasciitilde"), QStringLiteral("~")},
        {QStringLiteral("textbackslash"), QStringLiteral("\\")},
        {QStringLiteral("textless"), QStringLiteral("<")},
        {QStringLiteral("textgreater"), QStringLiteral(">")},
        {QStringLiteral("textbar"), QStringLiteral("|")},
        {QStringLiteral("textunderscore"), QStringLiteral("_")},
        {QStringLiteral("LaTeX"), QStringLiteral("LaTeX")},
        {QStringLiteral("TeX"), QStringLiteral("TeX")},
        {QStringLiteral("BibTeX"), QStringLiteral("BibTeX")},
        {QStringLiteral("quad"), QStringLiteral(" ")},
        {QStringLiteral("qquad"), QStringLiteral(" ")},
        {QStringLiteral("enspace"), QStringLiteral(" ")},
        {QStringLiteral("thinspace"), QString(QChar(0x2009))},
        {QStringLiteral("nobreakspace"), QString(QChar(0x00A0))},
        {QStringLiteral("alpha"), QStringLiteral("α")},
        {QStringLiteral("beta"), QStringLiteral("β")},
        {QStringLiteral("gamma"), QStringLiteral("γ")},
        {QStringLiteral("delta"), QStringLiteral("δ")},
        {QStringLiteral("epsilon"), QStringLiteral("ε")},
        {QStringLiteral("lambda"), QStringLiteral("λ")},
        {QStringLiteral("mu"), QStringLiteral("μ")},
        {QStringLiteral("pi"), QStringLiteral("π")},
        {QStringLiteral("sigma"), QStringLiteral("σ")},
        {QStringLiteral("omega"), QStringLiteral("ω")},
        {QStringLiteral("times"), QStringLiteral("×")},
        {QStringLiteral("pm"), QStringLiteral("±")},
        {QStringLiteral("leq"), QStringLiteral("≤")},
        {QStringLiteral("geq"), QStringLiteral("≥")},
    };
    return table;
}

// Commands whose one argument is shown with a format.
QString formatTag(const QString &command, bool closing)
{
    static const QHash<QString, QPair<QString, QString>> tags{
        {QStringLiteral("emph"), {QStringLiteral("<i>"), QStringLiteral("</i>")}},
        {QStringLiteral("textit"), {QStringLiteral("<i>"), QStringLiteral("</i>")}},
        {QStringLiteral("textsl"), {QStringLiteral("<i>"), QStringLiteral("</i>")}},
        {QStringLiteral("mkbibemph"), {QStringLiteral("<i>"), QStringLiteral("</i>")}},
        {QStringLiteral("mkbibitalic"), {QStringLiteral("<i>"), QStringLiteral("</i>")}},
        {QStringLiteral("textbf"), {QStringLiteral("<b>"), QStringLiteral("</b>")}},
        {QStringLiteral("mkbibbold"), {QStringLiteral("<b>"), QStringLiteral("</b>")}},
        {QStringLiteral("textsc"),
         {QStringLiteral("<span style=\"font-variant:small-caps;\">"), QStringLiteral("</span>")}},
        {QStringLiteral("textsuperscript"), {QStringLiteral("<sup>"), QStringLiteral("</sup>")}},
        {QStringLiteral("mkbibsuperscript"), {QStringLiteral("<sup>"), QStringLiteral("</sup>")}},
        {QStringLiteral("textsubscript"), {QStringLiteral("<sub>"), QStringLiteral("</sub>")}},
        {QStringLiteral("mkbibsubscript"), {QStringLiteral("<sub>"), QStringLiteral("</sub>")}},
        {QStringLiteral("mkbibquote"), {QStringLiteral("\""), QStringLiteral("\"")}},
        {QStringLiteral("enquote"), {QStringLiteral("\""), QStringLiteral("\"")}},
        {QStringLiteral("NoCaseChange"), {QStringLiteral("<span class=\"nocase\">"), QStringLiteral("</span>")}},
        {QStringLiteral("mbox"), {QStringLiteral("<span class=\"nocase\">"), QStringLiteral("</span>")}},
    };
    const auto it = tags.constFind(command);
    if (it == tags.constEnd()) return {};
    return closing ? it->second : it->first;
}

// Declarations that format the rest of their group, as in {\em text}.
QString declarationTag(const QString &command, bool closing)
{
    static const QHash<QString, QPair<QString, QString>> tags{
        {QStringLiteral("em"), {QStringLiteral("<i>"), QStringLiteral("</i>")}},
        {QStringLiteral("it"), {QStringLiteral("<i>"), QStringLiteral("</i>")}},
        {QStringLiteral("itshape"), {QStringLiteral("<i>"), QStringLiteral("</i>")}},
        {QStringLiteral("sl"), {QStringLiteral("<i>"), QStringLiteral("</i>")}},
        {QStringLiteral("slshape"), {QStringLiteral("<i>"), QStringLiteral("</i>")}},
        {QStringLiteral("bf"), {QStringLiteral("<b>"), QStringLiteral("</b>")}},
        {QStringLiteral("bfseries"), {QStringLiteral("<b>"), QStringLiteral("</b>")}},
        {QStringLiteral("sc"), {QStringLiteral("<span style=\"font-variant:small-caps;\">"), QStringLiteral("</span>")}},
        {QStringLiteral("scshape"),
         {QStringLiteral("<span style=\"font-variant:small-caps;\">"), QStringLiteral("</span>")}},
        {QStringLiteral("rm"), {QString(), QString()}},
        {QStringLiteral("rmfamily"), {QString(), QString()}},
        {QStringLiteral("upshape"), {QString(), QString()}},
        {QStringLiteral("normalfont"), {QString(), QString()}},
        {QStringLiteral("tt"), {QString(), QString()}},
        {QStringLiteral("ttfamily"), {QString(), QString()}},
        {QStringLiteral("sffamily"), {QString(), QString()}},
    };
    const auto it = tags.constFind(command);
    if (it == tags.constEnd()) return QStringLiteral("\x01");
    return closing ? it->second : it->first;
}

// No value of a bibliography nests braces, commands and declarations more
// than a few levels deep. Converting one recurses once per level, on a
// worker's stack, so a value that nests deeper stops there, and parse()
// refuses its file.
constexpr int MaxLatexNesting = 256;
// Set on the thread that converts a value nested deeper.
thread_local bool latexTooDeep = false;

class LatexConverter {
public:
    // `level` is how deep the value this one converts an argument of was.
    LatexConverter(const QString &text, bool protectCase, int level = 0)
        : m_text(text), m_protect(protectCase), m_level(level)
    {
    }

    QString run()
    {
        convert(0);
        return tidy(m_out);
    }

private:
    bool atEnd() const { return m_pos >= m_text.size(); }
    QChar peek(qsizetype ahead = 0) const
    {
        return m_pos + ahead < m_text.size() ? m_text[m_pos + ahead] : QChar();
    }

    void put(const QString &text)
    {
        m_out += CslMarkup::escapeHtml(text).replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    }

    // Converts up to the '}' that closes the current group, which it takes;
    // true if it found that brace rather than the end of the text.
    bool convert(int depth)
    {
        if (m_level >= MaxLatexNesting) {
            latexTooDeep = true;
            m_pos = m_text.size();
            return false;
        }
        ++m_level;
        const auto leave = qScopeGuard([this] { --m_level; });
        while (!atEnd()) {
            const QChar c = m_text[m_pos];
            if (c == u'}') {
                ++m_pos;
                if (depth > 0) return true;
                continue;
            }
            if (c == u'{') {
                ++m_pos;
                group(depth + 1);
            } else if (c == u'\\') {
                command(depth);
            } else if (c == u'$') {
                math();
            } else if (c == u'~') {
                m_out += QChar(0x00A0);
                ++m_pos;
            } else if (c == u'-') {
                qsizetype count = 0;
                while (peek(count) == u'-') ++count;
                m_pos += count;
                m_out += count >= 3 ? QStringLiteral("—") : count == 2 ? QStringLiteral("–") : QStringLiteral("-");
            } else if (c == u'`') {
                if (peek(1) == u'`') {
                    m_out += QStringLiteral("“");
                    m_pos += 2;
                } else {
                    m_out += QStringLiteral("‘");
                    ++m_pos;
                }
            } else if (c == u'\'' && peek(1) == u'\'') {
                m_out += QStringLiteral("”");
                m_pos += 2;
            } else if (c.isSpace()) {
                m_out += u' ';
                ++m_pos;
            } else {
                put(QString(c));
                ++m_pos;
            }
        }
        return false;
    }

    // A brace group: case-protected at the top level of a title, unless it
    // starts with a command, as BibTeX's special characters such as {\'E} do.
    void group(int depth)
    {
        if (peek() == u'\\') {
            const qsizetype start = m_pos + 1;
            qsizetype end = start;
            while (end < m_text.size() && m_text[end].isLetter()) ++end;
            const QString name = end == start ? m_text.mid(start, 1) : m_text.mid(start, end - start);
            const QString open = declarationTag(name, false);
            if (end > start && open != u"\x01") {
                m_pos = end;
                skipSpaces();
                m_out += open;
                convert(depth);
                m_out += declarationTag(name, true);
                return;
            }
            // A special character, such as {\'E} or {\ss}, changes case like a letter.
            const bool special = (name.size() == 1 && !combiningAccent(name[0]).isNull())
                                 || symbols().contains(name) || (end == start && !name.isEmpty());
            if (special) {
                convert(depth);
                return;
            }
        }
        const bool nocase = m_protect && depth == 1 && !m_inNocase;
        if (nocase) {
            m_out += QStringLiteral("<span class=\"nocase\">");
            m_inNocase = true;
        }
        convert(depth);
        if (nocase) {
            m_out += QStringLiteral("</span>");
            m_inNocase = false;
        }
    }

    void skipSpaces()
    {
        while (!atEnd() && m_text[m_pos].isSpace()) ++m_pos;
    }

    // The raw text of a following braced argument, or of one character.
    QString rawArgument()
    {
        skipSpaces();
        if (atEnd()) return {};
        if (m_text[m_pos] == u'{') {
            int level = 0;
            const qsizetype start = m_pos + 1;
            for (; m_pos < m_text.size(); ++m_pos) {
                if (m_text[m_pos] == u'\\') {
                    ++m_pos;
                    continue;
                }
                if (m_text[m_pos] == u'{') ++level;
                if (m_text[m_pos] == u'}' && --level == 0) break;
            }
            const QString argument = m_text.mid(start, m_pos - start);
            if (m_pos < m_text.size()) ++m_pos;
            return argument;
        }
        if (m_text[m_pos] == u'\\') {
            const qsizetype start = m_pos++;
            while (!atEnd() && m_text[m_pos].isLetter()) ++m_pos;
            if (m_pos == start + 1 && !atEnd()) ++m_pos;
            return m_text.mid(start, m_pos - start);
        }
        return QString(m_text[m_pos++]);
    }

    void command(int depth)
    {
        ++m_pos;
        if (atEnd()) return;
        QString name;
        if (m_text[m_pos].isLetter()) {
            const qsizetype start = m_pos;
            while (!atEnd() && m_text[m_pos].isLetter()) ++m_pos;
            name = m_text.mid(start, m_pos - start);
        } else {
            name = QString(m_text[m_pos++]);
        }
        const bool letters = name[0].isLetter();
        // An accent: one punctuation character, or one letter followed by a
        // space or a brace.
        if (name.size() == 1 && !combiningAccent(name[0]).isNull()
            && (!letters || atEnd() || m_text[m_pos].isSpace() || m_text[m_pos] == u'{')) {
            const QString argument = LatexConverter(rawArgument(), false, m_level).run();
            QString plain = CslText::fromHtml(argument).toPlainText();
            if (plain.isEmpty()) plain = QStringLiteral(" ");
            // The dotless i and j of \"{\i} take the accent as i and j do.
            if (plain.startsWith(QChar(0x0131))) plain[0] = u'i';
            if (plain.startsWith(QChar(0x0237))) plain[0] = u'j';
            put((plain.left(1) + combiningAccent(name[0]) + plain.mid(1)).normalized(QString::NormalizationForm_C));
            return;
        }
        if (letters) skipSpaces();
        if (!letters) {
            switch (name[0].unicode()) {
            case u'\\':
            case u' ':
            case u'\n':
                m_out += u' ';
                return;
            case u',':
                m_out += QChar(0x202F);
                return;
            case u';':
            case u':':
                m_out += u' ';
                return;
            case u'-':
            case u'/':
            case u'@':
                return;
            default:
                put(name);
                return;
            }
        }
        if (const auto it = symbols().constFind(name); it != symbols().constEnd()) {
            put(*it);
            // A command ending in letters ends its own word: "\ss{}" and "\ss " alike.
            if (peek() == u'{' && peek(1) == u'}') m_pos += 2;
            return;
        }
        if (name == u"url" || name == u"path" || name == u"nolinkurl") {
            put(rawArgument());
            return;
        }
        if (name == u"href") {
            rawArgument();
            put(CslText::fromHtml(LatexConverter(rawArgument(), false, m_level).run()).toPlainText());
            return;
        }
        if (const QString open = formatTag(name, false); !open.isEmpty()) {
            const QString argument = LatexConverter(rawArgument(), false, m_level).run();
            const bool nocase = open.contains(u"nocase");
            if (nocase && m_inNocase) {
                m_out += argument;
                return;
            }
            m_out += open + argument + formatTag(name, true);
            return;
        }
        if (const QString open = declarationTag(name, false); open != u"\x01") {
            // A declaration outside a group of its own formats the rest of the current group.
            m_out += open;
            // The brace that ends the group is left for the group itself.
            const bool closed = convert(depth);
            m_out += declarationTag(name, true);
            if (closed && depth > 0) --m_pos;
            return;
        }
        // Any other command shows the text of its braced arguments.
        while (peek() == u'{') {
            ++m_pos;
            const bool wasNocase = m_inNocase;
            m_inNocase = true;
            convert(depth + 1);
            m_inNocase = wasNocase;
        }
    }

    void math()
    {
        ++m_pos;
        const qsizetype end = m_text.indexOf(u'$', m_pos);
        const qsizetype stop = end < 0 ? m_text.size() : end;
        QString content = m_text.mid(m_pos, stop - m_pos);
        static const QRegularExpression command(QStringLiteral("\\\\([A-Za-z]+)"));
        QString converted;
        qsizetype last = 0;
        auto it = command.globalMatch(content);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            converted += content.mid(last, match.capturedStart() - last);
            converted += symbols().value(match.captured(1), match.captured(0));
            last = match.capturedEnd();
        }
        converted += content.mid(last);
        converted.remove(u'{').remove(u'}');
        put(converted);
        m_pos = end < 0 ? m_text.size() : end + 1;
    }

    static QString tidy(const QString &markup)
    {
        QString text = markup;
        static const QRegularExpression spaces(QStringLiteral("[ \\t\\r\\n]+"));
        text.replace(spaces, QStringLiteral(" "));
        static const QRegularExpression empty(QStringLiteral("<span class=\"nocase\"></span>"));
        text.remove(empty);
        return text.trimmed();
    }

    const QString m_text;
    const bool m_protect;
    int m_level = 0;
    qsizetype m_pos = 0;
    bool m_inNocase = false;
    QString m_out;
};

QString plain(const QString &latex)
{
    return CslText::fromHtml(BibTeX::toMarkup(latex)).toPlainText().trimmed();
}

// ---------------------------------------------------------------------------
// Names

// White space as a regular expression's \s has it: ASCII alone.
bool isAsciiSpace(QChar c)
{
    return c == u' ' || (c >= u'\t' && c <= u'\r');
}

qsizetype spacesAt(const QString &text, qsizetype i)
{
    qsizetype end = i;
    while (end < text.size() && isAsciiSpace(text[end])) ++end;
    return end - i;
}

// The separators names and their parts are split at: the length of one
// that starts at `i`, or 0.
//   " and " between names: \s+and\s+, in any case
qsizetype andAt(const QString &text, qsizetype i)
{
    const qsizetype before = spacesAt(text, i);
    const qsizetype word = i + before;
    if (before == 0 || word + 3 > text.size()
        || QStringView(text).sliced(word, 3).compare(QLatin1StringView("and"), Qt::CaseInsensitive) != 0)
        return 0;
    const qsizetype after = spacesAt(text, word + 3);
    return after == 0 ? 0 : before + 3 + after;
}

//   a comma, alone
qsizetype commaAt(const QString &text, qsizetype i)
{
    return text[i] == u',' ? 1 : 0;
}

//   a comma with the spaces around it: \s*,\s*
qsizetype spacedCommaAt(const QString &text, qsizetype i)
{
    const qsizetype before = spacesAt(text, i);
    if (i + before >= text.size() || text[i + before] != u',') return 0;
    return before + 1 + spacesAt(text, i + before + 1);
}

//   the spaces and ties between the words of a name: [\s~]+
qsizetype tieAt(const QString &text, qsizetype i)
{
    qsizetype end = i;
    while (end < text.size() && (isAsciiSpace(text[end]) || text[end] == u'~')) ++end;
    return end - i;
}

// Splits at a separator found at brace depth 0, in one pass.
QStringList splitTopLevel(const QString &text, qsizetype (*separator)(const QString &, qsizetype))
{
    QStringList parts;
    int depth = 0;
    qsizetype start = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c == u'\\') {
            ++i;
            continue;
        }
        if (c == u'{') ++depth;
        if (c == u'}') --depth;
        if (depth != 0) continue;
        if (const qsizetype length = separator(text, i); length > 0) {
            parts.append(text.mid(start, i - start));
            start = i + length;
            i = start - 1;
        }
    }
    parts.append(text.mid(start));
    return parts;
}

// BibTeX's case of a name token: the case of its first letter at brace
// depth 0, or of the letter a special character such as {\'e} stands for.
// A token whose letters are all inside braces has no case.
enum class TokenCase { Upper, Lower, None };

TokenCase tokenCase(const QString &token)
{
    int depth = 0;
    for (qsizetype i = 0; i < token.size(); ++i) {
        const QChar c = token[i];
        if (c == u'{') {
            if (depth == 0 && i + 1 < token.size() && token[i + 1] == u'\\') {
                // A special character: the case of the first letter after the command.
                qsizetype j = i + 2;
                if (j < token.size() && token[j].isLetter()) {
                    const qsizetype start = j;
                    while (j < token.size() && token[j].isLetter()) ++j;
                    const QString name = token.mid(start, j - start);
                    if (symbols().contains(name)) return name[0].isLower() ? TokenCase::Lower : TokenCase::Upper;
                } else {
                    ++j;
                }
                for (; j < token.size(); ++j) {
                    if (token[j].isLetter()) return token[j].isLower() ? TokenCase::Lower : TokenCase::Upper;
                }
                return TokenCase::None;
            }
            ++depth;
            continue;
        }
        if (c == u'}') {
            --depth;
            continue;
        }
        if (c == u'\\' && depth == 0) {
            // A command outside braces, as in \'emile: its argument's case.
            qsizetype j = i + 1;
            if (j < token.size() && !token[j].isLetter()) ++j;
            else
                while (j < token.size() && token[j].isLetter()) ++j;
            for (; j < token.size(); ++j) {
                if (token[j].isLetter()) return token[j].isLower() ? TokenCase::Lower : TokenCase::Upper;
            }
            return TokenCase::None;
        }
        if (depth == 0 && c.isLetter()) return c.isLower() ? TokenCase::Lower : TokenCase::Upper;
    }
    return TokenCase::None;
}

QStringList nameTokens(const QString &part)
{
    QStringList tokens;
    for (const QString &token : splitTopLevel(part.trimmed(), tieAt)) {
        if (!token.isEmpty()) tokens.append(token);
    }
    return tokens;
}

CslName nameFromParts(const QStringList &first, const QStringList &von, const QStringList &last,
                      const QStringList &jr, bool usePrefix)
{
    CslName name;
    name.given = plain(first.join(u' '));
    name.family = plain(last.join(u' '));
    const QString particle = plain(von.join(u' '));
    if (usePrefix)
        name.nonDroppingParticle = particle;
    else
        name.droppingParticle = particle;
    name.suffix = plain(jr.join(u' '));
    return name;
}

// BibLaTeX's extended name format: "family=Doe, given=John, prefix=van".
bool extendedName(const QString &text, bool usePrefix, CslName &name)
{
    const QStringList parts = splitTopLevel(text, spacedCommaAt);
    QHash<QString, QString> values;
    for (const QString &part : parts) {
        const qsizetype equals = part.indexOf(u'=');
        if (equals < 0) return false;
        QString key = part.left(equals).trimmed().toLower();
        if (key.startsWith(u'"')) key = key.mid(1);
        QString value = part.mid(equals + 1).trimmed();
        if (value.startsWith(u'"') && value.endsWith(u'"') && value.size() >= 2) value = value.mid(1, value.size() - 2);
        values.insert(key, value);
    }
    static const QSet<QString> known{QStringLiteral("family"), QStringLiteral("given"), QStringLiteral("prefix"),
                                     QStringLiteral("suffix"), QStringLiteral("useprefix")};
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        if (!known.contains(it.key())) return false;
    }
    if (!values.contains(QStringLiteral("family")) && !values.contains(QStringLiteral("given"))) return false;
    name.family = plain(values.value(QStringLiteral("family")));
    name.given = plain(values.value(QStringLiteral("given")));
    name.suffix = plain(values.value(QStringLiteral("suffix")));
    const QString use = values.value(QStringLiteral("useprefix")).toLower();
    const bool prefixUsed = use.isEmpty() ? usePrefix : use == u"true";
    const QString prefix = plain(values.value(QStringLiteral("prefix")));
    if (prefixUsed)
        name.nonDroppingParticle = prefix;
    else
        name.droppingParticle = prefix;
    return true;
}

CslName parseName(const QString &text, bool usePrefix)
{
    CslName name;
    const QString trimmed = text.trimmed();
    // A name wholly in braces is one literal name, such as an institution.
    if (trimmed.startsWith(u'{') && trimmed.endsWith(u'}')) {
        int depth = 0;
        bool whole = true;
        for (qsizetype i = 0; i < trimmed.size(); ++i) {
            if (trimmed[i] == u'\\') {
                ++i;
                continue;
            }
            if (trimmed[i] == u'{') ++depth;
            if (trimmed[i] == u'}' && --depth == 0 && i != trimmed.size() - 1) whole = false;
        }
        if (whole && !(trimmed.size() > 1 && trimmed[1] == u'\\')) {
            name.literal = plain(trimmed);
            return name;
        }
    }
    if (trimmed.contains(u'=') && extendedName(trimmed, usePrefix, name)) return name;
    QStringList parts = splitTopLevel(trimmed, commaAt);
    for (QString &part : parts) part = part.trimmed();
    if (parts.size() == 1) {
        const QStringList tokens = nameTokens(parts[0]);
        if (tokens.isEmpty()) return name;
        const qsizetype n = tokens.size();
        qsizetype vonStart = -1;
        qsizetype vonEnd = -1;
        for (qsizetype i = 0; i < n - 1; ++i) {
            if (tokenCase(tokens[i]) == TokenCase::Lower) {
                if (vonStart < 0) vonStart = i;
                vonEnd = i;
            }
        }
        if (vonStart < 0)
            return nameFromParts(tokens.mid(0, n - 1), {}, tokens.mid(n - 1), {}, usePrefix);
        return nameFromParts(tokens.mid(0, vonStart), tokens.mid(vonStart, vonEnd - vonStart + 1),
                             tokens.mid(vonEnd + 1), {}, usePrefix);
    }
    // "von Last, First" or "von Last, Jr, First".
    const QStringList lastTokens = nameTokens(parts[0]);
    qsizetype vonEnd = -1;
    for (qsizetype i = 0; i < lastTokens.size() - 1; ++i) {
        if (tokenCase(lastTokens[i]) == TokenCase::Lower) vonEnd = i;
    }
    const QStringList von = lastTokens.mid(0, vonEnd + 1);
    const QStringList last = lastTokens.mid(vonEnd + 1);
    const QStringList jr = parts.size() >= 3 ? nameTokens(parts[1]) : QStringList();
    const QStringList first = nameTokens(parts.size() >= 3 ? parts.mid(2).join(QStringLiteral(", ")) : parts[1]);
    return nameFromParts(first, von, last, jr, usePrefix);
}

// ---------------------------------------------------------------------------
// The database syntax

struct RawEntry {
    QString type;
    QString key;
    QList<QPair<QString, QString>> fields;
    int line = 0;

    QString value(const QString &name) const
    {
        for (const auto &field : fields) {
            if (field.first == name) return field.second;
        }
        return {};
    }
    bool has(const QString &name) const
    {
        return std::any_of(fields.cbegin(), fields.cend(), [&name](const auto &field) { return field.first == name; });
    }
    void set(const QString &name, const QString &value)
    {
        for (auto &field : fields) {
            if (field.first == name) {
                field.second = value;
                return;
            }
        }
        fields.append({name, value});
    }
};

// No value of a bibliography is longer than this many characters: a longer
// one, written out, joined with '#' or made from @string definitions, is
// cut short with a warning. Nor does a database expand to more than four
// times its own size, or MaxExpansion, whichever is less: @string
// definitions that double one another would otherwise make 2^n characters
// out of a few hundred bytes. Past that, the file is refused.
constexpr qsizetype MaxValueChars = qsizetype(64) << 10;
constexpr qsizetype MaxExpansion = qsizetype(64) << 20;

class Reader {
public:
    explicit Reader(const QString &text)
        : m_text(text), m_budget(std::min<qsizetype>(MaxExpansion, std::max<qsizetype>(text.size(), 1) * 4))
    {
        static const char *const months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                             "jul", "aug", "sep", "oct", "nov", "dec"};
        for (int i = 0; i < 12; ++i) m_macros.insert(QLatin1String(months[i]), QString::number(i + 1));
        for (qsizetype i = 0; i < text.size(); ++i) {
            if (text[i] == u'\n') m_lineStarts.append(i + 1);
        }
    }

    QList<RawEntry> read(QList<BibliographyDiagnostic> &diagnostics)
    {
        m_diagnostics = &diagnostics;
        while (!m_refused) {
            const qsizetype at = m_text.indexOf(u'@', m_pos);
            if (at < 0) break;
            m_pos = at + 1;
            entry(at);
        }
        if (m_refused) return {};
        return m_entries;
    }

private:
    // The line, from 1: how many lines start at or before the offset.
    int lineAt(qsizetype offset) const
    {
        return int(std::upper_bound(m_lineStarts.cbegin(), m_lineStarts.cend(), offset) - m_lineStarts.cbegin());
    }

    void warn(qsizetype offset, const QString &message, const QString &key = {})
    {
        m_diagnostics->append({Severity::Warning, message, key, lineAt(offset)});
    }
    void error(qsizetype offset, const QString &message, const QString &key = {})
    {
        m_diagnostics->append({Severity::Error, message, key, lineAt(offset)});
    }

    bool atEnd() const { return m_pos >= m_text.size(); }

    void skipSpace()
    {
        while (!atEnd() && m_text[m_pos].isSpace()) ++m_pos;
    }

    QString identifier()
    {
        const qsizetype start = m_pos;
        while (!atEnd()) {
            const QChar c = m_text[m_pos];
            if (c.isSpace() || c == u'{' || c == u'}' || c == u'(' || c == u')' || c == u',' || c == u'='
                || c == u'#' || c == u'"' || c == u'@')
                break;
            ++m_pos;
        }
        return m_text.mid(start, m_pos - start);
    }

    // The text inside braces that start at m_pos, which is left after them.
    bool braced(QString &out)
    {
        const qsizetype start = m_pos;
        int depth = 0;
        for (; m_pos < m_text.size(); ++m_pos) {
            const QChar c = m_text[m_pos];
            if (c == u'\\') {
                ++m_pos;
                continue;
            }
            if (c == u'{') ++depth;
            if (c == u'}' && --depth == 0) {
                out = m_text.mid(start + 1, m_pos - start - 1);
                ++m_pos;
                return true;
            }
        }
        m_pos = start;
        return false;
    }

    bool quoted(QString &out)
    {
        const qsizetype start = m_pos++;
        int depth = 0;
        for (; m_pos < m_text.size(); ++m_pos) {
            const QChar c = m_text[m_pos];
            if (c == u'\\') {
                ++m_pos;
                continue;
            }
            if (c == u'{') ++depth;
            if (c == u'}') --depth;
            if (c == u'"' && depth == 0) {
                out = m_text.mid(start + 1, m_pos - start - 1);
                ++m_pos;
                return true;
            }
        }
        m_pos = start;
        return false;
    }

    // A field value: pieces joined by '#', each appended only as far as the
    // value has room, so that the work is never more than the text made.
    bool value(QString &out, const QString &key, const QString &field = {})
    {
        out.clear();
        const qsizetype at = m_pos;
        bool cut = false;
        const auto append = [&](const QString &piece) {
            const qsizetype room = MaxValueChars - out.size();
            if (piece.size() <= room) {
                out += piece;
                return;
            }
            out += QStringView(piece).first(room);
            cut = true;
        };
        const auto finish = [&] {
            if (cut)
                warn(at,
                     (key.isEmpty() ? QStringLiteral("The string “%1” is longer than %2 characters, so it is cut short.")
                                    : QStringLiteral("The field “%1” of “%3” is longer than %2 characters, so it is cut short."))
                         .arg(field)
                         .arg(MaxValueChars)
                         .arg(key),
                     key);
            m_expanded += out.size();
            if (m_expanded <= m_budget) return true;
            m_refused = true;
            m_pos = m_text.size();
            error(at, QStringLiteral("The bibliography expands to more than %1 characters, four times its size or 64 MiB, "
                                     "through @string definitions that repeat one another, so it is not read.")
                          .arg(m_budget));
            return false;
        };
        for (;;) {
            skipSpace();
            if (atEnd()) return false;
            const QChar c = m_text[m_pos];
            QString piece;
            if (c == u'{') {
                if (!braced(piece)) {
                    error(m_pos, QStringLiteral("A value's braces are not closed."), key);
                    return false;
                }
            } else if (c == u'"') {
                if (!quoted(piece)) {
                    error(m_pos, QStringLiteral("A quoted value is not closed."), key);
                    return false;
                }
            } else {
                const qsizetype start = m_pos;
                const QString name = identifier();
                if (name.isEmpty()) {
                    error(start, QStringLiteral("A value is missing."), key);
                    return false;
                }
                bool number = true;
                for (const QChar d : name) number = number && d.isDigit();
                if (number) {
                    piece = name;
                } else if (const auto it = m_macros.constFind(name.toLower()); it != m_macros.constEnd()) {
                    piece = *it;
                } else {
                    warn(start, QStringLiteral("The string “%1” is not defined.").arg(name), key);
                }
            }
            append(piece);
            skipSpace();
            if (!atEnd() && m_text[m_pos] == u'#') {
                ++m_pos;
                continue;
            }
            return finish();
        }
    }

    // Reads "name = value" pairs up to the closing character.
    bool fields(QChar close, QList<QPair<QString, QString>> &out, const QString &key)
    {
        for (;;) {
            skipSpace();
            if (atEnd()) return false;
            if (m_text[m_pos] == close) {
                ++m_pos;
                return true;
            }
            if (m_text[m_pos] == u',') {
                ++m_pos;
                continue;
            }
            const qsizetype start = m_pos;
            const QString name = identifier().toLower();
            skipSpace();
            if (name.isEmpty() || atEnd() || m_text[m_pos] != u'=') {
                error(start, QStringLiteral("Expected a field name and “=”."), key);
                return false;
            }
            ++m_pos;
            QString text;
            if (!value(text, key, name)) return false;
            out.append({name, text});
        }
    }

    void entry(qsizetype at)
    {
        skipSpace();
        const QString type = identifier().toLower();
        skipSpace();
        if (type.isEmpty() || atEnd() || (m_text[m_pos] != u'{' && m_text[m_pos] != u'(')) {
            // An '@' in text outside entries is a comment.
            return;
        }
        const QChar close = m_text[m_pos] == u'{' ? u'}' : u')';
        if (type == u"comment") {
            QString ignored;
            if (close == u'}' && !braced(ignored)) m_pos = m_text.size();
            return;
        }
        ++m_pos;
        if (type == u"preamble") {
            QString ignored;
            value(ignored, {});
            skipSpace();
            if (!atEnd() && m_text[m_pos] == close) ++m_pos;
            return;
        }
        if (type == u"string") {
            QList<QPair<QString, QString>> definitions;
            if (!fields(close, definitions, {})) {
                if (!m_refused) error(at, QStringLiteral("An @string definition is not complete."));
                return;
            }
            for (const auto &definition : definitions) m_macros.insert(definition.first, definition.second);
            return;
        }
        skipSpace();
        const qsizetype keyStart = m_pos;
        while (!atEnd() && m_text[m_pos] != u',' && m_text[m_pos] != close && !m_text[m_pos].isSpace()) ++m_pos;
        RawEntry raw;
        raw.type = type;
        raw.key = m_text.mid(keyStart, m_pos - keyStart);
        raw.line = lineAt(at);
        skipSpace();
        if (raw.key.isEmpty() || raw.key.contains(u'=')) {
            error(at, QStringLiteral("An @%1 entry has no key.").arg(type));
            return;
        }
        if (!fields(close, raw.fields, raw.key)) {
            if (m_refused) return;
            error(at, QStringLiteral("The entry “%1” is not complete.").arg(raw.key), raw.key);
            // Whatever follows is read as new entries.
            m_pos = qMin(m_text.size(), keyStart);
            return;
        }
        m_entries.append(raw);
    }

    const QString &m_text;
    qsizetype m_pos = 0;
    // How many characters the values read so far hold, and the most they may.
    qsizetype m_expanded = 0;
    const qsizetype m_budget;
    bool m_refused = false;
    QList<qsizetype> m_lineStarts{0};
    QHash<QString, QString> m_macros;
    QList<RawEntry> m_entries;
    QList<BibliographyDiagnostic> *m_diagnostics = nullptr;
};

// ---------------------------------------------------------------------------
// BibLaTeX to CSL

bool isEnglish(const QString &language)
{
    static const QSet<QString> english{
        QStringLiteral("english"),   QStringLiteral("american"),  QStringLiteral("british"),
        QStringLiteral("usenglish"), QStringLiteral("ukenglish"), QStringLiteral("canadian"),
        QStringLiteral("australian"), QStringLiteral("newzealand"),
    };
    const QString lower = language.trimmed().toLower();
    return lower.isEmpty() || english.contains(lower) || lower.startsWith(u"en");
}

QString cslLanguage(const QString &language)
{
    static const QHash<QString, QString> codes{
        {QStringLiteral("english"), QStringLiteral("en")},     {QStringLiteral("american"), QStringLiteral("en-US")},
        {QStringLiteral("usenglish"), QStringLiteral("en-US")}, {QStringLiteral("british"), QStringLiteral("en-GB")},
        {QStringLiteral("ukenglish"), QStringLiteral("en-GB")}, {QStringLiteral("canadian"), QStringLiteral("en-CA")},
        {QStringLiteral("australian"), QStringLiteral("en-AU")}, {QStringLiteral("newzealand"), QStringLiteral("en-NZ")},
        {QStringLiteral("french"), QStringLiteral("fr")},      {QStringLiteral("german"), QStringLiteral("de")},
        {QStringLiteral("ngerman"), QStringLiteral("de")},     {QStringLiteral("spanish"), QStringLiteral("es")},
        {QStringLiteral("italian"), QStringLiteral("it")},     {QStringLiteral("dutch"), QStringLiteral("nl")},
        {QStringLiteral("portuguese"), QStringLiteral("pt")},  {QStringLiteral("brazilian"), QStringLiteral("pt-BR")},
        {QStringLiteral("russian"), QStringLiteral("ru")},     {QStringLiteral("swedish"), QStringLiteral("sv")},
        {QStringLiteral("norwegian"), QStringLiteral("nb")},   {QStringLiteral("danish"), QStringLiteral("da")},
        {QStringLiteral("finnish"), QStringLiteral("fi")},     {QStringLiteral("polish"), QStringLiteral("pl")},
        {QStringLiteral("czech"), QStringLiteral("cs")},       {QStringLiteral("greek"), QStringLiteral("el")},
        {QStringLiteral("japanese"), QStringLiteral("ja")},    {QStringLiteral("chinese"), QStringLiteral("zh")},
        {QStringLiteral("latin"), QStringLiteral("la")},
    };
    const QString lower = language.trimmed().toLower();
    return codes.value(lower, language.trimmed());
}

QString cslType(const RawEntry &entry)
{
    const QString &type = entry.type;
    const QString subtype = plain(entry.value(QStringLiteral("entrysubtype"))).toLower();
    if (type == u"article") {
        if (subtype == u"magazine") return QStringLiteral("article-magazine");
        if (subtype == u"newspaper") return QStringLiteral("article-newspaper");
        return QStringLiteral("article-journal");
    }
    static const QHash<QString, QString> types{
        {QStringLiteral("book"), QStringLiteral("book")},
        {QStringLiteral("mvbook"), QStringLiteral("book")},
        {QStringLiteral("collection"), QStringLiteral("book")},
        {QStringLiteral("mvcollection"), QStringLiteral("book")},
        {QStringLiteral("proceedings"), QStringLiteral("book")},
        {QStringLiteral("mvproceedings"), QStringLiteral("book")},
        {QStringLiteral("reference"), QStringLiteral("book")},
        {QStringLiteral("mvreference"), QStringLiteral("book")},
        {QStringLiteral("manual"), QStringLiteral("book")},
        {QStringLiteral("booklet"), QStringLiteral("pamphlet")},
        {QStringLiteral("inbook"), QStringLiteral("chapter")},
        {QStringLiteral("bookinbook"), QStringLiteral("chapter")},
        {QStringLiteral("incollection"), QStringLiteral("chapter")},
        {QStringLiteral("suppbook"), QStringLiteral("chapter")},
        {QStringLiteral("suppcollection"), QStringLiteral("chapter")},
        {QStringLiteral("inreference"), QStringLiteral("entry-encyclopedia")},
        {QStringLiteral("inproceedings"), QStringLiteral("paper-conference")},
        {QStringLiteral("conference"), QStringLiteral("paper-conference")},
        {QStringLiteral("online"), QStringLiteral("webpage")},
        {QStringLiteral("electronic"), QStringLiteral("webpage")},
        {QStringLiteral("www"), QStringLiteral("webpage")},
        {QStringLiteral("patent"), QStringLiteral("patent")},
        {QStringLiteral("periodical"), QStringLiteral("periodical")},
        {QStringLiteral("suppperiodical"), QStringLiteral("article-journal")},
        {QStringLiteral("report"), QStringLiteral("report")},
        {QStringLiteral("techreport"), QStringLiteral("report")},
        {QStringLiteral("thesis"), QStringLiteral("thesis")},
        {QStringLiteral("mastersthesis"), QStringLiteral("thesis")},
        {QStringLiteral("phdthesis"), QStringLiteral("thesis")},
        {QStringLiteral("unpublished"), QStringLiteral("manuscript")},
        {QStringLiteral("software"), QStringLiteral("software")},
        {QStringLiteral("dataset"), QStringLiteral("dataset")},
        {QStringLiteral("video"), QStringLiteral("motion_picture")},
        {QStringLiteral("movie"), QStringLiteral("motion_picture")},
        {QStringLiteral("audio"), QStringLiteral("song")},
        {QStringLiteral("music"), QStringLiteral("song")},
        {QStringLiteral("artwork"), QStringLiteral("graphic")},
        {QStringLiteral("image"), QStringLiteral("graphic")},
        {QStringLiteral("letter"), QStringLiteral("personal_communication")},
        {QStringLiteral("review"), QStringLiteral("review")},
        {QStringLiteral("standard"), QStringLiteral("standard")},
        {QStringLiteral("legislation"), QStringLiteral("legislation")},
        {QStringLiteral("jurisdiction"), QStringLiteral("legal_case")},
        {QStringLiteral("misc"), QStringLiteral("document")},
    };
    return types.value(type, QStringLiteral("document"));
}

// "Title: Subtitle. Addon", joining as BibLaTeX prints them.
QString joinedTitle(const QString &title, const QString &subtitle, const QString &addon)
{
    QString text = title;
    if (!subtitle.isEmpty()) {
        if (text.isEmpty())
            text = subtitle;
        else {
            const QString bare = CslText::fromHtml(text).toPlainText();
            const bool ended = bare.endsWith(u'?') || bare.endsWith(u'!');
            text += (ended ? QStringLiteral(" ") : QStringLiteral(": ")) + subtitle;
        }
    }
    if (!addon.isEmpty()) {
        const QString bare = CslText::fromHtml(text).toPlainText();
        const bool ended = bare.endsWith(u'.') || bare.endsWith(u'?') || bare.endsWith(u'!');
        text += (text.isEmpty() ? QString() : ended ? QStringLiteral(" ") : QStringLiteral(". ")) + addon;
    }
    return text;
}

int monthNumber(const QString &text)
{
    bool ok = false;
    const int number = text.toInt(&ok);
    if (ok) return number >= 1 && number <= 12 ? number : 0;
    static const QStringList names{QStringLiteral("jan"), QStringLiteral("feb"), QStringLiteral("mar"),
                                   QStringLiteral("apr"), QStringLiteral("may"), QStringLiteral("jun"),
                                   QStringLiteral("jul"), QStringLiteral("aug"), QStringLiteral("sep"),
                                   QStringLiteral("oct"), QStringLiteral("nov"), QStringLiteral("dec")};
    const QString lower = text.trimmed().toLower();
    for (int i = 0; i < names.size(); ++i) {
        if (lower.startsWith(names[i])) return i + 1;
    }
    return 0;
}

CslDate biblatexDate(const QString &text)
{
    QString value = plain(text);
    // BibLaTeX also writes ranges with a double hyphen.
    value.replace(QStringLiteral("–"), QStringLiteral("/"));
    return CslDate::fromText(value);
}

class Converter {
public:
    Converter(const QList<RawEntry> &entries, QList<BibliographyDiagnostic> &diagnostics)
        : m_entries(entries), m_diagnostics(diagnostics)
    {
        for (qsizetype i = 0; i < entries.size(); ++i) {
            if (!m_byKey.contains(entries[i].key)) m_byKey.insert(entries[i].key, i);
            // BibLaTeX keys are case-sensitive; crossref lookups fall back to any case.
            if (!m_byLowerKey.contains(entries[i].key.toLower())) m_byLowerKey.insert(entries[i].key.toLower(), i);
        }
    }

    CslReference convert(const RawEntry &input)
    {
        RawEntry entry = input;
        inherit(entry);
        CslReference reference;
        reference.id = entry.key;
        reference.type = cslType(entry);
        const QString &type = entry.type;

        const QString options = entry.value(QStringLiteral("options")).toLower();
        const bool usePrefix = !options.contains(QStringLiteral("useprefix=false"));
        QString language = entry.value(QStringLiteral("langid"));
        if (language.isEmpty()) language = entry.value(QStringLiteral("language"));
        const bool english = isEnglish(plain(language));
        if (!language.isEmpty()) reference.fields.insert(QStringLiteral("language"), cslLanguage(plain(language)));

        auto text = [&entry](const char *field) { return BibTeX::toMarkup(entry.value(QLatin1String(field))); };
        auto title = [&entry, english](const char *field) {
            const QString markup = BibTeX::toMarkup(entry.value(QLatin1String(field)), true);
            return english ? BibTeX::sentenceCase(markup) : markup;
        };
        auto periodical = [&entry](const char *field) {
            return BibTeX::toMarkup(entry.value(QLatin1String(field)), true);
        };
        auto set = [&reference](const QString &variable, const QString &value) {
            if (!value.isEmpty() && !reference.fields.contains(variable)) reference.fields.insert(variable, value);
        };
        auto setNames = [&reference, &entry, usePrefix](const QString &variable, const char *field) {
            const QString value = entry.value(QLatin1String(field));
            if (value.isEmpty() || reference.names.contains(variable)) return;
            const QList<CslName> names = BibTeX::parseNames(value, usePrefix);
            if (!names.isEmpty()) reference.names.insert(variable, names);
        };

        // Names.
        setNames(QStringLiteral("author"), "author");
        const QString editorType = plain(entry.value(QStringLiteral("editortype"))).toLower();
        if (editorType.isEmpty() || editorType == u"editor")
            setNames(QStringLiteral("editor"), "editor");
        else if (editorType == u"compiler")
            setNames(QStringLiteral("compiler"), "editor");
        else if (editorType == u"director")
            setNames(QStringLiteral("director"), "editor");
        else if (editorType == u"organizer")
            setNames(QStringLiteral("organizer"), "editor");
        else
            setNames(QStringLiteral("editor"), "editor");
        setNames(QStringLiteral("translator"), "translator");
        setNames(QStringLiteral("container-author"), "bookauthor");
        setNames(QStringLiteral("director"), "director");
        setNames(QStringLiteral("illustrator"), "illustrator");
        setNames(QStringLiteral("composer"), "composer");
        setNames(QStringLiteral("original-author"), "origauthor");
        for (const char *extra : {"editora", "editorb", "editorc"}) {
            const QString role =
                plain(entry.value(QLatin1String(extra) + QStringLiteral("type"))).toLower();
            if (role == u"director")
                setNames(QStringLiteral("director"), extra);
            else if (role == u"compiler")
                setNames(QStringLiteral("compiler"), extra);
            else if (role == u"translator")
                setNames(QStringLiteral("translator"), extra);
        }

        // Titles.
        const bool inWork = type.startsWith(u"in") || type == u"bookinbook" || type == u"suppbook"
                            || type == u"suppcollection" || type == u"conference";
        const QString mainTitle = joinedTitle(title("title"), title("subtitle"), title("titleaddon"));
        set(QStringLiteral("title"), mainTitle);
        const QString shortTitle = title("shorttitle");
        set(QStringLiteral("title-short"), shortTitle);
        const QString bookTitle = joinedTitle(title("booktitle"), title("booksubtitle"), title("booktitleaddon"));
        const QString maintitle = joinedTitle(title("maintitle"), title("mainsubtitle"), title("maintitleaddon"));
        if (type == u"article" || type == u"periodical" || type == u"suppperiodical") {
            QString journal = joinedTitle(periodical("journaltitle"), periodical("journalsubtitle"), {});
            if (journal.isEmpty()) journal = periodical("journal");
            set(QStringLiteral("container-title"), journal);
            set(QStringLiteral("container-title-short"), periodical("shortjournal"));
            const QString issueTitle = joinedTitle(title("issuetitle"), title("issuesubtitle"), {});
            set(QStringLiteral("volume-title"), issueTitle);
        } else if (inWork) {
            if (!bookTitle.isEmpty()) {
                set(QStringLiteral("container-title"), bookTitle);
                set(QStringLiteral("volume-title"), {});
            } else {
                set(QStringLiteral("container-title"), maintitle);
            }
        } else if (type == u"online" || type == u"electronic" || type == u"www") {
            QString site = periodical("journaltitle");
            if (site.isEmpty()) site = maintitle;
            set(QStringLiteral("container-title"), site);
        }
        set(QStringLiteral("collection-title"), periodical("series"));
        set(QStringLiteral("event-title"), title("eventtitle"));
        set(QStringLiteral("original-title"), title("origtitle"));
        set(QStringLiteral("reviewed-title"), {});

        // Publication details.
        QString publisher = text("publisher");
        if (publisher.isEmpty()) publisher = text("institution");
        if (publisher.isEmpty()) publisher = text("school");
        if (publisher.isEmpty()) publisher = text("organization");
        set(QStringLiteral("publisher"), publisher);
        QString place = text("location");
        if (place.isEmpty()) place = text("address");
        set(QStringLiteral("publisher-place"), place);
        set(QStringLiteral("event-place"), text("venue"));
        set(QStringLiteral("original-publisher"), text("origpublisher"));
        set(QStringLiteral("original-publisher-place"), text("origlocation"));

        // Numbers.
        set(QStringLiteral("volume"), text("volume"));
        set(QStringLiteral("number-of-volumes"), text("volumes"));
        set(QStringLiteral("edition"), text("edition"));
        set(QStringLiteral("page"), text("pages"));
        set(QStringLiteral("number-of-pages"), text("pagetotal"));
        set(QStringLiteral("chapter-number"), text("chapter"));
        set(QStringLiteral("part-number"), text("part"));
        set(QStringLiteral("version"), text("version"));
        const QString number = text("number");
        if (type == u"article" || type == u"periodical") {
            set(QStringLiteral("issue"), number);
            set(QStringLiteral("issue"), text("issue"));
        } else if (!entry.value(QStringLiteral("series")).isEmpty()
                   && (type.contains(u"book") || type.contains(u"collection") || type.contains(u"proceedings")
                       || type == u"inreference" || type == u"reference")) {
            set(QStringLiteral("collection-number"), number);
        } else {
            set(QStringLiteral("number"), number);
            set(QStringLiteral("issue"), text("issue"));
        }

        // Genre, from the type field or the kind of entry.
        QString genre = plain(entry.value(QStringLiteral("type")));
        const QString lowerGenre = genre.toLower();
        if (lowerGenre == u"phdthesis")
            genre = QStringLiteral("PhD thesis");
        else if (lowerGenre == u"mathesis" || lowerGenre == u"mastersthesis")
            genre = QStringLiteral("Master’s thesis");
        else if (lowerGenre == u"techreport")
            genre = QStringLiteral("technical report");
        else if (lowerGenre == u"resreport")
            genre = QStringLiteral("research report");
        else if (!genre.isEmpty())
            genre = text("type");
        if (genre.isEmpty()) {
            if (type == u"phdthesis") genre = QStringLiteral("PhD thesis");
            if (type == u"mastersthesis") genre = QStringLiteral("Master’s thesis");
            if (type == u"techreport") genre = QStringLiteral("technical report");
        }
        set(QStringLiteral("genre"), genre);

        // Identifiers and links.
        QString url = plain(entry.value(QStringLiteral("url")));
        const QString howpublished = entry.value(QStringLiteral("howpublished"));
        if (url.isEmpty() && howpublished.contains(QStringLiteral("\\url"))) url = plain(howpublished);
        else if (!howpublished.isEmpty())
            set(QStringLiteral("medium"), text("howpublished"));
        QString doi = plain(entry.value(QStringLiteral("doi")));
        const QString eprintType = plain(entry.value(QStringLiteral("eprinttype"))).toLower();
        const QString eprint = plain(entry.value(QStringLiteral("eprint")));
        if (!eprint.isEmpty()) {
            if (eprintType == u"doi" && doi.isEmpty())
                doi = eprint;
            else if (eprintType == u"pubmed")
                set(QStringLiteral("PMID"), eprint);
            else if (eprintType == u"arxiv" && url.isEmpty())
                url = QStringLiteral("https://arxiv.org/abs/") + eprint;
        }
        set(QStringLiteral("URL"), url);
        set(QStringLiteral("DOI"), doi);
        set(QStringLiteral("ISBN"), plain(entry.value(QStringLiteral("isbn"))));
        set(QStringLiteral("ISSN"), plain(entry.value(QStringLiteral("issn"))));
        set(QStringLiteral("PMID"), plain(entry.value(QStringLiteral("pmid"))));
        set(QStringLiteral("PMCID"), plain(entry.value(QStringLiteral("pmcid"))));

        // Other text.
        QString note = text("note");
        const QString addendum = text("addendum");
        if (!addendum.isEmpty()) note = note.isEmpty() ? addendum : note + QStringLiteral(". ") + addendum;
        set(QStringLiteral("note"), note);
        set(QStringLiteral("abstract"), text("abstract"));
        set(QStringLiteral("keyword"), text("keywords"));
        QString annote = text("annotation");
        if (annote.isEmpty()) annote = text("annote");
        set(QStringLiteral("annote"), annote);
        const QString pubstate = plain(entry.value(QStringLiteral("pubstate"))).toLower();
        if (pubstate == u"inpress")
            set(QStringLiteral("status"), QStringLiteral("in press"));
        else if (!pubstate.isEmpty())
            set(QStringLiteral("status"), text("pubstate"));

        // Dates.
        auto setDate = [&reference](const QString &variable, const CslDate &date) {
            if (!date.isEmpty() && !reference.dates.contains(variable)) reference.dates.insert(variable, date);
        };
        if (entry.has(QStringLiteral("date"))) {
            setDate(QStringLiteral("issued"), biblatexDate(entry.value(QStringLiteral("date"))));
        } else if (entry.has(QStringLiteral("year"))) {
            const QString year = plain(entry.value(QStringLiteral("year")));
            CslDate date = biblatexDate(year);
            if (date.literal.isEmpty() && !date.isRange && date.start.parts == 1) {
                const int month = monthNumber(plain(entry.value(QStringLiteral("month"))));
                if (month > 0) {
                    date.start.month = month;
                    date.start.parts = 2;
                    bool ok = false;
                    const int day = plain(entry.value(QStringLiteral("day"))).toInt(&ok);
                    if (ok && day >= 1 && day <= 31) {
                        date.start.day = day;
                        date.start.parts = 3;
                    }
                }
            } else if (!date.literal.isEmpty()) {
                const QString lower = year.toLower();
                if (lower == u"forthcoming" || lower == u"in press" || lower == u"inpress") {
                    set(QStringLiteral("status"), lower == u"forthcoming" ? year : QStringLiteral("in press"));
                    date = {};
                }
            }
            setDate(QStringLiteral("issued"), date);
        }
        setDate(QStringLiteral("accessed"), biblatexDate(entry.value(QStringLiteral("urldate"))));
        setDate(QStringLiteral("original-date"), biblatexDate(entry.value(QStringLiteral("origdate"))));
        setDate(QStringLiteral("event-date"), biblatexDate(entry.value(QStringLiteral("eventdate"))));
        return reference;
    }

private:
    const RawEntry *lookup(const QString &key) const
    {
        if (const auto it = m_byKey.constFind(key); it != m_byKey.constEnd()) return &m_entries[*it];
        if (const auto it = m_byLowerKey.constFind(key.toLower()); it != m_byLowerKey.constEnd())
            return &m_entries[*it];
        return nullptr;
    }

    // Fills in fields from a crossref'd or xdata parent, as BibLaTeX does.
    void inherit(RawEntry &entry, int depth = 0)
    {
        if (depth > 8) return;
        for (const QString &xdata : plain(entry.value(QStringLiteral("xdata"))).split(u',', Qt::SkipEmptyParts)) {
            const RawEntry *parent = lookup(xdata.trimmed());
            if (!parent) {
                m_diagnostics.append({Severity::Warning,
                                      QStringLiteral("The entry “%1” uses xdata “%2”, which is not defined.")
                                          .arg(entry.key, xdata.trimmed()),
                                      entry.key, entry.line});
                continue;
            }
            for (const auto &field : parent->fields) {
                if (!entry.has(field.first)) entry.fields.append(field);
            }
        }
        const QString parentKey = plain(entry.value(QStringLiteral("crossref")));
        if (parentKey.isEmpty()) return;
        const RawEntry *found = lookup(parentKey);
        if (!found) {
            m_diagnostics.append({Severity::Warning,
                                  QStringLiteral("The entry “%1” refers to “%2”, which is not defined.")
                                      .arg(entry.key, parentKey),
                                  entry.key, entry.line});
            return;
        }
        RawEntry parent = *found;
        inherit(parent, depth + 1);
        const bool parentIsMulti = parent.type.startsWith(u"mv");
        const bool childIsIn = entry.type.startsWith(u"in") || entry.type == u"bookinbook"
                               || entry.type == u"suppbook" || entry.type == u"suppcollection";
        static const QSet<QString> never{QStringLiteral("ids"),     QStringLiteral("crossref"),
                                         QStringLiteral("xref"),    QStringLiteral("entryset"),
                                         QStringLiteral("entrysubtype"), QStringLiteral("execute"),
                                         QStringLiteral("label"),   QStringLiteral("options"),
                                         QStringLiteral("presort"), QStringLiteral("related"),
                                         QStringLiteral("relatedoptions"), QStringLiteral("relatedstring"),
                                         QStringLiteral("relatedtype"), QStringLiteral("shorthand"),
                                         QStringLiteral("shorthandintro"), QStringLiteral("sortkey"),
                                         QStringLiteral("xdata")};
        for (const auto &field : parent.fields) {
            QString name = field.first;
            if (never.contains(name)) continue;
            if (childIsIn || parentIsMulti) {
                const QString prefix = parentIsMulti && !childIsIn ? QStringLiteral("main") : QStringLiteral("book");
                if (name == u"title" || name == u"subtitle" || name == u"titleaddon")
                    name = prefix + name;
                else if (name == u"shorttitle" || name == u"sorttitle" || name == u"indextitle"
                         || name == u"indexsorttitle")
                    continue;
                else if (name == u"author" && entry.type == u"inbook")
                    name = QStringLiteral("bookauthor");
            }
            if (!entry.has(name)) entry.fields.append({name, field.second});
        }
    }

    const QList<RawEntry> &m_entries;
    QList<BibliographyDiagnostic> &m_diagnostics;
    QHash<QString, qsizetype> m_byKey;
    QHash<QString, qsizetype> m_byLowerKey;
};

} // namespace

QString BibTeX::toMarkup(const QString &latex, bool protectCase)
{
    if (latex.isEmpty()) return {};
    return LatexConverter(latex, protectCase).run();
}

QList<CslName> BibTeX::parseNames(const QString &value, bool usePrefix)
{
    QList<CslName> names;
    for (const QString &part : splitTopLevel(value.simplified(), andAt)) {
        const QString trimmed = part.trimmed();
        if (trimmed.isEmpty() || trimmed.compare(u"others", Qt::CaseInsensitive) == 0) continue;
        const CslName name = parseName(trimmed, usePrefix);
        if (!name.isEmpty()) names.append(name);
    }
    return names;
}

QString BibTeX::sentenceCase(const QString &markup)
{
    const QList<CslMarkup::Token> tokens = CslMarkup::tokenize(markup);
    QString out;
    QList<bool> nocaseStack;
    bool sentenceStart = true;
    bool afterBreak = false;
    for (const CslMarkup::Token &token : tokens) {
        if (token.type == CslMarkup::Token::Type::Open) {
            const bool nocase = token.text == u"span" && token.cssClass.split(u' ').contains(u"nocase");
            nocaseStack.append(nocase);
            out += u'<' + token.text;
            if (!token.cssClass.isEmpty()) out += QStringLiteral(" class=\"%1\"").arg(token.cssClass);
            if (!token.style.isEmpty()) out += QStringLiteral(" style=\"%1\"").arg(token.style);
            out += u'>';
            continue;
        }
        if (token.type == CslMarkup::Token::Type::Close) {
            if (!nocaseStack.isEmpty()) nocaseStack.removeLast();
            out += QStringLiteral("</%1>").arg(token.text);
            continue;
        }
        const bool protectedText = nocaseStack.contains(true);
        const QString &text = token.text;
        QString result;
        qsizetype i = 0;
        while (i < text.size()) {
            const QChar c = text[i];
            if (!c.isLetterOrNumber()) {
                if (c == u':' || c == u'?' || c == u'!' || c == u'.') afterBreak = true;
                else if (!c.isSpace() && c != u'"' && c != u'“' && c != u'‘' && c != u'(')
                    afterBreak = false;
                if (c.isSpace() && afterBreak) sentenceStart = true;
                result += c;
                ++i;
                continue;
            }
            qsizetype end = i;
            while (end < text.size() && (text[end].isLetterOrNumber() || text[end] == u'\'' || text[end] == u'’'))
                ++end;
            QString word = text.mid(i, end - i);
            if (!protectedText && !sentenceStart && word.size() > 1 && word[0].isUpper()) {
                bool restLower = true;
                for (qsizetype k = 1; k < word.size(); ++k) {
                    if (word[k].isLetter() && !word[k].isLower()) restLower = false;
                }
                if (restLower) word[0] = word[0].toLower();
            }
            sentenceStart = false;
            afterBreak = false;
            result += word;
            i = end;
        }
        out += CslMarkup::escapeHtml(result).replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    }
    return out;
}

BibTeX::Result BibTeX::parse(const QString &text)
{
    Result result;
    Reader reader(text);
    const QList<RawEntry> entries = reader.read(result.diagnostics);
    Converter converter(entries, result.diagnostics);
    latexTooDeep = false;
    for (const RawEntry &entry : entries) {
        result.entries.append({converter.convert(entry), entry.line});
        if (!latexTooDeep) continue;
        result.entries.clear();
        result.diagnostics.append({Severity::Error,
                                   QStringLiteral("The entry “%1” nests braces or commands more than %2 deep, which no "
                                                  "bibliography needs, so the file is not read.")
                                       .arg(entry.key)
                                       .arg(MaxLatexNesting),
                                   entry.key, entry.line, BibliographyDiagnostic::Kind::Unreadable});
        break;
    }
    return result;
}
