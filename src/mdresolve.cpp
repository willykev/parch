// md::parse(): reads the document's blocks, assembles the files it includes
// into the same tree, reads the inline content of every leaf with the scopes
// of its file, and then resolves what only the whole document can settle:
// heading slugs and references, note and citation numbers, list numbers and
// the table of contents.
#include "mdinclude.h"
#include "mdparser_p.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

#include <algorithm>
#include <atomic>

namespace md {
namespace detail {
namespace {

QString folderOf(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    if (slash < 0) return QString();
    return slash == 0 ? QStringLiteral("/") : path.first(slash);
}

qint64 utf8Size(const QString &text)
{
    qint64 bytes = 0;
    for (const QChar ch : text) {
        const char16_t c = ch.unicode();
        // Each half of a surrogate pair counts two of the pair's four bytes.
        bytes += c < 0x80 ? 1 : (c < 0x800 || QChar::isSurrogate(c)) ? 2 : 3;
    }
    return bytes;
}

// The size guard as the writer set it: "100 MiB", "512 KiB", "40 bytes".
QString sizeText(qint64 bytes)
{
    constexpr qint64 kib = 1024, mib = kib * 1024;
    if (bytes >= mib && bytes % mib == 0) return QStringLiteral("%1 MiB").arg(bytes / mib);
    if (bytes >= kib && bytes % kib == 0) return QStringLiteral("%1 KiB").arg(bytes / kib);
    return bytes == 1 ? QStringLiteral("1 byte") : QStringLiteral("%1 bytes").arg(bytes);
}

class Assembler {
public:
    Assembler(ParseState &state, Document &document, const Source &source)
        : st(state), doc(document), source(source), inlines(state)
    {
    }

    void run(const QString &text);

private:
    void fail(Block *block, DiagnosticCode code, const QString &message);
    void expandIncludes(Block *container, int depth);
    void include(Block *block, int depth);
    bool inReach(const QString &canonicalPath);
    QString displayPath(const QString &path) const;

    void collectHeadings(Block *block);
    void prepareHeadings(Block *root);
    void convert(Block *block, Node &node);

    void structure(Node &node);
    void flow(Node &node);
    void numberNote(Node &node);
    void numberCitation(Node &node);
    void resolve();
    Node tocList(size_t &index, int level, const Node &toc) const;

    ParseState &st;
    Document &doc;
    const Source &source;
    InlineParser inlines;

    struct Loaded {
        int file = -1;
        qint64 bytes = 0;
        QString canonicalPath;
    };
    QHash<QString, Loaded> loaded;  // by the absolute path asked for
    QHash<QString, int> fileIds;    // by canonical path
    QStringList chain;              // canonical paths of the files being read
    qint64 expandedBytes = 0;
    int includeCount = 0;
    QSet<QString> slugs;
    QHash<QString, int> nextSuffix;
    std::vector<QString> headingSlugs; // by heading index
    std::vector<std::pair<Node *, int>> headingLinks;
    std::vector<Node *> anchorLinks;
    std::vector<Node *> tocs;
    std::vector<Node *> numbered;      // note references and citations
    QHash<quint64, int> inlineNotes; // by file and position, in included files
    int lastFootnote = 0;
    int lastCitation = 0;
    // How many notes the flow is inside: a citation in one is no note of its own.
    int noteDepth = 0;
};

void Assembler::fail(Block *block, DiagnosticCode code, const QString &message)
{
    st.diagnose(block->file, {block->start, block->end}, Severity::Error, code, message);
    doc.incomplete = true;
}

// A path as a diagnostic shows it: relative to the document where possible.
QString Assembler::displayPath(const QString &path) const
{
    const QString &base = doc.files.front().directory;
    if (!base.isEmpty() && path.startsWith(base + u'/')) return path.mid(base.size() + 1);
    return path;
}

// Whether an include's file, by its canonical path, lies within the library
// or the document's folder.
bool Assembler::inReach(const QString &canonicalPath)
{
    for (const QString &folder : std::as_const(doc.resourceFolders))
        if (isWithin(canonicalPath, folder)) return true;
    return false;
}

void Assembler::expandIncludes(Block *container, int depth)
{
    for (Block *block = container->firstChild; block; block = block->next)
        if (block->type == NodeType::Include) include(block, depth);
}

void Assembler::include(Block *block, int depth)
{
    const QString &written = block->dest;
    const QString &directory = doc.files[block->file].directory;
    if (written.startsWith(u'/') && source.libraryRoot.isEmpty()) {
        fail(block, DiagnosticCode::IncludeNoLibrary,
             QStringLiteral("\"%1\" is found from the library folder, and this document is not in a library").arg(written));
        return;
    }
    if (!written.startsWith(u'/') && directory.isEmpty()) {
        fail(block, DiagnosticCode::IncludeNoBaseFolder,
             QStringLiteral("\"%1\" is found from the document's folder; save the document first").arg(written));
        return;
    }
    if (depth >= source.maxIncludeDepth) {
        fail(block, DiagnosticCode::IncludeDepthLimit,
             QStringLiteral("Includes are nested more than %1 deep; \"%2\" is not included")
                 .arg(source.maxIncludeDepth).arg(written));
        return;
    }
    if (includeCount >= source.maxIncludeCount) {
        fail(block, DiagnosticCode::IncludeCountLimit,
             QStringLiteral("More than %1 includes; \"%2\" is not included").arg(source.maxIncludeCount).arg(written));
        return;
    }
    ++includeCount;

    const QString target = includeTarget(written, directory, source.libraryRoot);
    // Only a file within the library, or the document's folder, is read: the
    // file itself, every link resolved, and once more the file that was read.
    const auto outside = [&] {
        fail(block, DiagnosticCode::IncludeOutside,
             source.libraryRoot.isEmpty()
                 ? QStringLiteral("\"%1\" is outside the document's folder, so it is not included").arg(written)
                 : QStringLiteral("\"%1\" is outside the library, so it is not included").arg(written));
    };
    if (const QString canonical = source.resolver->canonicalPath(target); !canonical.isEmpty() && !inReach(canonical)) {
        outside();
        return;
    }
    auto known = loaded.constFind(target);
    if (known == loaded.constEnd()) {
        IncludeResolver::File file = source.resolver->read(target, source.maxExpandedBytes - expandedBytes);
        if (file.ok && !inReach(file.canonicalPath)) {
            outside();
            return;
        }
        if (file.tooLarge) {
            fail(block, DiagnosticCode::IncludeSizeLimit,
                 QStringLiteral("The document and its includes are larger than %1; \"%2\" is not included")
                     .arg(sizeText(source.maxExpandedBytes), written));
            return;
        }
        if (!file.ok) {
            fail(block, file.missing ? DiagnosticCode::IncludeNotFound : DiagnosticCode::IncludeUnreadable,
                 file.missing ? QStringLiteral("The included file \"%1\" does not exist").arg(written)
                              : QStringLiteral("The included file \"%1\" cannot be read: %2").arg(written, file.error));
            return;
        }
        Loaded entry;
        entry.bytes = file.bytes;
        entry.canonicalPath = file.canonicalPath;
        const auto id = fileIds.constFind(file.canonicalPath);
        if (id != fileIds.constEnd()) {
            entry.file = *id;
        } else {
            SourceFile sourceFile;
            sourceFile.path = file.canonicalPath;
            sourceFile.directory = folderOf(file.canonicalPath);
            sourceFile.text = file.text;
            sourceFile.includedFrom = block->file;
            doc.files.push_back(sourceFile);
            st.files.emplace_back();
            st.files.back().text = file.text;
            entry.file = int(doc.files.size()) - 1;
            fileIds.insert(file.canonicalPath, entry.file);
        }
        known = loaded.insert(target, entry);
    }
    const Loaded entry = *known;

    if (chain.contains(entry.canonicalPath)) {
        QStringList names;
        for (const QString &path : std::as_const(chain)) names.append(displayPath(path));
        names.append(displayPath(entry.canonicalPath));
        fail(block, DiagnosticCode::IncludeCycle,
             QStringLiteral("This include leads back to itself: %1").arg(names.join(QStringLiteral(" → "))));
        return;
    }
    if (expandedBytes + entry.bytes > source.maxExpandedBytes) {
        fail(block, DiagnosticCode::IncludeSizeLimit,
             QStringLiteral("The document and its includes are larger than %1; \"%2\" is not included")
                 .arg(sizeText(source.maxExpandedBytes), written));
        return;
    }
    expandedBytes += entry.bytes;

    st.runs.emplace_back();
    st.runs.back().file = entry.file;
    block->resolved = true;
    block->includedFile = entry.file;
    parseBlocks(st, block, entry.file, int(st.runs.size()) - 1);
    chain.append(entry.canonicalPath);
    expandIncludes(block, depth + 1);
    chain.removeLast();
}

void Assembler::collectHeadings(Block *block)
{
    for (Block *child = block->firstChild; child; child = child->next) {
        if (child->type == NodeType::Heading) {
            child->heading = int(st.headings.size());
            st.headings.push_back(child);
        } else if (child->firstChild) {
            collectHeadings(child);
        }
    }
}

// Headings are read before everything else: their labels and texts are what
// `[text][Label]` and `[Title][]` resolve against.
void Assembler::prepareHeadings(Block *root)
{
    collectHeadings(root);
    auto add = [](QHash<QString, Alias> &table, const QString &key, int heading) {
        if (key.isEmpty()) return;
        const auto it = table.find(key);
        if (it == table.end()) table.insert(key, {heading, false});
        else if (it->heading != heading) it->ambiguous = true;
    };
    for (Block *heading : st.headings) {
        bool bracket = false;
        const QString &text = st.files[heading->file].text;
        for (const Piece &piece : heading->pieces)
            if (QStringView(text).sliced(piece.src, piece.len).contains(u'[')) bracket = true;
        InlineContext ctx;
        ctx.file = heading->file;
        ctx.run = heading->run;
        ctx.heading = true;
        // A heading with brackets may refer to another heading, which is
        // only known after this pass: it is read again with the tree.
        ctx.quiet = bracket;
        Node holder;
        holder.type = NodeType::Heading;
        inlines.parse(heading->pieces, ctx, holder.children);
        if (st.options.headingLabels) {
            Run &run = st.runs[heading->run];
            QString label;
            int textEnd = heading->pieces.empty() ? 0 : heading->pieces.back().src + heading->pieces.back().len;
            if (!holder.children.empty() && holder.children.back().type == NodeType::HeadingLabel) {
                label = normalizeLabel(holder.children.back().label);
                textEnd = holder.children.back().range.start;
            }
            add(run.labels, label, heading->heading);
            add(st.labels, label, heading->heading);
            const QString plain = normalizeLabel(plainText(holder));
            add(run.titles, plain, heading->heading);
            add(st.titles, plain, heading->heading);
            if (!heading->pieces.empty()) {
                // The text as written matches too, as labels of definitions do.
                const int start = heading->pieces.front().src;
                const QString raw = normalizeLabel(QStringView(text).sliced(start, qMax(0, textEnd - start)));
                if (raw != plain) {
                    add(run.titles, raw, heading->heading);
                    add(st.titles, raw, heading->heading);
                }
            }
        }
        if (!bracket) {
            heading->inlines = std::move(holder.children);
            heading->inlinesReady = true;
        }
    }
    st.aliasesReady = true;
}

void Assembler::convert(Block *block, Node &node)
{
    node.type = block->type;
    node.file = block->file;
    node.range = {block->start, qMax(block->start, block->end)};
    node.open = block->openMarker.isEmpty() ? Range{node.range.start, node.range.start} : block->openMarker;
    node.close = block->closeMarker.isEmpty() ? Range{node.range.end, node.range.end} : block->closeMarker;
    node.target = block->target;

    InlineContext ctx;
    ctx.file = block->file;
    ctx.run = block->run;

    switch (block->type) {
    case NodeType::Paragraph:
        ctx.task = block->parent && block->parent->type == NodeType::ListItem && block->parent->firstChild == block;
        inlines.parse(block->pieces, ctx, node.children);
        return;
    case NodeType::Heading:
        node.level = block->level;
        node.setext = block->setext;
        if (block->inlinesReady) {
            node.children = std::move(block->inlines);
        } else {
            ctx.heading = true;
            inlines.parse(block->pieces, ctx, node.children);
        }
        if (!node.children.empty() && node.children.back().type == NodeType::HeadingLabel)
            node.label = node.children.back().label;
        node.number = block->heading + 1; // until the outline is built
        return;
    case NodeType::DefinitionTerm:
        inlines.parse(block->pieces, ctx, node.children);
        return;
    case NodeType::TableCell:
        node.header = block->header;
        node.align = block->align;
        node.column = block->column;
        node.span = block->span;
        if (!block->pieces.empty()) inlines.parse(block->pieces, ctx, node.children);
        return;
    case NodeType::CodeBlock:
    case NodeType::MermaidBlock:
        node.text = block->literal;
        node.label = block->label;
        node.fenced = block->fenced;
        node.marker = block->listChar;
        if (st.files[block->file].hasNul) node.text.replace(QChar(0), QChar(0xfffd));
        return;
    case NodeType::MathBlock:
    case NodeType::FrontMatter:
    case NodeType::CommentLine:
        node.text = block->literal;
        if (st.files[block->file].hasNul) node.text.replace(QChar(0), QChar(0xfffd));
        return;
    case NodeType::HtmlBlock: {
        node.text = block->literal;
        if (st.files[block->file].hasNul) node.text.replace(QChar(0), QChar(0xfffd));
        node.target = {};
        // Where the raw text is one unbroken run of the source, the nodes
        // read from it carry their own ranges.
        const QString &text = st.files[block->file].text;
        const int rawStart = block->target.start;
        const bool contiguous = rawStart + block->literal.size() <= text.size()
            && QStringView(text).sliced(rawStart, block->literal.size()) == block->literal;
        parseHtmlFragment(st, block->literal, block->file, node.range, contiguous ? rawStart : -1,
                          qMax(0, block->start - rawStart), node.children);
        return;
    }
    case NodeType::LinkDefinition:
        node.label = block->label;
        node.dest = block->dest;
        node.title = block->title;
        return;
    case NodeType::List:
        node.ordered = block->ordered;
        node.tight = block->tight;
        node.sourceNumber = block->listStart;
        node.marker = block->listChar;
        break;
    case NodeType::Table:
        node.columns = block->columns;
        break;
    case NodeType::TableRow:
        node.header = block->header;
        node.markers = std::move(block->markers);
        break;
    case NodeType::BlockQuote:
        node.markers = std::move(block->markers);
        break;
    case NodeType::DefinitionDescription:
        node.tight = block->tight;
        break;
    case NodeType::FootnoteDefinition:
    case NodeType::CitationDefinition:
        node.label = block->label;
        node.number = -(block->definition + 1); // until notes are numbered
        break;
    case NodeType::Include:
        node.dest = block->dest;
        node.title = block->title;
        node.resolved = block->resolved;
        node.includedFile = block->includedFile;
        break;
    default:
        break;
    }

    size_t count = 0;
    for (Block *child = block->firstChild; child; child = child->next) ++count;
    node.children.reserve(count);
    for (Block *child = block->firstChild; child; child = child->next) {
        node.children.emplace_back();
        convert(child, node.children.back());
    }

    if (block->type == NodeType::ListItem && !node.children.empty()) {
        const Node &first = node.children.front();
        if (first.type == NodeType::Paragraph && !first.children.empty()
            && first.children.front().type == NodeType::TaskMarker) {
            node.task = true;
            node.checked = first.children.front().checked;
        }
    }
}

// --- Resolution ------------------------------------------------------------------

// The first pass over the finished tree: everything that does not depend
// on the order in which notes are read.
void Assembler::structure(Node &node)
{
    switch (node.type) {
    case NodeType::Heading: {
        Heading heading;
        heading.level = node.level;
        heading.text = plainText(node).simplified();
        heading.label = node.label;
        heading.file = node.file;
        heading.range = node.range;
        heading.node = &node;
        QString base = slugify(heading.text);
        if (base.isEmpty()) base = QStringLiteral("section");
        QString slug = base;
        if (slugs.contains(slug)) {
            int &suffix = nextSuffix[base];
            if (suffix < 2) suffix = 2;
            do {
                slug = base + u'-' + QString::number(suffix++);
            } while (slugs.contains(slug));
        }
        slugs.insert(slug);
        heading.slug = slug;
        node.dest = slug;
        if (node.number > 0 && node.number <= int(headingSlugs.size())) headingSlugs[node.number - 1] = slug;
        node.number = 0;
        doc.headings.push_back(heading);
        break;
    }
    case NodeType::List: {
        // Writer numbers a list from one whatever its first marker says.
        node.number = !node.ordered ? 0 : st.writer() ? 1 : node.sourceNumber;
        int next = node.number;
        for (Node &item : node.children)
            if (item.type == NodeType::ListItem) item.number = node.ordered ? next++ : 0;
        break;
    }
    case NodeType::FootnoteDefinition:
    case NodeType::CitationDefinition:
        if (node.number < 0) {
            NoteEntry &entry = (node.type == NodeType::FootnoteDefinition ? st.footnotes : st.citations)[-node.number - 1];
            entry.node = &node;
            entry.range = node.range;
        }
        node.number = 0;
        break;
    case NodeType::Link:
        if (node.number > 0) {
            headingLinks.emplace_back(&node, node.number - 1);
            node.number = 0;
        } else if (node.dest.size() > 1 && node.dest.startsWith(u'#')) {
            anchorLinks.push_back(&node);
        }
        break;
    case NodeType::TableOfContents:
        tocs.push_back(&node);
        break;
    case NodeType::FootnoteReference:
    case NodeType::Citation:
        numbered.push_back(&node);
        break;
    default:
        break;
    }
    for (Node &child : node.children) structure(child);
}

void Assembler::numberNote(Node &node)
{
    if (node.number >= 0) return;
    NoteEntry &entry = st.footnotes[-node.number - 1];
    if (!entry.node) {
        node.number = 0;
        return;
    }
    if (entry.number == 0) {
        entry.number = ++lastFootnote;
        Footnote note;
        note.number = entry.number;
        note.label = entry.label;
        note.file = entry.file;
        note.range = entry.range;
        note.content = entry.node;
        doc.footnotes.push_back(note);
        // The definition shows the number its references carry.
        const_cast<Node *>(entry.node)->number = entry.number;
    }
    node.number = entry.number;
    ++doc.footnotes[entry.number - 1].references;
}

void Assembler::numberCitation(Node &node)
{
    if (node.number >= 0) return;
    NoteEntry &entry = st.citations[-node.number - 1];
    if (entry.number == 0) {
        entry.number = ++lastCitation;
        BibliographyEntry work;
        work.number = entry.number;
        work.key = entry.label;
        work.file = entry.file;
        work.definition = entry.node;
        work.external = entry.external;
        doc.bibliography.push_back(work);
        if (entry.node) const_cast<Node *>(entry.node)->number = entry.number;
    }
    node.number = entry.number;
}

// The second pass, in the order a reader meets the text: definitions are
// passed over here and read when the note they define is first numbered.
void Assembler::flow(Node &node)
{
    switch (node.type) {
    case NodeType::FootnoteDefinition:
    case NodeType::CitationDefinition:
        return;
    case NodeType::FootnoteReference:
        numberNote(node);
        return;
    case NodeType::InlineNote: {
        // An inline note is known by its file and place: a file included
        // twice has the note once, referred to twice.
        const quint64 key = (quint64(quint32(node.file)) << 32) | quint32(node.range.start);
        const auto known = node.file == 0 ? inlineNotes.end() : inlineNotes.find(key);
        if (known != inlineNotes.end()) {
            node.number = *known;
            ++doc.footnotes[node.number - 1].references;
            return;
        }
        node.number = ++lastFootnote;
        if (node.file != 0) inlineNotes.insert(key, node.number);
        Footnote note;
        note.number = node.number;
        note.file = node.file;
        note.range = node.range;
        note.content = &node;
        note.references = 1;
        doc.footnotes.push_back(note);
        ++noteDepth;
        for (Node &child : node.children) flow(child);
        --noteDepth;
        return;
    }
    case NodeType::Citation:
        if (source.citationNotes && noteDepth == 0) {
            // A note style: the citation is a note of its own, where it stands.
            Footnote note;
            note.number = ++lastFootnote;
            note.file = node.file;
            note.range = node.range;
            note.content = &node;
            note.references = 1;
            doc.footnotes.push_back(note);
        }
        if (node.children.empty()) numberCitation(node);
        // The citations of a group are in its note, not notes of their own.
        ++noteDepth;
        for (Node &child : node.children) flow(child);
        --noteDepth;
        return;
    default:
        break;
    }
    for (Node &child : node.children) flow(child);
}

Node Assembler::tocList(size_t &index, int level, const Node &toc) const
{
    auto generated = [&](NodeType type) {
        Node node;
        node.type = type;
        node.file = toc.file;
        node.range = toc.range;
        node.open = {toc.range.start, toc.range.start};
        node.close = {toc.range.end, toc.range.end};
        node.generated = true;
        return node;
    };
    Node list = generated(NodeType::List);
    list.tight = true;
    list.marker = u'-';
    while (index < doc.headings.size() && doc.headings[index].level >= level) {
        const Heading &heading = doc.headings[index++];
        Node text = generated(NodeType::Text);
        text.text = heading.text;
        Node link = generated(NodeType::Link);
        link.dest = u'#' + heading.slug;
        link.children.push_back(std::move(text));
        Node paragraph = generated(NodeType::Paragraph);
        paragraph.children.push_back(std::move(link));
        Node item = generated(NodeType::ListItem);
        item.children.push_back(std::move(paragraph));
        if (index < doc.headings.size() && doc.headings[index].level > heading.level)
            item.children.push_back(tocList(index, heading.level + 1, toc));
        list.children.push_back(std::move(item));
    }
    return list;
}

void Assembler::resolve()
{
    headingSlugs.resize(st.headings.size());
    structure(doc.root);

    flow(doc.root);
    // The notes, in the order of their numbers: a note may refer to another,
    // which is then numbered after every note the text refers to.
    ++noteDepth;
    size_t noteIndex = 0, workIndex = 0;
    while (noteIndex < doc.footnotes.size() || workIndex < doc.bibliography.size()) {
        if (noteIndex < doc.footnotes.size()) {
            const Node *content = doc.footnotes[noteIndex++].content;
            if (content && content->type == NodeType::FootnoteDefinition)
                for (const Node &child : content->children) flow(const_cast<Node &>(child));
        } else {
            const Node *definition = doc.bibliography[workIndex++].definition;
            if (definition)
                for (const Node &child : definition->children) flow(const_cast<Node &>(child));
        }
    }
    --noteDepth;
    for (Node *node : numbered)
        if (node->number < 0) node->number = 0;

    for (const NoteEntry &entry : st.footnotes) {
        if (entry.number == 0 && entry.node)
            st.diagnose(entry.file, entry.node->open, Severity::Info, DiagnosticCode::UnusedFootnote,
                        QStringLiteral("The footnote \"%1\" is never referred to").arg(entry.label));
    }

    for (const auto &[link, heading] : headingLinks)
        link->dest = u'#' + headingSlugs[heading];
    for (Node *link : anchorLinks) {
        const QString slug = QUrl::fromPercentEncoding(link->dest.mid(1).toUtf8());
        if (slugs.contains(slug)) continue;
        link->resolved = false;
        st.diagnose(link->file, link->range, Severity::Warning, DiagnosticCode::UnresolvedAnchor,
                    QStringLiteral("No heading has the anchor \"#%1\"").arg(slug));
    }
    for (Node *toc : tocs) {
        size_t index = 0;
        if (!doc.headings.empty()) toc->children.push_back(tocList(index, 1, *toc));
    }

    std::stable_sort(doc.diagnostics.begin(), doc.diagnostics.end(), [](const Diagnostic &a, const Diagnostic &b) {
        if (a.file != b.file) return a.file < b.file;
        return a.range.start < b.range.start;
    });
}

void Assembler::run(const QString &text)
{
    SourceFile rootFile;
    rootFile.path = source.path;
    rootFile.directory = source.directory.isEmpty() ? folderOf(source.path) : source.directory;
    rootFile.text = text;
    for (const QString &folder : {source.libraryRoot, rootFile.directory}) {
        if (folder.isEmpty()) continue;
        const QString canonical = source.resolver ? source.resolver->canonicalPath(folder) : QFileInfo(folder).canonicalFilePath();
        doc.resourceFolders.append(canonical.isEmpty() ? QDir::cleanPath(folder) : canonical);
    }
    doc.files.push_back(rootFile);
    st.files.emplace_back();
    st.files.back().text = text;
    st.runs.emplace_back();

    Block *root = st.newBlock(NodeType::Document, 0, 0);
    root->end = int(text.size());
    parseBlocks(st, root, 0, 0);

    if (source.resolver && root->firstChild) {
        bool any = false;
        for (Block *block = root->firstChild; block && !any; block = block->next) any = block->type == NodeType::Include;
        if (any) {
            expandedBytes = utf8Size(text);
            if (!source.path.isEmpty()) {
                fileIds.insert(source.path, 0);
                chain.append(source.path);
            }
            expandIncludes(root, 0);
        }
    }

    prepareHeadings(root);
    convert(root, doc.root);
    resolve();
}

} // namespace
} // namespace detail

Document parse(const QString &text, const Options &options)
{
    return parse(text, options, Source());
}

namespace {
std::atomic<quint64> parsed{0};
}

quint64 parseCount() { return parsed.load(std::memory_order_relaxed); }

Document parse(const QString &text, const Options &options, const Source &source)
{
    parsed.fetch_add(1, std::memory_order_relaxed);
    Document document;
    document.options = options;
    detail::ParseState state;
    state.options = options;
    state.source = &source;
    state.document = &document;
    detail::Assembler(state, document, source).run(text);
    return document;
}

} // namespace md
