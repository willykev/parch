#include "mdhtml.h"

#include "mdparser_p.h"

namespace md {
namespace {

void appendEscaped(QString &out, QStringView text)
{
    qsizetype from = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t c = text[i].unicode();
        const char *entity = nullptr;
        switch (c) {
        case u'&': entity = "&amp;"; break;
        case u'<': entity = "&lt;"; break;
        case u'>': entity = "&gt;"; break;
        case u'"': entity = "&quot;"; break;
        default: continue;
        }
        out.append(text.sliced(from, i - from));
        out.append(QLatin1StringView(entity));
        from = i + 1;
    }
    out.append(text.sliced(from));
}

class Writer {
public:
    Writer(const Document &document, const HtmlOptions &options)
        : doc(document), opt(options), safe(options.rawHtml == HtmlOptions::RawHtml::Safe),
          referencesSeen(document.footnotes.size(), 0)
    {
        // In a note style, a citation of the running text is a note.
        for (const Footnote &note : document.footnotes)
            if (note.content && note.content->type == NodeType::Citation) citationNotes.insert(note.content, note.number);
    }

    QString run()
    {
        block(doc.root, false);
        if (opt.endnotes) {
            footnotes();
            bibliography();
        }
        cr();
        return out;
    }

private:
    void cr()
    {
        if (!out.isEmpty() && out.back() != u'\n') out.append(u'\n');
    }
    void lit(QLatin1StringView text) { out.append(text); }
    void esc(QStringView text) { appendEscaped(out, text); }
    void attribute(QLatin1StringView name, QStringView value)
    {
        out.append(u' ');
        out.append(name);
        lit(QLatin1StringView("=\""));
        esc(value);
        out.append(u'"');
    }
    QString url(const Node &node, const QString &written, bool image) const
    {
        const QString dest = opt.destination ? opt.destination(node, written) : written;
        if (safe && !detail::isSafeUrl(dest, image)) return QString();
        return encodeUrl(dest);
    }

    void blocks(const std::vector<Node> &nodes, bool tight)
    {
        for (const Node &node : nodes) block(node, tight);
    }
    void inlines(const std::vector<Node> &nodes)
    {
        for (const Node &node : nodes) inl(node);
    }

    void block(const Node &node, bool tight);
    void inl(const Node &node);
    void table(const Node &node);
    void htmlBlock(const Node &node);
    void element(const Node &node);
    void citation(const Node &node);
    // The citation's own text, wherever it stands.
    void citationText(const Node &node);
    void noteReference(const Node &node);
    void noteMark(int number);
    void footnotes();
    void bibliography();

    const Document &doc;
    const HtmlOptions &opt;
    const bool safe;
    QString out;
    std::vector<int> referencesSeen; // per note, how many references are written
    QHash<const Node *, int> citationNotes; // the citations that are notes, and their numbers
};

void Writer::block(const Node &node, bool tight)
{
    switch (node.type) {
    case NodeType::Document:
        blocks(node.children, false);
        break;
    case NodeType::Paragraph:
        if (tight) {
            inlines(node.children);
        } else {
            cr();
            lit(QLatin1StringView("<p>"));
            inlines(node.children);
            lit(QLatin1StringView("</p>"));
            cr();
        }
        break;
    case NodeType::Heading: {
        cr();
        const QString tag = u'h' + QString::number(node.level);
        out.append(u'<');
        out.append(tag);
        if (opt.headingIds && !node.dest.isEmpty()) attribute(QLatin1StringView("id"), node.dest);
        out.append(u'>');
        inlines(node.children);
        lit(QLatin1StringView("</"));
        out.append(tag);
        out.append(u'>');
        cr();
        break;
    }
    case NodeType::ThematicBreak:
        cr();
        lit(QLatin1StringView("<hr />"));
        cr();
        break;
    case NodeType::BlockQuote:
        cr();
        lit(QLatin1StringView("<blockquote>"));
        cr();
        blocks(node.children, false);
        cr();
        lit(QLatin1StringView("</blockquote>"));
        cr();
        break;
    case NodeType::List:
        cr();
        if (node.ordered) {
            lit(QLatin1StringView("<ol"));
            if (node.number != 1) attribute(QLatin1StringView("start"), QString::number(node.number));
            out.append(u'>');
        } else {
            lit(QLatin1StringView("<ul>"));
        }
        cr();
        for (const Node &item : node.children) {
            if (item.type != NodeType::ListItem) continue;
            lit(QLatin1StringView("<li>"));
            blocks(item.children, node.tight);
            lit(QLatin1StringView("</li>"));
            cr();
        }
        cr();
        lit(node.ordered ? QLatin1StringView("</ol>") : QLatin1StringView("</ul>"));
        cr();
        break;
    case NodeType::CodeBlock: {
        cr();
        lit(QLatin1StringView("<pre><code"));
        const QStringView info = QStringView(node.label).trimmed();
        qsizetype word = 0;
        while (word < info.size() && !info[word].isSpace()) ++word;
        if (word > 0) {
            QString language = info.first(word).toString();
            if (!language.startsWith(QLatin1StringView("language-")) && !language.startsWith(QLatin1StringView("lang-")))
                language.prepend(QLatin1StringView("language-"));
            attribute(QLatin1StringView("class"), language);
        }
        out.append(u'>');
        esc(node.text);
        lit(QLatin1StringView("</code></pre>"));
        cr();
        break;
    }
    case NodeType::MermaidBlock:
        cr();
        if (const QString drawn = opt.picture ? opt.picture(node) : QString(); !drawn.isEmpty()) {
            out.append(drawn);
            cr();
            break;
        }
        lit(QLatin1StringView("<pre class=\"mermaid\">"));
        esc(node.text);
        lit(QLatin1StringView("</pre>"));
        cr();
        break;
    case NodeType::MathBlock:
        cr();
        if (const QString drawn = opt.picture ? opt.picture(node) : QString(); !drawn.isEmpty()) {
            out.append(drawn);
            cr();
            break;
        }
        lit(QLatin1StringView("<div class=\"math display\">"));
        esc(node.text);
        lit(QLatin1StringView("</div>"));
        cr();
        break;
    case NodeType::HtmlBlock:
        cr();
        if (safe) htmlBlock(node);
        else out.append(node.text);
        cr();
        break;
    case NodeType::Table:
        table(node);
        break;
    case NodeType::DefinitionList:
        cr();
        lit(QLatin1StringView("<dl>"));
        cr();
        for (const Node &child : node.children) {
            if (child.type == NodeType::DefinitionTerm) {
                lit(QLatin1StringView("<dt>"));
                inlines(child.children);
                lit(QLatin1StringView("</dt>"));
                cr();
            } else if (child.type == NodeType::DefinitionDescription) {
                lit(QLatin1StringView("<dd>"));
                blocks(child.children, child.tight);
                lit(QLatin1StringView("</dd>"));
                cr();
            }
        }
        lit(QLatin1StringView("</dl>"));
        cr();
        break;
    case NodeType::PageBreak:
        cr();
        lit(QLatin1StringView("<div class=\"page-break\"></div>"));
        cr();
        break;
    case NodeType::TableOfContents:
        if (node.children.empty()) break;
        cr();
        lit(QLatin1StringView("<nav class=\"toc\">"));
        blocks(node.children, false);
        lit(QLatin1StringView("</nav>"));
        cr();
        break;
    case NodeType::Include:
        if (node.resolved) {
            blocks(node.children, false);
        } else {
            cr();
            lit(QLatin1StringView("<p>"));
            esc(doc.source(node));
            lit(QLatin1StringView("</p>"));
            cr();
        }
        break;
    // Not part of the reading text: metadata, definitions and comments.
    case NodeType::FrontMatter:
    case NodeType::LinkDefinition:
    case NodeType::FootnoteDefinition:
    case NodeType::CitationDefinition:
    case NodeType::CommentLine:
        break;
    default:
        blocks(node.children, tight);
        break;
    }
}

void Writer::table(const Node &node)
{
    cr();
    lit(QLatin1StringView("<table>"));
    cr();
    bool body = false;
    for (const Node &row : node.children) {
        if (row.type != NodeType::TableRow) continue;
        if (row.header) {
            lit(QLatin1StringView("<thead>"));
            cr();
        } else if (!body) {
            lit(QLatin1StringView("<tbody>"));
            cr();
            body = true;
        }
        lit(QLatin1StringView("<tr>"));
        cr();
        for (const Node &cell : row.children) {
            if (cell.type != NodeType::TableCell) continue;
            lit(row.header ? QLatin1StringView("<th") : QLatin1StringView("<td"));
            if (cell.span > 1) attribute(QLatin1StringView("colspan"), QString::number(cell.span));
            switch (cell.align) {
            case Align::Left: attribute(QLatin1StringView("align"), u"left"); break;
            case Align::Center: attribute(QLatin1StringView("align"), u"center"); break;
            case Align::Right: attribute(QLatin1StringView("align"), u"right"); break;
            case Align::None: break;
            }
            out.append(u'>');
            inlines(cell.children);
            lit(row.header ? QLatin1StringView("</th>") : QLatin1StringView("</td>"));
            cr();
        }
        lit(QLatin1StringView("</tr>"));
        cr();
        if (row.header) {
            lit(QLatin1StringView("</thead>"));
            cr();
        }
    }
    if (body) {
        lit(QLatin1StringView("</tbody>"));
        cr();
    }
    lit(QLatin1StringView("</table>"));
    cr();
}

// A supported element, written afresh from its name and allowed attributes.
void Writer::element(const Node &node)
{
    if (node.label == u"img" && opt.picture) {
        const QString drawn = opt.picture(node);
        if (!drawn.isEmpty()) {
            out.append(drawn);
            return;
        }
    }
    const detail::HtmlTagInfo info = detail::htmlTagInfo(node.label);
    out.append(u'<');
    out.append(node.label);
    const bool image = node.label == u"img";
    const QLatin1StringView leads(image ? "src" : "href");
    for (const Attribute &a : node.attributes) {
        out.append(u' ');
        out.append(a.name);
        lit(QLatin1StringView("=\""));
        if (opt.destination && (image || node.label == u"a") && a.name == leads) {
            const QString dest = opt.destination(node, a.value);
            if (!safe || detail::isSafeUrl(dest, image)) esc(dest);
        } else {
            esc(a.value);
        }
        out.append(u'"');
    }
    if (info.isVoid) {
        lit(QLatin1StringView(" />"));
        return;
    }
    out.append(u'>');
    inlines(node.children);
    lit(QLatin1StringView("</"));
    out.append(node.label);
    out.append(u'>');
}

// A raw HTML block under the policy: its supported elements as markup, the
// rest as text. Text that stands outside any block element gets a paragraph.
void Writer::htmlBlock(const Node &node)
{
    bool visible = false;
    bool blockLevel = false;
    for (const Node &child : node.children) {
        if (child.type == NodeType::HtmlElement) {
            visible = true;
            if (detail::htmlTagInfo(child.label).block) blockLevel = true;
        } else if (child.isTextual() && !child.text.trimmed().isEmpty()) {
            visible = true;
        }
    }
    if (!visible) return;
    if (!blockLevel) lit(QLatin1StringView("<p>"));
    inlines(node.children);
    if (!blockLevel) {
        while (out.endsWith(u'\n')) out.chop(1);
        lit(QLatin1StringView("</p>"));
    }
}

void Writer::noteReference(const Node &node)
{
    if (node.number <= 0 || node.number > int(doc.footnotes.size())) {
        esc(doc.source(node));
        return;
    }
    noteMark(node.number);
}

void Writer::noteMark(int noteNumber)
{
    const int seen = ++referencesSeen[noteNumber - 1];
    const QString number = QString::number(noteNumber);
    lit(QLatin1StringView("<sup class=\"footnote-ref\"><a href=\"#fn-"));
    out.append(number);
    lit(QLatin1StringView("\" id=\"fnref-"));
    out.append(number);
    if (seen > 1) {
        out.append(u'-');
        out.append(QString::number(seen));
    }
    lit(QLatin1StringView("\">"));
    out.append(number);
    lit(QLatin1StringView("</a></sup>"));
}

void Writer::citation(const Node &node)
{
    const auto note = citationNotes.constFind(&node);
    if (note != citationNotes.constEnd() && *note <= int(doc.footnotes.size())) {
        noteMark(*note);
        return;
    }
    citationText(node);
}

void Writer::citationText(const Node &node)
{
    if (opt.citations && opt.citations->citation) {
        // Nothing for a citation the style did not format: the engine's own then.
        const QString formatted = opt.citations->citation(node);
        if (!formatted.isNull()) {
            out.append(formatted);
            return;
        }
    }
    auto one = [&](const Node &item) {
        if (item.number <= 0) {
            esc(doc.source(item));
            return;
        }
        const QString number = QString::number(item.number);
        // A prefix, as in `[see @key]`, is written before the number.
        if (!item.title.isEmpty()) {
            esc(item.title);
            out.append(u' ');
        }
        lit(QLatin1StringView("<a href=\"#cite-"));
        out.append(number);
        lit(QLatin1StringView("\">"));
        out.append(number);
        lit(QLatin1StringView("</a>"));
        if (!item.text.isEmpty()) {
            lit(QLatin1StringView(", "));
            esc(item.text);
        }
    };
    if (node.children.empty() && node.number <= 0) {
        esc(doc.source(node));
        return;
    }
    lit(QLatin1StringView("<span class=\"citation\">["));
    if (node.children.empty()) {
        one(node);
    } else {
        bool first = true;
        for (const Node &item : node.children) {
            if (!first) lit(QLatin1StringView("; "));
            first = false;
            one(item);
        }
    }
    lit(QLatin1StringView("]</span>"));
}

void Writer::inl(const Node &node)
{
    auto wrap = [&](QLatin1StringView open, QLatin1StringView close) {
        lit(open);
        inlines(node.children);
        lit(close);
    };
    switch (node.type) {
    case NodeType::Text:
    case NodeType::Escape:
    case NodeType::Entity:
        esc(node.text);
        break;
    case NodeType::SoftBreak:
        out.append(u'\n');
        break;
    case NodeType::HardBreak:
        lit(QLatin1StringView("<br />\n"));
        break;
    case NodeType::Code:
        lit(QLatin1StringView("<code>"));
        esc(node.text);
        lit(QLatin1StringView("</code>"));
        break;
    case NodeType::Emphasis: wrap(QLatin1StringView("<em>"), QLatin1StringView("</em>")); break;
    case NodeType::Strong: wrap(QLatin1StringView("<strong>"), QLatin1StringView("</strong>")); break;
    case NodeType::Strikethrough: wrap(QLatin1StringView("<del>"), QLatin1StringView("</del>")); break;
    case NodeType::Highlight: wrap(QLatin1StringView("<mark>"), QLatin1StringView("</mark>")); break;
    case NodeType::Superscript: wrap(QLatin1StringView("<sup>"), QLatin1StringView("</sup>")); break;
    case NodeType::Subscript: wrap(QLatin1StringView("<sub>"), QLatin1StringView("</sub>")); break;
    case NodeType::Link:
    case NodeType::Autolink:
        lit(QLatin1StringView("<a"));
        attribute(QLatin1StringView("href"), url(node, node.dest, false));
        if (!node.title.isEmpty()) attribute(QLatin1StringView("title"), node.title);
        out.append(u'>');
        inlines(node.children);
        lit(QLatin1StringView("</a>"));
        break;
    case NodeType::Image:
        if (const QString drawn = opt.picture ? opt.picture(node) : QString(); !drawn.isEmpty()) {
            out.append(drawn);
            break;
        }
        lit(QLatin1StringView("<img"));
        attribute(QLatin1StringView("src"), url(node, node.dest, true));
        attribute(QLatin1StringView("alt"), plainText(node));
        if (!node.title.isEmpty()) attribute(QLatin1StringView("title"), node.title);
        lit(QLatin1StringView(" />"));
        break;
    case NodeType::HtmlInline:
        if (safe) esc(node.text);
        else out.append(node.text);
        break;
    case NodeType::HtmlComment:
        if (!safe) out.append(node.text);
        break;
    case NodeType::HtmlElement:
        if (safe) {
            element(node);
        } else {
            out.append(doc.source(node.file, node.open));
            inlines(node.children);
            out.append(doc.source(node.file, node.close));
        }
        break;
    case NodeType::MathInline:
        if (const QString drawn = opt.picture ? opt.picture(node) : QString(); !drawn.isEmpty()) {
            out.append(drawn);
            break;
        }
        lit(node.display ? QLatin1StringView("<span class=\"math display\">") : QLatin1StringView("<span class=\"math\">"));
        esc(node.text);
        lit(QLatin1StringView("</span>"));
        break;
    case NodeType::FootnoteReference:
    case NodeType::InlineNote:
        noteReference(node);
        break;
    case NodeType::Citation:
        citation(node);
        break;
    case NodeType::Variable:
        if (node.resolved) esc(node.text);
        else esc(doc.source(node));
        break;
    case NodeType::TaskMarker:
        lit(node.checked ? QLatin1StringView("<input checked=\"\" disabled=\"\" type=\"checkbox\" />")
                         : QLatin1StringView("<input disabled=\"\" type=\"checkbox\" />"));
        break;
    case NodeType::HeadingLabel:
    case NodeType::CommentLine:
        break;
    default:
        // A block inside inline content: the blocks of a raw HTML fragment.
        block(node, false);
        break;
    }
}

void Writer::footnotes()
{
    if (doc.footnotes.empty()) return;
    cr();
    lit(QLatin1StringView("<section class=\"footnotes\">"));
    cr();
    lit(QLatin1StringView("<ol>"));
    cr();
    for (const Footnote &note : doc.footnotes) {
        const QString number = QString::number(note.number);
        lit(QLatin1StringView("<li id=\"fn-"));
        out.append(number);
        lit(QLatin1StringView("\">"));
        cr();
        if (note.content && note.content->type == NodeType::InlineNote) {
            lit(QLatin1StringView("<p>"));
            inlines(note.content->children);
            lit(QLatin1StringView("</p>"));
            cr();
        } else if (note.content && note.content->type == NodeType::Citation) {
            lit(QLatin1StringView("<p>"));
            citationText(*note.content);
            lit(QLatin1StringView("</p>"));
            cr();
        } else if (note.content) {
            blocks(note.content->children, false);
            cr();
        }
        // The way back to each place the note is referred to, at the end of
        // its last paragraph.
        QString back;
        for (int i = 1; i <= note.references; ++i) {
            back.append(QLatin1StringView(" <a href=\"#fnref-"));
            back.append(number);
            if (i > 1) back.append(u'-' + QString::number(i));
            back.append(QLatin1StringView("\" class=\"footnote-backref\">"));
            back.append(QChar(0x21a9));
            if (i > 1) back.append(QString::number(i));
            back.append(QLatin1StringView("</a>"));
        }
        if (out.endsWith(QLatin1StringView("</p>\n"))) {
            out.insert(out.size() - 5, back);
        } else {
            out.append(QStringView(back).sliced(1));
            cr();
        }
        lit(QLatin1StringView("</li>"));
        cr();
    }
    lit(QLatin1StringView("</ol>"));
    cr();
    lit(QLatin1StringView("</section>"));
    cr();
}

void Writer::bibliography()
{
    if (opt.citations) {
        if (!opt.citations->bibliography.isEmpty()) {
            cr();
            out.append(opt.citations->bibliography);
            cr();
        }
        return;
    }
    if (doc.bibliography.empty()) return;
    cr();
    lit(QLatin1StringView("<section class=\"bibliography\">"));
    cr();
    lit(QLatin1StringView("<ol>"));
    cr();
    for (const BibliographyEntry &entry : doc.bibliography) {
        lit(QLatin1StringView("<li id=\"cite-"));
        out.append(QString::number(entry.number));
        lit(QLatin1StringView("\">"));
        if (entry.definition) {
            // One paragraph is the usual entry, and is written without its tags.
            const std::vector<Node> &content = entry.definition->children;
            blocks(content, content.size() == 1);
        } else {
            esc(entry.key);
        }
        lit(QLatin1StringView("</li>"));
        cr();
    }
    lit(QLatin1StringView("</ol>"));
    cr();
    lit(QLatin1StringView("</section>"));
    cr();
}

} // namespace

HtmlOptions HtmlOptions::commonMark()
{
    HtmlOptions options;
    options.rawHtml = RawHtml::Passthrough;
    options.headingIds = false;
    return options;
}

QString toHtml(const Document &document, const HtmlOptions &options)
{
    return Writer(document, options).run();
}

QString escapeHtml(QStringView text)
{
    QString out;
    out.reserve(text.size());
    appendEscaped(out, text);
    return out;
}

QString encodeUrl(QStringView url)
{
    static const char hex[] = "0123456789ABCDEF";
    const QByteArray bytes = url.toUtf8();
    QString out;
    out.reserve(bytes.size());
    auto isHex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const uchar c = uchar(bytes[i]);
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || (c < 128 && c != 0 && strchr(";/?:@&=+$,-_.!~*'()#", c));
        if (plain || (c == '%' && i + 2 < bytes.size() && isHex(bytes[i + 1]) && isHex(bytes[i + 2]))) {
            out.append(QLatin1Char(char(c)));
        } else {
            out.append(u'%');
            out.append(QLatin1Char(hex[c >> 4]));
            out.append(QLatin1Char(hex[c & 15]));
        }
    }
    return out;
}

} // namespace md
