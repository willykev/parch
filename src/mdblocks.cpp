// The block parser: reads a file line by line into a tree of blocks.
//
// It is the CommonMark algorithm: each line is first matched against the
// open blocks, then may start new ones, and what is left is text for the
// innermost block. The dialect's own blocks (front matter, tables,
// definition lists, footnote and citation definitions, display math, page
// breaks, comment lines) are block starts among the standard ones.
// Inline content is not read here: leaves keep the source pieces they are
// made of, and the inline parser reads those once every file is in the tree.
#include "mdparser_p.h"

namespace md::detail {

Block *ParseState::newBlock(NodeType type, int file, int run)
{
    blocks.emplace_back();
    Block *block = &blocks.back();
    block->type = type;
    block->file = file;
    block->run = run;
    return block;
}

void ParseState::diagnose(int file, Range range, Severity severity, DiagnosticCode code, const QString &message)
{
    document->diagnostics.push_back({file, range, severity, code, message});
}

void appendChild(Block *parent, Block *child)
{
    child->parent = parent;
    child->prev = parent->lastChild;
    child->next = nullptr;
    if (parent->lastChild) parent->lastChild->next = child;
    else parent->firstChild = child;
    parent->lastChild = child;
}

void insertBefore(Block *sibling, Block *block)
{
    block->parent = sibling->parent;
    block->prev = sibling->prev;
    block->next = sibling;
    if (sibling->prev) sibling->prev->next = block;
    else sibling->parent->firstChild = block;
    sibling->prev = block;
}

void unlink(Block *block)
{
    if (!block->parent) return;
    if (block->prev) block->prev->next = block->next;
    else block->parent->firstChild = block->next;
    if (block->next) block->next->prev = block->prev;
    else block->parent->lastChild = block->prev;
    block->parent = block->prev = block->next = nullptr;
}

namespace {

bool isContainer(NodeType type)
{
    switch (type) {
    case NodeType::Document:
    case NodeType::Include:
    case NodeType::BlockQuote:
    case NodeType::List:
    case NodeType::ListItem:
    case NodeType::DefinitionList:
    case NodeType::DefinitionDescription:
    case NodeType::FootnoteDefinition:
    case NodeType::CitationDefinition:
        return true;
    default:
        return false;
    }
}

// Whether a block may hold a child of a type; when it may not, the block
// ends and the child goes to its parent.
bool canContain(NodeType parent, NodeType child)
{
    switch (parent) {
    case NodeType::List:
        return child == NodeType::ListItem;
    case NodeType::DefinitionList:
        return child == NodeType::DefinitionTerm || child == NodeType::DefinitionDescription;
    case NodeType::Table:
        return child == NodeType::TableRow || child == NodeType::CommentLine;
    case NodeType::Document:
    case NodeType::Include:
    case NodeType::BlockQuote:
    case NodeType::ListItem:
    case NodeType::DefinitionDescription:
    case NodeType::FootnoteDefinition:
    case NodeType::CitationDefinition:
        return child != NodeType::ListItem && child != NodeType::DefinitionDescription
            && child != NodeType::DefinitionTerm && child != NodeType::TableRow;
    default:
        return false;
    }
}

// The tag names that start an HTML block of the sixth kind.
const char *const blockTagNames[] = {
    "address", "article", "aside", "base", "basefont", "blockquote", "body", "caption", "center", "col",
    "colgroup", "dd", "details", "dialog", "dir", "div", "dl", "dt", "fieldset", "figcaption", "figure",
    "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hr", "html",
    "iframe", "legend", "li", "link", "main", "menu", "menuitem", "nav", "noframes", "ol", "optgroup", "option",
    "p", "param", "search", "section", "summary", "table", "tbody", "td", "tfoot", "th", "thead", "title", "tr",
    "track", "ul",
};

// Whether a blank line separates two of a block's children. Comment lines
// are not content, so they neither end nor start a gap.
bool blankBetweenChildren(const Block *block)
{
    const Block *previous = nullptr;
    for (const Block *child = block->firstChild; child; child = child->next) {
        if (child->type == NodeType::CommentLine) continue;
        if (previous) {
            // Comment lines between the two account for some of the distance.
            int comments = 0;
            for (const Block *between = previous->next; between != child; between = between->next) ++comments;
            if (previous->endLine + comments != child->startLine - 1) return true;
        }
        previous = child;
    }
    return false;
}

bool equalsIgnoreCase(QStringView text, const char *word)
{
    return text.compare(QLatin1StringView(word), Qt::CaseInsensitive) == 0;
}

struct ListData {
    bool ordered = false;
    QChar ch;
    int start = 1;
    int markerOffset = 0;
    int padding = 0;
    Range marker;
};

struct Cell {
    int start = 0;
    int end = 0;
    bool zeroWidth = false;
    int span = 1;
};

class BlockParser {
public:
    BlockParser(ParseState &state, Block *root, int file, int run)
        : st(state), opt(state.options), root(root), fileId(file), runId(run), fs(state.files[file]),
          s(fs.text.constData()), n(int(fs.text.size())), tip(root)
    {
    }

    void run();

private:
    char16_t peek(int i) const { return i < lineEnd ? s[i].unicode() : 0; }
    void findNextNonspace();
    void advanceOffset(int count, bool columns);
    void advanceNextNonspace();
    int trimmedLineEnd() const;

    void processLine();
    int continueBlock(Block *block);
    int tryStart(Block *container);
    void closeUnmatchedBlocks();
    Block *addChild(NodeType type, int startOffset);
    void finalize(Block *block, int line);
    void finalizeParagraph(Block *block);
    void consumeReferences(Block *block);
    void addParagraphLine(Block *block);
    void addLiteralLine(Block *block);
    void addMathLine(Block *block);
    bool commentLine();
    Block *newComment(int start, int length, int line, int depth);
    bool unclosedFrontMatter(int pos) const;

    bool atxHeading();
    bool fenceStart();
    bool mathStart();
    bool htmlStart(Block *container);
    int htmlBlockType(bool mayInterrupt) const;
    bool htmlBlockEnds(const Block *block) const;
    bool setextHeading(Block *container);
    bool thematicBreak();
    bool pageBreak(Block *container);
    bool noteDefinition();
    bool definition(Block *container);
    bool listItem(Block *container);
    bool parseListMarker(Block *container, ListData *data);
    bool tableStart(Block *container);
    void addTableRow(Block *table, int from, int to, bool header, int line);
    bool splitRow(int from, int to, bool protectCode, std::vector<Cell> &cells, std::vector<Range> *pipes) const;
    bool delimiterRow(int from, int to, std::vector<Align> &aligns) const;
    bool includeLine(Block *block, int from, int to);
    Block *termsFromParagraph(Block *paragraph);

    ParseState &st;
    const Options &opt;
    Block *root;
    int fileId;
    int runId;
    FileState &fs;
    const QChar *s;
    int n;

    Block *tip;
    Block *oldTip = nullptr;
    Block *lastMatched = nullptr;
    bool allClosed = true;

    int lineNumber = 0;
    int lineStart = 0;
    int lineEnd = 0;
    int prevLineEnd = 0;
    int offset = 0;
    int column = 0;
    bool partialTab = false;
    int nns = 0;
    int nnsColumn = 0;
    int indent = 0;
    bool blank = false;
    bool indented = false;
    bool prevBlank = true; // the line before this one was blank, or there is none
};

void BlockParser::findNextNonspace()
{
    // The scan is kept while the offset is still short of what it found, so
    // that deep indentation is not read again for every container.
    if (nns <= offset) {
        int i = offset;
        int cols = column;
        while (i < lineEnd) {
            const char16_t c = s[i].unicode();
            if (c == u' ') {
                ++i;
                ++cols;
            } else if (c == u'\t') {
                ++i;
                cols += 4 - (cols % 4);
            } else {
                break;
            }
        }
        nns = i;
        nnsColumn = cols;
    }
    blank = nns >= lineEnd;
    indent = nnsColumn - column;
    indented = indent >= 4;
}

void BlockParser::advanceOffset(int count, bool columns)
{
    while (count > 0 && offset < lineEnd) {
        if (s[offset] == u'\t') {
            const int toTab = 4 - (column % 4);
            if (columns) {
                partialTab = toTab > count;
                const int advance = toTab > count ? count : toTab;
                column += advance;
                offset += partialTab ? 0 : 1;
                count -= advance;
            } else {
                partialTab = false;
                column += toTab;
                ++offset;
                --count;
            }
        } else {
            partialTab = false;
            ++offset;
            ++column;
            --count;
        }
    }
}

void BlockParser::advanceNextNonspace()
{
    offset = nns;
    column = nnsColumn;
    partialTab = false;
}

int BlockParser::trimmedLineEnd() const
{
    int e = lineEnd;
    while (e > lineStart && isSpaceOrTab(s[e - 1].unicode())) --e;
    return e;
}

void BlockParser::run()
{
    int pos = 0;
    if (n > 0 && s[0] == QChar(0xfeff)) pos = 1;
    fs.hasNul = fs.text.contains(QChar(0));

    if (opt.frontMatter) {
        SourceFile &source = st.document->files[fileId];
        const bool firstReading = !source.frontMatter.present;
        if (firstReading) source.frontMatter = parseFrontMatter(fs.text);
        const FrontMatter &fm = source.frontMatter;
        if (fm.present) {
            Block *block = st.newBlock(NodeType::FrontMatter, fileId, runId);
            block->open = false;
            block->start = fm.start;
            block->end = fm.end;
            block->openMarker = {fm.start, fm.start + 3};
            block->closeMarker = {fm.contentEnd, qMin(fm.end, fm.contentEnd + 3)};
            block->literal = fm.source;
            block->depth = root->depth + 1;
            lineNumber = 1;
            for (int i = fm.start; i < fm.end; ++i)
                if (s[i] == u'\n' || (s[i] == u'\r' && !(i + 1 < n && s[i + 1] == u'\n'))) ++lineNumber;
            block->startLine = 1;
            block->endLine = lineNumber;
            appendChild(root, block);
            if (firstReading) {
                for (const FrontMatterIssue &issue : fm.issues)
                    st.diagnose(fileId, {issue.offset, issue.offset}, Severity::Warning,
                                DiagnosticCode::FrontMatter, issue.message);
            }
            pos = fm.bodyStart;
            prevLineEnd = fm.end;
        } else if (firstReading && unclosedFrontMatter(pos)) {
            st.diagnose(fileId, {pos, pos + 3}, Severity::Warning, DiagnosticCode::FrontMatter,
                        QStringLiteral("This metadata block is never closed with --- and is read as text"));
        }
    }

    while (pos < n) {
        int end = pos;
        while (end < n && s[end] != u'\n' && s[end] != u'\r') ++end;
        lineStart = pos;
        lineEnd = end;
        processLine();
        prevLineEnd = lineEnd;
        pos = end;
        if (pos < n) pos += (s[pos] == u'\r' && pos + 1 < n && s[pos + 1] == u'\n') ? 2 : 1;
    }
    while (tip != root) finalize(tip, lineNumber);
    root->open = false;
}

// Whether the file opens with `---` and a `key:` line, which is metadata
// in everything but its missing closing line.
bool BlockParser::unclosedFrontMatter(int pos) const
{
    if (pos + 3 >= n || s[pos] != u'-' || s[pos + 1] != u'-' || s[pos + 2] != u'-') return false;
    int i = pos + 3;
    while (i < n && isSpaceOrTab(s[i].unicode())) ++i;
    if (i >= n || (s[i] != u'\n' && s[i] != u'\r')) return false;
    i += (s[i] == u'\r' && i + 1 < n && s[i + 1] == u'\n') ? 2 : 1;
    if (i >= n || !isAsciiAlpha(s[i].unicode())) return false;
    while (i < n && (isAsciiAlnum(s[i].unicode()) || s[i] == u'_' || s[i] == u'-' || s[i] == u' ')) ++i;
    return i + 1 < n && s[i] == u':' && (s[i + 1] == u' ' || s[i + 1] == u'\n' || s[i + 1] == u'\r');
}

// A `//` line is a comment wherever it is not literal text. It leaves the
// blocks around it as they are: a paragraph goes on after it, a list stays
// one list.
bool BlockParser::commentLine()
{
    if (lineEnd - lineStart < 2 || s[lineStart] != u'/' || s[lineStart + 1] != u'/') return false;
    // Raw blocks at the top level keep the line as their own text.
    if (tip->parent == root && tip->open) {
        if (tip->type == NodeType::HtmlBlock || tip->type == NodeType::MathBlock) return false;
        if (tip->type == NodeType::CodeBlock && tip->fenced) return false;
    }
    ++lineNumber;
    if (tip->type == NodeType::Paragraph) {
        Piece piece;
        piece.src = lineStart;
        piece.len = lineEnd - lineStart;
        piece.line = lineNumber;
        piece.comment = true;
        tip->pieces.push_back(piece);
        return true;
    }
    while (!isContainer(tip->type) && tip->type != NodeType::Table) finalize(tip, lineNumber - 1);
    appendChild(tip, newComment(lineStart, lineEnd - lineStart, lineNumber, tip->depth + 1));
    return true;
}

Block *BlockParser::newComment(int start, int length, int line, int depth)
{
    Block *comment = st.newBlock(NodeType::CommentLine, fileId, runId);
    comment->open = false;
    comment->start = start;
    comment->end = start + length;
    comment->startLine = comment->endLine = line;
    comment->openMarker = {start, start + 2};
    comment->depth = depth;
    comment->literal = QStringView(s + start + 2, length - 2).trimmed().toString();
    return comment;
}

void BlockParser::processLine()
{
    if (opt.lineComments && commentLine()) return;

    ++lineNumber;
    offset = lineStart;
    column = 0;
    partialTab = false;
    blank = false;
    nns = -1;
    oldTip = tip;

    // Which of the open blocks does this line continue?
    Block *container = root;
    while (Block *last = container->lastChild) {
        if (!last->open) break;
        container = last;
        findNextNonspace();
        const int result = continueBlock(container);
        if (result == 2) {
            prevBlank = false;
            return; // the line closed a fenced block and is used up
        }
        if (result == 1) {
            container = container->parent;
            break;
        }
    }
    allClosed = container == oldTip;
    lastMatched = container;

    // Which new blocks does it start?
    bool leaf = container->type != NodeType::Paragraph && !isContainer(container->type)
        && container->type != NodeType::Table;
    bool consumed = false;
    while (!leaf) {
        findNextNonspace();
        const int result = tryStart(container);
        if (result == 0) {
            advanceNextNonspace();
            break;
        }
        container = tip;
        if (result == 1) continue;
        leaf = true;
        consumed = result == 3;
    }

    // What is left is text for the innermost block.
    if (consumed) {
        // Nothing left.
    } else if (!allClosed && !blank && tip->type == NodeType::Paragraph) {
        addParagraphLine(tip); // a lazy continuation line
    } else {
        closeUnmatchedBlocks();
        switch (container->type) {
        case NodeType::CodeBlock:
        case NodeType::HtmlBlock:
            addLiteralLine(container);
            break;
        case NodeType::MathBlock:
            addMathLine(container);
            break;
        case NodeType::Paragraph:
            addParagraphLine(container);
            break;
        case NodeType::Table:
            if (!blank) addTableRow(container, nns, trimmedLineEnd(), false, lineNumber);
            break;
        default:
            if (!blank) {
                Block *paragraph = addChild(NodeType::Paragraph, nns);
                addParagraphLine(paragraph);
            }
            break;
        }
    }
    prevBlank = blank;
}

// 0: the line continues the block; 1: it does not; 2: it closed the block
// and nothing of the line is left.
int BlockParser::continueBlock(Block *block)
{
    switch (block->type) {
    case NodeType::BlockQuote:
        if (indented || peek(nns) != u'>') return 1;
        block->markers.push_back({nns, nns + 1});
        advanceNextNonspace();
        advanceOffset(1, false);
        if (isSpaceOrTab(peek(offset))) advanceOffset(1, true);
        return 0;
    case NodeType::ListItem:
    case NodeType::DefinitionDescription:
        if (blank) {
            if (!block->firstChild) return 1; // a blank line after an empty item ends it
            advanceNextNonspace();
        } else if (indent >= block->markerOffset + block->padding) {
            advanceOffset(block->markerOffset + block->padding, true);
        } else {
            return 1;
        }
        return 0;
    case NodeType::FootnoteDefinition:
    case NodeType::CitationDefinition:
        if (blank) {
            if (!block->firstChild) return 1;
            advanceNextNonspace();
        } else if (indent >= 4) {
            advanceOffset(4, true);
        } else {
            return 1;
        }
        return 0;
    case NodeType::CodeBlock:
        if (block->fenced) {
            if (indent <= 3 && peek(nns) == block->listChar.unicode()) {
                int e = nns;
                while (peek(e) == block->listChar.unicode()) ++e;
                if (e - nns >= block->fenceLength && trimmedLineEnd() <= e) {
                    block->closed = true;
                    block->closeMarker = {nns, e};
                    finalize(block, lineNumber);
                    return 2;
                }
            }
            int skip = block->fenceOffset;
            while (skip > 0 && isSpaceOrTab(peek(offset))) {
                advanceOffset(1, true);
                --skip;
            }
            return 0;
        }
        if (indent >= 4) advanceOffset(4, true);
        else if (blank) advanceNextNonspace();
        else return 1;
        return 0;
    case NodeType::HtmlBlock:
        return blank && block->level >= 6 ? 1 : 0;
    case NodeType::MathBlock:
    case NodeType::Paragraph:
    case NodeType::Table:
        return blank ? 1 : 0;
    case NodeType::List:
    case NodeType::DefinitionList:
    case NodeType::Document:
    case NodeType::Include:
        return 0;
    default:
        return 1;
    }
}

void BlockParser::closeUnmatchedBlocks()
{
    if (allClosed) return;
    while (oldTip != lastMatched) {
        Block *parent = oldTip->parent;
        finalize(oldTip, lineNumber - 1);
        oldTip = parent;
    }
    allClosed = true;
}

Block *BlockParser::addChild(NodeType type, int startOffset)
{
    while (!canContain(tip->type, type)) finalize(tip, lineNumber - 1);
    Block *block = st.newBlock(type, fileId, runId);
    block->start = startOffset;
    block->startLine = lineNumber;
    block->depth = tip->depth + 1;
    appendChild(tip, block);
    tip = block;
    return block;
}

// 0: nothing starts here; 1: a container was opened, and the rest of the
// line is looked at again; 2: a leaf was opened that takes the rest of the
// line as text; 3: the line is used up.
int BlockParser::tryStart(Block *container)
{
    const char16_t c = peek(nns);
    const bool roomForContainer = container->depth < MaxBlockDepth;
    if (!indented) {
        switch (c) {
        case u'>':
            if (!roomForContainer) break;
            {
                const int marker = nns;
                advanceNextNonspace();
                advanceOffset(1, false);
                if (isSpaceOrTab(peek(offset))) advanceOffset(1, true);
                closeUnmatchedBlocks();
                Block *quote = addChild(NodeType::BlockQuote, marker);
                quote->markers.push_back({marker, marker + 1});
                quote->openMarker = {marker, marker + 1};
            }
            return 1;
        case u'#':
            if (atxHeading()) return 3;
            break;
        case u'`':
        case u'~':
            if (fenceStart()) return 3;
            break;
        case u'$':
            if (opt.math && mathStart()) return 3;
            break;
        case u'<':
            if (htmlStart(container)) return 2;
            break;
        case u'[':
            if (roomForContainer && noteDefinition()) return 1;
            break;
        case u':':
            if (opt.definitionLists && roomForContainer && definition(container)) return 1;
            break;
        default:
            break;
        }
        if (container->type == NodeType::Paragraph && (c == u'=' || c == u'-') && setextHeading(container)) return 3;
        if ((c == u'*' || c == u'-' || c == u'_') && thematicBreak()) return 3;
        if (c == u'+' && opt.pageBreaks && pageBreak(container)) return 3;
    }
    if ((!indented || container->type == NodeType::List) && roomForContainer && listItem(container)) return 1;
    if (indented && tip->type != NodeType::Paragraph && !blank) {
        advanceOffset(4, true);
        closeUnmatchedBlocks();
        Block *code = addChild(NodeType::CodeBlock, offset);
        code->openMarker = code->closeMarker = {};
        return 2;
    }
    if (!indented && opt.tables && container->type == NodeType::Paragraph && tableStart(container)) return 3;
    return 0;
}

bool BlockParser::atxHeading()
{
    int e = nns;
    while (peek(e) == u'#') ++e;
    const int level = e - nns;
    if (level > 6 || (e < lineEnd && !isSpaceOrTab(peek(e)))) return false;
    closeUnmatchedBlocks();
    Block *heading = addChild(NodeType::Heading, nns);
    heading->level = quint8(level);
    heading->openMarker = {nns, e};
    // The content, without the closing run of #s and the space around it.
    int contentStart = e;
    while (contentStart < lineEnd && isSpaceOrTab(peek(contentStart))) ++contentStart;
    int contentEnd = trimmedLineEnd();
    if (contentEnd < contentStart) contentEnd = contentStart;
    int hashes = contentEnd;
    while (hashes > contentStart && s[hashes - 1] == u'#') --hashes;
    if (hashes < contentEnd && (hashes == contentStart || isSpaceOrTab(s[hashes - 1].unicode()))) {
        heading->closeMarker = {hashes, contentEnd};
        contentEnd = hashes;
        while (contentEnd > contentStart && isSpaceOrTab(s[contentEnd - 1].unicode())) --contentEnd;
    } else {
        heading->closeMarker = {contentEnd, contentEnd};
    }
    if (contentEnd > contentStart) {
        Piece piece;
        piece.src = contentStart;
        piece.len = contentEnd - contentStart;
        piece.line = lineNumber;
        heading->pieces.push_back(piece);
    }
    advanceOffset(lineEnd - offset, false);
    finalize(heading, lineNumber);
    return true;
}

bool BlockParser::fenceStart()
{
    const char16_t ch = peek(nns);
    int e = nns;
    while (peek(e) == ch) ++e;
    const int length = e - nns;
    if (length < 3) return false;
    const int infoEnd = trimmedLineEnd();
    int infoStart = e;
    while (infoStart < infoEnd && isSpaceOrTab(peek(infoStart))) ++infoStart;
    if (ch == u'`') {
        for (int i = e; i < infoEnd; ++i)
            if (s[i] == u'`') return false; // an info string cannot hold a backtick
    }
    closeUnmatchedBlocks();
    Block *code = addChild(NodeType::CodeBlock, nns);
    code->fenced = true;
    code->listChar = QChar(ch);
    code->fenceLength = length;
    code->fenceOffset = indent;
    code->openMarker = {nns, e};
    code->closeMarker = {};
    if (infoEnd > infoStart) {
        code->target = {infoStart, infoEnd};
        code->label = unescape(QStringView(s + infoStart, infoEnd - infoStart));
    }
    advanceOffset(lineEnd - offset, false);
    return true;
}

// `$$` at the start of a line opens display math: closed on the same line,
// or open until a line that ends with `$$`. A blank line before that, and
// the block was never math: it is a paragraph again.
bool BlockParser::mathStart()
{
    if (peek(nns + 1) != u'$') return false;
    if (tip->type == NodeType::Paragraph) return false; // inline display math handles it
    const int end = trimmedLineEnd();
    int contentStart = nns + 2;
    QStringView rest(s + contentStart, end - contentStart);
    const qsizetype close = rest.indexOf(QLatin1StringView("$$"));
    const bool sameLine = close >= 0 && close + 2 == rest.size();
    if (close >= 0 && !sameLine) return false; // text follows the math: an inline matter
    if (sameLine && rest.first(close).trimmed().isEmpty()) return false;
    closeUnmatchedBlocks();
    Block *math = addChild(NodeType::MathBlock, nns);
    math->openMarker = {nns, nns + 2};
    Piece piece;
    piece.src = nns;
    piece.len = end - nns;
    piece.line = lineNumber;
    math->pieces.push_back(piece);
    if (sameLine) {
        math->literal = rest.first(close).trimmed().toString();
        math->closed = true;
        math->closeMarker = {end - 2, end};
        advanceOffset(lineEnd - offset, false);
        finalize(math, lineNumber);
        return true;
    }
    const QStringView first = rest.trimmed();
    if (!first.isEmpty()) {
        math->literal = first.toString();
        math->literal.append(u'\n');
    }
    advanceOffset(lineEnd - offset, false);
    return true;
}

void BlockParser::addMathLine(Block *block)
{
    const int end = trimmedLineEnd();
    Piece piece;
    piece.src = nns;
    piece.len = end - nns;
    piece.line = lineNumber;
    block->pieces.push_back(piece);
    QStringView line(s + nns, end - nns);
    if (line.endsWith(QLatin1StringView("$$"))) {
        line = line.first(line.size() - 2).trimmed();
        if (!line.isEmpty()) {
            block->literal.append(line);
            block->literal.append(u'\n');
        }
        block->closed = true;
        block->closeMarker = {end - 2, end};
        finalize(block, lineNumber);
        return;
    }
    block->literal.append(QStringView(s + offset, end - offset));
    block->literal.append(u'\n');
}

int BlockParser::htmlBlockType(bool mayInterrupt) const
{
    const QStringView line(s + nns, lineEnd - nns);
    if (line.size() < 2) return 0;
    const char16_t second = line[1].unicode();
    if (second == u'!') {
        if (line.startsWith(QLatin1StringView("<!--"))) return 2;
        if (line.startsWith(QLatin1StringView("<![CDATA["))) return 5;
        if (line.size() > 2 && isAsciiAlpha(line[2].unicode())) return 4;
        return 0;
    }
    if (second == u'?') return 3;
    // The name of the tag, open or closing.
    qsizetype i = second == u'/' ? 2 : 1;
    const qsizetype nameStart = i;
    while (i < line.size() && (isAsciiAlnum(line[i].unicode()) || line[i] == u'-')) ++i;
    const QStringView name = line.sliced(nameStart, i - nameStart);
    if (!name.isEmpty()) {
        const char16_t next = i < line.size() ? line[i].unicode() : 0;
        const bool ends = next == 0 || next == u' ' || next == u'\t' || next == u'>';
        if (second != u'/' && ends) {
            for (const char *word : {"script", "pre", "textarea", "style"})
                if (equalsIgnoreCase(name, word)) return 1;
        }
        if (ends || (next == u'/' && i + 1 < line.size() && line[i + 1] == u'>')) {
            for (const char *word : blockTagNames)
                if (equalsIgnoreCase(name, word)) return 6;
        }
    }
    // Any complete tag alone on its line, except where it would interrupt a paragraph.
    if (mayInterrupt) return 0;
    HtmlKind kind = HtmlKind::None;
    const int length = scanHtml(s, lineEnd, nns, &kind);
    if (!length || (kind != HtmlKind::OpenTag && kind != HtmlKind::CloseTag)) return 0;
    for (int k = nns + length; k < lineEnd; ++k)
        if (!isSpaceOrTab(s[k].unicode())) return 0;
    return 7;
}

bool BlockParser::htmlStart(Block *container)
{
    const bool lazy = !allClosed && !blank && tip->type == NodeType::Paragraph;
    const int type = htmlBlockType(container->type == NodeType::Paragraph || lazy);
    if (!type) return false;
    closeUnmatchedBlocks();
    // The indentation stays in the block: it is part of the raw text.
    Block *html = addChild(NodeType::HtmlBlock, nns);
    html->level = quint8(type);
    html->target = {offset, offset};
    return true;
}

bool BlockParser::htmlBlockEnds(const Block *block) const
{
    const QStringView line(s + offset, lineEnd - offset);
    switch (block->level) {
    case 1:
        for (const char *word : {"</script>", "</pre>", "</style>", "</textarea>"})
            if (line.contains(QLatin1StringView(word), Qt::CaseInsensitive)) return true;
        return false;
    case 2: return line.contains(QLatin1StringView("-->"));
    case 3: return line.contains(QLatin1StringView("?>"));
    case 4: return line.contains(u'>');
    case 5: return line.contains(QLatin1StringView("]]>"));
    default: return false;
    }
}

void BlockParser::addLiteralLine(Block *block)
{
    if (partialTab) {
        ++offset;
        block->literal.append(QString(4 - (column % 4), u' '));
    }
    block->literal.append(QStringView(s + offset, lineEnd - offset));
    block->literal.append(u'\n');
    if (!blank || (block->type == NodeType::CodeBlock && block->fenced)) {
        block->lastTextLine = lineNumber;
        block->lastTextEnd = lineEnd;
        block->literalTextLength = int(block->literal.size());
    }
    if (block->type == NodeType::HtmlBlock && htmlBlockEnds(block)) finalize(block, lineNumber);
}

void BlockParser::addParagraphLine(Block *block)
{
    Piece piece;
    piece.src = nns;
    piece.len = lineEnd - nns;
    piece.line = lineNumber;
    block->pieces.push_back(piece);
}

bool BlockParser::setextHeading(Block *container)
{
    const char16_t c = peek(nns);
    int e = nns;
    while (peek(e) == c) ++e;
    if (trimmedLineEnd() > e) return false;
    closeUnmatchedBlocks();
    // Definitions that open the paragraph are not part of the heading.
    consumeReferences(container);
    bool hasText = false;
    for (const Piece &piece : container->pieces)
        if (!piece.comment) hasText = true;
    if (!hasText) return false;
    container->type = NodeType::Heading;
    container->setext = true;
    container->level = c == u'=' ? 1 : 2;
    container->closeMarker = {nns, e};
    Piece &last = container->pieces.back();
    if (!last.comment)
        while (last.len > 0 && isSpaceOrTab(s[last.src + last.len - 1].unicode())) --last.len;
    container->start = container->pieces.front().src;
    container->startLine = container->pieces.front().line;
    container->openMarker = {container->start, container->start};
    advanceOffset(lineEnd - offset, false);
    container->open = false;
    container->end = e;
    container->endLine = lineNumber;
    tip = container->parent;
    return true;
}

bool BlockParser::thematicBreak()
{
    const char16_t c = peek(nns);
    int count = 0;
    for (int i = nns; i < lineEnd; ++i) {
        const char16_t x = s[i].unicode();
        if (x == c) ++count;
        else if (!isSpaceOrTab(x)) return false;
    }
    if (count < 3) return false;
    closeUnmatchedBlocks();
    Block *rule = addChild(NodeType::ThematicBreak, nns);
    advanceOffset(lineEnd - offset, false);
    finalize(rule, lineNumber);
    rule->end = trimmedLineEnd();
    return true;
}

// `+++` alone on a line, after a blank line, at the top level of a file.
bool BlockParser::pageBreak(Block *container)
{
    if (container != root || !prevBlank) return false;
    if (trimmedLineEnd() - nns != 3 || peek(nns + 1) != u'+' || peek(nns + 2) != u'+') return false;
    closeUnmatchedBlocks();
    Block *page = addChild(NodeType::PageBreak, nns);
    advanceOffset(lineEnd - offset, false);
    finalize(page, lineNumber);
    page->end = nns + 3;
    return true;
}

// `[^label]: text` and `[#key]: entry`. What follows on the line, and
// lines indented by four columns after it, are the definition's content.
bool BlockParser::noteDefinition()
{
    const char16_t kind = peek(nns + 1);
    const bool footnote = kind == u'^' && opt.footnotes;
    const bool citation = kind == u'#' && opt.citations;
    if (!footnote && !citation) return false;
    int e = nns + 2;
    const int limit = qMin(lineEnd, nns + 1002);
    while (e < limit && s[e] != u']' && s[e] != u'[') {
        if (s[e] == u'\\') ++e;
        ++e;
    }
    if (e >= limit || s[e] != u']' || peek(e + 1) != u':') return false;
    const QStringView label = QStringView(s + nns + 2, e - nns - 2).trimmed();
    if (label.isEmpty()) return false;

    closeUnmatchedBlocks();
    Block *definition = addChild(footnote ? NodeType::FootnoteDefinition : NodeType::CitationDefinition, nns);
    definition->label = label.toString();
    definition->openMarker = {nns, e + 2};
    definition->target = {nns + 2, e};

    const QString key = normalizeLabel(label);
    Run &run = st.runs[runId];
    QSet<QString> &seen = footnote ? run.footnoteLabels : run.citationKeys;
    QHash<QString, int> &table = footnote ? fs.footnotes : fs.citations;
    std::vector<NoteEntry> &entries = footnote ? st.footnotes : st.citations;
    if (seen.contains(key)) {
        st.diagnose(fileId, {nns, e + 2}, Severity::Warning,
                    footnote ? DiagnosticCode::DuplicateFootnote : DiagnosticCode::DuplicateCitation,
                    (footnote ? QStringLiteral("The footnote \"%1\" is defined more than once; the first definition is used")
                              : QStringLiteral("The citation \"%1\" is defined more than once; the first definition is used"))
                        .arg(label));
    }
    seen.insert(key);
    const auto known = table.constFind(key);
    if (known == table.constEnd()) {
        NoteEntry entry;
        entry.file = fileId;
        entry.label = label.toString();
        entry.block = definition;
        entries.push_back(entry);
        definition->definition = int(entries.size()) - 1;
        table.insert(key, definition->definition);
    } else {
        // The same file read again, or a duplicate: the first definition stands.
        definition->definition = -1;
    }

    advanceNextNonspace();
    advanceOffset(e + 2 - nns, false);
    while (isSpaceOrTab(peek(offset))) advanceOffset(1, true);
    return true;
}

// Turns the lines of a paragraph into the terms of a definition list, which
// is the list just before the paragraph when there is one.
Block *BlockParser::termsFromParagraph(Block *paragraph)
{
    Block *list = nullptr;
    if (paragraph->prev && paragraph->prev->type == NodeType::DefinitionList) {
        list = paragraph->prev;
        list->open = true;
    } else {
        list = st.newBlock(NodeType::DefinitionList, fileId, runId);
        list->start = paragraph->pieces.front().src;
        list->startLine = paragraph->pieces.front().line;
        list->depth = paragraph->depth;
        insertBefore(paragraph, list);
    }
    for (const Piece &piece : paragraph->pieces) {
        if (piece.comment) {
            appendChild(list, newComment(piece.src, piece.len, piece.line, list->depth + 1));
            continue;
        }
        Block *term = st.newBlock(NodeType::DefinitionTerm, fileId, runId);
        term->open = false;
        Piece text = piece;
        while (text.len > 0 && isSpaceOrTab(s[text.src + text.len - 1].unicode())) --text.len;
        term->pieces.push_back(text);
        term->start = text.src;
        term->end = text.src + text.len;
        term->startLine = term->endLine = text.line;
        term->depth = list->depth + 1;
        appendChild(list, term);
    }
    unlink(paragraph);
    return list;
}

// `: definition` under one or more term lines. The terms are the paragraph
// just above: still open, or closed by blank lines.
bool BlockParser::definition(Block *container)
{
    if (!isSpaceOrTab(peek(nns + 1))) return false;
    Block *list = nullptr;
    bool afterBlank = prevBlank;
    if (container->type == NodeType::Paragraph) {
        closeUnmatchedBlocks();
        consumeReferences(container);
        bool hasText = false;
        for (const Piece &piece : container->pieces)
            if (!piece.comment) hasText = true;
        if (!hasText) return false;
        tip = container->parent;
        list = termsFromParagraph(container);
        afterBlank = false;
    } else if (container->type == NodeType::DefinitionList) {
        closeUnmatchedBlocks();
        list = container;
    } else if (isContainer(container->type) && container->lastChild) {
        Block *last = container->lastChild;
        if (last->type == NodeType::Paragraph && !last->open && !last->pieces.empty()) {
            closeUnmatchedBlocks();
            list = termsFromParagraph(last);
        } else if (last->type == NodeType::DefinitionList && !last->open) {
            closeUnmatchedBlocks();
            list = last;
            list->open = true;
        } else {
            return false;
        }
    } else {
        return false;
    }
    tip = list;

    // The marker and the space after it, measured as for a list item.
    const int marker = nns;
    const int markerIndent = indent;
    advanceNextNonspace();
    advanceOffset(1, true);
    const int spacesColumn = column;
    const int spacesOffset = offset;
    const bool spacesTab = partialTab;
    while (column - spacesColumn < 5 && isSpaceOrTab(peek(offset))) advanceOffset(1, true);
    const int spaces = column - spacesColumn;
    int padding = 1 + spaces;
    if (spaces >= 5 || spaces < 1 || offset >= lineEnd) {
        padding = 2;
        column = spacesColumn;
        offset = spacesOffset;
        partialTab = spacesTab;
        if (isSpaceOrTab(peek(offset))) advanceOffset(1, true);
    }
    Block *description = addChild(NodeType::DefinitionDescription, marker);
    description->markerOffset = markerIndent;
    description->padding = padding;
    description->openMarker = {marker, marker + 1};
    description->tight = !afterBlank;
    return true;
}

bool BlockParser::parseListMarker(Block *container, ListData *data)
{
    if (indent >= 4) return false;
    const char16_t c = peek(nns);
    int markerEnd = nns;
    if (c == u'*' || c == u'+' || c == u'-') {
        data->ordered = false;
        data->ch = QChar(c);
        markerEnd = nns + 1;
    } else if (isAsciiDigit(c)) {
        int e = nns;
        int value = 0;
        while (e < lineEnd && e - nns < 9 && isAsciiDigit(s[e].unicode())) {
            value = value * 10 + (s[e].unicode() - u'0');
            ++e;
        }
        const char16_t delimiter = peek(e);
        if (delimiter != u'.' && delimiter != u')') return false;
        // Only a list that starts at 1 may interrupt a paragraph.
        if (container->type == NodeType::Paragraph && value != 1) return false;
        data->ordered = true;
        data->start = value;
        data->ch = QChar(delimiter);
        markerEnd = e + 1;
    } else {
        return false;
    }
    if (markerEnd < lineEnd && !isSpaceOrTab(peek(markerEnd))) return false;
    if (container->type == NodeType::Paragraph) {
        // An empty item cannot interrupt a paragraph.
        bool content = false;
        for (int i = markerEnd; i < lineEnd; ++i) {
            if (!isSpaceOrTab(s[i].unicode())) {
                content = true;
                break;
            }
        }
        if (!content) return false;
    }
    data->marker = {nns, markerEnd};
    data->markerOffset = indent;
    const int width = markerEnd - nns;

    advanceNextNonspace();
    advanceOffset(width, true);
    const int spacesColumn = column;
    const int spacesOffset = offset;
    const bool spacesTab = partialTab;
    do {
        advanceOffset(1, true);
    } while (column - spacesColumn < 5 && isSpaceOrTab(peek(offset)));
    const bool blankItem = offset >= lineEnd;
    const int spaces = column - spacesColumn;
    if (spaces >= 5 || spaces < 1 || blankItem) {
        data->padding = width + 1;
        column = spacesColumn;
        offset = spacesOffset;
        partialTab = spacesTab;
        if (isSpaceOrTab(peek(offset))) advanceOffset(1, true);
    } else {
        data->padding = width + spaces;
    }
    return true;
}

bool BlockParser::listItem(Block *container)
{
    ListData data;
    if (!parseListMarker(container, &data)) return false;
    closeUnmatchedBlocks();
    if (tip->type != NodeType::List || tip->ordered != data.ordered || tip->listChar != data.ch) {
        Block *list = addChild(NodeType::List, data.marker.start);
        list->ordered = data.ordered;
        list->listChar = data.ch;
        list->listStart = data.start;
    }
    Block *item = addChild(NodeType::ListItem, data.marker.start);
    item->ordered = data.ordered;
    item->listChar = data.ch;
    item->listStart = data.start;
    item->markerOffset = data.markerOffset;
    item->padding = data.padding;
    item->openMarker = data.marker;
    return true;
}

// --- Tables ----------------------------------------------------------------------

// Splits the row in [from, to) into cells at its unescaped pipes. With
// `protectCode`, a pipe inside a code span belongs to the code.
bool BlockParser::splitRow(int from, int to, bool protectCode, std::vector<Cell> &cells,
                           std::vector<Range> *pipes) const
{
    cells.clear();
    int i = from;
    if (i < to && s[i] == u'|') {
        if (pipes) pipes->push_back({i, i + 1});
        ++i;
    }
    quint64 unclosed = 0; // lengths of backtick runs known to have no partner
    while (i < to) {
        const int start = i;
        while (i < to) {
            const char16_t c = s[i].unicode();
            if (c == u'\\' && i + 1 < to && isAsciiPunct(s[i + 1].unicode())) {
                i += 2;
                continue;
            }
            if (c == u'|') break;
            if (c == u'`' && protectCode) {
                int run = i;
                while (run < to && s[run] == u'`') ++run;
                const int length = run - i;
                int close = -1;
                if (length < 64 && !(unclosed & (quint64(1) << length))) {
                    for (int k = run; k < to;) {
                        if (s[k] != u'`') {
                            ++k;
                            continue;
                        }
                        int e = k;
                        while (e < to && s[e] == u'`') ++e;
                        if (e - k == length) {
                            close = e;
                            break;
                        }
                        k = e;
                    }
                    if (close < 0) unclosed |= quint64(1) << length;
                }
                i = close >= 0 ? close : run;
                continue;
            }
            ++i;
        }
        const bool pipe = i < to;
        if (i > start || pipe) {
            Cell cell;
            cell.start = start;
            cell.end = i;
            cell.zeroWidth = i == start;
            while (cell.start < cell.end && isSpaceOrTab(s[cell.start].unicode())) ++cell.start;
            while (cell.end > cell.start && isSpaceOrTab(s[cell.end - 1].unicode())) --cell.end;
            cells.push_back(cell);
        }
        if (pipe) {
            if (pipes) pipes->push_back({i, i + 1});
            ++i;
        }
    }
    return !cells.empty();
}

bool BlockParser::delimiterRow(int from, int to, std::vector<Align> &aligns) const
{
    std::vector<Cell> cells;
    if (!splitRow(from, to, false, cells, nullptr)) return false;
    aligns.clear();
    for (const Cell &cell : cells) {
        int a = cell.start, b = cell.end;
        const bool left = a < b && s[a] == u':';
        if (left) ++a;
        const bool right = b > a && s[b - 1] == u':';
        if (right) --b;
        if (a >= b) return false;
        for (int i = a; i < b; ++i)
            if (s[i] != u'-') return false;
        aligns.push_back(left && right ? Align::Center : left ? Align::Left : right ? Align::Right : Align::None);
    }
    return true;
}

// The line under a paragraph's last line is a delimiter row with as many
// columns as that line has cells: the two start a table.
bool BlockParser::tableStart(Block *container)
{
    const int to = trimmedLineEnd();
    bool plausible = false;
    for (int i = nns; i < to && !plausible; ++i) plausible = s[i] == u'-';
    if (!plausible) return false;
    std::vector<Align> aligns;
    if (!delimiterRow(nns, to, aligns)) return false;
    // The header is the paragraph's last line; comment lines after it are
    // passed over and kept.
    std::vector<Piece> &pieces = container->pieces;
    size_t headerIndex = pieces.size();
    while (headerIndex > 0 && pieces[headerIndex - 1].comment) --headerIndex;
    if (headerIndex == 0) return false;
    --headerIndex;
    const Piece header = pieces[headerIndex];
    int headerEnd = header.src + header.len;
    while (headerEnd > header.src && isSpaceOrTab(s[headerEnd - 1].unicode())) --headerEnd;
    const bool writer = st.writer();
    std::vector<Cell> cells;
    if (!splitRow(header.src, headerEnd, writer, cells, nullptr)) return false;
    // A merged cell covers a column for itself and one for each marker after it.
    if (cells.size() != aligns.size()) return false;

    closeUnmatchedBlocks();
    const std::vector<Piece> comments(pieces.begin() + headerIndex + 1, pieces.end());
    pieces.erase(pieces.begin() + headerIndex, pieces.end());
    Block *table = nullptr;
    if (pieces.empty()) {
        table = container;
        table->type = NodeType::Table;
    } else {
        // The lines above the header stay a paragraph.
        tip = container;
        finalize(container, header.line - 1);
        table = addChild(NodeType::Table, header.src);
    }
    table->start = header.src;
    table->startLine = header.line;
    table->aligns = aligns;
    table->columns = int(aligns.size());
    table->openMarker = {nns, to};
    table->lastTextLine = lineNumber;
    tip = table;
    addTableRow(table, header.src, headerEnd, true, header.line);
    for (const Piece &comment : comments)
        appendChild(table, newComment(comment.src, comment.len, comment.line, table->depth + 1));
    table->end = to;
    table->endLine = lineNumber;
    advanceOffset(lineEnd - offset, false);
    return true;
}

void BlockParser::addTableRow(Block *table, int from, int to, bool header, int line)
{
    const bool writer = st.writer();
    const bool merge = writer && opt.mergedCells;
    Block *row = st.newBlock(NodeType::TableRow, fileId, runId);
    row->open = false;
    row->header = header;
    row->start = from;
    row->end = to;
    row->startLine = row->endLine = line;
    row->depth = table->depth + 1;
    std::vector<Cell> cells;
    splitRow(from, to, writer, cells, &row->markers);
    appendChild(table, row);

    // Merged cells: a cell of no width widens the cell before it.
    std::vector<Cell> merged;
    bool malformed = false;
    for (const Cell &cell : cells) {
        if (merge && cell.zeroWidth) {
            if (merged.empty()) {
                malformed = true;
                merged.push_back(cell);
            } else {
                ++merged.back().span;
            }
            continue;
        }
        merged.push_back(cell);
    }

    int column = 0;
    bool overflow = false;
    for (Cell &cell : merged) {
        if (column >= table->columns) {
            overflow = true;
            break;
        }
        if (column + cell.span > table->columns) {
            overflow = true;
            cell.span = table->columns - column;
        }
        Block *block = st.newBlock(NodeType::TableCell, fileId, runId);
        block->open = false;
        block->header = header;
        block->start = cell.start;
        block->end = cell.end;
        block->startLine = block->endLine = line;
        block->depth = row->depth + 1;
        block->column = column;
        block->span = cell.span;
        block->align = table->aligns[column];
        // The content, with `\|` read as a pipe.
        int pieceStart = cell.start;
        for (int i = cell.start; i < cell.end; ++i) {
            if (s[i] == u'\\' && i + 1 < cell.end && s[i + 1] == u'|') {
                if (i > pieceStart) {
                    Piece piece;
                    piece.src = pieceStart;
                    piece.len = i - pieceStart;
                    piece.line = line;
                    piece.newline = false;
                    block->pieces.push_back(piece);
                }
                pieceStart = i + 1;
                ++i;
            }
        }
        if (cell.end > pieceStart) {
            Piece piece;
            piece.src = pieceStart;
            piece.len = cell.end - pieceStart;
            piece.line = line;
            piece.newline = false;
            block->pieces.push_back(piece);
        }
        appendChild(row, block);
        column += cell.span;
    }
    while (column < table->columns) {
        Block *block = st.newBlock(NodeType::TableCell, fileId, runId);
        block->open = false;
        block->header = header;
        block->start = block->end = to;
        block->startLine = block->endLine = line;
        block->depth = row->depth + 1;
        block->column = column;
        block->align = table->aligns[column];
        appendChild(row, block);
        ++column;
    }
    if (merge && (overflow || malformed)) {
        st.diagnose(fileId, {from, to}, Severity::Warning, DiagnosticCode::MalformedTableRow,
                    malformed ? QStringLiteral("This row starts with a merge marker that has no cell to widen")
                              : QStringLiteral("This row has more cells than the table has columns (%1)")
                                    .arg(table->columns));
    }
    table->end = to;
    table->endLine = line;
}

// --- Finishing blocks ---------------------------------------------------------------

// Link reference definitions that open a paragraph become blocks of their
// own before it, and leave the paragraph.
void BlockParser::consumeReferences(Block *block)
{
    std::vector<Piece> &pieces = block->pieces;
    if (pieces.empty() || pieces.front().comment || s[pieces.front().src] != u'[') return;
    size_t count = 0;
    while (count < pieces.size() && !pieces[count].comment) ++count;
    QString content;
    std::vector<int> starts;
    for (size_t i = 0; i < count; ++i) {
        starts.push_back(int(content.size()));
        content.append(QStringView(s + pieces[i].src, pieces[i].len));
        if (i + 1 < count) content.append(u'\n');
    }
    const QChar *c = content.constData();
    const int size = int(content.size());
    // The piece a position is in: the last that starts at or before it,
    // found by bisection, since a paragraph can hold a great many definitions.
    auto pieceOf = [&](int pos) {
        const auto after = std::upper_bound(starts.cbegin() + 1, starts.cend(), pos);
        return size_t(after - starts.cbegin()) - 1;
    };
    auto toSource = [&](int pos) {
        const size_t i = pieceOf(pos);
        return pieces[i].src + (pos - starts[i]);
    };
    int pos = 0;
    size_t consumed = 0;
    while (pos < size && c[pos] == u'[') {
        const ReferenceMatch match = scanReference(c + pos, size - pos);
        if (!match.length) break;
        const int defEnd = pos + match.length;
        const size_t first = pieceOf(pos);
        const size_t last = pieceOf(defEnd - 1);
        const QStringView label(c + pos + match.label.start, match.label.length());
        Block *def = st.newBlock(NodeType::LinkDefinition, fileId, runId);
        def->open = false;
        def->depth = block->depth;
        def->start = pieces[first].src;
        def->end = pieces[last].src + pieces[last].len;
        while (def->end > def->start && isSpaceOrTab(s[def->end - 1].unicode())) --def->end;
        def->startLine = pieces[first].line;
        def->endLine = pieces[last].line;
        def->label = label.trimmed().toString();
        def->dest = unescape(QStringView(c + pos + match.dest.start, match.dest.length()));
        if (match.hasTitle) def->title = unescape(QStringView(c + pos + match.title.start, match.title.length()));
        def->openMarker = {def->start, toSource(pos + match.label.end + 1) + 1};
        def->target = {toSource(pos + match.dest.start), toSource(pos + match.dest.start) + match.dest.length()};
        insertBefore(block, def);
        const QString key = normalizeLabel(label);
        if (!fs.references.contains(key)) fs.references.insert(key, {def->dest, def->title, fileId});
        pos = defEnd;
        consumed = last + 1;
    }
    if (consumed) pieces.erase(pieces.begin(), pieces.begin() + consumed);
}

// A path to a .md, .markdown or .txt file, alone in its paragraph and
// perhaps followed by a quoted caption.
bool BlockParser::includeLine(Block *block, int from, int to)
{
    int i = from;
    int pathStart = from, pathEnd = from;
    const char16_t first = s[i].unicode();
    if (first == u'"' || first == u'\'') {
        pathStart = ++i;
        while (i < to && s[i].unicode() != first) {
            if (s[i] == u'\\' && i + 1 < to && s[i + 1] == first) ++i;
            ++i;
        }
        if (i >= to) return false;
        pathEnd = i++;
    } else {
        while (i < to && !isSpaceOrTab(s[i].unicode())) ++i;
        pathEnd = i;
    }
    const QStringView written(s + pathStart, pathEnd - pathStart);
    const bool known = written.endsWith(QLatin1StringView(".md"), Qt::CaseInsensitive)
        || written.endsWith(QLatin1StringView(".markdown"), Qt::CaseInsensitive)
        || written.endsWith(QLatin1StringView(".txt"), Qt::CaseInsensitive);
    if (!known || written.size() < 4) return false;
    QString path = written.toString();
    // Inside a quoted path, a backslash may escape its closing quote.
    if (first == u'"' || first == u'\'') path.replace(u'\\' + QString(QChar(first)), QString(QChar(first)));
    // A URL is a link, not a file to include.
    if (path.contains(QLatin1StringView("://")) || path.startsWith(QLatin1StringView("www."), Qt::CaseInsensitive))
        return false;
    for (qsizetype k = 0; k < path.size(); ++k) {
        const char16_t c = path[k].unicode();
        if (c < 0x20 || c == u'<' || c == u'>' || c == u'[' || c == u']' || c == u'*' || c == u'`' || c == u'|'
            || c == u'\\')
            return false;
        if (c == u':' && k > 1) {
            // `scheme:` before any slash names something other than a file.
            bool scheme = isAsciiAlpha(path[0].unicode());
            for (qsizetype j = 1; j < k && scheme; ++j)
                scheme = isAsciiAlnum(path[j].unicode()) || path[j] == u'+' || path[j] == u'-' || path[j] == u'.';
            if (scheme) return false;
        }
    }
    const QChar firstPathChar = path[0];
    if (firstPathChar == u'#' || firstPathChar == u'!' || firstPathChar == u'(' || firstPathChar == u'=') return false;

    while (i < to && isSpaceOrTab(s[i].unicode())) ++i;
    QString caption;
    if (i < to) {
        const char16_t open = s[i].unicode();
        const char16_t close = open == u'(' ? u')' : open;
        if (open != u'"' && open != u'\'' && open != u'(') return false;
        if (to - i < 2 || s[to - 1].unicode() != close) return false;
        caption = QStringView(s + i + 1, to - i - 2).toString();
    }
    block->type = NodeType::Include;
    block->dest = path;
    block->title = caption;
    block->target = {pathStart, pathEnd};
    block->resolved = false;
    block->pieces.clear();
    return true;
}

void BlockParser::finalizeParagraph(Block *block)
{
    consumeReferences(block);
    std::vector<Piece> &pieces = block->pieces;
    bool hasText = false;
    for (const Piece &piece : pieces)
        if (!piece.comment) hasText = true;
    if (!hasText) {
        // Nothing but definitions and comments: the comments stand alone.
        for (const Piece &piece : pieces)
            insertBefore(block, newComment(piece.src, piece.len, piece.line, block->depth));
        unlink(block);
        return;
    }
    Piece &last = pieces.back();
    if (!last.comment)
        while (last.len > 0 && isSpaceOrTab(s[last.src + last.len - 1].unicode())) --last.len;
    block->start = pieces.front().src;
    block->startLine = pieces.front().line;
    block->end = last.src + last.len;
    block->endLine = last.line;

    if (pieces.size() != 1) return;
    const Piece &only = pieces.front();
    const QStringView text(s + only.src, only.len);
    if (opt.toc && text == QLatin1StringView("{{TOC}}")) {
        block->type = NodeType::TableOfContents;
        block->pieces.clear();
        return;
    }
    if (opt.includes && block->parent == root && only.len > 0) includeLine(block, only.src, only.src + only.len);
}

void BlockParser::finalize(Block *block, int line)
{
    Block *above = block->parent;
    block->open = false;
    block->endLine = line;
    block->end = line >= lineNumber ? lineEnd : prevLineEnd;
    if (block->end < block->start) block->end = block->start;

    switch (block->type) {
    case NodeType::Paragraph:
        finalizeParagraph(block);
        break;
    case NodeType::CodeBlock:
        if (block->fenced) {
            if (opt.mermaid) {
                const QStringView info = QStringView(block->label).trimmed();
                qsizetype word = 0;
                while (word < info.size() && !info[word].isSpace()) ++word;
                if (info.first(word).compare(QLatin1StringView("mermaid"), Qt::CaseInsensitive) == 0)
                    block->type = NodeType::MermaidBlock;
            }
            if (block->closed) {
                block->end = block->closeMarker.end;
            } else if (block->lastTextLine > 0) {
                block->end = qMax(block->start, block->lastTextEnd);
                block->endLine = block->lastTextLine;
            } else {
                block->end = qMax(block->openMarker.end, block->target.end);
                block->endLine = block->startLine;
            }
        } else {
            block->literal.truncate(block->literalTextLength);
            block->end = block->lastTextEnd;
            block->endLine = block->lastTextLine;
        }
        break;
    case NodeType::HtmlBlock:
        block->literal.truncate(block->literalTextLength);
        if (block->literal.endsWith(u'\n')) block->literal.chop(1);
        block->end = block->lastTextEnd;
        block->endLine = block->lastTextLine;
        break;
    case NodeType::MathBlock:
        if (!block->closed) {
            // Never closed: it is a paragraph, and says so unless the text
            // closes the math further along a line.
            bool closedInline = false;
            for (size_t i = 0; i < block->pieces.size() && !closedInline; ++i) {
                const Piece &piece = block->pieces[i];
                const int skip = i == 0 ? 2 : 0;
                closedInline = QStringView(s + piece.src + skip, piece.len - skip).contains(QLatin1StringView("$$"));
            }
            if (!closedInline)
                st.diagnose(fileId, block->openMarker, Severity::Warning, DiagnosticCode::UnclosedMath,
                            QStringLiteral("This display math is never closed with $$ and is shown as text"));
            block->type = NodeType::Paragraph;
            block->literal.clear();
            block->openMarker = block->closeMarker = {};
            finalizeParagraph(block);
        } else {
            if (block->literal.endsWith(u'\n')) block->literal.chop(1);
            block->end = block->closeMarker.end;
        }
        break;
    case NodeType::List: {
        for (Block *item = block->firstChild; item && block->tight; item = item->next) {
            if (item->next && item->endLine != item->next->startLine - 1) block->tight = false;
            if (blankBetweenChildren(item)) block->tight = false;
        }
        if (block->lastChild) {
            block->end = block->lastChild->end;
            block->endLine = block->lastChild->endLine;
        }
        break;
    }
    case NodeType::DefinitionDescription:
        if (blankBetweenChildren(block)) block->tight = false;
        [[fallthrough]];
    case NodeType::ListItem:
    case NodeType::FootnoteDefinition:
    case NodeType::CitationDefinition: {
        // The block ends with its last child. A comment line is no content:
        // blank lines before one do not make the block's end a blank line.
        Block *last = block->lastChild;
        while (last && last->type == NodeType::CommentLine) last = last->prev;
        block->end = block->lastChild ? block->lastChild->end : block->openMarker.end;
        block->endLine = last ? last->endLine : block->startLine;
        break;
    }
    case NodeType::DefinitionList:
        if (block->lastChild) {
            block->end = block->lastChild->end;
            block->endLine = block->lastChild->endLine;
        }
        break;
    case NodeType::Table:
        // The delimiter row is part of the table though it is no child of it.
        block->end = qMax(block->openMarker.end, block->lastChild ? block->lastChild->end : 0);
        block->endLine = qMax(block->lastTextLine, block->lastChild ? block->lastChild->endLine : 0);
        break;
    default:
        break;
    }
    tip = above;
}

} // namespace

void parseBlocks(ParseState &state, Block *root, int file, int run)
{
    BlockParser(state, root, file, run).run();
}

} // namespace md::detail
