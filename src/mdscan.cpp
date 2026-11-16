// The scanners the block parser and the inline parser share: character
// references, link labels, destinations and titles, raw HTML, and link
// reference definitions. They follow the grammar of CommonMark 0.31.2.
#include "mdparser_p.h"

namespace md::detail {
namespace {

int skipWhitespace(const QChar *s, int len, int i)
{
    while (i < len) {
        const char16_t c = s[i].unicode();
        if (c != u' ' && c != u'\t' && c != u'\n' && c != u'\r' && c != u'\f' && c != 0x0b) break;
        ++i;
    }
    return i;
}

int tagNameLength(const QChar *s, int len, int i)
{
    if (i >= len || !isAsciiAlpha(s[i].unicode())) return 0;
    int e = i + 1;
    while (e < len && (isAsciiAlnum(s[e].unicode()) || s[e] == u'-')) ++e;
    return e - i;
}

bool startsWith(const QChar *s, int len, int pos, QLatin1StringView word)
{
    if (pos + word.size() > len) return false;
    for (qsizetype i = 0; i < word.size(); ++i)
        if (s[pos + i] != QLatin1Char(word[i])) return false;
    return true;
}

} // namespace

int scanEntity(const QChar *s, int len, int pos, QString *value)
{
    if (pos + 2 >= len || s[pos] != u'&') return 0;
    int i = pos + 1;
    if (s[i] == u'#') {
        ++i;
        bool hex = false;
        if (i < len && (s[i] == u'x' || s[i] == u'X')) {
            hex = true;
            ++i;
        }
        const int digits = i;
        const int maxDigits = hex ? 6 : 7;
        char32_t code = 0;
        while (i < len && i - digits <= maxDigits) {
            const char16_t c = s[i].unicode();
            int digit = 0;
            if (isAsciiDigit(c)) digit = c - u'0';
            else if (hex && c >= u'a' && c <= u'f') digit = c - u'a' + 10;
            else if (hex && c >= u'A' && c <= u'F') digit = c - u'A' + 10;
            else break;
            code = code * (hex ? 16 : 10) + digit;
            ++i;
        }
        const int count = i - digits;
        if (count < 1 || count > maxDigits || i >= len || s[i] != u';') return 0;
        if (code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) code = 0xfffd;
        if (value) *value = QString::fromUcs4(&code, 1);
        return i + 1 - pos;
    }
    const int start = i;
    while (i < len && i - start < 32 && isAsciiAlnum(s[i].unicode())) ++i;
    if (i == start || i >= len || s[i] != u';') return 0;
    const QString named = namedEntity(QStringView(s + start, i - start));
    if (named.isEmpty()) return 0;
    if (value) *value = named;
    return i + 1 - pos;
}

int scanLinkLabel(const QChar *s, int len, int pos)
{
    if (pos >= len || s[pos] != u'[') return 0;
    const int limit = qMin(len, pos + 1001);
    int i = pos + 1;
    while (i < limit) {
        const char16_t c = s[i].unicode();
        if (c == u'\\') {
            i += 2;
            continue;
        }
        if (c == u'[') return 0;
        if (c == u']') return i + 1 - pos;
        ++i;
    }
    return 0;
}

int scanLinkDestination(const QChar *s, int len, int pos, int *start, int *end)
{
    if (pos < len && s[pos] == u'<') {
        int i = pos + 1;
        while (i < len) {
            const char16_t c = s[i].unicode();
            if (c == u'>') {
                *start = pos + 1;
                *end = i;
                return i + 1 - pos;
            }
            if (c == u'<' || c == u'\n' || c == u'\r') return -1;
            if (c == u'\\' && i + 1 < len && s[i + 1] != u'\n') {
                i += 2;
                continue;
            }
            ++i;
        }
        return -1;
    }
    int i = pos;
    int parens = 0;
    while (i < len) {
        const char16_t c = s[i].unicode();
        if (c == u'\\' && i + 1 < len && isAsciiPunct(s[i + 1].unicode())) {
            i += 2;
            continue;
        }
        if (c == u'(') {
            if (++parens > 32) return -1;
        } else if (c == u')') {
            if (parens == 0) break;
            --parens;
        } else if (c <= 0x20 || c == 0x7f) {
            break;
        }
        ++i;
    }
    if (parens != 0) return -1;
    if (i == pos && !(i < len && s[i] == u')')) return -1;
    *start = pos;
    *end = i;
    return i - pos;
}

int scanLinkTitle(const QChar *s, int len, int pos)
{
    if (pos >= len) return 0;
    const char16_t open = s[pos].unicode();
    char16_t close = open;
    if (open == u'(') close = u')';
    else if (open != u'"' && open != u'\'') return 0;
    int i = pos + 1;
    while (i < len) {
        const char16_t c = s[i].unicode();
        if (c == u'\\' && i + 1 < len) {
            i += 2;
            continue;
        }
        if (c == close) return i + 1 - pos;
        if (open == u'(' && c == u'(') return 0;
        ++i;
    }
    return 0;
}

int skipSpacesAndNewline(const QChar *s, int len, int pos)
{
    while (pos < len && isSpaceOrTab(s[pos].unicode())) ++pos;
    if (pos < len && s[pos] == u'\n') {
        ++pos;
        while (pos < len && isSpaceOrTab(s[pos].unicode())) ++pos;
    }
    return pos;
}

int scanHtml(const QChar *s, int len, int pos, HtmlKind *kind)
{
    *kind = HtmlKind::None;
    if (pos + 2 >= len || s[pos] != u'<') return 0;
    const char16_t c = s[pos + 1].unicode();
    if (c == u'/') {
        int i = pos + 2;
        const int name = tagNameLength(s, len, i);
        if (!name) return 0;
        i = skipWhitespace(s, len, i + name);
        if (i >= len || s[i] != u'>') return 0;
        *kind = HtmlKind::CloseTag;
        return i + 1 - pos;
    }
    if (c == u'?') {
        for (int i = pos + 2; i + 1 < len; ++i) {
            if (s[i] == u'?' && s[i + 1] == u'>') {
                *kind = HtmlKind::Instruction;
                return i + 2 - pos;
            }
        }
        return 0;
    }
    if (c == u'!') {
        if (startsWith(s, len, pos, QLatin1StringView("<!--"))) {
            int i = pos + 4;
            *kind = HtmlKind::Comment;
            if (i < len && s[i] == u'>') return 5;
            if (i + 1 < len && s[i] == u'-' && s[i + 1] == u'>') return 6;
            for (; i + 2 < len; ++i)
                if (s[i] == u'-' && s[i + 1] == u'-' && s[i + 2] == u'>') return i + 3 - pos;
            *kind = HtmlKind::None;
            return 0;
        }
        if (startsWith(s, len, pos, QLatin1StringView("<![CDATA["))) {
            for (int i = pos + 9; i + 2 < len; ++i) {
                if (s[i] == u']' && s[i + 1] == u']' && s[i + 2] == u'>') {
                    *kind = HtmlKind::CData;
                    return i + 3 - pos;
                }
            }
            return 0;
        }
        if (isAsciiAlpha(s[pos + 2].unicode())) {
            for (int i = pos + 3; i < len; ++i) {
                if (s[i] == u'>') {
                    *kind = HtmlKind::Declaration;
                    return i + 1 - pos;
                }
            }
        }
        return 0;
    }
    int i = pos + 1;
    const int name = tagNameLength(s, len, i);
    if (!name) return 0;
    i += name;
    for (;;) {
        const int w = skipWhitespace(s, len, i);
        if (w == i || w >= len) {
            i = w;
            break;
        }
        const char16_t a = s[w].unicode();
        if (!isAsciiAlpha(a) && a != u'_' && a != u':') {
            i = w;
            break;
        }
        int e = w + 1;
        while (e < len) {
            const char16_t n = s[e].unicode();
            if (!isAsciiAlnum(n) && n != u'_' && n != u'.' && n != u':' && n != u'-') break;
            ++e;
        }
        int v = skipWhitespace(s, len, e);
        if (v < len && s[v] == u'=') {
            v = skipWhitespace(s, len, v + 1);
            if (v >= len) return 0;
            const char16_t q = s[v].unicode();
            if (q == u'"' || q == u'\'') {
                int close = v + 1;
                while (close < len && s[close].unicode() != q) ++close;
                if (close >= len) return 0;
                e = close + 1;
            } else {
                int close = v;
                while (close < len) {
                    const char16_t n = s[close].unicode();
                    if (n <= 0x20 || n == u'"' || n == u'\'' || n == u'=' || n == u'<' || n == u'>' || n == u'`') break;
                    ++close;
                }
                if (close == v) return 0;
                e = close;
            }
        }
        i = e;
    }
    if (i < len && s[i] == u'/') ++i;
    if (i >= len || s[i] != u'>') return 0;
    *kind = HtmlKind::OpenTag;
    return i + 1 - pos;
}

QString unescape(QStringView text)
{
    qsizetype i = 0;
    const qsizetype size = text.size();
    while (i < size && text[i] != u'\\' && text[i] != u'&') ++i;
    if (i == size) return text.toString();
    QString out;
    out.reserve(size);
    out.append(text.first(i));
    const QChar *s = text.data();
    while (i < size) {
        const char16_t c = s[i].unicode();
        if (c == u'\\' && i + 1 < size && isAsciiPunct(s[i + 1].unicode())) {
            out.append(s[i + 1]);
            i += 2;
            continue;
        }
        if (c == u'&') {
            QString value;
            if (const int n = scanEntity(s, int(size), int(i), &value)) {
                out.append(value);
                i += n;
                continue;
            }
        }
        out.append(s[i]);
        ++i;
    }
    return out;
}

QString normalizeLabel(QStringView label)
{
    QString out;
    out.reserve(label.size());
    bool space = false;
    bool ascii = true;
    for (const QChar ch : label) {
        const char16_t c = ch.unicode();
        if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r') {
            space = !out.isEmpty();
            continue;
        }
        if (space) {
            out.append(u' ');
            space = false;
        }
        if (c >= 128) ascii = false;
        out.append(ch);
    }
    // Lower then upper is a full case fold: it maps both the capital sharp s
    // and "SS" to "SS".
    return ascii ? out.toUpper() : out.toLower().toUpper();
}

ReferenceMatch scanReference(const QChar *s, int len)
{
    ReferenceMatch match;
    const int label = scanLinkLabel(s, len, 0);
    if (label == 0) return match;
    int i = label;
    if (i >= len || s[i] != u':') return match;
    i = skipSpacesAndNewline(s, len, i + 1);
    int destStart = 0, destEnd = 0;
    const int dest = scanLinkDestination(s, len, i, &destStart, &destEnd);
    if (dest <= 0) return match;
    i += dest;

    auto lineEnd = [&](int from) {
        int e = from;
        while (e < len && isSpaceOrTab(s[e].unicode())) ++e;
        if (e >= len) return e;
        return s[e] == u'\n' ? e + 1 : -1;
    };

    int end = -1;
    const int titleAt = skipSpacesAndNewline(s, len, i);
    if (titleAt > i) {
        if (const int title = scanLinkTitle(s, len, titleAt)) {
            end = lineEnd(titleAt + title);
            if (end >= 0) {
                match.title = {titleAt + 1, titleAt + title - 1};
                match.hasTitle = true;
            }
        }
    }
    // A title that is not followed by the end of its line is not one; the
    // definition may still end with the destination.
    if (end < 0) end = lineEnd(i);
    if (end < 0) return match;

    bool blank = true;
    for (int k = 1; k < label - 1 && blank; ++k) {
        const char16_t c = s[k].unicode();
        blank = c == u' ' || c == u'\t' || c == u'\n' || c == u'\r';
    }
    if (blank) return {};
    match.length = end;
    match.label = {1, label - 1};
    match.dest = {destStart, destEnd};
    return match;
}

} // namespace md::detail
