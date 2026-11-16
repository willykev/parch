#include "previewbuilder.h"

#include "codehighlight.h"
#include "documentcitations.h"
#include "mdparser_p.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextBlock>
#include <QTextTable>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace preview {

using md::Node;
using md::NodeType;

namespace {

QString tr(const char *text) { return QCoreApplication::translate("Preview", text); }

// --- Keys ----------------------------------------------------------------------------

struct Hasher {
    quint64 value = 0xcbf29ce484222325ULL;
    void add(quint64 v) { value ^= v + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2); }
    void add(QStringView text)
    {
        add(quint64(text.size()));
        if (!text.isEmpty()) add(quint64(qHash(text, 0x9747b28cu)));
    }
};

using Citations = DocumentCitations::Formatted;

// The style's formatting, when it formatted the document at all.
const Citations *active(const Citations *citations) { return citations && citations->active ? citations : nullptr; }

// Text a citation style formatted: its words and how each run is set.
void hashStyled(const CslText &words, Hasher &h)
{
    h.add(quint64(words.spans().size()));
    for (const CslSpan &span : words.spans()) {
        const CslFormat &format = span.format;
        h.add(quint64(format.italic) | quint64(format.bold) << 1 | quint64(format.smallCaps) << 2
              | quint64(format.underline) << 3 | quint64(format.position) << 8 | quint64(format.display) << 16);
        h.add(span.text);
        h.add(format.link);
    }
}

// What the style sets apart before the rest of an entry, as it does a number.
QString firstFieldOf(const CslText &words)
{
    QString first;
    for (const CslSpan &span : words.spans())
        if (span.format.display == CslFormat::Display::LeftMargin) first += span.text;
    return first.trimmed();
}

// Whether the style sets the first field of its entries apart, so that the
// rest of every entry starts at one place.
bool alignsSecondField(const Citations &citations) { return !citations.layout.secondFieldAlign.isEmpty(); }

// The longest first field among the entries, by its characters.
QString longestFirstField(const Citations &citations)
{
    QString longest;
    if (!alignsSecondField(citations)) return longest;
    for (const CitationBibliographyEntry &entry : citations.bibliography) {
        const QString first = firstFieldOf(entry.text);
        if (first.size() > longest.size()) longest = first;
    }
    return longest;
}

// The keys a citation names that nothing defines.
QStringList missingKeys(const Node &citation)
{
    QStringList missing;
    if (citation.children.empty()) {
        if (!citation.resolved) missing << citation.label;
        return missing;
    }
    for (const Node &item : citation.children)
        if (item.type == NodeType::Citation && !item.resolved) missing << item.label;
    return missing;
}

// Everything about a node and what it holds that decides how it reads,
// which is everything but where it lies in its file. A citation reads as
// its style words it, which depends on the bibliography and on every
// citation before it.
void hashNode(const Node &node, Hasher &h, const Citations *citations)
{
    h.add(quint64(node.type) | quint64(node.align) << 8 | quint64(node.level) << 16
          | quint64(node.marker.unicode()) << 24 | quint64(node.tight) << 40 | quint64(node.ordered) << 41
          | quint64(node.task) << 42 | quint64(node.checked) << 43 | quint64(node.header) << 44
          | quint64(node.fenced) << 45 | quint64(node.setext) << 46 | quint64(node.display) << 47
          | quint64(node.resolved) << 48 | quint64(node.generated) << 49);
    h.add(quint64(quint32(node.number)) << 32 | quint32(node.sourceNumber));
    h.add(quint64(quint32(node.columns)) << 32 | quint32(node.column));
    h.add(quint64(quint32(node.span)) << 32 | quint32(node.includedFile));
    h.add(node.text);
    h.add(node.dest);
    h.add(node.title);
    h.add(node.label);
    for (const md::Attribute &attribute : node.attributes) {
        h.add(attribute.name);
        h.add(attribute.value);
    }
    if (citations && node.type == NodeType::Citation) {
        if (const Citations::Citation *words = citations->find(node)) {
            hashStyled(words->text, h);
            h.add(quint64(words->note) << 1 | quint64(citations->linkCitations));
        }
    }
    h.add(quint64(node.children.size()));
    for (const Node &child : node.children) hashNode(child, h, citations);
}

// A node's own source too, so that equal keys mean equal offsets inside it.
void hashWithSource(const md::Document &document, const Node &node, Hasher &h, const Citations *citations)
{
    if (node.file == 0 && !node.generated) h.add(document.source(node));
    hashNode(node, h, citations);
}

// The definition in the document's own text that an entry of the style's
// bibliography was made from; null for an entry from a bibliography file.
const Node *definitionOf(const md::Document &document, const CitationBibliographyEntry &entry)
{
    // Only the works the text defines carry a number of their own.
    if (entry.number <= 0) return nullptr;
    for (const md::BibliographyEntry &work : document.bibliography) {
        if (!work.definition || (work.key != entry.key && work.definition->label != entry.key)) continue;
        return work.file == 0 && !work.definition->generated ? work.definition : nullptr;
    }
    return nullptr;
}

bool readsAsNothing(NodeType type)
{
    switch (type) {
    case NodeType::FrontMatter:
    case NodeType::LinkDefinition:
    case NodeType::FootnoteDefinition:
    case NodeType::CitationDefinition:
    case NodeType::CommentLine:
        return true;
    default:
        return false;
    }
}

// Text as a block of a QTextDocument can hold it: a line break inside it
// would start another block.
QString oneBlock(const QString &text, bool collapse)
{
    QString out;
    out.reserve(text.size());
    bool space = false;
    for (const QChar c : text) {
        const bool breaks = c == u'\n' || c == u'\r' || c == QChar::ParagraphSeparator || c == u'\f' || c == u'\v'
                            || c == QChar::Null;
        if (breaks || (collapse && c.isSpace())) {
            if (!space) out.append(u' ');
            space = true;
            continue;
        }
        space = false;
        out.append(c);
    }
    return out;
}

} // namespace

std::vector<Part> parts(const md::Document &document, const DocumentCitations::Formatted *given)
{
    const Citations *citations = active(given);
    std::vector<Part> list;
    list.reserve(document.root.children.size() + document.footnotes.size()
                 + (citations ? size_t(citations->bibliography.size()) : document.bibliography.size()) + 2);
    for (const Node &node : document.root.children) {
        if (readsAsNothing(node.type)) continue;
        Part part;
        part.kind = Part::Block;
        part.node = &node;
        Hasher h;
        h.add(quint64(Part::Block));
        hashWithSource(document, node, h, citations);
        part.key = h.value;
        list.push_back(part);
    }
    if (!document.footnotes.empty()) {
        list.push_back({Part::NotesRule, nullptr, 0, 0x6e6f7465ULL});
        for (size_t i = 0; i < document.footnotes.size(); ++i) {
            const md::Footnote &note = document.footnotes[i];
            Hasher h;
            h.add(quint64(Part::Note));
            h.add(quint64(quint32(note.number)) << 32 | quint32(note.references));
            // Every note is set to the width of the widest number.
            h.add(quint64(QString::number(document.footnotes.size()).size()));
            if (note.content) hashWithSource(document, *note.content, h, citations);
            list.push_back({Part::Note, nullptr, int(i), h.value});
        }
    }
    if (citations) {
        // The style's bibliography, in the style's order, in place of the
        // works the text defines.
        if (!citations->bibliography.isEmpty()) {
            list.push_back({Part::BibliographyRule, nullptr, 0, 0x62696262ULL});
            const QString longest = longestFirstField(*citations);
            for (qsizetype i = 0; i < citations->bibliography.size(); ++i) {
                const CitationBibliographyEntry &entry = citations->bibliography.at(i);
                Hasher h;
                h.add(quint64(Part::Entry) | quint64(1) << 8 | quint64(citations->layout.hangingIndent) << 9
                      | quint64(quint32(entry.number)) << 32);
                h.add(citations->layout.secondFieldAlign);
                h.add(quint64(std::llround(citations->layout.entrySpacing * 100)));
                // Every entry's text starts where the widest first field ends.
                h.add(longest);
                h.add(entry.key);
                hashStyled(entry.text, h);
                // Where it was written, for an entry of the text's own.
                if (const Node *definition = definitionOf(document, entry)) h.add(document.source(*definition));
                list.push_back({Part::Entry, nullptr, int(i), h.value});
            }
        }
        return list;
    }
    if (!document.bibliography.empty()) {
        list.push_back({Part::BibliographyRule, nullptr, 0, 0x62696262ULL});
        for (size_t i = 0; i < document.bibliography.size(); ++i) {
            const md::BibliographyEntry &entry = document.bibliography[i];
            Hasher h;
            h.add(quint64(Part::Entry));
            h.add(quint64(quint32(entry.number)) << 1 | quint64(entry.external));
            h.add(quint64(QString::number(document.bibliography.size()).size()));
            h.add(entry.key);
            if (entry.definition) hashWithSource(document, *entry.definition, h, nullptr);
            list.push_back({Part::Entry, nullptr, int(i), h.value});
        }
    }
    return list;
}

void sourceOf(const md::Document &document, const Part &part, int *start, int *end,
              const DocumentCitations::Formatted *given)
{
    *start = *end = -1;
    md::Range range;
    if (const Citations *citations = active(given); citations && part.kind == Part::Entry) {
        if (part.index < 0 || part.index >= citations->bibliography.size()) return;
        const Node *definition = definitionOf(document, citations->bibliography.at(part.index));
        if (!definition) return;
        *start = definition->range.start;
        *end = definition->range.end;
        return;
    }
    switch (part.kind) {
    case Part::Block:
        if (!part.node || part.node->file != 0 || part.node->generated) return;
        range = part.node->range;
        break;
    case Part::Note: {
        const md::Footnote &note = document.footnotes[size_t(part.index)];
        if (note.file != 0 || !note.content || note.content->generated) return;
        range = note.range;
        break;
    }
    case Part::Entry: {
        const md::BibliographyEntry &work = document.bibliography[size_t(part.index)];
        if (work.file != 0 || !work.definition || work.definition->generated) return;
        range = work.definition->range;
        break;
    }
    default:
        return;
    }
    *start = range.start;
    *end = range.end;
}

QString shownCode(const QString &code)
{
    QString shown = code;
    if (shown.endsWith(u'\n')) shown.chop(1);
    shown.remove(u'\r');
    shown.replace(QChar::ParagraphSeparator, u' ');
    shown.replace(QChar::LineSeparator, u' ');
    shown.replace(QChar::Null, QChar(0xfffd));
    return shown;
}

namespace {
void warmNode(const Node &node)
{
    if (node.type == NodeType::CodeBlock) {
        const QString language = CodeHighlight::languageOf(node.label);
        if (!language.isEmpty()) CodeHighlight::spans(language, shownCode(node.text));
        return;
    }
    for (const Node &child : node.children) warmNode(child);
}
} // namespace

void warm(const md::Document &document) { warmNode(document.root); }

// --- The builder ----------------------------------------------------------------------

// Where blocks are laid: how far in, at what size, and how close together.
struct Builder::Flow {
    qreal left = 0;
    qreal size = 0;
    bool tight = false;
    bool italic = false;
    // How deep in lists, for the bullets.
    int depth = 0;
    // Raw HTML: white space folds as HTML folds it.
    bool html = false;
};

// How a run of text is set.
struct Builder::Char {
    qreal size = 0;
    int weight = QFont::Normal;
    bool italic = false;
    bool strike = false;
    bool underline = false;
    bool smallCaps = false;
    bool code = false;
    bool html = false;
    int script = 0;
    PreviewStyle::Ink ink = PreviewStyle::NoInk;
    PreviewStyle::Paper paper = PreviewStyle::NoPaper;
    QString href;
    QString tip;
};

// Where text is being written: the main document or an island.
struct Builder::Out {
    QTextCursor cursor;
    // Positions count from here.
    int base = 0;
    QList<Anchor> *anchors = nullptr;
    int island = -1;
    // The cursor stands in an empty block, which the next block takes over.
    bool reuse = false;
};

// The bullet, number or box of a list item, written with the item's first
// line of text.
struct Builder::Marker {
    bool pending = false;
    QString text;
    bool task = false;
    bool checked = false;
    // Where the task's `[ ]` is, from the start of the section's source.
    int taskSource = -1;
    // The marker and the space after it.
    qreal advance = 0;
};

struct Builder::Cell {
    const Node *node = nullptr;
    bool header = false;
    md::Align align = md::Align::None;
    int column = 0;
    int span = 1;
    int rows = 1;
};

Builder::Builder(const md::Document &document, const PreviewStyle &style, Resources *resources,
                 const BuildOptions &options)
    : m_document(document), m_style(style), m_resources(resources), m_options(options)
{
}

QString Builder::taskImageName(bool checked)
{
    return checked ? QStringLiteral("parch:task/checked") : QStringLiteral("parch:task/empty");
}

QString Builder::taskLink(int offset) { return QStringLiteral("parch:task/%1").arg(offset); }

QImage Builder::taskImage(const PreviewStyle &style, bool checked, qreal size)
{
    const qreal scale = std::max<qreal>(1, style.scale);
    const int side = std::max(4, int(std::lround(size * scale)));
    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal line = std::max<qreal>(1, side / 12.0);
    const QRectF box = QRectF(0, 0, side, side).adjusted(line, line, -line, -line);
    const qreal corner = side / 6.0;
    if (checked) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(style.accent);
        painter.drawRoundedRect(box, corner, corner);
        // The tick is drawn in whichever of the page's two colours shows on the accent.
        const QColor tick = PreviewStyle::contrast(style.background, style.accent)
                                    >= PreviewStyle::contrast(style.text, style.accent)
                                ? style.background
                                : style.text;
        QPen pen(tick, std::max<qreal>(1.5, side / 8.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        QPainterPath path;
        path.moveTo(side * 0.27, side * 0.52);
        path.lineTo(side * 0.44, side * 0.69);
        path.lineTo(side * 0.74, side * 0.33);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    } else {
        painter.setPen(QPen(style.ink(PreviewStyle::Dim), line));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(box, corner, corner);
    }
    return image;
}

QTextBlockFormat Builder::blockFormat(const Flow &flow, qreal fontSize, qreal top, qreal bottom) const
{
    QTextBlockFormat format;
    format.setLeftMargin(flow.left);
    format.setTopMargin(top);
    format.setBottomMargin(bottom);
    format.setLineHeight(m_style.lineSpacing(fontSize), QTextBlockFormat::MinimumHeight);
    return format;
}

QTextCharFormat Builder::charFormat(const Char &state) const
{
    QTextCharFormat format;
    if (state.code) {
        format.setFontFamilies({m_style.codeFamily});
        format.setProperty(QTextFormat::FontPixelSize, std::max(1, int(std::lround(state.size * 0.9))));
    } else if (std::lround(state.size) != std::lround(m_style.size)) {
        format.setProperty(QTextFormat::FontPixelSize, std::max(1, int(std::lround(state.size))));
    }
    if (state.weight != QFont::Normal) format.setFontWeight(state.weight);
    if (state.italic) format.setFontItalic(true);
    if (state.strike) format.setFontStrikeOut(true);
    if (state.underline) format.setFontUnderline(true);
    if (state.smallCaps) format.setFontCapitalization(QFont::SmallCaps);
    if (state.script > 0) format.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    else if (state.script < 0) format.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    if (state.ink != PreviewStyle::NoInk) {
        format.setForeground(m_style.ink(state.ink));
        format.setProperty(PreviewStyle::InkProperty, int(state.ink));
    }
    if (state.paper != PreviewStyle::NoPaper) {
        format.setBackground(m_style.paper(state.paper));
        format.setProperty(PreviewStyle::PaperProperty, int(state.paper));
    }
    if (!state.href.isEmpty() && m_options.interactive) {
        format.setAnchor(true);
        format.setAnchorHref(state.href);
    }
    if (!state.tip.isEmpty()) format.setToolTip(state.tip);
    return format;
}

Builder::Char Builder::bodyChar(const Flow &flow) const
{
    Char state;
    state.size = flow.size;
    state.italic = flow.italic;
    state.html = flow.html;
    return state;
}

int Builder::here() const { return m_out->cursor.position() - m_out->base; }

void Builder::anchor(const Node &node, int length, bool block, int source)
{
    if (!m_out->anchors || node.file != 0 || node.generated || m_sourceBase < 0) return;
    m_out->anchors->append({here(), (source >= 0 ? source : node.range.start) - m_sourceBase, length, block});
}

void Builder::name(const QString &name)
{
    if (name.isEmpty()) return;
    if (m_out->island >= 0)
        m_section.names.append({name, m_section.islands[size_t(m_out->island)].placeholder, m_out->island});
    else
        m_section.names.append({name, here(), -1});
}

void Builder::text(const QString &text, const Char &state)
{
    if (text.isEmpty()) return;
    m_out->cursor.insertText(oneBlock(text, state.html), charFormat(state));
}

void Builder::uses(const Picture &picture)
{
    if (!picture.key.isEmpty() && !m_section.pictures.contains(picture.key)) m_section.pictures.append(picture.key);
}

void Builder::openBlock(const Flow &flow, qreal fontSize, qreal top, qreal bottom, const Node *source,
                        const Char &markerState, Qt::Alignment alignment)
{
    QTextBlockFormat format = blockFormat(flow, fontSize, top, bottom);
    if (alignment) format.setAlignment(alignment);
    Marker *marker = m_marker && m_marker->pending ? m_marker : nullptr;
    // The marker hangs to the left of the text, ending where the text begins.
    if (marker) format.setTextIndent(-marker->advance);
    Char base;
    base.size = fontSize;
    QTextCursor &cursor = m_out->cursor;
    if (m_out->reuse) {
        cursor.setBlockFormat(format);
        cursor.setBlockCharFormat(charFormat(base));
        m_out->reuse = false;
    } else {
        cursor.insertBlock(format, charFormat(base));
    }
    if (source) anchor(*source, 0, true);
    if (!marker) return;
    marker->pending = false;
    Char state = markerState;
    state.ink = PreviewStyle::Dim;
    state.italic = false;
    state.weight = QFont::Normal;
    state.href.clear();
    if (marker->task) {
        const qreal side = std::round(flow.size * 0.92);
        const QString imageName = taskImageName(marker->checked);
        cursor.document()->addResource(QTextDocument::ImageResource, QUrl(imageName),
                                       taskImage(m_style, marker->checked, side));
        QTextImageFormat box;
        box.setName(imageName);
        box.setWidth(side);
        box.setHeight(side);
        box.setVerticalAlignment(QTextCharFormat::AlignBottom);
        if (m_options.interactive && marker->taskSource >= 0) {
            box.setAnchor(true);
            box.setAnchorHref(taskLink(marker->taskSource));
        }
        box.setToolTip(marker->checked ? tr("Done") : tr("To do"));
        cursor.insertImage(box);
        cursor.insertText(QString(QChar(0x2002)), charFormat(state));
    } else {
        cursor.insertText(marker->text + QChar(0x2002), charFormat(state));
    }
}

void Builder::emptyBlock(const Flow &flow, qreal height, qreal top, qreal bottom, const Node *source, int island)
{
    QTextBlockFormat format;
    format.setLeftMargin(flow.left);
    format.setTopMargin(top);
    format.setBottomMargin(bottom);
    format.setLineHeight(std::max<qreal>(1, height), QTextBlockFormat::FixedHeight);
    if (island > 0) format.setProperty(IslandProperty, island);
    QTextCharFormat tiny;
    tiny.setProperty(QTextFormat::FontPixelSize, 1);
    QTextCursor &cursor = m_out->cursor;
    if (m_out->reuse) {
        cursor.setBlockFormat(format);
        cursor.setBlockCharFormat(tiny);
        m_out->reuse = false;
    } else {
        cursor.insertBlock(format, tiny);
    }
    if (source) anchor(*source, 0, true);
}

void Builder::flushMarker(const Flow &flow)
{
    if (!m_marker || !m_marker->pending) return;
    openBlock(flow, flow.size, 0, flow.tight ? m_style.gap() * 0.2 : m_style.gap(), nullptr, bodyChar(flow));
}

void Builder::raiseBottomMargin(qreal margin)
{
    if (here() < 0) return;
    QTextBlockFormat format = m_out->cursor.blockFormat();
    if (format.bottomMargin() >= margin) return;
    format.setBottomMargin(margin);
    m_out->cursor.setBlockFormat(format);
}

void Builder::blocks(const std::vector<Node> &nodes, const Flow &flow)
{
    for (const Node &node : nodes) block(node, flow);
}

void Builder::block(const Node &node, const Flow &flow)
{
    if (readsAsNothing(node.type)) return;
    // Only a line of text can carry a list marker beside it.
    if (node.type != NodeType::Paragraph && node.type != NodeType::Heading && node.type != NodeType::Include
        && node.type != NodeType::Document)
        flushMarker(flow);
    const qreal gap = m_style.gap();
    switch (node.type) {
    case NodeType::Document:
        blocks(node.children, flow);
        break;
    case NodeType::Paragraph:
        paragraph(node, flow);
        break;
    case NodeType::Heading:
        heading(node, flow, node.level, node.children);
        break;
    case NodeType::ThematicBreak:
        rule(flow, &node, Mark::Rule);
        break;
    case NodeType::PageBreak:
        rule(flow, &node, Mark::PageBreak);
        break;
    case NodeType::BlockQuote:
        quote(node.children, flow, false);
        break;
    case NodeType::List:
        list(node, flow);
        break;
    case NodeType::CodeBlock:
        codeBlock(node, flow, node.text, CodeHighlight::languageOf(node.label), QString(), PreviewStyle::NoInk,
                  PreviewStyle::Code);
        break;
    case NodeType::MermaidBlock:
        diagram(node, flow);
        break;
    case NodeType::MathBlock:
        mathBlock(node, flow);
        break;
    case NodeType::HtmlBlock:
        htmlBlock(node, flow);
        break;
    case NodeType::Table:
        table(node, flow);
        break;
    case NodeType::DefinitionList:
        definitionList(node, flow);
        break;
    case NodeType::TableOfContents:
        contents(node, flow, 0);
        raiseBottomMargin(gap);
        break;
    case NodeType::Include:
        if (node.resolved) {
            // The line that includes stands for everything it brings in.
            const int before = int(m_section.anchors.size());
            blocks(node.children, flow);
            if (m_out->island < 0 && m_section.anchors.size() == before && here() >= 0 && node.file == 0
                && m_sourceBase >= 0)
                m_section.anchors.append({0, node.range.start - m_sourceBase, 0, true});
        } else {
            Char state = bodyChar(flow);
            state.ink = PreviewStyle::Dim;
            openBlock(flow, flow.size, 0, gap, &node, state);
            text(m_document.source(node).toString(), state);
        }
        break;
    default:
        // A block the builder has no form for is shown through what it holds.
        blocks(node.children, flow);
        break;
    }
}

void Builder::paragraph(const Node &node, const Flow &flow)
{
    const Char state = bodyChar(flow);
    openBlock(flow, flow.size, 0, flow.tight ? m_style.gap() * 0.2 : m_style.gap(), &node, state);
    inlines(node.children, state);
}

void Builder::heading(const Node &node, const Flow &flow, int level, const std::vector<Node> &children)
{
    const qreal gap = m_style.gap();
    const qreal size = std::round(m_style.headingSize(level) * flow.size / m_style.size);
    Char state = bodyChar(flow);
    state.size = size;
    state.weight = QFont::DemiBold;
    state.italic = false;
    state.ink = PreviewStyle::Heading;
    openBlock(flow, size, gap * (level <= 2 ? 1.5 : 1.15), gap * 0.55, &node, state);
    QTextBlockFormat format = m_out->cursor.blockFormat();
    format.setHeadingLevel(std::clamp(level, 1, 6));
    m_out->cursor.setBlockFormat(format);
    if (node.type == NodeType::Heading) name(node.dest);
    if (m_out->island < 0) {
        const int at = m_out->cursor.block().position() - m_out->base;
        Mark mark;
        mark.kind = Mark::Heading;
        mark.first = mark.last = at;
        mark.x = flow.left;
        mark.level = level;
        mark.text = md::plainText(node).simplified();
        m_section.marks.append(mark);
    }
    inlines(children, state);
}

void Builder::rule(const Flow &flow, const Node *node, Mark::Kind kind)
{
    const qreal gap = m_style.gap();
    const qreal height = kind == Mark::PageBreak ? std::round(m_style.smallSize() * 1.5) : 1;
    emptyBlock(flow, height, kind == Mark::NotesRule ? gap * 1.6 : gap * 0.9, gap * 1.1, node);
    if (m_out->island >= 0) return;
    Mark mark;
    mark.kind = kind;
    mark.first = mark.last = m_out->cursor.block().position() - m_out->base;
    mark.x = flow.left;
    m_section.marks.append(mark);
}

void Builder::quote(const std::vector<Node> &children, const Flow &flow, bool html)
{
    Flow inner = flow;
    inner.left += m_style.indent();
    inner.italic = true;
    inner.tight = false;
    const int count = m_out->cursor.document()->blockCount();
    const int after = m_out->cursor.block().position();
    if (html) htmlFlow(children, inner);
    else blocks(children, inner);
    if (m_out->island >= 0 || m_out->cursor.document()->blockCount() == count) return;
    // From the first block made here to the last.
    const QTextBlock first = m_out->cursor.document()->findBlock(after).next();
    Mark mark;
    mark.kind = Mark::QuoteBar;
    mark.first = (first.isValid() ? first.position() : m_out->cursor.block().position()) - m_out->base;
    mark.last = m_out->cursor.block().position() - m_out->base;
    mark.x = flow.left;
    m_section.marks.append(mark);
}

void Builder::list(const Node &node, const Flow &flow)
{
    QFont font(m_style.family);
    font.setPixelSize(std::max(1, int(std::lround(flow.size))));
    const QFontMetricsF metrics(font);
    const qreal space = metrics.horizontalAdvance(QChar(0x2002));
    static const QChar bullets[] = {QChar(0x2022), QChar(0x25e6), QChar(0x25aa)};
    struct Item {
        const Node *node;
        Marker marker;
    };
    std::vector<Item> items;
    qreal widest = 0;
    for (const Node &child : node.children) {
        if (child.type != NodeType::ListItem) continue;
        Marker marker;
        marker.pending = true;
        if (child.task) {
            marker.task = true;
            marker.checked = child.checked;
            marker.advance = std::round(flow.size * 0.92) + space;
            const Node &first = child.children.front();
            if (!first.children.empty() && first.children.front().type == NodeType::TaskMarker
                && first.children.front().file == 0 && !first.children.front().generated && m_sourceBase >= 0)
                marker.taskSource = first.children.front().range.start - m_sourceBase;
        } else {
            if (node.ordered) marker.text = QString::number(child.number) + (node.marker == u')' ? u')' : u'.');
            else marker.text = QString(bullets[std::min(flow.depth, 2)]);
            marker.advance = metrics.horizontalAdvance(marker.text) + space;
        }
        widest = std::max(widest, marker.advance);
        items.push_back({&child, marker});
    }
    Flow inner = flow;
    inner.left += widest;
    inner.tight = node.tight;
    inner.depth = flow.depth + 1;
    Marker *outer = m_marker;
    for (Item &item : items) {
        m_marker = &item.marker;
        blocks(item.node->children, inner);
        // An item without text still shows its marker.
        flushMarker(inner);
    }
    m_marker = outer;
    // A list ends as a paragraph does, unless it lies inside a tight item.
    if (flow.depth == 0 || !flow.tight) raiseBottomMargin(m_style.gap());
}

void Builder::definitionList(const Node &node, const Flow &flow)
{
    const qreal gap = m_style.gap();
    for (const Node &child : node.children) {
        if (child.type == NodeType::DefinitionTerm) {
            Char state = bodyChar(flow);
            state.weight = QFont::DemiBold;
            openBlock(flow, flow.size, 0, gap * 0.15, &child, state);
            inlines(child.children, state);
        } else if (child.type == NodeType::DefinitionDescription) {
            Flow inner = flow;
            inner.left += m_style.indent();
            inner.tight = child.tight;
            blocks(child.children, inner);
            raiseBottomMargin(gap * 0.45);
        }
    }
    raiseBottomMargin(gap);
}

// The generated list of a table of contents: a line for each heading,
// stepped in by its depth, without bullets.
void Builder::contents(const Node &node, const Flow &flow, int depth)
{
    for (const Node &child : node.children) {
        if (child.type == NodeType::List || child.type == NodeType::ListItem) {
            contents(child, flow, child.type == NodeType::List ? depth + 1 : depth);
        } else if (child.type == NodeType::Paragraph) {
            Flow line = flow;
            line.left += std::max(0, depth - 1) * m_style.indent();
            const Char state = bodyChar(line);
            openBlock(line, line.size, 0, m_style.gap() * 0.15, nullptr, state);
            inlines(child.children, state);
        }
    }
}

Island &Builder::addIsland(Island::Kind kind, const Flow &flow, const Node &node, const QFont &font)
{
    Island island;
    island.kind = kind;
    island.indent = flow.left;
    island.document = new QTextDocument(m_options.islandParent);
    island.document->setUndoRedoEnabled(false);
    island.document->setDocumentMargin(0);
    island.document->setDefaultFont(font);
    if (node.file == 0 && !node.generated && m_sourceBase >= 0) {
        island.sourceStart = node.range.start - m_sourceBase;
        island.sourceEnd = node.range.end - m_sourceBase;
    }
    m_section.islands.push_back(std::move(island));
    return m_section.islands.back();
}

// The empty block that holds the last island's place.
void Builder::placeIsland(const Flow &flow, const Node &node, qreal top, qreal bottom)
{
    const int number = int(m_section.islands.size());
    emptyBlock(flow, 1, top, bottom, &node, number);
    m_section.islands.back().placeholder = m_out->cursor.block().position() - m_out->base;
}

void Builder::codeBlock(const Node &node, const Flow &flow, const QString &code, const QString &language,
                        const QString &caption, PreviewStyle::Ink captionInk, PreviewStyle::Ink ink)
{
    const qreal gap = m_style.gap();
    const QString shown = shownCode(code);
    const QList<CodeHighlight::Span> spans = ink == PreviewStyle::Code ? CodeHighlight::spans(language, shown)
                                                                        : QList<CodeHighlight::Span>();
    // The lines of the source the lines of code were written on: after the
    // opening fence, or from the first line for indented code.
    QList<int> lineSources;
    if (node.file == 0 && !node.generated && m_sourceBase >= 0
        && (node.type == NodeType::CodeBlock || node.type == NodeType::MermaidBlock || node.type == NodeType::MathBlock)) {
        const QStringView source = m_document.source(node);
        qsizetype at = 0;
        const bool fenced = node.type != NodeType::CodeBlock || node.fenced;
        if (fenced) {
            at = source.indexOf(u'\n');
            at = at < 0 ? source.size() : at + 1;
        }
        while (at <= source.size()) {
            lineSources.append(node.range.start + int(at) - m_sourceBase);
            const qsizetype next = source.indexOf(u'\n', at);
            if (next < 0) break;
            at = next + 1;
        }
    }
    const auto inkFormat = [this](PreviewStyle::Ink role) {
        QTextCharFormat format;
        format.setForeground(m_style.ink(role));
        format.setProperty(PreviewStyle::InkProperty, int(role));
        return format;
    };
    const qreal spacing = std::round(m_style.codeSize() * 1.5);

    if (!m_options.islands || m_out->island >= 0) {
        // In place: one block on the tint, its lines broken inside it.
        Flow inner = flow;
        QTextBlockFormat format = blockFormat(inner, m_style.codeSize(), 0, gap);
        format.setBackground(m_style.paper(PreviewStyle::CodePaper));
        format.setProperty(PreviewStyle::PaperProperty, int(PreviewStyle::CodePaper));
        format.setLineHeight(spacing, QTextBlockFormat::MinimumHeight);
        QTextCharFormat base = inkFormat(ink);
        base.setFontFamilies({m_style.codeFamily});
        base.setProperty(QTextFormat::FontPixelSize, std::max(1, int(std::lround(m_style.codeSize()))));
        QTextCursor &cursor = m_out->cursor;
        if (m_out->reuse) {
            cursor.setBlockFormat(format);
            cursor.setBlockCharFormat(base);
            m_out->reuse = false;
        } else {
            cursor.insertBlock(format, base);
        }
        anchor(node, 0, true);
        qsizetype at = 0;
        for (const CodeHighlight::Span &span : spans) {
            if (span.start > at) cursor.insertText(shown.mid(at, span.start - at).replace(u'\n', QChar::LineSeparator), base);
            QTextCharFormat run = base;
            run.setForeground(m_style.ink(span.ink));
            run.setProperty(PreviewStyle::InkProperty, int(span.ink));
            cursor.insertText(shown.mid(span.start, span.length).replace(u'\n', QChar::LineSeparator), run);
            at = span.start + span.length;
        }
        if (at < shown.size()) cursor.insertText(shown.mid(at).replace(u'\n', QChar::LineSeparator), base);
        if (!caption.isEmpty()) {
            Char state = bodyChar(flow);
            state.size = m_style.smallSize();
            state.ink = captionInk;
            openBlock(flow, state.size, 0, gap, nullptr, state);
            text(caption, state);
        }
        return;
    }

    Island &island = addIsland(Island::Code, flow, node, m_style.codeFont());
    island.tinted = true;
    island.description = language.isEmpty() ? tr("Code") : tr("Code, %1").arg(language);
    QTextDocument *document = island.document;
    QTextOption option = document->defaultTextOption();
    option.setWrapMode(QTextOption::NoWrap);
    option.setTabStopDistance(QFontMetricsF(m_style.codeFont()).horizontalAdvance(u' ') * 4);
    document->setDefaultTextOption(option);
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    QTextBlockFormat lineFormat;
    lineFormat.setLineHeight(spacing, QTextBlockFormat::FixedHeight);
    const QTextCharFormat plain = inkFormat(ink);
    qsizetype lineStart = 0;
    qsizetype spanIndex = 0;
    int line = 0;
    while (lineStart <= shown.size()) {
        qsizetype end = shown.indexOf(u'\n', lineStart);
        if (end < 0) end = shown.size();
        if (line == 0) {
            cursor.setBlockFormat(lineFormat);
            cursor.setBlockCharFormat(plain);
        } else {
            cursor.insertBlock(lineFormat, plain);
        }
        if (line < lineSources.size()) island.anchors.append({cursor.position(), lineSources.at(line), int(end - lineStart), true});
        qsizetype at = lineStart;
        while (at < end) {
            while (spanIndex < spans.size() && spans.at(spanIndex).start + spans.at(spanIndex).length <= at) ++spanIndex;
            qsizetype runEnd = end;
            PreviewStyle::Ink role = ink;
            if (spanIndex < spans.size() && spans.at(spanIndex).start <= at) {
                runEnd = std::min<qsizetype>(end, spans.at(spanIndex).start + spans.at(spanIndex).length);
                role = spans.at(spanIndex).ink;
            } else if (spanIndex < spans.size()) {
                runEnd = std::min<qsizetype>(end, spans.at(spanIndex).start);
            }
            cursor.insertText(shown.mid(at, runEnd - at), role == ink ? plain : inkFormat(role));
            at = runEnd;
        }
        lineStart = end + 1;
        ++line;
    }
    if (!caption.isEmpty()) {
        QTextBlockFormat captionFormat = lineFormat;
        captionFormat.setTopMargin(spacing * 0.5);
        cursor.insertBlock(captionFormat, inkFormat(captionInk));
        cursor.insertText(oneBlock(caption, true), inkFormat(captionInk));
    }
    cursor.endEditBlock();
    placeIsland(flow, node, 0, gap);
}

QTextTable *Builder::fillTable(QTextCursor &cursor, const std::vector<std::vector<Cell>> &rows, int columns,
                               const Flow &flow)
{
    QTextTableFormat tableFormat;
    tableFormat.setBorder(0);
    tableFormat.setCellSpacing(0);
    tableFormat.setCellPadding(0);
    tableFormat.setAlignment(Qt::AlignLeft);
    QTextTable *table = cursor.insertTable(int(rows.size()), std::max(1, columns), tableFormat);
    const qreal padding = m_style.padding();
    Out *outer = m_out;
    for (int r = 0; r < int(rows.size()); ++r) {
        for (const Cell &cell : rows[size_t(r)]) {
            if (cell.column < 0 || cell.column >= table->columns()) continue;
            const int span = std::clamp(cell.span, 1, table->columns() - cell.column);
            const int tall = std::clamp(cell.rows, 1, int(rows.size()) - r);
            if (span > 1 || tall > 1) table->mergeCells(r, cell.column, tall, span);
            QTextTableCell place = table->cellAt(r, cell.column);
            QTextTableCellFormat cellFormat;
            cellFormat.setTopPadding(padding * 0.45);
            cellFormat.setBottomPadding(padding * 0.45);
            cellFormat.setLeftPadding(cell.column == 0 ? 0 : padding * 0.7);
            cellFormat.setRightPadding(padding * 0.7);
            place.setFormat(cellFormat);
            Out out;
            out.cursor = place.firstCursorPosition();
            out.base = outer->base;
            out.anchors = outer->anchors;
            out.island = outer->island;
            Flow inner;
            inner.size = flow.size;
            inner.html = flow.html;
            QTextBlockFormat format = blockFormat(inner, flow.size, 0, 0);
            switch (cell.align) {
            case md::Align::Center: format.setAlignment(Qt::AlignHCenter); break;
            case md::Align::Right: format.setAlignment(Qt::AlignRight); break;
            default: format.setAlignment(Qt::AlignLeft); break;
            }
            Char state = bodyChar(inner);
            if (cell.header) state.weight = QFont::DemiBold;
            // The block's own character format is the cell's, with its spans
            // and padding, and is left as it is.
            out.cursor.setBlockFormat(format);
            m_out = &out;
            if (cell.node) {
                anchor(*cell.node, 0, true);
                inlines(cell.node->children, state);
            }
            m_out = outer;
        }
    }
    return table;
}

void Builder::table(const Node &node, const Flow &flow)
{
    std::vector<std::vector<Cell>> rows;
    int headers = 0;
    for (const Node &row : node.children) {
        if (row.type != NodeType::TableRow) continue;
        std::vector<Cell> cells;
        for (const Node &cell : row.children) {
            if (cell.type != NodeType::TableCell) continue;
            cells.push_back({&cell, row.header || cell.header, cell.align, cell.column, cell.span, 1});
        }
        if (row.header) ++headers;
        rows.push_back(std::move(cells));
    }
    if (rows.empty()) return;
    const qreal gap = m_style.gap();
    if (!m_options.islands || m_out->island >= 0) {
        QTextCursor &cursor = m_out->cursor;
        if (!m_out->reuse) cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
        QTextTable *made = fillTable(cursor, rows, node.columns, flow);
        // After the table stands an empty block, which the next block takes.
        cursor.setPosition(made->lastPosition() + 1);
        m_out->reuse = true;
        return;
    }
    Island &island = addIsland(Island::Table, flow, node, m_style.bodyFont());
    island.wraps = true;
    island.headerRows = headers;
    island.description = tr("Table, %1 columns, %2 rows").arg(node.columns).arg(rows.size());
    const int index = int(m_section.islands.size()) - 1;
    Out out;
    out.cursor = QTextCursor(island.document);
    out.base = 0;
    out.anchors = &island.anchors;
    out.island = index;
    Out *outer = m_out;
    m_out = &out;
    out.cursor.beginEditBlock();
    Flow inner = flow;
    inner.left = 0;
    fillTable(out.cursor, rows, node.columns, inner);
    out.cursor.endEditBlock();
    m_out = outer;
    placeIsland(flow, node, 0, gap);
}

void Builder::mathBlock(const Node &node, const Flow &flow)
{
    const qreal gap = m_style.gap();
    Picture formula;
    if (m_resources) formula = m_resources->math(node.text, true, flow.size);
    uses(formula);
    if (formula.state == Picture::Failed) {
        codeBlock(node, flow, node.text, QString(),
                  formula.diagnostic.isEmpty() ? tr("The formula could not be drawn.") : formula.diagnostic,
                  PreviewStyle::Danger, PreviewStyle::Code);
        return;
    }
    if (formula.state != Picture::Ready || formula.image.isNull()) {
        codeBlock(node, flow, node.text, QString(), QString(), PreviewStyle::NoInk, PreviewStyle::Dim);
        return;
    }
    if (!m_options.islands || m_out->island >= 0) {
        openBlock(flow, flow.size, 0, gap, &node, bodyChar(flow), Qt::AlignHCenter);
        picture(formula, formula.size, QTextCharFormat::AlignNormal, bodyChar(flow), true);
        return;
    }
    Island &island = addIsland(Island::Math, flow, node, m_style.bodyFont());
    island.centered = true;
    island.description = formula.description.isEmpty() ? tr("Formula") : tr("Formula: %1").arg(formula.description);
    QTextOption option = island.document->defaultTextOption();
    option.setWrapMode(QTextOption::NoWrap);
    island.document->setDefaultTextOption(option);
    const int index = int(m_section.islands.size()) - 1;
    Out out;
    out.cursor = QTextCursor(island.document);
    out.anchors = &island.anchors;
    out.island = index;
    Out *outer = m_out;
    m_out = &out;
    anchor(node, 0, true);
    picture(formula, formula.size, QTextCharFormat::AlignNormal, bodyChar(flow), false);
    m_out = outer;
    placeIsland(flow, node, gap * 0.2, gap * 1.2);
}

void Builder::diagram(const Node &node, const Flow &flow)
{
    Picture drawing;
    if (m_resources) drawing = m_resources->diagram(node.text);
    uses(drawing);
    if (drawing.state == Picture::Failed) {
        codeBlock(node, flow, node.text, QString(),
                  drawing.diagnostic.isEmpty() ? tr("The diagram could not be drawn.") : drawing.diagnostic,
                  PreviewStyle::Danger, PreviewStyle::Code);
        return;
    }
    if (drawing.state != Picture::Ready || drawing.image.isNull()) {
        codeBlock(node, flow, node.text, QString(), tr("Drawing the diagram…"), PreviewStyle::Dim, PreviewStyle::Dim);
        return;
    }
    openBlock(flow, flow.size, 0, m_style.gap(), &node, bodyChar(flow), Qt::AlignHCenter);
    if (m_out->island < 0) {
        Mark mark;
        mark.kind = Mark::Image;
        mark.first = mark.last = m_out->cursor.block().position() - m_out->base;
        mark.x = flow.left;
        mark.text = drawing.description.isEmpty() ? tr("Diagram") : drawing.description;
        m_section.marks.append(mark);
    }
    picture(drawing, drawing.size, QTextCharFormat::AlignNormal, bodyChar(flow), true);
}

// --- Raw HTML under the engine's policy ------------------------------------------------

void Builder::htmlBlock(const Node &node, const Flow &flow)
{
    Flow inner = flow;
    inner.html = true;
    htmlFlow(node.children, inner);
}

// Blocks and runs of inline content as they come: each run between two
// blocks is a paragraph, unless it is only white space.
void Builder::htmlFlow(const std::vector<Node> &nodes, const Flow &flow)
{
    std::vector<const Node *> run;
    const auto flush = [&] {
        bool visible = false;
        for (const Node *node : run) {
            if (node->type == NodeType::HtmlComment) continue;
            if (node->isTextual()) visible = visible || !node->text.trimmed().isEmpty();
            else visible = true;
        }
        if (visible) {
            const Char state = bodyChar(flow);
            openBlock(flow, flow.size, 0, flow.tight ? m_style.gap() * 0.2 : m_style.gap(), run.front(), state);
            const int start = m_out->cursor.position();
            for (const Node *node : run) {
                // White space at the start of a paragraph is not shown.
                if (node->isTextual() && m_out->cursor.position() == start && node->text.trimmed().isEmpty()) continue;
                if (node->isTextual() && m_out->cursor.position() == start) {
                    Node trimmed = *node;
                    qsizetype lead = 0;
                    while (lead < trimmed.text.size() && trimmed.text.at(lead).isSpace()) ++lead;
                    trimmed.text.remove(0, lead);
                    trimmed.range.start += int(lead);
                    inl(trimmed, state);
                } else {
                    inl(*node, state);
                }
            }
        }
        run.clear();
    };
    for (const Node &node : nodes) {
        if (node.type == NodeType::HtmlElement && md::detail::htmlTagInfo(node.label).block) {
            flush();
            flushMarker(flow);
            htmlElementBlock(node, flow);
        } else if (node.isBlock()) {
            flush();
            block(node, flow);
        } else {
            run.push_back(&node);
        }
    }
    flush();
}

void Builder::htmlElementBlock(const Node &node, const Flow &flow)
{
    const QString &tag = node.label;
    const qreal gap = m_style.gap();
    if (tag.size() == 2 && tag.at(0) == u'h' && tag.at(1) >= u'1' && tag.at(1) <= u'6') {
        heading(node, flow, tag.at(1).digitValue(), node.children);
    } else if (tag == QLatin1StringView("blockquote")) {
        quote(node.children, flow, true);
    } else if (tag == QLatin1StringView("pre")) {
        Flow plain = flow;
        plain.html = false;
        codeBlock(node, plain, md::plainText(node), QString(), QString(), PreviewStyle::NoInk, PreviewStyle::Code);
    } else if (tag == QLatin1StringView("hr")) {
        rule(flow, &node, Mark::Rule);
    } else if (tag == QLatin1StringView("ul") || tag == QLatin1StringView("ol")) {
        const bool ordered = tag == QLatin1StringView("ol");
        int number = 1;
        for (const md::Attribute &attribute : node.attributes)
            if (attribute.name == QLatin1StringView("start")) number = attribute.value.toInt();
        QFont font(m_style.family);
        font.setPixelSize(std::max(1, int(std::lround(flow.size))));
        const QFontMetricsF metrics(font);
        const qreal space = metrics.horizontalAdvance(QChar(0x2002));
        static const QChar bullets[] = {QChar(0x2022), QChar(0x25e6), QChar(0x25aa)};
        std::vector<std::pair<const Node *, Marker>> items;
        qreal widest = 0;
        for (const Node &child : node.children) {
            if (child.type != NodeType::HtmlElement || child.label != QLatin1StringView("li")) continue;
            Marker marker;
            marker.pending = true;
            marker.text = ordered ? QString::number(number++) + u'.' : QString(bullets[std::min(flow.depth, 2)]);
            marker.advance = metrics.horizontalAdvance(marker.text) + space;
            widest = std::max(widest, marker.advance);
            items.emplace_back(&child, marker);
        }
        Flow inner = flow;
        inner.left += widest;
        inner.tight = true;
        inner.depth = flow.depth + 1;
        Marker *outer = m_marker;
        for (auto &[item, marker] : items) {
            m_marker = &marker;
            htmlFlow(item->children, inner);
            flushMarker(inner);
        }
        m_marker = outer;
        if (flow.depth == 0) raiseBottomMargin(gap);
    } else if (tag == QLatin1StringView("dl")) {
        for (const Node &child : node.children) {
            if (child.type != NodeType::HtmlElement) continue;
            if (child.label == QLatin1StringView("dt")) {
                Flow term = flow;
                term.tight = true;
                Char state = bodyChar(term);
                state.weight = QFont::DemiBold;
                openBlock(term, term.size, 0, gap * 0.15, &child, state);
                inlines(child.children, state);
            } else if (child.label == QLatin1StringView("dd")) {
                Flow inner = flow;
                inner.left += m_style.indent();
                inner.tight = true;
                htmlFlow(child.children, inner);
                raiseBottomMargin(gap * 0.45);
            }
        }
        raiseBottomMargin(gap);
    } else if (tag == QLatin1StringView("table")) {
        htmlTable(node, flow);
    } else {
        // A paragraph, or a part of a list or table that stands outside one.
        htmlFlow(node.children, flow);
    }
}

void Builder::htmlTable(const Node &node, const Flow &flow)
{
    // The rows wherever the sections put them, and the caption.
    std::vector<const Node *> rowNodes;
    const Node *caption = nullptr;
    const std::function<void(const Node &)> collect = [&](const Node &parent) {
        for (const Node &child : parent.children) {
            if (child.type != NodeType::HtmlElement) continue;
            if (child.label == QLatin1StringView("tr")) rowNodes.push_back(&child);
            else if (child.label == QLatin1StringView("caption")) caption = &child;
            else if (child.label == QLatin1StringView("thead") || child.label == QLatin1StringView("tbody")
                     || child.label == QLatin1StringView("tfoot"))
                collect(child);
        }
    };
    collect(node);
    // Cells take the first free columns of their row; a cell that spans
    // rows keeps its columns in the rows below.
    std::vector<std::vector<Cell>> rows;
    std::vector<std::vector<bool>> taken(rowNodes.size());
    int columns = 0;
    int headers = 0;
    for (size_t r = 0; r < rowNodes.size(); ++r) {
        std::vector<Cell> cells;
        int column = 0;
        bool allHeaders = true;
        for (const Node &child : rowNodes[r]->children) {
            if (child.type != NodeType::HtmlElement) continue;
            const bool header = child.label == QLatin1StringView("th");
            if (!header && child.label != QLatin1StringView("td")) continue;
            allHeaders = allHeaders && header;
            Cell cell;
            cell.node = &child;
            cell.header = header;
            for (const md::Attribute &attribute : child.attributes) {
                if (attribute.name == QLatin1StringView("colspan")) cell.span = std::clamp(attribute.value.toInt(), 1, 1000);
                else if (attribute.name == QLatin1StringView("rowspan"))
                    cell.rows = std::clamp(attribute.value.toInt(), 1, int(rowNodes.size() - r));
                else if (attribute.name == QLatin1StringView("align")) {
                    const QString value = attribute.value.toLower();
                    cell.align = value == QLatin1StringView("center")  ? md::Align::Center
                                 : value == QLatin1StringView("right") ? md::Align::Right
                                                                       : md::Align::Left;
                }
            }
            while (column < int(taken[r].size()) && taken[r][size_t(column)]) ++column;
            cell.column = column;
            for (int down = 0; down < cell.rows; ++down) {
                std::vector<bool> &line = taken[r + size_t(down)];
                if (int(line.size()) < column + cell.span) line.resize(size_t(column + cell.span), false);
                std::fill(line.begin() + column, line.begin() + column + cell.span, true);
            }
            column += cell.span;
            columns = std::max(columns, column);
            cells.push_back(cell);
        }
        if (allHeaders && !cells.empty() && headers == int(r)) ++headers;
        rows.push_back(std::move(cells));
    }
    if (caption) {
        Char state = bodyChar(flow);
        state.ink = PreviewStyle::Dim;
        openBlock(flow, flow.size, 0, m_style.gap() * 0.3, caption, state);
        inlines(caption->children, state);
    }
    if (rows.empty() || columns == 0) return;
    if (columns > 1000) columns = 1000;
    if (!m_options.islands || m_out->island >= 0) {
        QTextCursor &cursor = m_out->cursor;
        if (!m_out->reuse) cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
        QTextTable *made = fillTable(cursor, rows, columns, flow);
        cursor.setPosition(made->lastPosition() + 1);
        m_out->reuse = true;
        return;
    }
    Island &island = addIsland(Island::Table, flow, node, m_style.bodyFont());
    island.wraps = true;
    island.headerRows = headers;
    island.description = tr("Table, %1 columns, %2 rows").arg(columns).arg(rows.size());
    Out out;
    out.cursor = QTextCursor(island.document);
    out.anchors = &island.anchors;
    out.island = int(m_section.islands.size()) - 1;
    Out *outer = m_out;
    m_out = &out;
    Flow inner = flow;
    inner.left = 0;
    fillTable(out.cursor, rows, columns, inner);
    m_out = outer;
    placeIsland(flow, node, 0, m_style.gap());
}

// --- Notes and the bibliography -----------------------------------------------------------

void Builder::note(const Part &part)
{
    const md::Footnote &note = m_document.footnotes[size_t(part.index)];
    const bool own = note.file == 0 && note.content && !note.content->generated;
    m_sourceBase = own ? note.range.start : -1;
    m_section.sourceStart = own ? note.range.start : -1;
    m_section.sourceEnd = own ? note.range.end : -1;
    Flow flow;
    flow.size = std::round(m_style.smallSize());
    flow.tight = true;
    QFont font(m_style.family);
    font.setPixelSize(std::max(1, int(flow.size)));
    const QFontMetricsF metrics(font);
    Marker marker;
    marker.pending = true;
    marker.text = QString::number(note.number) + u'.';
    marker.advance = metrics.horizontalAdvance(marker.text + QChar(0x2002));
    // Every note's text starts where the widest number's does.
    flow.left = metrics.horizontalAdvance(QString::number(m_document.footnotes.size()) + u'.' + QChar(0x2002));
    m_marker = &marker;
    const int start = here() + 1;
    if (note.content && note.content->type == NodeType::InlineNote) {
        const Char state = bodyChar(flow);
        openBlock(flow, flow.size, 0, m_style.gap() * 0.2, note.content, state);
        inlines(note.content->children, state);
    } else if (note.content && note.content->type == NodeType::Citation) {
        // In a note style a citation is the note: its words are the note's text.
        const Char state = bodyChar(flow);
        openBlock(flow, flow.size, 0, m_style.gap() * 0.2, note.content, state);
        citationText(*note.content, state);
    } else if (note.content) {
        blocks(note.content->children, flow);
    }
    flushMarker(flow);
    m_marker = nullptr;
    m_section.names.append({QStringLiteral("fn-%1").arg(note.number), start, -1});
    // The way back to each place that refers to the note, after its text.
    if (m_options.interactive && note.references > 0) {
        if (m_out->cursor.block().length() <= 1) openBlock(flow, flow.size, 0, 0, nullptr, bodyChar(flow));
        for (int i = 1; i <= note.references; ++i) {
            Char back = bodyChar(flow);
            text(QStringLiteral(" "), back);
            back.ink = PreviewStyle::Link;
            back.href = i == 1 ? QStringLiteral("#fnref-%1").arg(note.number)
                               : QStringLiteral("#fnref-%1-%2").arg(note.number).arg(i);
            back.tip = tr("Back to the text");
            text(i == 1 ? QString(QChar(0x21a9)) : QChar(0x21a9) + QString::number(i), back);
        }
    }
    raiseBottomMargin(m_style.gap() * 0.5);
}

// A bibliography entry as the citation style words and lays it out: with a
// first field the style sets apart, such as a number, hung before the rest;
// with a hanging indent, every line after the first set in; or as a plain
// paragraph.
void Builder::styledEntry(const Part &part)
{
    const Citations &citations = *m_options.citations;
    const CitationBibliographyEntry &work = citations.bibliography.at(part.index);
    const Node *definition = definitionOf(m_document, work);
    m_sourceBase = definition ? definition->range.start : -1;
    m_section.sourceStart = definition ? definition->range.start : -1;
    m_section.sourceEnd = definition ? definition->range.end : -1;
    Flow flow;
    flow.size = std::round(m_style.smallSize());
    flow.tight = true;
    const Char state = bodyChar(flow);
    const QString first = alignsSecondField(citations) ? firstFieldOf(work.text) : QString();
    const int start = here() + 1;
    if (!first.isEmpty()) {
        if (m_firstField < 0) {
            QFont font(m_style.family);
            font.setPixelSize(std::max(1, int(flow.size)));
            m_firstField = QFontMetricsF(font).horizontalAdvance(longestFirstField(citations) + QChar(0x2002));
        }
        Marker marker;
        marker.pending = true;
        marker.text = first;
        marker.advance = m_firstField;
        flow.left = m_firstField;
        m_marker = &marker;
        openBlock(flow, flow.size, 0, m_style.gap() * 0.2, definition, state);
        m_marker = nullptr;
        CslText rest;
        for (const CslSpan &span : work.text.spans())
            if (span.format.display != CslFormat::Display::LeftMargin) rest.append(span.text, span.format);
        styledText(rest, state, true);
    } else {
        if (citations.layout.hangingIndent) flow.left = m_style.indent();
        openBlock(flow, flow.size, 0, m_style.gap() * 0.2, definition, state);
        if (citations.layout.hangingIndent) {
            QTextBlockFormat format = m_out->cursor.blockFormat();
            format.setTextIndent(-m_style.indent());
            m_out->cursor.setBlockFormat(format);
        }
        styledText(work.text, state, true);
    }
    m_section.names.append({DocumentCitations::entryId(work.key), start, -1});
    if (work.number > 0) m_section.names.append({QStringLiteral("cite-%1").arg(work.number), start, -1});
    raiseBottomMargin(m_style.gap() * 0.5 * std::clamp(citations.layout.entrySpacing, 0.0, 3.0));
}

// A bibliography entry. Everything the bibliography shows of a work is made
// here and in styledEntry().
void Builder::entry(const Part &part)
{
    if (styled()) {
        styledEntry(part);
        return;
    }
    const md::BibliographyEntry &work = m_document.bibliography[size_t(part.index)];
    const bool own = work.file == 0 && work.definition && !work.definition->generated;
    m_sourceBase = own ? work.definition->range.start : -1;
    m_section.sourceStart = own ? work.definition->range.start : -1;
    m_section.sourceEnd = own ? work.definition->range.end : -1;
    Flow flow;
    flow.size = std::round(m_style.smallSize());
    flow.tight = true;
    QFont font(m_style.family);
    font.setPixelSize(std::max(1, int(flow.size)));
    const QFontMetricsF metrics(font);
    Marker marker;
    marker.pending = true;
    marker.text = u'[' + QString::number(work.number) + u']';
    marker.advance = metrics.horizontalAdvance(marker.text + QChar(0x2002));
    flow.left = metrics.horizontalAdvance(u'[' + QString::number(m_document.bibliography.size()) + u']' + QChar(0x2002));
    m_marker = &marker;
    const int start = here() + 1;
    if (work.definition) {
        blocks(work.definition->children, flow);
    } else {
        const Char state = bodyChar(flow);
        openBlock(flow, flow.size, 0, m_style.gap() * 0.2, nullptr, state);
        text(work.key, state);
    }
    flushMarker(flow);
    m_marker = nullptr;
    m_section.names.append({QStringLiteral("cite-%1").arg(work.number), start, -1});
    raiseBottomMargin(m_style.gap() * 0.5);
}

// --- Inline content ---------------------------------------------------------------------

void Builder::inlines(const std::vector<Node> &nodes, const Char &state)
{
    bool afterTask = false;
    for (const Node &node : nodes) {
        if (node.type == NodeType::TaskMarker) {
            afterTask = true;
            continue;
        }
        // The box of a task is its marker: the space after `[ ]` is not text.
        if (afterTask && node.type == NodeType::Text && node.text.startsWith(u' ')) {
            Node rest = node;
            rest.text.remove(0, 1);
            rest.range.start += 1;
            inl(rest, state);
        } else {
            inl(node, state);
        }
        afterTask = false;
    }
}

void Builder::inl(const Node &node, const Char &state)
{
    switch (node.type) {
    case NodeType::Text:
        anchor(node, state.html ? 0 : int(node.text.size()));
        text(node.text, state);
        break;
    case NodeType::Escape:
    case NodeType::Entity:
        anchor(node, 0);
        text(node.text, state);
        break;
    case NodeType::SoftBreak:
        text(QStringLiteral(" "), state);
        break;
    case NodeType::HardBreak:
        m_out->cursor.insertText(QString(QChar::LineSeparator), charFormat(state));
        break;
    case NodeType::Emphasis: {
        Char inner = state;
        inner.italic = true;
        inlines(node.children, inner);
        break;
    }
    case NodeType::Strong: {
        Char inner = state;
        inner.weight = QFont::Bold;
        inlines(node.children, inner);
        break;
    }
    case NodeType::Strikethrough: {
        Char inner = state;
        inner.strike = true;
        inlines(node.children, inner);
        break;
    }
    case NodeType::Highlight: {
        Char inner = state;
        inner.paper = PreviewStyle::MarkPaper;
        if (inner.ink == PreviewStyle::NoInk || inner.ink == PreviewStyle::Heading) inner.ink = PreviewStyle::Marked;
        inlines(node.children, inner);
        break;
    }
    case NodeType::Superscript:
    case NodeType::Subscript: {
        Char inner = state;
        inner.script = node.type == NodeType::Superscript ? 1 : -1;
        inlines(node.children, inner);
        break;
    }
    case NodeType::Code: {
        Char inner = state;
        inner.code = true;
        inner.html = false;
        inner.paper = PreviewStyle::CodePaper;
        if (inner.ink == PreviewStyle::NoInk || inner.ink == PreviewStyle::Heading) inner.ink = PreviewStyle::Code;
        // The code lies after its opening mark; where it reads as it is
        // written, its positions run beside the source's.
        const int start = node.open.isEmpty() ? node.range.start : node.open.end;
        const int length = int(node.text.size());
        const bool same = node.file == 0 && !node.generated && start + length <= node.range.end
                          && m_document.source(0, {start, start + length}) == node.text;
        anchor(node, same ? length : 0, false, start);
        text(node.text, inner);
        break;
    }
    case NodeType::Link:
    case NodeType::Autolink: {
        Char inner = state;
        // An address that would run as a script is no link at all.
        md::Destination target;
        if (md::detail::isSafeUrl(node.dest, false)) target = md::classifyDestination(m_document, node);
        switch (target.kind) {
        case md::Destination::Kind::Empty:
            break;
        case md::Destination::Kind::Anchor:
            inner.href = u'#' + target.fragment;
            break;
        case md::Destination::Kind::LocalFile:
            if (target.absolute) {
                QUrl url = QUrl::fromLocalFile(target.path);
                if (!target.fragment.isEmpty()) url.setFragment(target.fragment);
                inner.href = url.toString();
            } else {
                inner.href = node.dest;
            }
            break;
        default:
            inner.href = node.dest;
            break;
        }
        if (!inner.href.isEmpty()) {
            inner.ink = PreviewStyle::Link;
            inner.underline = true;
            inner.tip = node.title.isEmpty() ? node.dest : node.title;
        }
        anchor(node, 0);
        inlines(node.children, inner);
        break;
    }
    case NodeType::Image:
        image(node, node.dest, md::plainText(node), node.title, state);
        break;
    case NodeType::HtmlInline:
        anchor(node, 0);
        text(node.text, state);
        break;
    case NodeType::HtmlElement:
        htmlInline(node, state);
        break;
    case NodeType::MathInline:
        mathInline(node, state);
        break;
    case NodeType::FootnoteReference:
    case NodeType::InlineNote:
        noteReference(node, state);
        break;
    case NodeType::Citation:
        citation(node, state);
        break;
    case NodeType::Variable:
        anchor(node, 0);
        text(node.resolved ? node.text : m_document.source(node).toString(), state);
        break;
    case NodeType::HtmlComment:
    case NodeType::HeadingLabel:
    case NodeType::CommentLine:
    case NodeType::TaskMarker:
        break;
    default:
        // A block inside a line, as raw HTML can nest them: its text, in line.
        inlines(node.children, state);
        break;
    }
}

void Builder::htmlInline(const Node &node, const Char &state)
{
    const QString &tag = node.label;
    Char inner = state;
    const auto attribute = [&node](QLatin1StringView name) {
        for (const md::Attribute &a : node.attributes)
            if (a.name == name) return a.value;
        return QString();
    };
    if (tag == QLatin1StringView("br")) {
        m_out->cursor.insertText(QString(QChar::LineSeparator), charFormat(state));
        return;
    }
    if (tag == QLatin1StringView("img")) {
        image(node, attribute(QLatin1StringView("src")), attribute(QLatin1StringView("alt")),
              attribute(QLatin1StringView("title")), state);
        return;
    }
    if (tag == QLatin1StringView("b") || tag == QLatin1StringView("strong")) inner.weight = QFont::Bold;
    else if (tag == QLatin1StringView("i") || tag == QLatin1StringView("em")) inner.italic = true;
    else if (tag == QLatin1StringView("u") || tag == QLatin1StringView("ins")) inner.underline = true;
    else if (tag == QLatin1StringView("s") || tag == QLatin1StringView("del")) inner.strike = true;
    else if (tag == QLatin1StringView("sup")) inner.script = 1;
    else if (tag == QLatin1StringView("sub")) inner.script = -1;
    else if (tag == QLatin1StringView("mark")) {
        inner.paper = PreviewStyle::MarkPaper;
        inner.ink = PreviewStyle::Marked;
    } else if (tag == QLatin1StringView("code") || tag == QLatin1StringView("kbd")) {
        inner.code = true;
        inner.paper = PreviewStyle::CodePaper;
        inner.ink = PreviewStyle::Code;
    } else if (tag == QLatin1StringView("a")) {
        const QString href = attribute(QLatin1StringView("href"));
        md::Destination target;
        if (md::detail::isSafeUrl(href, false))
            target = md::classifyDestination(
                href, m_document.files[size_t(std::clamp(node.file, 0, int(m_document.files.size()) - 1))].directory);
        if (target.kind == md::Destination::Kind::Anchor) inner.href = u'#' + target.fragment;
        else if (target.kind == md::Destination::Kind::LocalFile && target.absolute) inner.href = QUrl::fromLocalFile(target.path).toString();
        else if (target.kind != md::Destination::Kind::Empty) inner.href = href;
        if (!inner.href.isEmpty()) {
            inner.ink = PreviewStyle::Link;
            inner.underline = true;
            inner.tip = attribute(QLatin1StringView("title"));
            if (inner.tip.isEmpty()) inner.tip = href;
        }
    }
    anchor(node, 0);
    inlines(node.children, inner);
}

void Builder::picture(const Picture &picture, const QSizeF &size, QTextCharFormat::VerticalAlignment alignment,
                      const Char &state, bool fitColumn)
{
    QTextCursor &cursor = m_out->cursor;
    cursor.document()->addResource(QTextDocument::ImageResource, QUrl(picture.key), picture.image);
    QTextImageFormat format;
    format.setName(picture.key);
    if (size.isValid() && !size.isEmpty()) {
        format.setWidth(size.width());
        // A picture that shrinks to the column keeps its shape, which a
        // height of its own would not let it.
        if (!fitColumn) format.setHeight(size.height());
    }
    if (fitColumn) format.setMaximumWidth(QTextLength(QTextLength::PercentageLength, 100));
    format.setVerticalAlignment(alignment);
    if (!state.href.isEmpty() && m_options.interactive) {
        format.setAnchor(true);
        format.setAnchorHref(state.href);
    }
    if (!picture.description.isEmpty()) format.setToolTip(picture.description);
    cursor.insertImage(format);
}

void Builder::image(const Node &node, const QString &destination, const QString &alt, const QString &title,
                    const Char &state)
{
    const QString base = m_document.files[size_t(std::clamp(node.file, 0, int(m_document.files.size()) - 1))].directory;
    md::Destination target;
    if (md::detail::isSafeUrl(destination, true)) target = md::classifyDestination(destination, base);
    Picture shown;
    shown.state = Picture::Failed;
    if (m_resources && target.kind != md::Destination::Kind::Empty) shown = m_resources->image(target, destination);
    uses(shown);
    anchor(node, 0);
    const QString label = alt.simplified();
    if (shown.state == Picture::Ready && !shown.image.isNull()) {
        if (m_out->island < 0) {
            Mark mark;
            mark.kind = Mark::Image;
            mark.first = mark.last = m_out->cursor.block().position() - m_out->base;
            mark.text = !label.isEmpty() ? label : !title.isEmpty() ? title : tr("Image");
            m_section.marks.append(mark);
        }
        Picture described = shown;
        described.description = !title.isEmpty() ? title : label;
        picture(described, shown.size, QTextCharFormat::AlignNormal, state, true);
        return;
    }
    Char note = state;
    note.italic = true;
    if (shown.state == Picture::Pending) {
        note.ink = PreviewStyle::Dim;
        text(label.isEmpty() ? tr("Loading image…") : tr("Loading image: %1").arg(label), note);
    } else {
        note.ink = PreviewStyle::Danger;
        note.tip = shown.diagnostic;
        const QString what = label.isEmpty() ? destination : label;
        text(shown.diagnostic.isEmpty() ? tr("Image not shown: %1").arg(what)
                                        : tr("Image not shown: %1 (%2)").arg(what, shown.diagnostic),
             note);
    }
}

void Builder::mathInline(const Node &node, const Char &state)
{
    Picture formula;
    if (m_resources) formula = m_resources->math(node.text, node.display, state.size);
    uses(formula);
    anchor(node, 0);
    if (formula.state == Picture::Ready && !formula.image.isNull()) {
        // The line draws a picture with its bottom edge at the line's
        // descent, so a formula sits on the baseline when it reaches as far
        // below it as the text does: room is added beneath a shallower one.
        QFont font(state.code ? m_style.codeFamily : m_style.family);
        font.setPixelSize(std::max(1, int(std::lround(state.size))));
        const qreal descent = QFontMetricsF(font).descent();
        const qreal depth = formula.size.height() - formula.baseline;
        const qreal room = std::round(descent - depth);
        Picture placed = formula;
        QSizeF size = formula.size;
        if (room >= 1) {
            const qreal ratio = formula.image.height() / std::max<qreal>(1, formula.size.height());
            QImage padded(formula.image.width(), formula.image.height() + int(std::lround(room * ratio)),
                          QImage::Format_ARGB32_Premultiplied);
            padded.fill(Qt::transparent);
            QPainter painter(&padded);
            painter.drawImage(0, 0, formula.image);
            painter.end();
            placed.image = padded;
            placed.key = formula.key + QStringLiteral("/below-%1").arg(int(room));
            size.setHeight(size.height() + room);
        }
        if (placed.description.isEmpty()) placed.description = node.text;
        // Told to assistive technology with what it says, as a picture is.
        if (m_out->island < 0) {
            Mark mark;
            mark.kind = Mark::Image;
            mark.first = mark.last = m_out->cursor.block().position() - m_out->base;
            mark.text = tr("Formula: %1").arg(placed.description);
            m_section.marks.append(mark);
        }
        picture(placed, size, QTextCharFormat::AlignBottom, state, false);
        return;
    }
    Char source = state;
    source.code = true;
    source.html = false;
    if (formula.state == Picture::Failed) {
        source.ink = PreviewStyle::Danger;
        source.tip = formula.diagnostic.isEmpty() ? tr("The formula could not be drawn.") : formula.diagnostic;
    } else {
        source.ink = PreviewStyle::Dim;
    }
    text(node.text, source);
}

void Builder::noteReference(const Node &node, const Char &state)
{
    anchor(node, 0);
    if (node.number <= 0 || node.number > int(m_document.footnotes.size())) {
        text(m_document.source(node).toString(), state);
        return;
    }
    noteMark(node.number, state);
}

void Builder::noteMark(int number, const Char &state)
{
    name(QStringLiteral("fnref-%1").arg(number));
    Char mark = state;
    mark.script = 1;
    mark.ink = PreviewStyle::Link;
    mark.href = QStringLiteral("#fn-%1").arg(number);
    mark.tip = tr("Note %1").arg(number);
    mark.italic = false;
    text(QString::number(number), mark);
}

bool Builder::styled() const { return active(m_options.citations) != nullptr; }

int Builder::citationNote(const Node &node)
{
    // Only a note style makes notes of citations.
    if (!styled() || !m_options.citations->noteStyle) return 0;
    if (!m_citationNotesRead) {
        m_citationNotesRead = true;
        for (const md::Footnote &note : m_document.footnotes)
            if (note.content && note.content->type == NodeType::Citation) m_citationNotes.insert(note.content, note.number);
    }
    return m_citationNotes.value(&node, 0);
}

void Builder::citation(const Node &node, const Char &state)
{
    anchor(node, 0);
    // In a note style a citation in the text is a note: its mark stands
    // here and its words are the note's.
    if (const int number = citationNote(node)) {
        noteMark(number, state);
        return;
    }
    citationText(node, state);
}

void Builder::styledText(const CslText &words, const Char &state, bool lines)
{
    bool apart = false;
    bool written = false;
    for (const CslSpan &span : words.spans()) {
        const CslFormat &format = span.format;
        Char run = state;
        if (format.italic) run.italic = true;
        if (format.bold) run.weight = QFont::Bold;
        if (format.underline) run.underline = true;
        if (format.smallCaps) run.smallCaps = true;
        if (format.position == CslFormat::Position::Superscript) run.script = 1;
        else if (format.position == CslFormat::Position::Subscript) run.script = -1;
        // An address the style links, such as a DOI, unless it would run as a script.
        if (!format.link.isEmpty() && md::detail::isSafeUrl(format.link, false)) {
            run.href = format.link;
            run.ink = PreviewStyle::Link;
            run.underline = true;
            run.tip = format.link;
        }
        const bool block = lines && (format.display == CslFormat::Display::Block || format.display == CslFormat::Display::Indent);
        // What the style sets apart stands on a line of its own.
        if (written && (block || apart)) m_out->cursor.insertText(QString(QChar::LineSeparator), charFormat(state));
        text(span.text, run);
        apart = block;
        written = true;
    }
}

void Builder::citationText(const Node &node, const Char &state)
{
    if (styled()) {
        const Citations &citations = *m_options.citations;
        const Citations::Citation *words = citations.find(node);
        if (words && !words->text.isEmpty()) {
            Char run = state;
            const QStringList missing = missingKeys(node);
            if (!missing.isEmpty()) {
                // A key nothing defines stays in view, and says what it lacks.
                run.ink = PreviewStyle::Danger;
                run.tip = missing.size() == 1 ? tr("“%1” is not in the bibliography").arg(missing.first())
                                              : tr("Not in the bibliography: %1").arg(missing.join(QStringLiteral(", ")));
            } else if (citations.linkCitations && words->keys.size() == 1) {
                // One work, when the document asks for links to its entries.
                QString key = words->keys.first().trimmed();
                if (key.startsWith(u'#') || key.startsWith(u'@')) key = key.mid(1);
                const bool listed = std::any_of(citations.bibliography.cbegin(), citations.bibliography.cend(),
                                                [&](const CitationBibliographyEntry &entry) { return entry.key == key; });
                const bool linked = std::any_of(words->text.spans().cbegin(), words->text.spans().cend(),
                                                [](const CslSpan &span) { return !span.format.link.isEmpty(); });
                if (listed && !linked) {
                    run.href = u'#' + DocumentCitations::entryId(key);
                    run.ink = PreviewStyle::Link;
                }
            }
            styledText(words->text, run);
            return;
        }
    }
    // A citation of nothing that is defined is shown as it is written, and
    // says what it lacks.
    const auto asWritten = [&](const Node &item) {
        Char written = state;
        if (!item.resolved && !item.label.isEmpty()) {
            written.ink = PreviewStyle::Danger;
            written.tip = tr("“%1” is not defined").arg(item.label);
        }
        text(m_document.source(item).toString(), written);
    };
    if (node.children.empty() && node.number <= 0) {
        asWritten(node);
        return;
    }
    const auto one = [&](const Node &item) {
        if (item.number <= 0) {
            asWritten(item);
            return;
        }
        // A prefix, as in `[see @key]`, comes before the number.
        if (!item.title.isEmpty()) text(item.title + u' ', state);
        Char link = state;
        link.ink = PreviewStyle::Link;
        link.href = QStringLiteral("#cite-%1").arg(item.number);
        link.tip = item.label;
        text(QString::number(item.number), link);
        if (!item.text.isEmpty()) text(QStringLiteral(", ") + item.text, state);
    };
    text(QStringLiteral("["), state);
    if (node.children.empty()) {
        one(node);
    } else {
        bool first = true;
        for (const Node &item : node.children) {
            if (!first) text(QStringLiteral("; "), state);
            first = false;
            one(item);
        }
    }
    text(QStringLiteral("]"), state);
}

// --- Parts --------------------------------------------------------------------------------

Section Builder::build(const Part &part, QTextCursor &cursor)
{
    m_section = Section();
    m_section.kind = part.kind;
    m_section.key = part.key;
    m_marker = nullptr;
    const int before = cursor.position();
    Out out;
    out.cursor = cursor;
    out.anchors = &m_section.anchors;
    // In place of islands, a table leaves an empty block for what follows it.
    out.reuse = m_reuseBlock;
    out.base = out.reuse ? before : before + 1;
    m_out = &out;
    Flow flow;
    flow.size = m_style.size;
    switch (part.kind) {
    case Part::Block: {
        const Node &node = *part.node;
        const bool own = node.file == 0 && !node.generated;
        m_sourceBase = own ? node.range.start : -1;
        m_section.sourceStart = own ? node.range.start : -1;
        m_section.sourceEnd = own ? node.range.end : -1;
        block(node, flow);
        break;
    }
    case Part::NotesRule:
    case Part::BibliographyRule:
        m_sourceBase = -1;
        m_section.sourceStart = m_section.sourceEnd = -1;
        rule(flow, nullptr, Mark::NotesRule);
        break;
    case Part::Note:
        note(part);
        break;
    case Part::Entry:
        entry(part);
        break;
    }
    m_reuseBlock = out.reuse;
    cursor = out.cursor;
    m_section.length = std::max(0, cursor.position() - before);
    m_out = nullptr;
    return std::move(m_section);
}

void Builder::prepare(QTextDocument *document)
{
    document->setUndoRedoEnabled(false);
    document->setDocumentMargin(0);
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.removeSelectedText();
    QTextBlockFormat first;
    first.setLineHeight(1, QTextBlockFormat::FixedHeight);
    QTextCharFormat tiny;
    tiny.setProperty(QTextFormat::FontPixelSize, 1);
    cursor.setBlockFormat(first);
    cursor.setBlockCharFormat(tiny);
    cursor.endEditBlock();
}

std::vector<Section> Builder::buildAll(QTextDocument *document)
{
    prepare(document);
    document->setDefaultFont(m_style.bodyFont());
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    m_reuseBlock = false;
    std::vector<Section> sections;
    for (const Part &part : parts(m_document, m_options.citations)) sections.push_back(build(part, cursor));
    cursor.endEditBlock();
    return sections;
}

} // namespace preview
