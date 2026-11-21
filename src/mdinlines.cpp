// The inline parser: turns the text of one leaf block into inline nodes.
//
// It follows the CommonMark algorithm (a delimiter stack for emphasis, a
// bracket stack for links and images) and adds the extensions of the dialect
// at the same level, so that their precedence is the documented one: code
// spans, then links, notes and citations, then the paired markers.
//
// Nodes are built in a flat pool linked by index, in the coordinates of the
// block's content, and only become the tree, in source coordinates, at the
// end. Nothing here recurses on the input, so nesting cannot exhaust the stack.
#include "mdparser_p.h"

#include <algorithm>

namespace md::detail {
namespace {

// What a pool entry carries besides its place: only the few nodes that have
// text of their own or a destination need one.
struct Payload {
    QString text;
    QString dest;
    QString title;
    QString label;
    int destinationFile = -1;
    std::vector<Attribute> attributes;
};

struct INode {
    NodeType type = NodeType::Document;
    bool checked = false;
    bool display = false;
    bool resolved = true;
    int number = 0;
    int payload = -1;
    int parent = 0;
    int prev = -1;
    int next = -1;
    int first = -1;
    int last = -1;
    int start = 0;
    int end = 0;
    int openEnd = 0;     // the opening marker is [start, openEnd)
    int closeStart = 0;  // the closing marker is [closeStart, end)
    int targetStart = 0;
    int targetEnd = 0;
    bool tag = false;    // HtmlInline: an open or closing tag
    bool closing = false;
    char16_t marker = 0; // Citation: `-` when the author is left out
};

struct Delimiter {
    int node = -1;
    int prev = -1;
    int next = -1;
    char16_t cc = 0;
    int count = 0;
    int original = 0;
    bool canOpen = false;
    bool canClose = false;
};

struct Bracket {
    int node = -1;
    int previousDelimiter = -1;
    int index = 0;      // where the content starts
    bool image = false;
    bool note = false;  // `[^`
    bool active = true;
    bool bracketAfter = false;
};

struct PieceMap {
    int content = 0;
    int src = 0;
    int len = 0;
};

constexpr int MaxBackticks = 255;

bool isLabelToken(QStringView text)
{
    if (text.isEmpty()) return false;
    for (const QChar c : text)
        if (!c.isLetterOrNumber() && c != u'_' && c != u'-' && c != u':') return false;
    return true;
}

QStringView trimmedView(QStringView text)
{
    qsizetype a = 0, b = text.size();
    while (a < b && text[a].isSpace()) ++a;
    while (b > a && text[b - 1].isSpace()) --b;
    return text.sliced(a, b - a);
}

} // namespace

class InlineParserPrivate {
public:
    explicit InlineParserPrivate(ParseState &state)
        : st(state)
    {
        const Options &o = st.options;
        std::fill(std::begin(special), std::end(special), false);
        for (const char c : {'\n', '\\', '`', '&', '<', '*', '_', '[', ']', '!'}) special[int(c)] = true;
        if (o.strikethrough || (st.writer() && o.superSubscript)) special[int('~')] = true;
        if (o.highlight) special[int('=')] = true;
        if (st.writer() && o.superSubscript) special[int('^')] = true;
        if (o.math) special[int('$')] = true;
        if (o.autolinks) special[int(':')] = special[int('w')] = special[int('@')] = true;
        if (o.citations) special[int('@')] = true;
    }

    void parse(const std::vector<Piece> &pieces, const InlineContext &context, std::vector<Node> &out);

private:
    // Content and its mapping to the source.
    void setContent(const std::vector<Piece> &pieces);
    int toSource(int pos) const;
    Range sourceRange(int start, int end) const;
    const PieceMap &pieceAt(int pos) const;

    // The node pool.
    INode &n(int i) { return nodes[i]; }
    Payload &data(int i)
    {
        if (nodes[i].payload < 0) {
            nodes[i].payload = int(payloads.size());
            payloads.emplace_back();
        }
        return payloads[nodes[i].payload];
    }
    int newNode(NodeType type, int start, int end);
    int add(NodeType type, int start, int end, int parent = 0);
    void append(int parent, int node);
    void unlinkNode(int node);
    void insertAfter(int node, int after);
    void adopt(int parent, int from, int until);
    void flush(int upTo);
    void done(int pos) { m_pos = m_textStart = pos; }

    // Scanning.
    void run();
    void skipComments();
    bool newline();
    bool backslash();
    bool backticks();
    bool entity();
    bool angle();
    bool delimiterRun(char16_t cc);
    bool tilde();
    bool equals();
    bool script(NodeType type, char16_t marker);
    bool dollar();
    bool openBracket();
    bool bang();
    bool closeBracket();
    bool closeNote(const Bracket &opener, int closePos);
    bool variable();
    bool pandocCitation();
    bool inTextCitation();
    bool urlAutolink();
    bool wwwAutolink();
    int headingLabel(int *open, int *close);

    char32_t before(int pos) const;
    char32_t after(int pos) const;
    void pushDelimiter(int node, char16_t cc, int count, bool canOpen, bool canClose);
    void removeDelimiter(int index);
    void processEmphasis(int bottom);
    void pushBracket(int node, int index, bool image, bool note);
    void popBracket();
    void discardInside(const Bracket &opener);
    void wrapInside(const Bracket &opener, int node);
    void literalChildren(int parent, int start, int end);
    void addAutolink(int start, int end, const QString &dest);
    int domainLength(int start, bool allowShort) const;
    int autolinkDelimiter(int start, int length) const;
    int closingBackticks(int from, int length);
    bool inlineLinkTail(int from, int *end, QString *dest, QString *title, int *targetStart, int *targetEnd);

    // Lookups.
    const LinkReference *reference(const QString &label) const;
    int headingAlias(const QString &label, bool *ambiguous) const;
    int footnote(const QString &label) const;
    int citation(const QString &key, bool *external);
    void diagnose(int start, int end, Severity severity, DiagnosticCode code, const QString &message);

    // Building the tree.
    void pairHtml(int parent);
    void build(int parent, std::vector<Node> &out, int depth, bool inLink);
    void flatten(int parent, std::vector<Node> &out);
    Node finish(int index);
    void emitText(int start, int end, std::vector<Node> &out, bool inLink);
    void emitPlain(int start, int end, std::vector<Node> &out);
    Node textNode(int start, int end) const;

    ParseState &st;
    bool special[128];
    InlineContext ctx;
    FileState *file = nullptr;
    const FileState *rootFile = nullptr;
    const Run *runScope = nullptr;

    QString buffer;
    const QChar *s = nullptr;
    int m_len = 0;
    std::vector<PieceMap> map;
    std::vector<std::pair<int, int>> comments;
    size_t nextComment = 0;

    std::vector<INode> nodes;
    std::vector<Payload> payloads;
    std::vector<Delimiter> delimiters;
    int delimiterTop = -1;
    std::vector<Bracket> brackets;
    int plainBrackets = 0;   // pending brackets that are not note openers
    size_t inactiveBelow = 0; // brackets below this index are already inactive

    int m_pos = 0;
    int m_textStart = 0;
    bool sawAt = false;
    bool sawHtml = false;
    int backtickPositions[MaxBackticks + 1];
    bool backticksReset = false;
    bool backticksScanned = false;
    int displayFail = -1;
    int htmlFail[4] = {-1, -1, -1, -1}; // comment, instruction, declaration, CDATA
};

// --- Content -----------------------------------------------------------------

void InlineParserPrivate::setContent(const std::vector<Piece> &pieces)
{
    map.clear();
    comments.clear();
    nextComment = 0;
    const QString &text = file->text;
    // The common case needs no copy: the pieces are one unbroken run of the
    // source, joined by plain line feeds.
    bool contiguous = true;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const Piece &p = pieces[i];
        if (i + 1 < pieces.size()) {
            const int gap = p.src + p.len;
            if (!p.newline || pieces[i + 1].src != gap + 1 || text[gap] != u'\n') {
                contiguous = false;
                break;
            }
        }
    }
    if (pieces.empty()) {
        s = text.constData();
        m_len = 0;
        map.push_back({0, 0, 0});
        return;
    }
    if (contiguous) {
        const int start = pieces.front().src;
        s = text.constData() + start;
        m_len = pieces.back().src + pieces.back().len - start;
        map.push_back({0, start, m_len});
        for (const Piece &p : pieces)
            if (p.comment) comments.emplace_back(p.src - start, p.src - start + p.len);
        return;
    }
    buffer.resize(0);
    for (size_t i = 0; i < pieces.size(); ++i) {
        const Piece &p = pieces[i];
        const int at = int(buffer.size());
        map.push_back({at, p.src, p.len});
        buffer.append(QStringView(text).sliced(p.src, p.len));
        if (p.comment) comments.emplace_back(at, at + p.len);
        if (p.newline && i + 1 < pieces.size()) buffer.append(u'\n');
    }
    s = buffer.constData();
    m_len = int(buffer.size());
}

const PieceMap &InlineParserPrivate::pieceAt(int pos) const
{
    if (map.size() == 1) return map.front();
    auto it = std::upper_bound(map.begin(), map.end(), pos, [](int p, const PieceMap &m) { return p < m.content; });
    return it == map.begin() ? map.front() : *(it - 1);
}

int InlineParserPrivate::toSource(int pos) const
{
    const PieceMap &m = pieceAt(pos);
    return m.src + (pos - m.content);
}

// The source range of content [start, end). The end is taken from the piece
// that holds the last character, so that it never lands in the next line.
Range InlineParserPrivate::sourceRange(int start, int end) const
{
    const int a = toSource(start);
    if (end <= start) return {a, a};
    return {a, toSource(end - 1) + 1};
}

// --- Nodes ---------------------------------------------------------------------

int InlineParserPrivate::newNode(NodeType type, int start, int end)
{
    nodes.emplace_back();
    INode &node = nodes.back();
    node.type = type;
    node.start = start;
    node.end = end;
    node.openEnd = start;
    node.closeStart = end;
    return int(nodes.size()) - 1;
}

void InlineParserPrivate::append(int parent, int node)
{
    INode &child = nodes[node];
    INode &p = nodes[parent];
    child.parent = parent;
    child.prev = p.last;
    child.next = -1;
    if (p.last >= 0) nodes[p.last].next = node;
    else p.first = node;
    p.last = node;
}

int InlineParserPrivate::add(NodeType type, int start, int end, int parent)
{
    const int node = newNode(type, start, end);
    append(parent, node);
    return node;
}

void InlineParserPrivate::unlinkNode(int node)
{
    INode &x = nodes[node];
    if (x.prev >= 0) nodes[x.prev].next = x.next;
    else nodes[x.parent].first = x.next;
    if (x.next >= 0) nodes[x.next].prev = x.prev;
    else nodes[x.parent].last = x.prev;
    x.prev = x.next = -1;
}

void InlineParserPrivate::insertAfter(int node, int after)
{
    INode &x = nodes[node];
    INode &a = nodes[after];
    x.parent = a.parent;
    x.prev = after;
    x.next = a.next;
    if (a.next >= 0) nodes[a.next].prev = node;
    else nodes[a.parent].last = node;
    a.next = node;
}

// Moves the siblings from `from` up to, not including, `until` (or to the
// end of their list when `until` is -1) to the end of `parent`'s children.
void InlineParserPrivate::adopt(int parent, int from, int until)
{
    if (from < 0 || from == until) return;
    const int oldParent = nodes[from].parent;
    const int last = until >= 0 ? nodes[until].prev : nodes[oldParent].last;
    const int beforeFirst = nodes[from].prev;
    if (beforeFirst >= 0) nodes[beforeFirst].next = until;
    else nodes[oldParent].first = until;
    if (until >= 0) nodes[until].prev = beforeFirst;
    else nodes[oldParent].last = beforeFirst;

    INode &p = nodes[parent];
    nodes[from].prev = p.last;
    if (p.last >= 0) nodes[p.last].next = from;
    else p.first = from;
    p.last = last;
    nodes[last].next = -1;
    for (int i = from; i >= 0; i = nodes[i].next) nodes[i].parent = parent;
}

void InlineParserPrivate::flush(int upTo)
{
    if (upTo > m_textStart) add(NodeType::Text, m_textStart, upTo);
    m_textStart = upTo;
}

void InlineParserPrivate::diagnose(int start, int end, Severity severity, DiagnosticCode code, const QString &message)
{
    if (!ctx.quiet) st.diagnose(ctx.file, sourceRange(start, end), severity, code, message);
}

// --- Lookups -------------------------------------------------------------------

const LinkReference *InlineParserPrivate::reference(const QString &label) const
{
    auto it = file->references.constFind(label);
    if (it != file->references.constEnd()) return &*it;
    if (rootFile != file) {
        it = rootFile->references.constFind(label);
        if (it != rootFile->references.constEnd()) return &*it;
    }
    return nullptr;
}

// A heading by its explicit label or its text: in this reading of the file
// first, then anywhere in the assembled document.
int InlineParserPrivate::headingAlias(const QString &label, bool *ambiguous) const
{
    if (!st.aliasesReady) return -1;
    const QHash<QString, Alias> *const tables[] = {&runScope->labels, &runScope->titles, &st.labels, &st.titles};
    for (const QHash<QString, Alias> *table : tables) {
        const auto it = table->constFind(label);
        if (it != table->constEnd()) {
            *ambiguous = it->ambiguous;
            return it->heading;
        }
    }
    return -1;
}

int InlineParserPrivate::footnote(const QString &label) const
{
    auto it = file->footnotes.constFind(label);
    if (it != file->footnotes.constEnd()) return *it;
    if (rootFile != file) {
        it = rootFile->footnotes.constFind(label);
        if (it != rootFile->footnotes.constEnd()) return *it;
    }
    return -1;
}

// The entry of a citation key: defined in this file, else in the root
// document, else known to the external bibliography. -1 when it is none of these.
int InlineParserPrivate::citation(const QString &key, bool *external)
{
    *external = false;
    const QString label = normalizeLabel(key);
    auto it = file->citations.constFind(label);
    if (it != file->citations.constEnd()) return *it;
    if (rootFile != file) {
        it = rootFile->citations.constFind(label);
        if (it != rootFile->citations.constEnd()) return *it;
    }
    if (st.source && st.source->externalCitationKeys.contains(key)) {
        *external = true;
        const auto known = st.externalCitations.constFind(key);
        if (known != st.externalCitations.constEnd()) return *known;
        NoteEntry entry;
        entry.file = -1;
        entry.label = key;
        entry.external = true;
        st.citations.push_back(entry);
        const int index = int(st.citations.size()) - 1;
        st.externalCitations.insert(key, index);
        return index;
    }
    return -1;
}

// --- The scan --------------------------------------------------------------------

void InlineParserPrivate::parse(const std::vector<Piece> &pieces, const InlineContext &context, std::vector<Node> &out)
{
    ctx = context;
    file = &st.files[ctx.file];
    rootFile = &st.files[0];
    runScope = &st.runs[ctx.run];
    setContent(pieces);

    nodes.clear();
    nodes.emplace_back();
    payloads.clear();
    delimiters.clear();
    delimiterTop = -1;
    brackets.clear();
    plainBrackets = 0;
    inactiveBelow = 0;
    m_pos = m_textStart = 0;
    sawAt = sawHtml = false;
    backticksReset = backticksScanned = false;
    displayFail = -1;
    std::fill(std::begin(htmlFail), std::end(htmlFail), -1);

    int labelOpen = -1, labelClose = -1;
    if (ctx.heading && st.options.headingLabels) {
        const int cut = headingLabel(&labelOpen, &labelClose);
        if (cut >= 0) m_len = cut;
    }
    if (ctx.task && st.options.taskLists && m_len >= 3 && s[0] == u'[' && s[2] == u']'
        && (s[1] == u' ' || s[1] == u'x' || s[1] == u'X')
        && (m_len == 3 || isSpaceOrTab(s[3].unicode()) || s[3] == u'\n')) {
        const int marker = add(NodeType::TaskMarker, 0, 3);
        n(marker).checked = s[1] != u' ';
        done(3);
    }
    run();
    processEmphasis(-1);
    if (labelOpen >= 0) {
        const int label = add(NodeType::HeadingLabel, labelOpen, labelClose);
        n(label).openEnd = labelOpen + 1;
        n(label).closeStart = labelClose - 1;
        data(label).label = trimmedView(QStringView(s + labelOpen + 1, labelClose - labelOpen - 2)).toString();
    }

    // A comment that ends the block leaves no line break before it.
    int tail = nodes[0].last;
    while (tail >= 0 && nodes[tail].type == NodeType::CommentLine) tail = nodes[tail].prev;
    if (tail >= 0 && tail != nodes[0].last
        && (nodes[tail].type == NodeType::SoftBreak || nodes[tail].type == NodeType::HardBreak))
        unlinkNode(tail);

    build(0, out, 0, false);
}

void InlineParserPrivate::skipComments()
{
    while (nextComment < comments.size() && comments[nextComment].first < m_pos) ++nextComment;
    while (nextComment < comments.size() && comments[nextComment].first == m_pos) {
        const auto [start, end] = comments[nextComment++];
        const int node = add(NodeType::CommentLine, start, end);
        n(node).openEnd = qMin(end, start + 2);
        data(node).text = trimmedView(QStringView(s + start + 2, qMax(0, end - start - 2))).toString();
        m_pos = end;
        if (m_pos < m_len && s[m_pos] == u'\n') ++m_pos;
        m_textStart = m_pos;
    }
}

void InlineParserPrivate::run()
{
    skipComments();
    while (m_pos < m_len) {
        const char16_t c = s[m_pos].unicode();
        if (c >= 128 || !special[c]) {
            ++m_pos;
            continue;
        }
        bool handled = false;
        switch (c) {
        case u'\n': handled = newline(); break;
        case u'\\': handled = backslash(); break;
        case u'`': handled = backticks(); break;
        case u'&': handled = entity(); break;
        case u'<': handled = angle(); break;
        case u'*':
        case u'_': handled = delimiterRun(c); break;
        case u'~': handled = tilde(); break;
        case u'=': handled = equals(); break;
        case u'^': handled = script(NodeType::Superscript, u'^'); break;
        case u'$': handled = dollar(); break;
        case u'[': handled = openBracket(); break;
        case u'!': handled = bang(); break;
        case u']': handled = closeBracket(); break;
        case u':': handled = urlAutolink(); break;
        case u'w': handled = wwwAutolink(); break;
        case u'@':
            sawAt = true;
            handled = inTextCitation();
            break;
        }
        if (!handled) ++m_pos;
    }
    flush(m_len);
}

bool InlineParserPrivate::newline()
{
    const int at = m_pos;
    int textEnd = at;
    while (textEnd > m_textStart && s[textEnd - 1] == u' ') --textEnd;
    const bool hard = at - textEnd >= 2;
    flush(textEnd);
    int next = at + 1;
    while (next < m_len && s[next] == u' ') ++next;
    add(hard ? NodeType::HardBreak : NodeType::SoftBreak, hard ? textEnd : at, at + 1);
    done(next);
    skipComments();
    return true;
}

bool InlineParserPrivate::backslash()
{
    if (m_pos + 1 >= m_len) return false;
    const char16_t next = s[m_pos + 1].unicode();
    if (next == u'\n') {
        flush(m_pos);
        add(NodeType::HardBreak, m_pos, m_pos + 2);
        int after = m_pos + 2;
        while (after < m_len && s[after] == u' ') ++after;
        done(after);
        skipComments();
        return true;
    }
    if (!isAsciiPunct(next)) return false;
    flush(m_pos);
    const int node = add(NodeType::Escape, m_pos, m_pos + 2);
    n(node).openEnd = m_pos + 1;
    data(node).text = QString(QChar(next));
    done(m_pos + 2);
    return true;
}

// The start of the backtick run of `length` that closes a code span opened
// before `from`, or -1. Positions of runs are remembered by length, so that
// many unmatched runs cost one pass over the text, not one each.
int InlineParserPrivate::closingBackticks(int from, int length)
{
    if (length > MaxBackticks) return -1;
    if (!backticksReset) {
        std::fill(std::begin(backtickPositions), std::end(backtickPositions), -1);
        backticksReset = true;
    }
    if (backticksScanned && backtickPositions[length] < from) return -1;
    int i = from;
    for (;;) {
        while (i < m_len && s[i] != u'`') ++i;
        if (i >= m_len) break;
        const int start = i;
        while (i < m_len && s[i] == u'`') ++i;
        const int count = i - start;
        if (count <= MaxBackticks) backtickPositions[count] = start;
        if (count == length) return start;
    }
    backticksScanned = true;
    return -1;
}

bool InlineParserPrivate::backticks()
{
    const int start = m_pos;
    int count = 0;
    while (start + count < m_len && s[start + count] == u'`') ++count;
    const int closer = closingBackticks(start + count, count);
    if (closer < 0) {
        m_pos = start + count;
        return true;
    }
    flush(start);
    const int node = add(NodeType::Code, start, closer + count);
    n(node).openEnd = start + count;
    n(node).closeStart = closer;
    QString code(s + start + count, closer - start - count);
    code.replace(u'\n', u' ');
    if (code.size() >= 2 && code.front() == u' ' && code.back() == u' ') {
        bool allSpaces = true;
        for (const QChar c : std::as_const(code)) {
            if (c != u' ') {
                allSpaces = false;
                break;
            }
        }
        if (!allSpaces) code = code.sliced(1, code.size() - 2);
    }
    data(node).text = code;
    done(closer + count);
    return true;
}

bool InlineParserPrivate::entity()
{
    QString value;
    const int length = scanEntity(s, m_len, m_pos, &value);
    if (!length) return false;
    flush(m_pos);
    const int node = add(NodeType::Entity, m_pos, m_pos + length);
    data(node).text = value;
    done(m_pos + length);
    return true;
}

bool InlineParserPrivate::angle()
{
    const int start = m_pos;
    // <scheme:...> and <address@host>
    int i = start + 1;
    if (i < m_len && isAsciiAlpha(s[i].unicode())) {
        int e = i + 1;
        while (e < m_len && e - i < 32) {
            const char16_t c = s[e].unicode();
            if (!isAsciiAlnum(c) && c != u'+' && c != u'.' && c != u'-') break;
            ++e;
        }
        if (e - i >= 2 && e < m_len && s[e] == u':') {
            ++e;
            while (e < m_len) {
                const char16_t c = s[e].unicode();
                if (c == u'<' || c == u'>' || c <= 0x20) break;
                ++e;
            }
            if (e < m_len && s[e] == u'>') {
                flush(start);
                const int node = add(NodeType::Autolink, start, e + 1);
                n(node).openEnd = start + 1;
                n(node).closeStart = e;
                data(node).dest = QString(s + i, e - i);
                add(NodeType::Text, i, e, node);
                done(e + 1);
                return true;
            }
        }
    }
    {
        // An e-mail address: local part, `@`, and labels of letters, digits and hyphens.
        int e = i;
        while (e < m_len) {
            const char16_t c = s[e].unicode();
            if (!isAsciiAlnum(c) && !QStringView(u".!#$%&'*+/=?^_`{|}~-").contains(QChar(c))) break;
            ++e;
        }
        if (e > i && e < m_len && s[e] == u'@') {
            int p = e + 1;
            bool ok = false;
            for (;;) {
                const int labelStart = p;
                while (p < m_len && (isAsciiAlnum(s[p].unicode()) || s[p] == u'-')) ++p;
                const int length = p - labelStart;
                if (length < 1 || length > 63 || s[labelStart] == u'-' || s[p - 1] == u'-') break;
                if (p < m_len && s[p] == u'.') {
                    ++p;
                    continue;
                }
                ok = p < m_len && s[p] == u'>';
                break;
            }
            if (ok) {
                flush(start);
                const int node = add(NodeType::Autolink, start, p + 1);
                n(node).openEnd = start + 1;
                n(node).closeStart = p;
                data(node).dest = QStringLiteral("mailto:") + QStringView(s + i, p - i);
                add(NodeType::Text, i, p, node);
                done(p + 1);
                return true;
            }
        }
    }

    // Raw HTML. A construct that was once found unclosed stays unclosed for
    // every later start, so the search to the end is made once per kind.
    int failKind = -1;
    if (start + 1 < m_len) {
        const char16_t c = s[start + 1].unicode();
        if (c == u'?') failKind = 1;
        else if (c == u'!' && start + 3 < m_len && s[start + 2] == u'-' && s[start + 3] == u'-') failKind = 0;
        else if (c == u'!' && start + 2 < m_len && s[start + 2] == u'[') failKind = 3;
        else if (c == u'!' && start + 2 < m_len && isAsciiAlpha(s[start + 2].unicode())) failKind = 2;
    }
    if (failKind >= 0 && htmlFail[failKind] >= 0 && start >= htmlFail[failKind]) return false;
    HtmlKind kind = HtmlKind::None;
    const int length = scanHtml(s, m_len, start, &kind);
    if (!length) {
        if (failKind >= 0) htmlFail[failKind] = start;
        return false;
    }
    flush(start);
    const int node = add(kind == HtmlKind::Comment ? NodeType::HtmlComment : NodeType::HtmlInline, start, start + length);
    data(node).text = QString(s + start, length);
    if (kind == HtmlKind::OpenTag || kind == HtmlKind::CloseTag) {
        n(node).tag = true;
        n(node).closing = kind == HtmlKind::CloseTag;
        sawHtml = true;
    }
    done(start + length);
    return true;
}

char32_t InlineParserPrivate::before(int pos) const
{
    if (pos <= 0) return u'\n';
    const QChar c = s[pos - 1];
    if (c.isLowSurrogate() && pos >= 2 && s[pos - 2].isHighSurrogate())
        return QChar::surrogateToUcs4(s[pos - 2], c);
    return c.unicode();
}

char32_t InlineParserPrivate::after(int pos) const
{
    if (pos >= m_len) return u'\n';
    const QChar c = s[pos];
    if (c.isHighSurrogate() && pos + 1 < m_len && s[pos + 1].isLowSurrogate())
        return QChar::surrogateToUcs4(c, s[pos + 1]);
    return c.unicode();
}

void InlineParserPrivate::pushDelimiter(int node, char16_t cc, int count, bool canOpen, bool canClose)
{
    Delimiter d;
    d.node = node;
    d.cc = cc;
    d.count = d.original = count;
    d.canOpen = canOpen;
    d.canClose = canClose;
    d.prev = delimiterTop;
    delimiters.push_back(d);
    const int index = int(delimiters.size()) - 1;
    if (delimiterTop >= 0) delimiters[delimiterTop].next = index;
    delimiterTop = index;
}

void InlineParserPrivate::removeDelimiter(int index)
{
    Delimiter &d = delimiters[index];
    if (d.prev >= 0) delimiters[d.prev].next = d.next;
    if (d.next >= 0) delimiters[d.next].prev = d.prev;
    else delimiterTop = d.prev;
}

// A run of `*`, `_`, `~` or `=`: a text node now, emphasis later if the run
// finds its partner.
bool InlineParserPrivate::delimiterRun(char16_t cc)
{
    const int start = m_pos;
    int count = 0;
    while (start + count < m_len && s[start + count].unicode() == cc) ++count;
    const char32_t b = before(start);
    const char32_t a = after(start + count);
    const bool afterSpace = isUnicodeSpace(a);
    const bool afterPunct = isUnicodePunct(a);
    const bool beforeSpace = isUnicodeSpace(b);
    const bool beforePunct = isUnicodePunct(b);
    const bool left = !afterSpace && (!afterPunct || beforeSpace || beforePunct);
    const bool right = !beforeSpace && (!beforePunct || afterSpace || afterPunct);
    bool canOpen = left;
    bool canClose = right;
    if (cc == u'_') {
        canOpen = left && (!right || beforePunct);
        canClose = right && (!left || afterPunct);
    }
    flush(start);
    const int node = add(NodeType::Text, start, start + count);
    done(start + count);
    if (canOpen || canClose) pushDelimiter(node, cc, count, canOpen, canClose);
    return true;
}

bool InlineParserPrivate::tilde()
{
    int count = 0;
    while (m_pos + count < m_len && s[m_pos + count] == u'~') ++count;
    const Options &o = st.options;
    if (st.writer()) {
        // Two tildes strike through; one is a subscript.
        if (count == 2 && o.strikethrough) return delimiterRun(u'~');
        if (count == 1 && o.superSubscript) return script(NodeType::Subscript, u'~');
    } else if (count <= 2 && o.strikethrough) {
        return delimiterRun(u'~');
    }
    m_pos += count;
    return true;
}

bool InlineParserPrivate::equals()
{
    int count = 0;
    while (m_pos + count < m_len && s[m_pos + count] == u'=') ++count;
    if (count == 2) return delimiterRun(u'=');
    m_pos += count;
    return true;
}

void InlineParserPrivate::processEmphasis(int bottom)
{
    if (delimiterTop < 0 || delimiterTop == bottom) return;
    // One lower bound per kind of closer, below which no opener for it exists.
    enum { Slots = 17 };
    int openersBottom[Slots];
    std::fill(std::begin(openersBottom), std::end(openersBottom), bottom);

    int closer = delimiterTop;
    while (closer >= 0 && delimiters[closer].prev != bottom) closer = delimiters[closer].prev;

    while (closer >= 0) {
        if (!delimiters[closer].canClose) {
            closer = delimiters[closer].next;
            continue;
        }
        const Delimiter c = delimiters[closer];
        const bool emphasis = c.cc == u'*' || c.cc == u'_';
        int slot = 0;
        if (c.cc == u'_') slot = 2 + (c.canOpen ? 3 : 0) + c.original % 3;
        else if (c.cc == u'*') slot = 8 + (c.canOpen ? 3 : 0) + c.original % 3;
        else if (c.cc == u'~') slot = 13 + c.original;
        else slot = 16;

        int opener = c.prev;
        bool found = false;
        while (opener >= 0 && opener != bottom && opener != openersBottom[slot]) {
            const Delimiter &o = delimiters[opener];
            if (o.cc == c.cc && o.canOpen) {
                if (emphasis) {
                    const bool odd = (c.canOpen || o.canClose) && c.original % 3 != 0
                        && (o.original + c.original) % 3 == 0;
                    if (!odd) found = true;
                } else if (o.count == c.count) {
                    found = true;
                }
                if (found) break;
            }
            opener = o.prev;
        }

        const int oldCloser = closer;
        if (!found) {
            closer = c.next;
            openersBottom[slot] = c.prev;
            if (!c.canOpen) removeDelimiter(oldCloser);
            continue;
        }

        const int use = emphasis ? ((c.count >= 2 && delimiters[opener].count >= 2) ? 2 : 1) : c.count;
        const int openerNode = delimiters[opener].node;
        const int closerNode = c.node;
        NodeType type = NodeType::Emphasis;
        if (c.cc == u'~') type = NodeType::Strikethrough;
        else if (c.cc == u'=') type = NodeType::Highlight;
        else if (use == 2) type = NodeType::Strong;

        // The markers come off the inner ends of the two runs.
        const int wrapper = newNode(type, nodes[openerNode].end - use, nodes[closerNode].start + use);
        nodes[wrapper].openEnd = nodes[openerNode].end;
        nodes[wrapper].closeStart = nodes[closerNode].start;
        nodes[openerNode].end -= use;
        nodes[closerNode].start += use;
        adopt(wrapper, nodes[openerNode].next, closerNode);
        insertAfter(wrapper, openerNode);

        // Delimiters between the two can no longer match anything.
        if (delimiters[opener].next != closer) {
            delimiters[opener].next = closer;
            delimiters[closer].prev = opener;
        }
        delimiters[opener].count -= use;
        delimiters[closer].count -= use;
        if (delimiters[opener].count == 0) {
            unlinkNode(openerNode);
            removeDelimiter(opener);
        }
        if (delimiters[closer].count == 0) {
            unlinkNode(closerNode);
            const int next = delimiters[closer].next;
            removeDelimiter(closer);
            closer = next;
        }
    }

    while (delimiterTop >= 0 && delimiterTop != bottom) removeDelimiter(delimiterTop);
}

// Text between script markers, where only escapes and character references
// mean anything.
void InlineParserPrivate::literalChildren(int parent, int start, int end)
{
    int textStart = start;
    int i = start;
    auto text = [&](int upTo) {
        if (upTo > textStart) add(NodeType::Text, textStart, upTo, parent);
    };
    while (i < end) {
        const char16_t c = s[i].unicode();
        if (c == u'\\' && i + 1 < end && isAsciiPunct(s[i + 1].unicode())) {
            text(i);
            const int node = add(NodeType::Escape, i, i + 2, parent);
            n(node).openEnd = i + 1;
            data(node).text = QString(s[i + 1]);
            i += 2;
            textStart = i;
            continue;
        }
        if (c == u'&') {
            QString value;
            if (const int length = scanEntity(s, end, i, &value)) {
                text(i);
                const int node = add(NodeType::Entity, i, i + length, parent);
                data(node).text = value;
                i += length;
                textStart = i;
                continue;
            }
        }
        ++i;
    }
    text(end);
}

// `x^2`, `y^(a+b)^`, `x~z`, `x~y,z~`. The marker must follow the thing it
// raises or lowers. The delimited form is tried first.
bool InlineParserPrivate::script(NodeType type, char16_t marker)
{
    const int start = m_pos;
    if (start == 0) return false;
    // What is raised or lowered: a letter, a digit, or what closes a group.
    const char32_t base = before(start);
    const QChar::Category category = QChar::category(base);
    if (!QChar::isLetterOrNumber(base) && !QChar::isMark(base) && category != QChar::Punctuation_Close
        && category != QChar::Punctuation_FinalQuote)
        return false;
    int i = start + 1;
    bool delimited = false;
    while (i < m_len) {
        const char16_t c = s[i].unicode();
        if (c == marker) {
            // Two tildes are a strikethrough marker, which comes first.
            if (marker == u'~' && i + 1 < m_len && s[i + 1] == u'~') break;
            delimited = i > start + 1;
            break;
        }
        if (c == u'\\' && i + 1 < m_len && isAsciiPunct(s[i + 1].unicode())) {
            i += 2;
            continue;
        }
        if (c == u'`' || c == u'[' || c == u']' || QChar(c).isSpace()) break;
        ++i;
    }
    if (delimited) {
        flush(start);
        const int node = add(type, start, i + 1);
        n(node).openEnd = start + 1;
        n(node).closeStart = i;
        literalChildren(node, start + 1, i);
        done(i + 1);
        return true;
    }
    i = start + 1;
    while (i < m_len) {
        const char32_t c = after(i);
        if (!QChar::isLetterOrNumber(c)) break;
        i += c > 0xffff ? 2 : 1;
    }
    if (i == start + 1) return false;
    flush(start);
    const int node = add(type, start, i);
    n(node).openEnd = start + 1;
    add(NodeType::Text, start + 1, i, node);
    done(i);
    return true;
}

bool InlineParserPrivate::dollar()
{
    const int start = m_pos;
    int count = 0;
    while (start + count < m_len && s[start + count] == u'$') ++count;
    if (count > 2) {
        m_pos = start + count;
        return true;
    }
    if (count == 2) {
        // Display math within a paragraph: everything up to the next `$$`.
        if (displayFail < 0 || start < displayFail) {
            for (int i = start + 2; i + 1 < m_len; ++i) {
                if (s[i] == u'\\') {
                    ++i;
                    continue;
                }
                if (s[i] != u'$' || s[i + 1] != u'$') continue;
                const QStringView tex = trimmedView(QStringView(s + start + 2, i - start - 2));
                if (tex.isEmpty()) break;
                flush(start);
                const int node = add(NodeType::MathInline, start, i + 2);
                n(node).openEnd = start + 2;
                n(node).closeStart = i;
                n(node).display = true;
                data(node).text = tex.toString();
                done(i + 2);
                return true;
            }
            displayFail = start;
        }
        m_pos = start + 2;
        return true;
    }
    // Inline math: content that neither starts nor ends with a space, with
    // no letter or digit against the outside of either dollar sign. The
    // next dollar sign closes the math or nothing does: TeX has no bare
    // dollar inside math, and a sentence with several prices has.
    if (start + 1 >= m_len || s[start + 1].isSpace()) return false;
    if (start > 0 && QChar::isLetterOrNumber(before(start))) return false;
    int i = start + 1;
    while (i < m_len && s[i] != u'$') i += s[i] == u'\\' ? 2 : 1;
    if (i >= m_len || (i + 1 < m_len && s[i + 1] == u'$')) return false;
    if (s[i - 1].isSpace()) return false;
    if (i + 1 < m_len && QChar::isLetterOrNumber(after(i + 1))) return false;
    flush(start);
    const int node = add(NodeType::MathInline, start, i + 1);
    n(node).openEnd = start + 1;
    n(node).closeStart = i;
    data(node).text = QString(s + start + 1, i - start - 1);
    done(i + 1);
    return true;
}

// --- Brackets --------------------------------------------------------------------

void InlineParserPrivate::pushBracket(int node, int index, bool image, bool note)
{
    if (!brackets.empty()) brackets.back().bracketAfter = true;
    Bracket b;
    b.node = node;
    b.previousDelimiter = delimiterTop;
    b.index = index;
    b.image = image;
    b.note = note;
    brackets.push_back(b);
    if (!note) ++plainBrackets;
}

void InlineParserPrivate::popBracket()
{
    if (!brackets.back().note) --plainBrackets;
    brackets.pop_back();
    inactiveBelow = qMin(inactiveBelow, brackets.size());
}

bool InlineParserPrivate::openBracket()
{
    const int start = m_pos;
    const char16_t next = start + 1 < m_len ? s[start + 1].unicode() : 0;
    const Options &o = st.options;
    if (next == u'%' && o.variables && variable()) return true;
    // `[@key]`, `[-@key]`, which leaves the author out, and `[see @key]`;
    // `[^` is a note's and `[%` a variable's.
    if (o.citations && next != u'^' && next != u'%' && pandocCitation()) return true;
    const bool note = next == u'^' && o.footnotes && st.writer();
    const int width = note ? 2 : 1;
    flush(start);
    const int node = add(NodeType::Text, start, start + width);
    pushBracket(node, start + width, false, note);
    done(start + width);
    return true;
}

bool InlineParserPrivate::bang()
{
    const int start = m_pos;
    if (start + 1 >= m_len || s[start + 1] != u'[') return false;
    // In Writer `![^` is an exclamation mark before a note, not an image.
    if (st.writer() && st.options.footnotes && start + 2 < m_len && s[start + 2] == u'^') return false;
    flush(start);
    const int node = add(NodeType::Text, start, start + 2);
    pushBracket(node, start + 2, true, false);
    done(start + 2);
    return true;
}

// `[%key]`: a value of the front matter, of this file or else of the root.
bool InlineParserPrivate::variable()
{
    const int start = m_pos;
    int i = start + 2;
    while (i < m_len && s[i] != u']' && s[i] != u'[' && s[i] != u'\n') ++i;
    if (i >= m_len || s[i] != u']') return false;
    const QStringView key = trimmedView(QStringView(s + start + 2, i - start - 2));
    if (key.isEmpty()) return false;
    if (i + 1 < m_len && (s[i + 1] == u'(' || s[i + 1] == u'[')) return false;
    flush(start);
    const int node = add(NodeType::Variable, start, i + 1);
    n(node).openEnd = start + 2;
    n(node).closeStart = i;
    data(node).label = key.toString();

    const std::vector<SourceFile> &sources = st.document->files;
    std::optional<QString> value = sources[ctx.file].frontMatter.scalar(key);
    const YamlValue *found = sources[ctx.file].frontMatter.data.find(key);
    if (!value && !found && ctx.file != 0) {
        value = sources[0].frontMatter.scalar(key);
        found = sources[0].frontMatter.data.find(key);
    }
    if (value) {
        QString text = *value;
        while (text.endsWith(u'\n')) text.chop(1);
        data(node).text = text;
    } else {
        n(node).resolved = false;
        diagnose(start, i + 1, Severity::Warning, DiagnosticCode::UnresolvedVariable,
                 found ? QStringLiteral("The metadata value \"%1\" is not plain text").arg(key)
                       : QStringLiteral("No metadata value is named \"%1\"").arg(key));
    }
    done(i + 1);
    return true;
}

// `[@key]`, `[@key, locator]` and `[@one; @two, locator]`, where what
// follows a locator is a suffix (`[@key, p. 23, emphasis added]`), and
// `[-@key]`, which leaves the author out, as Pandoc has them. Words before
// an item's key are its prefix (`[see @one, p. 3; also @two]`). The key's
// `@` (or `-@`) starts the item or follows white space, so that an e-mail
// address is never one; every item needs a key, and a bracket followed by
// `(` or `[` is a link.
bool InlineParserPrivate::pandocCitation()
{
    const int start = m_pos;
    int close = start + 1;
    bool anyAt = false;
    while (close < m_len && s[close] != u']' && s[close] != u'[' && s[close] != u'\n') {
        if (s[close] == u'@') anyAt = true;
        ++close;
    }
    if (!anyAt || close >= m_len || s[close] != u']') return false;
    if (close + 1 < m_len && (s[close + 1] == u'(' || s[close + 1] == u'[')) return false;

    struct Item {
        // The item's own source, from its `@` or `-@`; its prefix is before it.
        int start, end, at, keyEnd;
        bool suppressAuthor;
        QString key, prefix, locator;
    };
    std::vector<Item> items;
    int i = start + 1;
    while (i < close) {
        int itemEnd = i;
        while (itemEnd < close && s[itemEnd] != u';') ++itemEnd;
        int a = i, b = itemEnd;
        while (a < b && s[a].isSpace()) ++a;
        while (b > a && s[b - 1].isSpace()) --b;
        int at = -1;
        bool suppress = false;
        for (int p = a; p < b; ++p) {
            if (s[p] != u'@') continue;
            const bool dash = p > a && s[p - 1] == u'-';
            const int marker = dash ? p - 1 : p;
            if (marker == a || s[marker - 1].isSpace()) {
                at = p;
                suppress = dash;
                break;
            }
        }
        if (at < 0) return false;
        const int marker = suppress ? at - 1 : at;
        // A prefix is words: code, markup, math or an escape before the key
        // could hold the key itself.
        for (int p = a; p < marker; ++p) {
            const char16_t c = s[p].unicode();
            if (c == u'`' || c == u'<' || c == u'\\' || c == u'$') return false;
        }
        int keyEnd = at + 1;
        while (keyEnd < b && !s[keyEnd].isSpace() && s[keyEnd] != u',') ++keyEnd;
        if (keyEnd == at + 1) return false;
        int locator = keyEnd;
        if (locator < b && s[locator] == u',') ++locator;
        Item item;
        item.start = marker;
        item.end = b;
        item.at = at;
        item.keyEnd = keyEnd;
        item.suppressAuthor = suppress;
        item.key = QString(s + at + 1, keyEnd - at - 1);
        item.prefix = trimmedView(QStringView(s + a, marker - a)).toString();
        item.locator = trimmedView(QStringView(s + locator, b - locator)).toString();
        items.push_back(item);
        i = itemEnd + 1;
    }
    if (items.empty()) return false;

    flush(start);
    const int group = add(NodeType::Citation, start, close + 1);
    n(group).openEnd = start + 1;
    n(group).closeStart = close;
    auto fill = [&](int node, const Item &item) {
        bool external = false;
        const int entry = citation(item.key, &external);
        data(node).label = item.key;
        data(node).text = item.locator;
        if (!item.prefix.isEmpty()) data(node).title = item.prefix;
        if (item.suppressAuthor) n(node).marker = u'-';
        n(node).targetStart = item.at + 1;
        n(node).targetEnd = item.keyEnd;
        n(node).number = -(entry + 1); // until notes are numbered
        if (entry < 0) {
            n(node).resolved = false;
            diagnose(item.at, item.keyEnd, Severity::Warning, DiagnosticCode::MissingCitation,
                     QStringLiteral("No citation is defined for \"%1\"").arg(item.key));
        }
    };
    if (items.size() == 1) {
        fill(group, items.front());
    } else {
        for (const Item &item : items) {
            const int node = add(NodeType::Citation, item.start, item.end, group);
            // Its `@` (or `-@`) is its marker.
            n(node).openEnd = item.at + 1;
            fill(node, item);
        }
    }
    done(close + 1);
    return true;
}

// `@key` in the running text, a citation whose author is part of the
// sentence, as Pandoc writes it: `@doe99 says`, and with a locator,
// `@doe99 [p. 33] says`. Its `@` starts the text or follows white space or
// an emphasis marker, so that an e-mail address is never one. A key holds
// letters, digits and `_`, and inside them also `:.#$%&-+?<>~/`, so that
// punctuation after it ends it. Only a key the document defines, or its
// bibliography has, is a citation: a handle such as `@someone` stays text.
// Inside brackets that are not a citation (link text, say) it is text.
bool InlineParserPrivate::inTextCitation()
{
    if (!st.options.citations || plainBrackets > 0) return false;
    const int at = m_pos;
    if (at > 0 && !s[at - 1].isSpace() && s[at - 1] != u'*' && s[at - 1] != u'_') return false;
    const auto keyChar = [](QChar c) { return c.isLetterOrNumber() || c == u'_'; };
    if (at + 1 >= m_len || !keyChar(s[at + 1])) return false;
    int end = at + 1;
    while (end < m_len && (keyChar(s[end]) || QStringView(u":.#$%&-+?<>~/").contains(s[end]))) ++end;
    while (!keyChar(s[end - 1])) --end;
    const QString key(s + at + 1, end - at - 1);
    bool external = false;
    const int entry = citation(key, &external);
    if (entry < 0) return false;

    // `@key [locator]`: one space, then brackets that are no link, note or
    // variable, and hold no other citation, code or escape.
    int nodeEnd = end, closeStart = end;
    QString locator;
    if (end + 2 < m_len && s[end] == u' ' && s[end + 1] == u'[' && s[end + 2] != u'^' && s[end + 2] != u'%') {
        int c = end + 2;
        while (c < m_len && !QStringView(u"[]\n@`\\").contains(s[c])) ++c;
        const bool link = c + 1 < m_len && (s[c + 1] == u'(' || s[c + 1] == u'[');
        if (c < m_len && s[c] == u']' && !link) {
            const QStringView written = trimmedView(QStringView(s + end + 2, c - end - 2));
            if (!written.isEmpty()) {
                locator = written.toString();
                closeStart = c;
                nodeEnd = c + 1;
            }
        }
    }
    flush(at);
    const int node = add(NodeType::Citation, at, nodeEnd);
    n(node).openEnd = at + 1;
    n(node).closeStart = closeStart;
    n(node).marker = u'@';
    n(node).targetStart = at + 1;
    n(node).targetEnd = end;
    n(node).number = -(entry + 1); // until notes are numbered
    data(node).label = key;
    data(node).text = locator;
    done(nodeEnd);
    return true;
}

bool InlineParserPrivate::inlineLinkTail(int from, int *end, QString *dest, QString *title, int *targetStart,
                                         int *targetEnd)
{
    int p = skipSpacesAndNewline(s, m_len, from + 1);
    int destStart = 0, destEnd = 0;
    const int length = scanLinkDestination(s, m_len, p, &destStart, &destEnd);
    if (length < 0) return false;
    p += length;
    const int afterDest = p;
    p = skipSpacesAndNewline(s, m_len, p);
    int titleStart = -1, titleEnd = -1;
    if (p > afterDest) {
        if (const int t = scanLinkTitle(s, m_len, p)) {
            titleStart = p + 1;
            titleEnd = p + t - 1;
            p = skipSpacesAndNewline(s, m_len, p + t);
        }
    }
    if (p >= m_len || s[p] != u')') return false;
    *end = p + 1;
    *dest = unescape(QStringView(s + destStart, destEnd - destStart));
    *title = titleStart >= 0 ? unescape(QStringView(s + titleStart, titleEnd - titleStart)) : QString();
    *targetStart = destStart;
    *targetEnd = destEnd;
    return true;
}

// Drops everything parsed since a bracket opened: the nodes after it and
// the delimiters above it.
void InlineParserPrivate::discardInside(const Bracket &opener)
{
    while (nodes[opener.node].next >= 0) unlinkNode(nodes[nodes[opener.node].parent].last);
    while (delimiterTop >= 0 && delimiterTop != opener.previousDelimiter) removeDelimiter(delimiterTop);
}

// Makes everything parsed since a bracket opened the content of `node`,
// which takes the opener's place.
void InlineParserPrivate::wrapInside(const Bracket &opener, int node)
{
    adopt(node, nodes[opener.node].next, -1);
    insertAfter(node, opener.node);
    unlinkNode(opener.node);
    processEmphasis(opener.previousDelimiter);
}

// The `]` of a Writer `[^…]`. A defined label is a reference. Without a
// definition, a single word is a reference that cannot be resolved, and
// anything longer is an inline note.
bool InlineParserPrivate::closeNote(const Bracket &opener, int closePos)
{
    const int contentStart = opener.index;
    const int openerStart = nodes[opener.node].start;
    const QStringView raw(s + contentStart, closePos - contentStart);
    int entry = -1;
    bool token = false;
    if (!opener.bracketAfter && raw.size() <= 999) {
        entry = footnote(normalizeLabel(raw));
        token = isLabelToken(raw);
    }
    if (entry < 0 && !token && trimmedView(raw).isEmpty()) {
        popBracket();
        return false;
    }
    flush(closePos);
    if (entry >= 0 || token) {
        discardInside(opener);
        const int node = newNode(NodeType::FootnoteReference, openerStart, closePos + 1);
        n(node).openEnd = contentStart;
        n(node).closeStart = closePos;
        n(node).targetStart = contentStart;
        n(node).targetEnd = closePos;
        data(node).label = trimmedView(raw).toString();
        n(node).number = -(entry + 1); // until notes are numbered
        if (entry < 0) {
            n(node).resolved = false;
            diagnose(openerStart, closePos + 1, Severity::Warning, DiagnosticCode::MissingFootnote,
                     QStringLiteral("No footnote is defined for \"%1\"").arg(trimmedView(raw)));
        }
        insertAfter(node, opener.node);
        unlinkNode(opener.node);
    } else {
        const int node = newNode(NodeType::InlineNote, openerStart, closePos + 1);
        n(node).openEnd = contentStart;
        n(node).closeStart = closePos;
        wrapInside(opener, node);
        // Space just inside the brackets is not part of the note.
        const int head = nodes[node].first;
        if (head >= 0 && nodes[head].type == NodeType::Text) {
            while (nodes[head].start < nodes[head].end && isSpaceOrTab(s[nodes[head].start].unicode())) ++nodes[head].start;
            if (nodes[head].start == nodes[head].end) unlinkNode(head);
        }
        const int tail = nodes[node].last;
        if (tail >= 0 && nodes[tail].type == NodeType::Text) {
            while (nodes[tail].end > nodes[tail].start && isSpaceOrTab(s[nodes[tail].end - 1].unicode())) --nodes[tail].end;
            if (nodes[tail].start == nodes[tail].end) unlinkNode(tail);
        }
    }
    popBracket();
    done(closePos + 1);
    return true;
}

bool InlineParserPrivate::closeBracket()
{
    if (brackets.empty()) return false;
    const Bracket opener = brackets.back();
    if (!opener.active) {
        popBracket();
        return false;
    }
    const int closePos = m_pos;
    if (opener.note) return closeNote(opener, closePos);

    const Options &o = st.options;
    const int contentStart = opener.index;
    const int openerStart = nodes[opener.node].start;
    const int afterClose = closePos + 1;
    int end = afterClose;
    bool matched = false;
    QString dest, title, label;
    int targetStart = 0, targetEnd = 0;
    int heading = -1;
    int destinationFile = -1;

    if (afterClose < m_len && s[afterClose] == u'(')
        matched = inlineLinkTail(afterClose, &end, &dest, &title, &targetStart, &targetEnd);

    if (!matched) {
        const int labelLength = scanLinkLabel(s, m_len, afterClose);
        QStringView raw;
        bool haveRaw = false;
        if (labelLength > 2) {
            raw = QStringView(s + afterClose + 1, labelLength - 2);
            haveRaw = true;
            targetStart = afterClose + 1;
            targetEnd = afterClose + labelLength - 1;
        } else if (!opener.bracketAfter && closePos - contentStart <= 999) {
            // A collapsed or shortcut reference: the text is the label.
            raw = QStringView(s + contentStart, closePos - contentStart);
            haveRaw = true;
            targetStart = contentStart;
            targetEnd = closePos;
        }
        const bool explicitLabel = labelLength >= 2;

        // `[locator][#key]` is a citation.
        if (labelLength > 2 && o.citations && !opener.image && raw[0] == u'#') {
            const QString key = trimmedView(raw.sliced(1)).toString();
            if (!key.isEmpty()) {
                flush(closePos);
                discardInside(opener);
                const int node = newNode(NodeType::Citation, openerStart, afterClose + labelLength);
                n(node).openEnd = contentStart;
                n(node).closeStart = closePos;
                n(node).targetStart = targetStart + 1;
                n(node).targetEnd = targetEnd;
                data(node).label = key;
                data(node).text = trimmedView(QStringView(s + contentStart, closePos - contentStart)).toString();
                bool external = false;
                const int entry = citation(key, &external);
                n(node).number = -(entry + 1); // until notes are numbered
                if (entry < 0) {
                    n(node).resolved = false;
                    diagnose(afterClose, afterClose + labelLength, Severity::Warning, DiagnosticCode::MissingCitation,
                             QStringLiteral("No citation is defined for \"%1\"").arg(key));
                }
                insertAfter(node, opener.node);
                unlinkNode(opener.node);
                popBracket();
                done(afterClose + labelLength);
                return true;
            }
        }

        QString normalized;
        if (haveRaw) normalized = normalizeLabel(raw);
        if (!normalized.isEmpty()) {
            if (const LinkReference *ref = reference(normalized)) {
                dest = ref->dest;
                title = ref->title;
                destinationFile = ref->file;
                matched = true;
            } else if (explicitLabel && o.headingLabels && !opener.image) {
                // Writer heading references: `[text][Label]` and `[Title][]`.
                bool ambiguous = false;
                heading = headingAlias(normalized, &ambiguous);
                if (heading >= 0) {
                    matched = true;
                    if (ambiguous)
                        diagnose(openerStart, afterClose + labelLength, Severity::Warning,
                                 DiagnosticCode::AmbiguousHeadingReference,
                                 QStringLiteral("More than one heading matches \"%1\"; the first is used")
                                     .arg(trimmedView(raw)));
                }
            }
            if (matched) {
                label = trimmedView(raw).toString();
                end = afterClose + (explicitLabel ? labelLength : 0);
            }
        }

        if (!matched) {
            // Standard footnotes: `[^label]` is a reference when it is defined.
            if (o.footnotes && !st.writer() && !opener.image && !opener.bracketAfter
                && closePos - contentStart >= 2 && closePos - contentStart <= 999 && s[contentStart] == u'^') {
                const QStringView name(s + contentStart + 1, closePos - contentStart - 1);
                const int entry = footnote(normalizeLabel(name));
                if (entry >= 0) {
                    flush(closePos);
                    discardInside(opener);
                    const int node = newNode(NodeType::FootnoteReference, openerStart, closePos + 1);
                    n(node).openEnd = contentStart + 1;
                    n(node).closeStart = closePos;
                    n(node).targetStart = contentStart + 1;
                    n(node).targetEnd = closePos;
                    data(node).label = trimmedView(name).toString();
                    n(node).number = -(entry + 1); // until notes are numbered
                    insertAfter(node, opener.node);
                    unlinkNode(opener.node);
                    popBracket();
                    done(closePos + 1);
                    return true;
                }
            }
            if (explicitLabel && !normalized.isEmpty() && st.aliasesReady)
                diagnose(openerStart, afterClose + labelLength, Severity::Warning, DiagnosticCode::UnresolvedReference,
                         QStringLiteral("Nothing is defined for the reference \"%1\"").arg(trimmedView(raw)));
        }
    }

    if (!matched) {
        popBracket();
        return false;
    }

    flush(closePos);
    const int node = newNode(opener.image ? NodeType::Image : NodeType::Link, openerStart, end);
    n(node).openEnd = contentStart;
    n(node).closeStart = closePos;
    n(node).targetStart = targetStart;
    n(node).targetEnd = targetEnd;
    data(node).dest = dest;
    data(node).title = title;
    data(node).label = label;
    data(node).destinationFile = destinationFile;
    n(node).number = heading + 1; // until the slugs are known
    wrapInside(opener, node);
    popBracket();
    if (!opener.image) {
        // No links inside links: the openers still pending can no longer be links.
        for (size_t i = inactiveBelow; i < brackets.size(); ++i)
            if (!brackets[i].image && !brackets[i].note) brackets[i].active = false;
        inactiveBelow = brackets.size();
    }
    done(end);
    return true;
}

// --- Bare autolinks (GFM) ------------------------------------------------------------

void InlineParserPrivate::addAutolink(int start, int end, const QString &dest)
{
    flush(start);
    const int node = add(NodeType::Autolink, start, end);
    data(node).dest = dest;
    add(NodeType::Text, start, end, node);
    done(end);
}

// The length of the host name that starts at `start`, 0 when it is not one.
int InlineParserPrivate::domainLength(int start, bool allowShort) const
{
    int dots = 0, underscoresLast = 0, underscoresBeforeLast = 0;
    int i = start + 1;
    for (; i < m_len - 1; ++i) {
        const char32_t c = after(i);
        if (c == u'_') {
            ++underscoresLast;
        } else if (c == u'.') {
            underscoresBeforeLast = underscoresLast;
            underscoresLast = 0;
            ++dots;
        } else if (c != u'-' && (isUnicodeSpace(c) || isUnicodePunct(c))) {
            break;
        }
        if (c > 0xffff) ++i;
    }
    // An underscore is not allowed in the last two segments of a host name.
    if ((underscoresLast > 0 || underscoresBeforeLast > 0) && dots <= 10) return 0;
    if (allowShort) return i - start;
    return dots > 0 ? i - start : 0;
}

// Trims what ends a sentence, not a link, from the end of an autolink.
int InlineParserPrivate::autolinkDelimiter(int start, int length) const
{
    int opening = 0, closing = 0;
    for (int i = 0; i < length; ++i) {
        const char16_t c = s[start + i].unicode();
        if (c == u'<') {
            length = i;
            break;
        }
        if (c == u'(') ++opening;
        else if (c == u')') ++closing;
    }
    while (length > 0) {
        const char16_t c = s[start + length - 1].unicode();
        if (c == u')') {
            if (closing <= opening) return length;
            --closing;
            --length;
        } else if (c == u'?' || c == u'!' || c == u'.' || c == u',' || c == u':' || c == u'*' || c == u'_'
                   || c == u'~' || c == u'\'' || c == u'"') {
            --length;
        } else if (c == u';') {
            // `&name;` at the end is a character reference, not part of the link.
            int e = length - 2;
            while (e > 0 && isAsciiAlpha(s[start + e].unicode())) --e;
            if (e >= 0 && e < length - 2 && s[start + e] == u'&') length = e;
            else --length;
        } else {
            return length;
        }
    }
    return length;
}

// The text an autolink runs to: up to white space or `<`, and inside a note
// up to the bracket that closes it.
static int autolinkRun(const QChar *s, int from, int len, bool stopAtBracket)
{
    int e = from;
    while (e < len) {
        const QChar c = s[e];
        if (c.isSpace() || c == u'<' || (stopAtBracket && c == u']')) break;
        ++e;
    }
    return e;
}

bool InlineParserPrivate::wwwAutolink()
{
    const int start = m_pos;
    if (plainBrackets > 0) return false;
    if (start + 4 > m_len || s[start + 1] != u'w' || s[start + 2] != u'w' || s[start + 3] != u'.') return false;
    if (start > 0) {
        const char16_t b = s[start - 1].unicode();
        if (b != u'*' && b != u'_' && b != u'~' && b != u'(' && !QChar(b).isSpace()) return false;
    }
    const int limit = autolinkRun(s, start, m_len, !brackets.empty());
    if (domainLength(start, false) == 0) return false;
    const int length = autolinkDelimiter(start, limit - start);
    if (length == 0) return false;
    addAutolink(start, start + length, QStringLiteral("http://") + QStringView(s + start, length));
    return true;
}

bool InlineParserPrivate::urlAutolink()
{
    const int colon = m_pos;
    if (plainBrackets > 0) return false;
    if (colon + 3 >= m_len || s[colon + 1] != u'/' || s[colon + 2] != u'/') return false;
    int start = colon;
    while (start > m_textStart && isAsciiAlpha(s[start - 1].unicode())) --start;
    const QStringView scheme(s + start, colon - start);
    if (scheme.compare(u"http", Qt::CaseInsensitive) != 0 && scheme.compare(u"https", Qt::CaseInsensitive) != 0
        && scheme.compare(u"ftp", Qt::CaseInsensitive) != 0)
        return false;
    const char32_t first = after(colon + 3);
    if (isUnicodeSpace(first) || first == u'<') return false;
    const int limit = autolinkRun(s, colon + 3, m_len, !brackets.empty());
    if (domainLength(colon + 3, true) == 0) return false;
    const int length = autolinkDelimiter(start, limit - start);
    if (length <= colon + 3 - start) return false;
    addAutolink(start, start + length, QString(s + start, length));
    return true;
}

// --- Headings ---------------------------------------------------------------------

// Where a heading's text ends when it closes with a `[Label]`; -1 when it
// does not. A label that is a declared link reference is a link instead.
int InlineParserPrivate::headingLabel(int *labelOpen, int *labelClose)
{
    auto escaped = [&](int i) {
        int slashes = 0;
        while (i - 1 - slashes >= 0 && s[i - 1 - slashes] == u'\\') ++slashes;
        return slashes % 2 == 1;
    };
    int end = m_len;
    while (end > 0 && isSpaceOrTab(s[end - 1].unicode())) --end;
    if (end < 4 || s[end - 1] != u']' || escaped(end - 1)) return -1;
    const int lowest = qMax(0, end - 1001);
    int open = end - 2;
    for (; open >= lowest; --open) {
        const char16_t c = s[open].unicode();
        if (c == u'\n') return -1;
        if ((c == u'[' || c == u']') && !escaped(open)) break;
    }
    if (open < lowest || s[open] != u'[') return -1;
    if (open == 0 || !isSpaceOrTab(s[open - 1].unicode())) return -1;
    const QStringView inner = trimmedView(QStringView(s + open + 1, end - open - 2));
    if (inner.isEmpty()) return -1;
    const char16_t first = inner[0].unicode();
    if (first == u'^' || first == u'#' || first == u'%' || first == u'@') return -1;
    if (reference(normalizeLabel(inner))) return -1;
    int text = open;
    while (text > 0 && isSpaceOrTab(s[text - 1].unicode())) --text;
    if (text == 0) return -1;
    *labelOpen = open;
    *labelClose = end;
    return text;
}

// --- Building the tree ---------------------------------------------------------------

// Pairs the open and closing tags of supported elements among the children
// of one node. A pair becomes an HtmlElement around what lies between; a
// tag without its partner, or of an element the policy does not support,
// stays an HtmlInline, which is shown as text.
void InlineParserPrivate::pairHtml(int parent)
{
    struct Open {
        int node;
        QString name;
    };
    std::vector<Open> stack;
    auto unsupported = [&](int node, const QString &why) {
        diagnose(nodes[node].start, nodes[node].end, Severity::Info, DiagnosticCode::UnsupportedHtml, why);
    };
    for (int i = nodes[parent].first; i >= 0;) {
        const int next = nodes[i].next;
        if (nodes[i].type != NodeType::HtmlInline || !nodes[i].tag) {
            i = next;
            continue;
        }
        HtmlTag tag = parseHtmlTag(data(i).text);
        const HtmlTagInfo info = htmlTagInfo(tag.name);
        if (!info.supported || info.block) {
            unsupported(i, QStringLiteral("<%1> is not supported here and is shown as text").arg(tag.name));
            i = next;
            continue;
        }
        if (tag.droppedAttributes)
            unsupported(i, QStringLiteral("Attributes of <%1> that are not supported are left out").arg(tag.name));
        if (!tag.closing) {
            for (Attribute &attribute : tag.attributes) {
                const Range value = attribute.valueRange;
                if (value.isEmpty()) continue;
                const Range source = sourceRange(nodes[i].start + value.start, nodes[i].start + value.end);
                attribute.valueRange = {};
                if (source.length() == value.length() && source.end <= file->text.size()
                    && QStringView(file->text).sliced(source.start, source.length())
                           == QStringView(data(i).text).sliced(value.start, value.length()))
                    attribute.valueRange = source;
            }
            if (info.isVoid || tag.selfClosing) {
                INode &x = nodes[i];
                x.type = NodeType::HtmlElement;
                x.openEnd = x.end;
                data(i).label = tag.name;
                data(i).attributes = std::move(tag.attributes);
            } else {
                data(i).label = tag.name;
                data(i).attributes = std::move(tag.attributes);
                stack.push_back({i, tag.name});
            }
            i = next;
            continue;
        }
        // A closing tag: find its opener.
        int match = int(stack.size()) - 1;
        while (match >= 0 && stack[match].name != tag.name) --match;
        if (match < 0 || info.isVoid) {
            unsupported(i, QStringLiteral("</%1> closes nothing and is shown as text").arg(tag.name));
            i = next;
            continue;
        }
        for (int k = int(stack.size()) - 1; k > match; --k) {
            data(stack[k].node).label.clear();
            data(stack[k].node).attributes.clear();
            unsupported(stack[k].node, QStringLiteral("<%1> is never closed and is shown as text").arg(stack[k].name));
        }
        const int open = stack[match].node;
        stack.resize(match);
        INode &x = nodes[open];
        x.type = NodeType::HtmlElement;
        x.openEnd = x.end;
        x.closeStart = nodes[i].start;
        x.end = nodes[i].end;
        adopt(open, nodes[open].next, i);
        unlinkNode(i);
        i = next;
    }
    for (const Open &open : stack) {
        data(open.node).label.clear();
        data(open.node).attributes.clear();
        unsupported(open.node, QStringLiteral("<%1> is never closed and is shown as text").arg(open.name));
    }
}

Node InlineParserPrivate::textNode(int start, int end) const
{
    Node node;
    node.type = NodeType::Text;
    node.file = ctx.file;
    node.range = sourceRange(start, end);
    node.open = {node.range.start, node.range.start};
    node.close = {node.range.end, node.range.end};
    node.text = QString(s + start, end - start);
    return node;
}

// Text within one piece of the content, so that it is one run of source.
void InlineParserPrivate::emitPlain(int start, int end, std::vector<Node> &out)
{
    while (start < end) {
        const PieceMap &m = pieceAt(start);
        const int stop = map.size() == 1 ? end : qMin(end, m.content + m.len);
        if (stop <= start) {
            ++start;
            continue;
        }
        if (!file->hasNul) {
            out.push_back(textNode(start, stop));
            start = stop;
            continue;
        }
        // U+0000 is never passed on: a run of it is a node that stands for
        // as many replacement characters, so that text stays its source.
        int i = start;
        while (i < stop) {
            int e = i;
            const bool nul = s[i].unicode() == 0;
            while (e < stop && (s[e].unicode() == 0) == nul) ++e;
            Node node = textNode(i, e);
            if (nul) {
                node.type = NodeType::Entity;
                node.text = QString(e - i, QChar(0xfffd));
            }
            out.push_back(std::move(node));
            i = e;
        }
        start = stop;
    }
}

// Text, with the e-mail addresses in it made links (GFM).
void InlineParserPrivate::emitText(int start, int end, std::vector<Node> &out, bool inLink)
{
    if (!sawAt || inLink || !st.options.autolinks) {
        emitPlain(start, end, out);
        return;
    }
    auto localChar = [](char16_t c) { return isAsciiAlnum(c) || c == u'.' || c == u'+' || c == u'-' || c == u'_'; };
    int textStart = start;
    int search = start;
    for (;;) {
        int at = search;
        while (at < end && s[at] != u'@') ++at;
        if (at >= end) break;
        int local = at;
        while (local > search && localChar(s[local - 1].unicode())) --local;
        if (local == at) {
            search = at + 1;
            continue;
        }
        int i = at + 1;
        int dots = 0;
        bool another = false;
        for (; i < end; ++i) {
            const char16_t c = s[i].unicode();
            if (isAsciiAlnum(c)) continue;
            if (c == u'@') {
                another = true;
                break;
            }
            if (c == u'.' && i + 1 < end && isAsciiAlnum(s[i + 1].unicode())) {
                ++dots;
                continue;
            }
            if (c != u'-' && c != u'_') break;
        }
        if (another) {
            search = at + 1;
            continue;
        }
        int length = i - at;
        if (length < 2 || dots == 0 || (!isAsciiAlpha(s[i - 1].unicode()) && s[i - 1] != u'.')) {
            search = i;
            continue;
        }
        length = autolinkDelimiter(at, length);
        if (length == 0) {
            search = at + 1;
            continue;
        }
        const int linkEnd = at + length;
        emitPlain(textStart, local, out);
        Node link;
        link.type = NodeType::Autolink;
        link.file = ctx.file;
        link.range = sourceRange(local, linkEnd);
        link.open = {link.range.start, link.range.start};
        link.close = {link.range.end, link.range.end};
        link.dest = QStringLiteral("mailto:") + QStringView(s + local, linkEnd - local);
        link.children.push_back(textNode(local, linkEnd));
        out.push_back(std::move(link));
        textStart = search = linkEnd;
    }
    emitPlain(textStart, end, out);
}

// The finished node of a pool entry, without its children.
Node InlineParserPrivate::finish(int index)
{
    INode &x = nodes[index];
    Node node;
    node.type = x.type;
    node.checked = x.checked;
    node.display = x.display;
    node.resolved = x.resolved;
    node.number = x.number;
    if (x.marker) node.marker = QChar(x.marker);
    if (x.payload >= 0) {
        Payload &payload = payloads[x.payload];
        node.text = std::move(payload.text);
        node.dest = std::move(payload.dest);
        node.title = std::move(payload.title);
        node.label = std::move(payload.label);
        node.destinationFile = payload.destinationFile;
        node.attributes = std::move(payload.attributes);
    }
    node.file = ctx.file;
    if (file->hasNul) node.text.replace(QChar(0), QChar(0xfffd));
    node.range = sourceRange(x.start, x.end);
    node.open = x.openEnd > x.start ? sourceRange(x.start, x.openEnd) : Range{node.range.start, node.range.start};
    node.close = x.closeStart < x.end ? sourceRange(x.closeStart, x.end) : Range{node.range.end, node.range.end};
    if (x.targetEnd > x.targetStart) node.target = sourceRange(x.targetStart, x.targetEnd);
    if (node.type == NodeType::SoftBreak || node.type == NodeType::HardBreak) {
        // The break ends with the line terminator, which may be two characters.
        const QString &text = file->text;
        const int at = toSource(x.end - 1);
        node.range.end = (at + 1 < text.size() && text[at] == u'\r' && text[at + 1] == u'\n') ? at + 2 : at + 1;
        node.close = {node.range.end, node.range.end};
    }
    return node;
}

void InlineParserPrivate::build(int parent, std::vector<Node> &out, int depth, bool inLink)
{
    if (sawHtml) pairHtml(parent);
    for (int i = nodes[parent].first; i >= 0;) {
        if (nodes[i].type == NodeType::Text) {
            // Adjacent runs of text are one node.
            const int start = nodes[i].start;
            int end = nodes[i].end;
            int j = nodes[i].next;
            while (j >= 0 && nodes[j].type == NodeType::Text && nodes[j].start == end) {
                end = nodes[j].end;
                j = nodes[j].next;
            }
            if (end > start) emitText(start, end, out, inLink);
            i = j;
            continue;
        }
        const int next = nodes[i].next;
        if (nodes[i].first >= 0 && depth >= MaxInlineDepth) {
            flatten(i, out);
            i = next;
            continue;
        }
        const bool link = nodes[i].type == NodeType::Link;
        Node node = finish(i);
        if (nodes[i].first >= 0) build(i, node.children, depth + 1, inLink || link);
        out.push_back(std::move(node));
        i = next;
    }
}

// What is nested too deep is shown as it was written: markers as text,
// around the leaves in order. A loop, since the depth here is unbounded.
void InlineParserPrivate::flatten(int top, std::vector<Node> &out)
{
    auto marker = [&](int start, int end) {
        if (end > start) emitPlain(start, end, out);
    };
    marker(nodes[top].start, nodes[top].openEnd);
    int i = nodes[top].first;
    while (i >= 0) {
        if (nodes[i].first >= 0) {
            marker(nodes[i].start, nodes[i].openEnd);
            i = nodes[i].first;
            continue;
        }
        if (nodes[i].type == NodeType::Text) marker(nodes[i].start, nodes[i].end);
        else out.push_back(finish(i));
        while (i >= 0 && nodes[i].next < 0) {
            i = nodes[i].parent;
            marker(nodes[i].closeStart, nodes[i].end);
            if (i == top) return;
        }
        i = nodes[i].next;
    }
}

// --- Public face ------------------------------------------------------------------

InlineParser::InlineParser(ParseState &state)
    : d(std::make_unique<InlineParserPrivate>(state))
{
}

InlineParser::~InlineParser() = default;

void InlineParser::parse(const std::vector<Piece> &pieces, const InlineContext &context, std::vector<Node> &out)
{
    d->parse(pieces, context, out);
}

} // namespace md::detail

md::Document md::parseTableCell(const QString &text, const Options &options)
{
    Document document;
    document.options = options;
    document.files.emplace_back().text = text;
    const Source source;
    detail::ParseState state;
    state.source = &source;
    state.options = options;
    state.document = &document;
    state.files.emplace_back().text = text;
    state.runs.emplace_back();
    document.root.type = NodeType::TableCell;
    document.root.range = {0, int(text.size())};
    detail::InlineParser(state).parse(detail::tableCellPieces(text, 0, int(text.size()), 1), {}, document.root.children);
    return document;
}
