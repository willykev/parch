#include "frontmatter.h"

#include <QStringList>

#include <algorithm>

namespace md {
namespace {

// Nesting deeper than this is read as plain text, so that hostile metadata
// cannot exhaust the stack.
constexpr int MaxDepth = 64;

bool isBlankChar(QChar c)
{ return c == u' ' || c == u'\t'; }

QStringView trimmed(QStringView s)
{
    qsizetype a = 0, b = s.size();
    while (a < b && isBlankChar(s[a])) ++a;
    while (b > a && isBlankChar(s[b - 1])) --b;
    return s.sliced(a, b - a);
}

struct Line {
    int start = 0;  // of the content, after the indentation
    int end = 0;    // without the line terminator
    int indent = 0; // columns of leading space
};

class Yaml {
public:
    Yaml(QStringView text, std::vector<FrontMatterIssue> *issues, int base)
        : m_text(text), m_issues(issues), m_base(base)
    {
        int pos = 0;
        const int size = int(text.size());
        while (pos < size) {
            int end = pos;
            while (end < size && text[end] != u'\n' && text[end] != u'\r') ++end;
            int start = pos;
            while (start < end && isBlankChar(text[start])) ++start;
            m_lines.push_back({start, end, start - pos});
            pos = end;
            if (pos < size) pos += (text[pos] == u'\r' && pos + 1 < size && text[pos + 1] == u'\n') ? 2 : 1;
        }
    }

    YamlValue parse()
    {
        YamlValue value = parseBlock(0, -1, 0);
        skipBlank();
        // Anything left is text the grammar above did not place.
        while (m_line < m_lines.size()) {
            issue(m_lines[m_line].start, QStringLiteral("This line is not part of the metadata above it"));
            ++m_line;
            skipBlank();
        }
        return value;
    }

private:
    QStringView content(const Line &line) const { return m_text.sliced(line.start, line.end - line.start); }

    void issue(int offset, const QString &message)
    {
        if (m_issues) m_issues->push_back({m_base + offset, message});
    }

    static bool skippable(QStringView text)
    { return text.isEmpty() || text[0] == u'#' || trimmed(text).isEmpty(); }

    void skipBlank()
    {
        while (m_line < m_lines.size() && skippable(content(m_lines[m_line]))) ++m_line;
    }

    static bool startsSequence(QStringView text)
    { return !text.isEmpty() && text[0] == u'-' && (text.size() == 1 || isBlankChar(text[1])); }

    // The index of the colon that ends a mapping key, or -1.
    static int keyEnd(QStringView text)
    {
        if (text.isEmpty()) return -1;
        const QChar first = text[0];
        if (first == u'[' || first == u'{' || first == u'|' || first == u'>' || first == u'!' || first == u'&'
            || first == u'*')
            return -1;
        qsizetype i = 0;
        if (first == u'"' || first == u'\'') {
            for (i = 1; i < text.size(); ++i) {
                if (first == u'"' && text[i] == u'\\') { ++i; continue; }
                if (text[i] == first) {
                    if (first == u'\'' && i + 1 < text.size() && text[i + 1] == u'\'') { ++i; continue; }
                    break;
                }
            }
            if (i >= text.size()) return -1;
            ++i;
            while (i < text.size() && isBlankChar(text[i])) ++i;
            if (i < text.size() && text[i] == u':' && (i + 1 == text.size() || isBlankChar(text[i + 1]))) return int(i);
            return -1;
        }
        for (; i < text.size(); ++i) {
            const QChar c = text[i];
            if (c == u'#' && i > 0 && isBlankChar(text[i - 1])) return -1;
            if (c == u':' && (i + 1 == text.size() || isBlankChar(text[i + 1]))) return int(i);
        }
        return -1;
    }

    static QString unquoteDouble(QStringView text)
    {
        QString out;
        out.reserve(text.size());
        for (qsizetype i = 0; i < text.size(); ++i) {
            const QChar c = text[i];
            if (c != u'\\' || i + 1 >= text.size()) { out.append(c); continue; }
            const QChar e = text[++i];
            auto hex = [&](int digits) {
                if (i + digits >= text.size()) { out.append(e); return; }
                bool ok = false;
                const uint code = text.sliced(i + 1, digits).toUInt(&ok, 16);
                if (!ok || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) { out.append(e); return; }
                out.append(QChar::fromUcs4(char32_t(code)));
                i += digits;
            };
            switch (e.unicode()) {
            case 'n': out.append(u'\n'); break;
            case 't': out.append(u'\t'); break;
            case 'r': out.append(u'\r'); break;
            case '0': out.append(QChar(0xfffd)); break;
            case 'a': out.append(QChar(7)); break;
            case 'b': out.append(QChar(8)); break;
            case 'e': out.append(QChar(27)); break;
            case 'f': out.append(QChar(12)); break;
            case 'v': out.append(QChar(11)); break;
            case 'x': hex(2); break;
            case 'u': hex(4); break;
            case 'U': hex(8); break;
            default: out.append(e); break; // \" \\ \/ and a literal for anything else
            }
        }
        return out;
    }

    static QString unquoteSingle(QStringView text)
    {
        QString out(text);
        out.replace(QLatin1String("''"), QLatin1String("'"));
        return out;
    }

    // A key as written before its colon.
    static QString keyText(QStringView text)
    {
        text = trimmed(text);
        if (text.size() >= 2 && text[0] == u'"' && text[text.size() - 1] == u'"')
            return unquoteDouble(text.sliced(1, text.size() - 2));
        if (text.size() >= 2 && text[0] == u'\'' && text[text.size() - 1] == u'\'')
            return unquoteSingle(text.sliced(1, text.size() - 2));
        return text.toString();
    }

    // A plain scalar without the comment that may follow it.
    static QStringView stripComment(QStringView text)
    {
        for (qsizetype i = 0; i < text.size(); ++i)
            if (text[i] == u'#' && (i == 0 || isBlankChar(text[i - 1]))) return trimmed(text.first(i));
        return trimmed(text);
    }

    static YamlValue plain(QStringView text)
    {
        YamlValue value;
        if (text.isEmpty() || text == u"~" || text == u"null" || text == u"Null" || text == u"NULL") return value;
        value.kind = YamlValue::Kind::Scalar;
        value.scalar = text.toString();
        return value;
    }

    // The node whose first line is the current one. `indent` is that line's
    // indentation; lines indented beyond `parentIndent` continue a scalar.
    YamlValue parseBlock(int indent, int parentIndent, int depth)
    {
        skipBlank();
        if (m_line >= m_lines.size()) return {};
        const Line &line = m_lines[m_line];
        if (line.indent < indent) return {};
        const QStringView text = content(line);
        if (depth < MaxDepth) {
            if (startsSequence(text)) return parseSequence(line.indent, depth);
            if (keyEnd(text) >= 0) return parseMapping(line.indent, depth);
        }
        ++m_line;
        return parseValue(text, line.start, parentIndent, depth);
    }

    YamlValue parseMapping(int indent, int depth)
    {
        YamlValue map;
        map.kind = YamlValue::Kind::Map;
        for (;;) {
            skipBlank();
            if (m_line >= m_lines.size()) break;
            const Line line = m_lines[m_line];
            if (line.indent < indent) break;
            const QStringView text = content(line);
            const int colon = line.indent == indent ? keyEnd(text) : -1;
            if (colon < 0) {
                issue(line.start, QStringLiteral("Expected a key followed by a colon"));
                ++m_line;
                continue;
            }
            const QString key = keyText(text.first(colon));
            QStringView rest = text.sliced(colon + 1);
            int offset = line.start + colon + 1;
            while (!rest.isEmpty() && isBlankChar(rest[0])) { rest = rest.sliced(1); ++offset; }
            ++m_line;
            YamlValue value;
            if (stripComment(rest).isEmpty()) {
                // The value is the block below: more indented, or a sequence
                // at the key's own indentation.
                skipBlank();
                if (m_line < m_lines.size()) {
                    const Line &next = m_lines[m_line];
                    if (next.indent > indent) value = parseBlock(next.indent, indent, depth + 1);
                    else if (next.indent == indent && startsSequence(content(next)) && depth < MaxDepth)
                        value = parseSequence(indent, depth + 1);
                }
            } else {
                value = parseValue(rest, offset, indent, depth + 1);
            }
            map.map.emplace_back(key, std::move(value));
        }
        return map;
    }

    YamlValue parseSequence(int indent, int depth)
    {
        YamlValue list;
        list.kind = YamlValue::Kind::List;
        for (;;) {
            skipBlank();
            if (m_line >= m_lines.size()) break;
            Line &line = m_lines[m_line];
            if (line.indent != indent || !startsSequence(content(line))) break;
            // The rest of the line stands as a line of its own, indented to
            // where it begins, so that `- key: value` opens a mapping there.
            int start = line.start + 1;
            while (start < line.end && isBlankChar(m_text[start])) ++start;
            if (start >= line.end) {
                ++m_line;
                skipBlank();
                if (m_line < m_lines.size() && m_lines[m_line].indent > indent)
                    list.list.push_back(parseBlock(m_lines[m_line].indent, indent, depth + 1));
                else
                    list.list.emplace_back();
                continue;
            }
            line.indent += start - line.start;
            line.start = start;
            list.list.push_back(parseBlock(line.indent, indent, depth + 1));
        }
        return list;
    }

    // A value written on the line of its key or dash; `offset` is where it
    // starts in the text. The current line is the one after it.
    YamlValue parseValue(QStringView rest, int offset, int parentIndent, int depth)
    {
        const QStringView trimmedRest = trimmed(rest);
        offset += int(trimmedRest.data() - rest.data());
        rest = trimmedRest;
        if (rest.isEmpty()) return {};
        const QChar first = rest[0];
        if (first == u'!' || first == u'&') {
            // A tag or an anchor: the word is dropped and the value read as data.
            qsizetype i = 0;
            while (i < rest.size() && !isBlankChar(rest[i])) ++i;
            if (first == u'!')
                issue(offset, QStringLiteral("The tag %1 is ignored; its value is read as plain data").arg(rest.first(i)));
            const QStringView after = trimmed(rest.sliced(i));
            if (after.isEmpty()) {
                skipBlank();
                if (m_line < m_lines.size() && m_lines[m_line].indent > parentIndent && depth < MaxDepth)
                    return parseBlock(m_lines[m_line].indent, parentIndent, depth + 1);
                return {};
            }
            return parseValue(after, offset + int(after.data() - rest.data()), parentIndent, depth);
        }
        if (first == u'|' || first == u'>') {
            YamlValue value = blockScalar(rest, parentIndent);
            value.sourceOffset = m_base + offset;
            return value;
        }
        if (first == u'[' || first == u'{') return flow(rest, offset, parentIndent);
        if (first == u'"' || first == u'\'') return quoted(rest, offset, parentIndent);

        // A plain scalar, which more indented lines continue.
        const QStringView original = stripComment(rest);
        QString text = original.toString();
        for (;;) {
            if (m_line >= m_lines.size()) break;
            const Line &next = m_lines[m_line];
            const QStringView more = content(next);
            if (trimmed(more).isEmpty()) {
                // A blank line ends the scalar unless indented text follows it.
                size_t ahead = m_line + 1;
                while (ahead < m_lines.size() && trimmed(content(m_lines[ahead])).isEmpty()) ++ahead;
                if (ahead >= m_lines.size() || m_lines[ahead].indent <= parentIndent) break;
                m_line = ahead;
                continue;
            }
            if (next.indent <= parentIndent || more[0] == u'#') break;
            const QStringView piece = stripComment(more);
            if (!piece.isEmpty()) {
                text.append(u' ');
                text.append(piece);
            }
            ++m_line;
        }
        YamlValue value = plain(text);
        if (value.isScalar()) {
            value.sourceOffset = m_base + offset;
            if (text.size() == original.size()) value.sourceLength = int(original.size());
        }
        return value;
    }

    YamlValue blockScalar(QStringView header, int parentIndent)
    {
        const bool folded = header[0] == u'>';
        QChar chomp = u'c';
        int explicitIndent = 0;
        for (qsizetype i = 1; i < header.size(); ++i) {
            const QChar c = header[i];
            if (c == u'-' || c == u'+') chomp = c;
            else if (c.isDigit()) explicitIndent = c.digitValue();
            else break;
        }
        int blockIndent = explicitIndent > 0 ? qMax(parentIndent, 0) + explicitIndent : -1;
        QStringList lines;
        while (m_line < m_lines.size()) {
            const Line &line = m_lines[m_line];
            const bool blank = line.start == line.end;
            if (!blank && line.indent <= parentIndent) break;
            if (blank) {
                lines.append(QString());
            } else {
                if (blockIndent < 0) blockIndent = line.indent;
                if (line.indent < blockIndent) break;
                // Indentation beyond the block's own belongs to the text.
                const int from = line.start - (line.indent - blockIndent);
                lines.append(m_text.sliced(from, line.end - from).toString());
            }
            ++m_line;
        }
        int trailing = 0;
        while (!lines.isEmpty() && lines.last().isEmpty()) { lines.removeLast(); ++trailing; }
        QString text;
        if (!folded) {
            text = lines.join(u'\n');
        } else {
            bool previousBlank = true;
            for (const QString &line : std::as_const(lines)) {
                if (line.isEmpty()) {
                    text.append(u'\n');
                    previousBlank = true;
                    continue;
                }
                if (!previousBlank) text.append(u' ');
                text.append(line);
                previousBlank = false;
            }
        }
        if (!lines.isEmpty()) {
            if (chomp == u'c') text.append(u'\n');
            else if (chomp == u'+') text.append(QString(trailing + 1, u'\n'));
        }
        YamlValue value;
        value.kind = YamlValue::Kind::Scalar;
        value.scalar = text;
        return value;
    }

    struct FlowLine {
        int start;  // in the gathered text
        int length;
        int offset; // in the source
    };

    // Gathers the lines a quoted scalar or a flow collection runs over.
    template <typename Closed>
    QString gather(QStringView rest, int parentIndent, Closed closed, std::vector<FlowLine> *lines = nullptr)
    {
        QString text = rest.toString();
        while (!closed(text) && m_line < m_lines.size()) {
            const Line &next = m_lines[m_line];
            if (next.start != next.end && next.indent <= parentIndent) break;
            const QStringView piece = trimmed(content(next));
            text.append(u' ');
            if (lines && !piece.isEmpty())
                lines->push_back({int(text.size()), int(piece.size()), int(piece.data() - m_text.data())});
            text.append(piece);
            ++m_line;
        }
        return text;
    }

    // The index just past the quoted scalar that starts `text`, or -1.
    static int quotedEnd(QStringView text)
    {
        const QChar quote = text[0];
        for (qsizetype i = 1; i < text.size(); ++i) {
            if (quote == u'"' && text[i] == u'\\') { ++i; continue; }
            if (text[i] != quote) continue;
            if (quote == u'\'' && i + 1 < text.size() && text[i + 1] == u'\'') { ++i; continue; }
            return int(i + 1);
        }
        return -1;
    }

    YamlValue quoted(QStringView rest, int offset, int parentIndent)
    {
        const QString text = gather(rest, parentIndent, [](const QString &t) { return quotedEnd(t) >= 0; });
        const int end = quotedEnd(text);
        YamlValue value;
        value.kind = YamlValue::Kind::Scalar;
        value.sourceOffset = m_base + offset;
        if (end < 0) {
            issue(offset, QStringLiteral("The quoted text is not closed"));
            value.scalar = text.mid(1);
            return value;
        }
        const QStringView inner = QStringView(text).sliced(1, end - 2);
        value.scalar = text[0] == u'"' ? unquoteDouble(inner) : unquoteSingle(inner);
        if (end <= rest.size()) value.sourceLength = end;
        return value;
    }

    static bool flowClosed(const QString &text)
    {
        int depth = 0;
        for (qsizetype i = 0; i < text.size(); ++i) {
            const QChar c = text[i];
            if (c == u'"' || c == u'\'') {
                const int end = quotedEnd(QStringView(text).sliced(i));
                if (end < 0) return false;
                i += end - 1;
            } else if (c == u'[' || c == u'{') {
                ++depth;
            } else if (c == u']' || c == u'}') {
                if (--depth <= 0) return true;
            }
        }
        return false;
    }

    YamlValue flow(QStringView rest, int offset, int parentIndent)
    {
        std::vector<FlowLine> lines{{0, int(rest.size()), offset}};
        const QString text = gather(rest, parentIndent, flowClosed, &lines);
        qsizetype pos = 0;
        bool ok = true;
        YamlValue value = flowValue(text, lines, pos, 0, ok);
        if (!ok) issue(offset, QStringLiteral("The bracketed list or mapping is not closed"));
        return value;
    }

    static void skipSpace(const QString &text, qsizetype &pos)
    {
        while (pos < text.size() && (isBlankChar(text[pos]) || text[pos] == u'\n')) ++pos;
    }

    void flowSpan(YamlValue &value, const std::vector<FlowLine> &lines, qsizetype start, qsizetype length)
    {
        if (!value.isScalar()) return;
        value.inFlow = true;
        const auto next = std::upper_bound(lines.begin(), lines.end(), start,
                                           [](qsizetype at, const FlowLine &line) { return at < line.start; });
        const FlowLine &line = *(next - 1);
        const int from = int(start) - line.start;
        if (from >= line.length) return;
        value.sourceOffset = m_base + line.offset + from;
        if (length <= line.length - from) value.sourceLength = int(length);
    }

    YamlValue flowValue(const QString &text, const std::vector<FlowLine> &lines, qsizetype &pos, int depth, bool &ok)
    {
        skipSpace(text, pos);
        YamlValue value;
        if (pos >= text.size()) { ok = false; return value; }
        const QChar c = text[pos];
        if ((c == u'[' || c == u'{') && depth < MaxDepth) {
            const bool map = c == u'{';
            const QChar close = map ? u'}' : u']';
            value.kind = map ? YamlValue::Kind::Map : YamlValue::Kind::List;
            ++pos;
            while (ok) {
                skipSpace(text, pos);
                if (pos >= text.size()) { ok = false; break; }
                if (text[pos] == close) { ++pos; break; }
                if (text[pos] == u',') { ++pos; continue; }
                if (!map) {
                    value.list.push_back(flowValue(text, lines, pos, depth + 1, ok));
                    continue;
                }
                const YamlValue key = flowValue(text, lines, pos, depth + 1, ok);
                skipSpace(text, pos);
                YamlValue item;
                if (pos < text.size() && text[pos] == u':') {
                    ++pos;
                    item = flowValue(text, lines, pos, depth + 1, ok);
                }
                value.map.emplace_back(key.scalar, std::move(item));
            }
            return value;
        }
        if (c == u'"' || c == u'\'') {
            const int end = quotedEnd(QStringView(text).sliced(pos));
            value.kind = YamlValue::Kind::Scalar;
            if (end < 0) {
                ok = false;
                value.scalar = text.mid(pos + 1);
                flowSpan(value, lines, pos, 0);
                pos = text.size();
                return value;
            }
            const QStringView inner = QStringView(text).sliced(pos + 1, end - 2);
            value.scalar = c == u'"' ? unquoteDouble(inner) : unquoteSingle(inner);
            flowSpan(value, lines, pos, end);
            pos += end;
            return value;
        }
        const qsizetype start = pos;
        while (pos < text.size()) {
            const QChar p = text[pos];
            if (p == u',' || p == u']' || p == u'}') break;
            if (p == u':' && (pos + 1 == text.size() || isBlankChar(text[pos + 1]))) break;
            ++pos;
        }
        if (pos == start) ok = false;
        const QStringView token = trimmed(QStringView(text).sliced(start, pos - start));
        value = plain(token);
        flowSpan(value, lines, start, token.size());
        return value;
    }

    QStringView m_text;
    std::vector<FrontMatterIssue> *m_issues;
    int m_base;
    std::vector<Line> m_lines;
    size_t m_line = 0;
};

// Whether the line that starts at `pos` is exactly `marker`, allowing blank
// space after it. Sets `end` to the end of the line and `next` to the start
// of the following one.
bool fenceLine(QStringView text, int pos, QStringView marker, int &end, int &next)
{
    const int size = int(text.size());
    end = pos;
    while (end < size && text[end] != u'\n' && text[end] != u'\r') ++end;
    next = end;
    if (next < size) next += (text[next] == u'\r' && next + 1 < size && text[next + 1] == u'\n') ? 2 : 1;
    const QStringView line = text.sliced(pos, end - pos);
    return line.startsWith(marker) && trimmed(line.sliced(marker.size())).isEmpty();
}

} // namespace

const YamlValue *YamlValue::find(QStringView key, Qt::CaseSensitivity sensitivity) const
{
    if (kind != Kind::Map) return nullptr;
    for (const auto &entry : map)
        if (QStringView(entry.first).compare(key, sensitivity) == 0) return &entry.second;
    return nullptr;
}

std::optional<QString> FrontMatter::scalar(QStringView key) const
{
    const YamlValue *value = data.find(key, Qt::CaseInsensitive);
    if (!value || !value->isScalar()) return std::nullopt;
    return value->scalar;
}

YamlValue parseYaml(QStringView yaml, std::vector<FrontMatterIssue> *issues, int baseOffset)
{
    return Yaml(yaml, issues, baseOffset).parse();
}

FrontMatter parseFrontMatter(QStringView text)
{
    FrontMatter result;
    const int size = int(text.size());
    int pos = (size > 0 && text[0] == QChar(0xfeff)) ? 1 : 0;
    int end = 0, next = 0;
    if (!fenceLine(text, pos, u"---", end, next) || end == size) return result;
    const int contentStart = next;
    for (int line = next; line < size;) {
        int lineEnd = 0, lineNext = 0;
        if (fenceLine(text, line, u"---", lineEnd, lineNext) || fenceLine(text, line, u"...", lineEnd, lineNext)) {
            result.present = true;
            result.start = pos;
            result.end = lineEnd;
            result.contentStart = contentStart;
            result.contentEnd = line;
            result.bodyStart = lineNext;
            result.source = text.sliced(contentStart, line - contentStart).toString();
            result.data = parseYaml(text.sliced(contentStart, line - contentStart), &result.issues, contentStart);
            if (result.data.kind == YamlValue::Kind::Null) result.data.kind = YamlValue::Kind::Map;
            return result;
        }
        if (lineNext == line) break;
        line = lineNext;
    }
    return result;
}

} // namespace md
