// The raw HTML policy. Markdown may carry HTML; parch understands a small
// semantic subset of it and shows the rest as the text it is. Nothing a
// document contains can run: scripts, event attributes, styles, frames and
// script URLs never survive this file.
//
// Supported elements
//   text level   a b i em strong u s del ins mark code kbd sub sup, and the
//                empty br and img
//   blocks       p blockquote pre h1-h6 ul ol li dl dt dd hr
//                table caption thead tbody tfoot tr th td
// Supported attributes
//   a: href title    img: src alt title width height    ol: start
//   td, th: colspan rowspan align
// Every other attribute is dropped. A link or image whose URL is a script
// or a non-image data URL loses that URL.
#include "mdhtml.h"
#include "mdparser_p.h"

#include <algorithm>

namespace md::detail {
namespace {

struct TagEntry {
    const char *name;
    bool isVoid;
    bool block;
};

const TagEntry supportedTags[] = {
    {"a", false, false},      {"b", false, false},      {"i", false, false},     {"em", false, false},
    {"strong", false, false}, {"u", false, false},      {"s", false, false},     {"del", false, false},
    {"ins", false, false},    {"mark", false, false},   {"code", false, false},  {"kbd", false, false},
    {"sub", false, false},    {"sup", false, false},    {"br", true, false},     {"img", true, false},
    {"p", false, true},       {"blockquote", false, true}, {"pre", false, true}, {"h1", false, true},
    {"h2", false, true},      {"h3", false, true},      {"h4", false, true},     {"h5", false, true},
    {"h6", false, true},      {"ul", false, true},      {"ol", false, true},     {"li", false, true},
    {"dl", false, true},      {"dt", false, true},      {"dd", false, true},     {"hr", true, true},
    {"table", false, true},   {"caption", false, true}, {"thead", false, true},  {"tbody", false, true},
    {"tfoot", false, true},   {"tr", false, true},      {"th", false, true},     {"td", false, true},
};

struct AttributeEntry {
    const char *tag;
    const char *attribute;
};

const AttributeEntry allowedAttributes[] = {
    {"a", "href"},       {"a", "title"},      {"img", "src"},      {"img", "alt"},       {"img", "title"},
    {"img", "width"},    {"img", "height"},   {"ol", "start"},     {"td", "colspan"},    {"td", "rowspan"},
    {"td", "align"},     {"th", "colspan"},   {"th", "rowspan"},   {"th", "align"},
};

bool attributeAllowed(QStringView tag, QStringView attribute)
{
    for (const AttributeEntry &entry : allowedAttributes)
        if (tag == QLatin1StringView(entry.tag) && attribute == QLatin1StringView(entry.attribute)) return true;
    return false;
}

// Character references in an attribute value or in text.
QString decodeEntities(QStringView text)
{
    if (!text.contains(u'&')) return text.toString();
    QString out;
    out.reserve(text.size());
    const QChar *s = text.data();
    const int size = int(text.size());
    for (int i = 0; i < size;) {
        if (s[i] == u'&') {
            QString value;
            if (const int n = scanEntity(s, size, i, &value)) {
                out.append(value);
                i += n;
                continue;
            }
        }
        out.append(s[i++]);
    }
    return out;
}

bool isHtmlSpace(char16_t c)
{ return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f'; }

// Builds the nodes of an HTML fragment. Open elements wait on a stack until
// their closing tag, the end of the fragment, or a tag that implies their end.
class Fragment {
public:
    Fragment(ParseState &state, const QString &raw, const std::vector<Piece> &pieces, int file, Range blockRange,
             int offset, std::vector<Node> &out)
        : st(state), raw(raw), s(raw.constData()), len(int(raw.size())), file(file), blockRange(blockRange),
          offset(offset), pieces(pieces), out(out)
    {
    }

    void run(int skip)
    {
        int textStart = qBound(0, skip, len);
        int i = textStart;
        // A comment, instruction, declaration or CDATA section once found
        // unclosed stays unclosed for every later start, since its search
        // ran to the end: it is made once per kind, not once per start.
        int unclosedFrom[4] = {-1, -1, -1, -1};
        while (i < len) {
            if (s[i] != u'<') {
                ++i;
                continue;
            }
            const int construct = unclosedKind(i);
            if (construct >= 0 && unclosedFrom[construct] >= 0 && i >= unclosedFrom[construct]) {
                ++i;
                continue;
            }
            HtmlKind kind = HtmlKind::None;
            const int n = scanHtml(s, len, i, &kind);
            if (!n) {
                if (construct >= 0) unclosedFrom[construct] = i;
                ++i;
                continue;
            }
            text(textStart, i);
            token(i, i + n, kind);
            i += n;
            textStart = i;
        }
        text(textStart, len);
        while (!stack.empty()) closeTop({});
    }

private:
    // Which of the constructs that run to an end marker starts at `<` here:
    // 0 a comment, 1 an instruction, 2 a declaration, 3 a CDATA section; -1
    // for anything else.
    int unclosedKind(int at) const
    {
        const auto has = [&](QLatin1StringView text) {
            return at + text.size() <= len && QStringView(s + at, text.size()) == text;
        };
        if (has(QLatin1StringView("<!--"))) return 0;
        if (has(QLatin1StringView("<?"))) return 1;
        if (has(QLatin1StringView("<![CDATA["))) return 3;
        if (at + 2 < len && s[at + 1] == u'!' && isAsciiAlpha(s[at + 2].unicode())) return 2;
        return -1;
    }

    Range range(int start, int end) const
    { return offset >= 0 ? Range{offset + start, offset + end} : blockRange; }

    int sourceOffset(int position)
    {
        // Attributes are visited in source order. Walk each retained line
        // once, ignoring indentation that was synthesized from a tab.
        while (pieceIndex < pieces.size()) {
            const Piece &piece = pieces[pieceIndex];
            const int start = pieceOffset + piece.padding;
            const int end = start + piece.len;
            if (position < end) return position < start ? -1 : piece.src + position - start;
            pieceOffset = end + 1;
            ++pieceIndex;
        }
        return -1;
    }

    Range valueRange(Range value)
    {
        if (value.isEmpty()) return {};
        if (offset >= 0) return range(value.start, value.end);
        const int start = sourceOffset(value.start);
        const int end = sourceOffset(value.end - 1) + 1;
        if (start < 0 || end - start != value.length()) return {};
        const QStringView source(st.files[file].text);
        if (source.sliced(start, end - start) != QStringView(raw).sliced(value.start, value.length())) return {};
        return {start, end};
    }

    Node make(NodeType type, int start, int end) const
    {
        Node node;
        node.type = type;
        node.file = file;
        node.range = range(start, end);
        node.open = {node.range.start, node.range.start};
        node.close = {node.range.end, node.range.end};
        node.generated = offset < 0;
        return node;
    }

    std::vector<Node> &sink() { return stack.empty() ? out : stack.back().children; }

    // Literal text, with character references as nodes of their own.
    void text(int start, int end)
    {
        int from = start;
        for (int i = start; i < end;) {
            if (s[i] == u'&') {
                QString value;
                if (const int n = scanEntity(s, end, i, &value)) {
                    plain(from, i);
                    Node entity = make(NodeType::Entity, i, i + n);
                    entity.text = value;
                    sink().push_back(std::move(entity));
                    i += n;
                    from = i;
                    continue;
                }
            }
            ++i;
        }
        plain(from, end);
    }

    void plain(int start, int end)
    {
        if (end <= start) return;
        Node node = make(NodeType::Text, start, end);
        node.text = raw.mid(start, end - start);
        sink().push_back(std::move(node));
    }

    void unsupported(int start, int end, const QString &message)
    {
        st.diagnose(file, range(start, end), Severity::Info, DiagnosticCode::UnsupportedHtml, message);
        plain(start, end);
    }

    void closeTop(Range closeTag)
    {
        Node node = std::move(stack.back());
        stack.pop_back();
        if (!closeTag.isEmpty() || offset >= 0) {
            if (!closeTag.isEmpty()) {
                node.close = closeTag;
                node.range.end = closeTag.end;
            } else {
                // Closed by what follows it: it ends where its content does.
                const int end = node.children.empty() ? node.open.end : node.children.back().range.end;
                node.range.end = end;
                node.close = {end, end};
            }
        }
        sink().push_back(std::move(node));
    }

    // Closes the nearest open element among `names`, and all inside it,
    // unless one of `bounds` is met first.
    void closeImplied(std::initializer_list<QLatin1StringView> names, std::initializer_list<QLatin1StringView> bounds)
    {
        for (int i = int(stack.size()) - 1; i >= 0; --i) {
            const QString &label = stack[i].label;
            for (const QLatin1StringView bound : bounds)
                if (label == bound) return;
            for (const QLatin1StringView name : names) {
                if (label != name) continue;
                while (int(stack.size()) > i) closeTop({});
                return;
            }
        }
    }

    void token(int start, int end, HtmlKind kind)
    {
        if (kind == HtmlKind::Comment) {
            Node node = make(NodeType::HtmlComment, start, end);
            node.text = raw.mid(start, end - start);
            sink().push_back(std::move(node));
            return;
        }
        if (kind != HtmlKind::OpenTag && kind != HtmlKind::CloseTag) {
            unsupported(start, end, QStringLiteral("This markup is not supported and is shown as text"));
            return;
        }
        HtmlTag tag = parseHtmlTag(QStringView(raw).sliced(start, end - start));
        const HtmlTagInfo info = htmlTagInfo(tag.name);
        if (!info.supported) {
            unsupported(start, end, QStringLiteral("<%1> is not supported and is shown as text").arg(tag.name));
            return;
        }
        if (tag.closing) {
            int match = int(stack.size()) - 1;
            while (match >= 0 && stack[match].label != tag.name) --match;
            if (match < 0 || info.isVoid) {
                unsupported(start, end, QStringLiteral("</%1> closes nothing and is shown as text").arg(tag.name));
                return;
            }
            while (int(stack.size()) > match + 1) closeTop({});
            closeTop(offset >= 0 ? range(start, end) : Range{});
            return;
        }

        if (tag.droppedAttributes)
            st.diagnose(file, range(start, end), Severity::Info, DiagnosticCode::UnsupportedHtml,
                        QStringLiteral("Attributes of <%1> that are not supported are left out").arg(tag.name));

        using L = QLatin1StringView;
        const QString &name = tag.name;
        if (name == L("li")) closeImplied({L("li")}, {L("ul"), L("ol")});
        else if (name == L("dt") || name == L("dd")) closeImplied({L("dt"), L("dd")}, {L("dl")});
        else if (name == L("tr")) closeImplied({L("tr")}, {L("table")});
        else if (name == L("td") || name == L("th")) closeImplied({L("td"), L("th")}, {L("tr"), L("table")});
        else if (name == L("thead") || name == L("tbody") || name == L("tfoot"))
            closeImplied({L("thead"), L("tbody"), L("tfoot")}, {L("table")});
        if (info.block)
            closeImplied({L("p")}, {L("li"), L("td"), L("th"), L("dd"), L("blockquote"), L("table")});

        Node node = make(NodeType::HtmlElement, start, end);
        node.label = tag.name;
        for (Attribute &attribute : tag.attributes) {
            const Range value = attribute.valueRange;
            if (!value.isEmpty()) attribute.valueRange = valueRange({start + value.start, start + value.end});
        }
        node.attributes = std::move(tag.attributes);
        node.open = node.range;
        node.close = {node.range.end, node.range.end};
        if (info.isVoid || tag.selfClosing) {
            sink().push_back(std::move(node));
            return;
        }
        if (int(stack.size()) >= MaxHtmlDepth) {
            unsupported(start, end, QStringLiteral("Markup nested this deeply is shown as text"));
            return;
        }
        stack.push_back(std::move(node));
    }

    ParseState &st;
    const QString &raw;
    const QChar *s;
    int len;
    int file;
    Range blockRange;
    int offset;
    const std::vector<Piece> &pieces;
    size_t pieceIndex = 0;
    int pieceOffset = 0;
    std::vector<Node> &out;
    std::vector<Node> stack;
};

} // namespace

HtmlTagInfo htmlTagInfo(QStringView lowerName)
{
    for (const TagEntry &entry : supportedTags)
        if (lowerName == QLatin1StringView(entry.name)) return {true, entry.isVoid, entry.block};
    return {};
}

bool isSafeUrl(QStringView url, bool image)
{
    // Browsers ignore control characters and spaces inside a scheme.
    QString compact;
    compact.reserve(qMin<qsizetype>(url.size(), 32));
    for (const QChar c : url) {
        if (c.unicode() <= 0x20) continue;
        compact.append(c.toLower());
        if (compact.size() >= 32) break;
    }
    const qsizetype colon = compact.indexOf(u':');
    if (colon <= 0) return true;
    const QStringView scheme = QStringView(compact).first(colon);
    for (const QChar c : scheme)
        if (!isAsciiAlnum(c.unicode()) && c != u'+' && c != u'-' && c != u'.') return true; // not a scheme
    if (scheme == u"javascript" || scheme == u"vbscript" || scheme == u"livescript") return false;
    if (scheme == u"data") {
        if (!image) return false;
        const QStringView rest = QStringView(compact).sliced(colon + 1);
        return rest.startsWith(u"image/png") || rest.startsWith(u"image/gif") || rest.startsWith(u"image/jpeg")
            || rest.startsWith(u"image/webp");
    }
    return true;
}

HtmlTag parseHtmlTag(QStringView raw)
{
    HtmlTag tag;
    const QChar *s = raw.data();
    const int len = int(raw.size());
    int i = 1;
    if (i < len && s[i] == u'/') {
        tag.closing = true;
        ++i;
    }
    const int nameStart = i;
    while (i < len && (isAsciiAlnum(s[i].unicode()) || s[i] == u'-')) ++i;
    tag.name = raw.sliced(nameStart, i - nameStart).toString().toLower();
    tag.selfClosing = len >= 2 && s[len - 2] == u'/';
    if (tag.closing) return tag;
    QSet<QString> seen;
    for (;;) {
        while (i < len && isHtmlSpace(s[i].unicode())) ++i;
        if (i >= len || s[i] == u'>' || s[i] == u'/') break;
        const int attributeStart = i;
        while (i < len && !isHtmlSpace(s[i].unicode()) && s[i] != u'=' && s[i] != u'>' && s[i] != u'/') ++i;
        const QString name = raw.sliced(attributeStart, i - attributeStart).toString().toLower();
        int v = i;
        while (v < len && isHtmlSpace(s[v].unicode())) ++v;
        Range valueRange;
        QChar quote;
        if (v < len && s[v] == u'=') {
            ++v;
            while (v < len && isHtmlSpace(s[v].unicode())) ++v;
            if (v < len && (s[v] == u'"' || s[v] == u'\'')) {
                quote = s[v];
                int e = v + 1;
                while (e < len && s[e] != quote) ++e;
                valueRange = {v + 1, e};
                i = qMin(len, e + 1);
            } else {
                int e = v;
                while (e < len && !isHtmlSpace(s[e].unicode()) && s[e] != u'>') ++e;
                valueRange = {v, e};
                i = e;
            }
        }
        if (name.isEmpty()) {
            if (i == attributeStart) ++i;
            continue;
        }
        if (!attributeAllowed(tag.name, name)) {
            tag.droppedAttributes = true;
            continue;
        }
        if (seen.contains(name)) continue;
        seen.insert(name);
        QString value = decodeEntities(raw.sliced(valueRange.start, valueRange.length()));
        if ((name == u"href" || name == u"src") && !isSafeUrl(value, name == u"src")) tag.droppedAttributes = true;
        else tag.attributes.push_back({name, std::move(value), valueRange, quote});
    }
    return tag;
}

void parseHtmlFragment(ParseState &state, const QString &raw, const std::vector<Piece> &pieces,
                       int file, Range blockRange, int offset, int skip,
                       std::vector<Node> &out)
{
    Fragment(state, raw, pieces, file, blockRange, offset, out).run(skip);
}

} // namespace md::detail

namespace md {

QString htmlAttributeValue(QStringView value, QChar quote)
{
    const bool addQuotes = quote.isNull() && std::any_of(value.begin(), value.end(), [](QChar c) {
        return detail::isHtmlSpace(c.unicode()) || QStringView(u"\"'\x60=<>").contains(c);
    });
    if (addQuotes) quote = u'"';
    QString written;
    written.reserve(value.size() + (addQuotes ? 2 : 0));
    if (addQuotes) written.append(quote);
    for (const QChar c : value) {
        if (c == u'&') written += QStringLiteral("&amp;");
        else if (c == u'<') written += QStringLiteral("&lt;");
        else if (c == u'"' && c == quote) written += QStringLiteral("&quot;");
        else if (c == u'\'' && c == quote) written += QStringLiteral("&#39;");
        else written.append(c);
    }
    if (addQuotes) written.append(quote);
    return written;
}

HtmlPolicy htmlPolicy()
{
    HtmlPolicy policy;
    for (const detail::TagEntry &entry : detail::supportedTags)
        (entry.block ? policy.blockElements : policy.textElements).append(QLatin1StringView(entry.name));
    for (const detail::AttributeEntry &entry : detail::allowedAttributes)
        policy.attributes[QLatin1StringView(entry.tag)].append(QLatin1StringView(entry.attribute));
    return policy;
}

} // namespace md
