#include "mdtree.h"

#include <QDir>
#include <QUrl>

#include <algorithm>

namespace md {
namespace {

struct ExtensionName {
    const char *name;
    bool Options::*flag;
};

const ExtensionName extensions[] = {
    {"tables", &Options::tables},
    {"strikethrough", &Options::strikethrough},
    {"task-lists", &Options::taskLists},
    {"autolinks", &Options::autolinks},
    {"front-matter", &Options::frontMatter},
    {"math", &Options::math},
    {"mermaid", &Options::mermaid},
    {"definition-lists", &Options::definitionLists},
    {"footnotes", &Options::footnotes},
    {"citations", &Options::citations},
    {"highlight", &Options::highlight},
    {"super-subscript", &Options::superSubscript},
    {"line-comments", &Options::lineComments},
    {"page-breaks", &Options::pageBreaks},
    {"toc", &Options::toc},
    {"variables", &Options::variables},
    {"includes", &Options::includes},
    {"merged-cells", &Options::mergedCells},
    {"heading-labels", &Options::headingLabels},
};

// A range of the document's own text that plain text keeps to.
struct Clip {
    QStringView text; // file 0
    Range range;

    // Nothing of the node lies in the range. A node of no length lies in it
    // when its position does.
    bool excludes(const Node &node) const
    {
        if (node.file != 0) return true;
        if (node.range.isEmpty()) return !range.contains(node.range.start);
        return node.range.end <= range.start || node.range.start >= range.end;
    }
    bool holds(const Node &node) const { return node.range.start >= range.start && node.range.end <= range.end; }
    // The source of the part of `part` inside the range.
    QStringView cut(Range part) const
    {
        const int start = std::clamp(std::max(part.start, range.start), 0, int(text.size()));
        const int end = std::clamp(std::min(part.end, range.end), start, int(text.size()));
        return text.sliced(start, end - start);
    }
};

// What a code span, inline math or code block holds, between its markers.
Range contentOf(const Node &node)
{
    return {node.open.isEmpty() ? node.range.start : node.open.end,
            node.close.isEmpty() ? node.range.end : node.close.start};
}

void appendPlain(const Node &node, QString &out, const Clip *clip = nullptr)
{
    if (clip && clip->excludes(node)) return;
    switch (node.type) {
    case NodeType::Text:
        if (clip) out.append(clip->cut(node.range));
        else out.append(node.text);
        return;
    case NodeType::Code:
    case NodeType::MathInline:
        if (clip && !clip->holds(node)) out.append(clip->cut(contentOf(node)));
        else out.append(node.text);
        return;
    case NodeType::Escape:
    case NodeType::Entity:
        out.append(node.text);
        return;
    case NodeType::SoftBreak:
    case NodeType::HardBreak:
        out.append(u' ');
        return;
    case NodeType::Variable:
        if (node.resolved) out.append(node.text);
        else out.append(QStringLiteral("[%%1]").arg(node.label));
        return;
    case NodeType::FootnoteReference:
    case NodeType::InlineNote:
    case NodeType::Citation:
    case NodeType::HtmlInline:
    case NodeType::HtmlComment:
    case NodeType::CommentLine:
    case NodeType::HeadingLabel:
    case NodeType::TaskMarker:
        return;
    default:
        break;
    }
    for (const Node &child : node.children) appendPlain(child, out, clip);
}

// The text of the blocks of a document, one block per line.
void appendBlocks(const Node &node, const PlainTextOptions &options, QString &out, const Clip *clip = nullptr)
{
    if (clip && clip->excludes(node)) return;
    auto line = [&](const QString &text) {
        if (text.isEmpty()) return;
        out.append(text);
        out.append(u'\n');
    };
    switch (node.type) {
    case NodeType::Paragraph:
    case NodeType::Heading:
    case NodeType::DefinitionTerm: {
        QString text;
        appendPlain(node, text, clip);
        if (options.notes) {
            // Inline notes are read where they are written.
            std::vector<const Node *> pending{&node};
            while (!pending.empty()) {
                const Node *current = pending.back();
                pending.pop_back();
                for (const Node &child : current->children) {
                    if (child.type == NodeType::InlineNote) {
                        if (clip && clip->excludes(child)) continue;
                        text.append(u' ');
                        for (const Node &inner : child.children) appendPlain(inner, text, clip);
                    } else if (!child.children.empty()) {
                        pending.push_back(&child);
                    }
                }
            }
        }
        line(text.trimmed());
        return;
    }
    case NodeType::CodeBlock:
        if (!options.code) return;
        if (clip && !clip->holds(node)) line(clip->cut(contentOf(node)).trimmed().toString());
        else line(node.text.trimmed());
        return;
    case NodeType::TableRow: {
        QStringList cells;
        for (const Node &cell : node.children) {
            QString text;
            appendPlain(cell, text, clip);
            cells.append(text.trimmed());
        }
        line(cells.join(u'\t').trimmed());
        return;
    }
    case NodeType::FootnoteDefinition:
        if (!options.notes) return;
        break;
    case NodeType::HtmlBlock: {
        QString text;
        appendPlain(node, text, clip);
        line(text.simplified());
        return;
    }
    case NodeType::FrontMatter:
    case NodeType::LinkDefinition:
    case NodeType::CitationDefinition:
    case NodeType::CommentLine:
    case NodeType::MathBlock:
    case NodeType::MermaidBlock:
    case NodeType::TableOfContents:
    case NodeType::ThematicBreak:
    case NodeType::PageBreak:
        return;
    default:
        break;
    }
    for (const Node &child : node.children) appendBlocks(child, options, out, clip);
}

// The Include node whose children are the blocks of a file.
const Node *findInclude(const Node &scope, int file)
{
    for (const Node &child : scope.children) {
        if (child.type != NodeType::Include) continue;
        if (child.includedFile == file) return &child;
        if (const Node *inner = findInclude(child, file)) return inner;
    }
    return nullptr;
}

bool skippedForProse(NodeType type)
{
    switch (type) {
    case NodeType::Code:
    case NodeType::CodeBlock:
    case NodeType::MermaidBlock:
    case NodeType::MathBlock:
    case NodeType::MathInline:
    case NodeType::HtmlBlock:
    case NodeType::HtmlInline:
    case NodeType::HtmlComment:
    case NodeType::CommentLine:
    case NodeType::FrontMatter:
    case NodeType::LinkDefinition:
    case NodeType::Autolink:
    case NodeType::Variable:
    case NodeType::Citation:
    case NodeType::FootnoteReference:
    case NodeType::HeadingLabel:
    case NodeType::TableOfContents:
        return true;
    default:
        return false;
    }
}

void collectProse(const Node &node, int file, const std::function<void(Range)> &visit)
{
    if (node.type == NodeType::Text) {
        if (node.file == file && !node.generated && !node.range.isEmpty()) visit(node.range);
        return;
    }
    if (skippedForProse(node.type)) return;
    for (const Node &child : node.children) collectProse(child, file, visit);
}

} // namespace

Options Options::writer()
{
    return Options();
}

Options Options::standard()
{
    Options o;
    o.profile = Profile::Standard;
    o.frontMatter = o.math = o.mermaid = o.definitionLists = o.footnotes = o.citations = false;
    o.highlight = o.superSubscript = o.lineComments = o.pageBreaks = o.toc = o.variables = false;
    o.includes = o.mergedCells = o.headingLabels = false;
    return o;
}

Options Options::forProfile(Profile profile)
{
    return profile == Profile::Standard ? standard() : writer();
}

QString Options::profileName() const
{
    return profile == Profile::Standard ? QStringLiteral("standard") : QStringLiteral("writer");
}

QStringList Options::extensionNames()
{
    QStringList names;
    for (const ExtensionName &e : extensions) names.append(QLatin1StringView(e.name));
    return names;
}

QStringList Options::enabledExtensions() const
{
    QStringList names;
    for (const ExtensionName &e : extensions)
        if (this->*e.flag) names.append(QLatin1StringView(e.name));
    return names;
}

bool Options::extension(QStringView name) const
{
    for (const ExtensionName &e : extensions)
        if (name == QLatin1StringView(e.name)) return this->*e.flag;
    return false;
}

bool Options::setExtension(QStringView name, bool on)
{
    for (const ExtensionName &e : extensions) {
        if (name == QLatin1StringView(e.name)) {
            this->*e.flag = on;
            return true;
        }
    }
    return false;
}

const char *typeName(NodeType type)
{
    switch (type) {
    case NodeType::Document: return "document";
    case NodeType::FrontMatter: return "front-matter";
    case NodeType::Paragraph: return "paragraph";
    case NodeType::Heading: return "heading";
    case NodeType::ThematicBreak: return "thematic-break";
    case NodeType::BlockQuote: return "block-quote";
    case NodeType::List: return "list";
    case NodeType::ListItem: return "list-item";
    case NodeType::CodeBlock: return "code-block";
    case NodeType::MermaidBlock: return "mermaid-block";
    case NodeType::MathBlock: return "math-block";
    case NodeType::HtmlBlock: return "html-block";
    case NodeType::LinkDefinition: return "link-definition";
    case NodeType::Table: return "table";
    case NodeType::TableRow: return "table-row";
    case NodeType::TableCell: return "table-cell";
    case NodeType::DefinitionList: return "definition-list";
    case NodeType::DefinitionTerm: return "definition-term";
    case NodeType::DefinitionDescription: return "definition-description";
    case NodeType::FootnoteDefinition: return "footnote-definition";
    case NodeType::CitationDefinition: return "citation-definition";
    case NodeType::PageBreak: return "page-break";
    case NodeType::TableOfContents: return "table-of-contents";
    case NodeType::Include: return "include";
    case NodeType::CommentLine: return "comment-line";
    case NodeType::Text: return "text";
    case NodeType::Escape: return "escape";
    case NodeType::Entity: return "entity";
    case NodeType::SoftBreak: return "soft-break";
    case NodeType::HardBreak: return "hard-break";
    case NodeType::Emphasis: return "emphasis";
    case NodeType::Strong: return "strong";
    case NodeType::Strikethrough: return "strikethrough";
    case NodeType::Highlight: return "highlight";
    case NodeType::Superscript: return "superscript";
    case NodeType::Subscript: return "subscript";
    case NodeType::Code: return "code";
    case NodeType::Link: return "link";
    case NodeType::Image: return "image";
    case NodeType::Autolink: return "autolink";
    case NodeType::HtmlInline: return "html-inline";
    case NodeType::HtmlElement: return "html-element";
    case NodeType::HtmlComment: return "html-comment";
    case NodeType::MathInline: return "math-inline";
    case NodeType::FootnoteReference: return "footnote-reference";
    case NodeType::InlineNote: return "inline-note";
    case NodeType::Citation: return "citation";
    case NodeType::Variable: return "variable";
    case NodeType::TaskMarker: return "task-marker";
    case NodeType::HeadingLabel: return "heading-label";
    }
    return "unknown";
}

const char *codeName(DiagnosticCode code)
{
    switch (code) {
    case DiagnosticCode::UnresolvedReference: return "unresolved-reference";
    case DiagnosticCode::UnresolvedAnchor: return "unresolved-anchor";
    case DiagnosticCode::AmbiguousHeadingReference: return "ambiguous-heading-reference";
    case DiagnosticCode::MissingFootnote: return "missing-footnote";
    case DiagnosticCode::DuplicateFootnote: return "duplicate-footnote";
    case DiagnosticCode::UnusedFootnote: return "unused-footnote";
    case DiagnosticCode::MissingCitation: return "missing-citation";
    case DiagnosticCode::DuplicateCitation: return "duplicate-citation";
    case DiagnosticCode::UnresolvedVariable: return "unresolved-variable";
    case DiagnosticCode::MalformedTableRow: return "malformed-table-row";
    case DiagnosticCode::UnclosedMath: return "unclosed-math";
    case DiagnosticCode::FrontMatter: return "front-matter";
    case DiagnosticCode::UnsupportedHtml: return "unsupported-html";
    case DiagnosticCode::IncludeNotFound: return "include-not-found";
    case DiagnosticCode::IncludeUnreadable: return "include-unreadable";
    case DiagnosticCode::IncludeCycle: return "include-cycle";
    case DiagnosticCode::IncludeDepthLimit: return "include-depth-limit";
    case DiagnosticCode::IncludeSizeLimit: return "include-size-limit";
    case DiagnosticCode::IncludeCountLimit: return "include-count-limit";
    case DiagnosticCode::IncludeNoLibrary: return "include-no-library";
    case DiagnosticCode::IncludeNoBaseFolder: return "include-no-base-folder";
    case DiagnosticCode::IncludeOutside: return "include-outside";
    }
    return "unknown";
}

const char *severityName(Severity severity)
{
    switch (severity) {
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    }
    return "unknown";
}

const FrontMatter &Document::frontMatter() const
{
    static const FrontMatter none;
    return files.empty() ? none : files.front().frontMatter;
}

QStringView Document::source(int file, Range range) const
{
    if (file < 0 || file >= int(files.size())) return {};
    const QString &text = files[file].text;
    const int start = std::clamp(range.start, 0, int(text.size()));
    const int end = std::clamp(range.end, start, int(text.size()));
    return QStringView(text).sliced(start, end - start);
}

std::vector<const Node *> Document::pathAt(int position, int file) const
{
    std::vector<const Node *> path;
    const Node *scope = &root;
    if (file != 0) {
        scope = findInclude(root, file);
        if (!scope) return path;
    } else if (position < root.range.start || position > root.range.end) {
        return path;
    }
    path.push_back(scope);
    const Node *node = scope;
    for (;;) {
        // An include's children belong to another file, except from the include itself.
        if (node->type == NodeType::Include && node != scope) break;
        const std::vector<Node> &children = node->children;
        if (children.empty() || children.front().generated) break;
        // The last child that starts at or before the position.
        auto it = std::upper_bound(children.begin(), children.end(), position,
                                   [](int p, const Node &child) { return p < child.range.start; });
        if (it == children.begin()) break;
        const Node *next = &*(it - 1);
        if (position > next->range.end) break;
        path.push_back(next);
        node = next;
    }
    return path;
}

const Node *Document::nodeAt(int position, int file) const
{
    const std::vector<const Node *> path = pathAt(position, file);
    return path.empty() ? nullptr : path.back();
}

void Document::forEachProseRange(int file, const std::function<void(Range)> &visit) const
{
    collectProse(root, file, visit);
}

std::vector<Range> Document::proseRanges(int file) const
{
    std::vector<Range> ranges;
    forEachProseRange(file, [&](Range range) { ranges.push_back(range); });
    return ranges;
}

QString Document::plainText(const PlainTextOptions &options) const
{
    QString out;
    if (options.range) {
        if (files.empty() || options.range->isEmpty()) return out;
        const Clip clip{files.front().text, *options.range};
        appendBlocks(root, options, out, &clip);
    } else {
        appendBlocks(root, options, out);
    }
    if (out.endsWith(u'\n')) out.chop(1);
    return out;
}

QString plainText(const Node &node)
{
    QString out;
    if (node.isInline() || node.children.empty() || node.children.front().isInline()) {
        appendPlain(node, out);
        return out;
    }
    switch (node.type) {
    case NodeType::FootnoteDefinition:
    case NodeType::CitationDefinition:
    case NodeType::TableOfContents:
        // Asked for by name, a block that is otherwise passed over is read.
        for (const Node &child : node.children) appendBlocks(child, PlainTextOptions(), out);
        break;
    default:
        appendBlocks(node, PlainTextOptions(), out);
        break;
    }
    if (out.endsWith(u'\n')) out.chop(1);
    return out;
}

QString slugify(QStringView headingText)
{
    QString slug;
    slug.reserve(headingText.size());
    bool space = false;
    for (qsizetype i = 0; i < headingText.size(); ++i) {
        char32_t c = headingText[i].unicode();
        if (QChar::isHighSurrogate(c) && i + 1 < headingText.size() && headingText[i + 1].isLowSurrogate()) {
            c = QChar::surrogateToUcs4(headingText[i], headingText[i + 1]);
            ++i;
        }
        if (QChar::isSpace(c)) {
            space = !slug.isEmpty();
            continue;
        }
        const bool keep = QChar::isLetterOrNumber(c) || QChar::isMark(c) || c == u'-' || c == u'_';
        if (!keep) continue;
        if (space) {
            slug.append(u'-');
            space = false;
        }
        slug.append(QChar::fromUcs4(QChar::toLower(c)));
    }
    return slug;
}

Destination classifyDestination(const QString &destination, const QString &baseDirectory)
{
    Destination result;
    if (destination.isEmpty()) return result;
    if (destination.startsWith(u'#')) {
        result.kind = Destination::Kind::Anchor;
        result.fragment = QUrl::fromPercentEncoding(destination.mid(1).toUtf8());
        return result;
    }
    if (destination.startsWith(QLatin1StringView("//"))) {
        result.kind = Destination::Kind::Remote;
        return result;
    }
    // A scheme is two or more characters, so that a drive letter is not one.
    qsizetype colon = -1;
    for (qsizetype i = 0; i < destination.size(); ++i) {
        const char16_t c = destination[i].unicode();
        const bool alpha = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
        if (c == u':') {
            if (i >= 2) colon = i;
            break;
        }
        if (!alpha && !(i > 0 && ((c >= u'0' && c <= u'9') || c == u'+' || c == u'-' || c == u'.'))) break;
    }
    QString local = destination;
    if (colon > 0) {
        result.scheme = destination.first(colon).toLower();
        if (result.scheme == u"http" || result.scheme == u"https") {
            result.kind = Destination::Kind::Remote;
            return result;
        }
        if (result.scheme == u"mailto") {
            result.kind = Destination::Kind::Mail;
            return result;
        }
        if (result.scheme != u"file") {
            result.kind = Destination::Kind::OtherScheme;
            return result;
        }
        const QUrl url(destination);
        local = url.toLocalFile();
        result.fragment = url.fragment();
        result.kind = Destination::Kind::LocalFile;
        result.path = local;
        result.absolute = local.startsWith(u'/');
        return result;
    }
    result.kind = Destination::Kind::LocalFile;
    const qsizetype hash = local.indexOf(u'#');
    if (hash >= 0) {
        result.fragment = QUrl::fromPercentEncoding(local.mid(hash + 1).toUtf8());
        local.truncate(hash);
    }
    local = QUrl::fromPercentEncoding(local.toUtf8());
    if (local.startsWith(u'/')) {
        result.path = QDir::cleanPath(local);
        result.absolute = true;
    } else if (!baseDirectory.isEmpty()) {
        result.path = QDir::cleanPath(baseDirectory + u'/' + local);
        result.absolute = true;
    } else {
        result.path = local;
    }
    return result;
}

QString encodeDestination(QStringView path)
{
    static const char hex[] = "0123456789ABCDEF";
    QString out;
    out.reserve(path.size());
    for (const QChar ch : path) {
        const char16_t c = ch.unicode();
        const bool encode = c <= 0x20 || c == 0x7f || c == u'(' || c == u')' || c == u'<' || c == u'>' || c == u'#'
            || c == u'%' || c == u'\\' || c == u'"' || c == u'&' || c == u':';
        if (!encode) {
            out.append(ch);
            continue;
        }
        out.append(u'%');
        out.append(QLatin1Char(hex[(c >> 4) & 15]));
        out.append(QLatin1Char(hex[c & 15]));
    }
    return out;
}

Destination classifyDestination(const Document &document, const Node &linkOrImage)
{
    const int file = linkOrImage.file;
    const QString base = file >= 0 && file < int(document.files.size()) ? document.files[file].directory : QString();
    return classifyDestination(linkOrImage.dest, base);
}

} // namespace md
