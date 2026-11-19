#pragma once

// Internals shared by the block parser, the inline parser and the pass that
// assembles and resolves the tree. Nothing outside the md*.cpp files includes
// this.

#include "mdtree.h"

#include <QHash>

#include <deque>

namespace md::detail {

// Containers nested deeper than this are not opened: the line is read as the
// text of the innermost one. Inline constructs nested deeper than their limit
// stay literal text. Both keep every walk of the tree, here and in the code
// that reads it, within a small, known stack depth whatever the input.
inline constexpr int MaxBlockDepth = 1000;
inline constexpr int MaxInlineDepth = 64;
inline constexpr int MaxHtmlDepth = 64;

// --- Characters ---------------------------------------------------------

inline bool isSpaceOrTab(char16_t c) { return c == u' ' || c == u'\t'; }
inline bool isAsciiDigit(char16_t c) { return c >= u'0' && c <= u'9'; }
inline bool isAsciiAlpha(char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); }
inline bool isAsciiAlnum(char16_t c) { return isAsciiDigit(c) || isAsciiAlpha(c); }
inline bool isAsciiPunct(char16_t c)
{
    return (c >= 0x21 && c <= 0x2f) || (c >= 0x3a && c <= 0x40) || (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e);
}
// CommonMark's "Unicode whitespace character" and "Unicode punctuation character".
inline bool isUnicodeSpace(char32_t c)
{
    if (c < 128) return c == u' ' || c == u'\t' || c == u'\n' || c == u'\f' || c == u'\r';
    return QChar::category(c) == QChar::Separator_Space;
}
inline bool isUnicodePunct(char32_t c)
{
    if (c < 128) return isAsciiPunct(char16_t(c));
    switch (QChar::category(c)) {
    case QChar::Punctuation_Connector:
    case QChar::Punctuation_Dash:
    case QChar::Punctuation_Open:
    case QChar::Punctuation_Close:
    case QChar::Punctuation_InitialQuote:
    case QChar::Punctuation_FinalQuote:
    case QChar::Punctuation_Other:
    case QChar::Symbol_Math:
    case QChar::Symbol_Currency:
    case QChar::Symbol_Modifier:
    case QChar::Symbol_Other:
        return true;
    default:
        return false;
    }
}

// The characters a named reference such as `amp` stands for; empty when there is none.
QString namedEntity(QStringView name);

// --- Scanners over a buffer -----------------------------------------------
// Each takes the buffer, its length and a position, and returns the length
// of what it matched there, 0 for no match.

// `&name;`, `&#123;` or `&#x1F;`; `value` receives what it stands for.
int scanEntity(const QChar *s, int len, int pos, QString *value);
// `[label]`: at most 999 characters between the brackets, none of them an unescaped bracket.
int scanLinkLabel(const QChar *s, int len, int pos);
// A link destination. `start` and `end` receive the destination without the
// `<` `>` that may enclose it. Returns -1 for no match, since an empty
// destination is a match of length 0.
int scanLinkDestination(const QChar *s, int len, int pos, int *start, int *end);
// A link title in quotes or parentheses, delimiters included.
int scanLinkTitle(const QChar *s, int len, int pos);
// An HTML open tag, closing tag, comment, processing instruction,
// declaration or CDATA section.
enum class HtmlKind { None, OpenTag, CloseTag, Comment, Instruction, Declaration, CData };
int scanHtml(const QChar *s, int len, int pos, HtmlKind *kind);
// Spaces and tabs, and at most one line ending among them.
int skipSpacesAndNewline(const QChar *s, int len, int pos);

// Backslash escapes and character references replaced by what they stand for.
QString unescape(QStringView text);
// A label as references are matched by: trimmed, inner white space
// collapsed, case folded.
QString normalizeLabel(QStringView label);

// A link reference definition at the start of `s`: `[label]: destination "title"`.
struct ReferenceMatch {
    int length = 0; // 0 when there is none; otherwise it ends at a line end
    Range label;    // between the brackets
    Range dest;
    Range title;    // between the quotes; empty when there is no title
    bool hasTitle = false;
};
ReferenceMatch scanReference(const QChar *s, int len);

// --- Raw HTML ---------------------------------------------------------------

// What the HTML policy makes of a tag name (lower case).
struct HtmlTagInfo {
    bool supported = false;
    bool isVoid = false;   // has no content and no closing tag
    bool block = false;    // a block-level element
};
HtmlTagInfo htmlTagInfo(QStringView lowerName);
// Splits an open or closing tag into its name (lower case), whether it
// closes, whether it ends in `/>`, and the attributes the policy allows for it.
struct HtmlTag {
    QString name;
    bool closing = false;
    bool selfClosing = false;
    std::vector<Attribute> attributes;
    bool droppedAttributes = false; // some attribute was not allowed
};
HtmlTag parseHtmlTag(QStringView raw);
// Whether a URL may be the target of a link or the source of an image:
// anything but script and non-image data URLs.
bool isSafeUrl(QStringView url, bool image);

struct ParseState;
// Reads the raw text of an HTML block (which starts at `offset` in `file`
// when its lines are contiguous there, else -1) into nodes under the policy.
// The first `skip` characters are the block's indentation and make no node.
void parseHtmlFragment(ParseState &state, const QString &raw, int file, Range blockRange, int offset, int skip,
                       std::vector<Node> &out);

// --- Blocks ---------------------------------------------------------------

// A run of source that is part of a leaf block's inline content.
struct Piece {
    int src = 0;          // offset in the file
    int len = 0;
    int line = 0;         // line number, from 1
    bool newline = true;  // a line ending follows it in the content
    bool comment = false; // a `//` line inside a paragraph
};

struct Block {
    NodeType type = NodeType::Document;
    Block *parent = nullptr;
    Block *firstChild = nullptr;
    Block *lastChild = nullptr;
    Block *prev = nullptr;
    Block *next = nullptr;
    bool open = true;
    int file = 0;
    int run = 0;
    int depth = 0;
    int start = 0;
    int end = 0;
    int startLine = 0;
    int endLine = 0;
    Range openMarker;
    Range closeMarker;
    Range target;
    std::vector<Piece> pieces;   // inline content of a leaf
    std::vector<Range> markers;
    std::vector<Align> aligns;   // Table
    std::vector<Node> inlines;   // parsed inline content, when parsed ahead of the tree
    bool inlinesReady = false;
    QString literal;             // CodeBlock, HtmlBlock, MathBlock, CommentLine, FrontMatter
    QString label;
    QString dest;
    QString title;

    quint8 level = 0;            // Heading; HtmlBlock: the start condition
    bool setext = false;
    bool fenced = false;
    bool closed = false;         // CodeBlock (fenced), MathBlock: the closing marker was seen
    bool tight = true;
    bool ordered = false;
    bool header = false;
    bool resolved = true;
    QChar listChar;              // bullet, delimiter or fence character
    int listStart = 1;
    int markerOffset = 0;
    int padding = 0;
    int fenceLength = 0;
    int fenceOffset = 0;
    int lastTextLine = 0;        // indented CodeBlock: its last line that is not blank
    int lastTextEnd = 0;
    int literalTextLength = 0;   // indented CodeBlock: length of `literal` up to that line
    int columns = 0;
    int column = 0;
    int span = 1;
    Align align = Align::None;
    int heading = -1;            // Heading: index into ParseState::headings
    int definition = -1;         // FootnoteDefinition, CitationDefinition: index of its entry
    int includedFile = -1;
};

struct LinkReference {
    QString dest;
    QString title;
    int file = 0;
};

struct NoteEntry {
    int file = 0;
    QString label;
    Block *block = nullptr;
    const Node *node = nullptr;
    Range range;
    int number = 0;
    int references = 0;
    bool external = false; // citations only
};

struct Alias {
    int heading = -1;
    bool ambiguous = false;
};

struct FileState {
    QString text;
    bool hasNul = false;
    QHash<QString, LinkReference> references;
    QHash<QString, int> footnotes; // normalized label -> ParseState::footnotes
    QHash<QString, int> citations; // normalized key -> ParseState::citations
};

// One reading of a file: the document itself, or one include of a file.
struct Run {
    int file = 0;
    QHash<QString, Alias> labels; // explicit heading labels
    QHash<QString, Alias> titles; // heading texts
    QSet<QString> footnoteLabels; // defined in this reading, to tell duplicates
    QSet<QString> citationKeys;
};

struct ParseState {
    Options options;
    const Source *source = nullptr;
    Document *document = nullptr;
    std::deque<Block> blocks;
    std::deque<FileState> files;
    std::deque<Run> runs;
    std::vector<NoteEntry> footnotes;
    std::vector<NoteEntry> citations;
    QHash<QString, int> externalCitations;
    std::vector<Block *> headings;
    QHash<QString, Alias> labels; // of the assembled document
    QHash<QString, Alias> titles;
    bool aliasesReady = false;

    bool writer() const { return options.profile == Profile::Writer; }
    Block *newBlock(NodeType type, int file, int run);
    void diagnose(int file, Range range, Severity severity, DiagnosticCode code, const QString &message);
};

void appendChild(Block *parent, Block *child);
void insertBefore(Block *sibling, Block *block);
void unlink(Block *block);

// Reads the blocks of a file into `root`. `run` is the reading of the file
// this is: the document itself, or one include of it.
void parseBlocks(ParseState &state, Block *root, int file, int run);

// --- Inlines --------------------------------------------------------------

struct InlineContext {
    int file = 0;
    int run = 0;
    bool heading = false; // a trailing `[Label]` is the heading's label
    bool task = false;    // a leading `[ ]` or `[x]` is a task marker
    bool quiet = false;   // report nothing: the content is parsed again later
};

class InlineParserPrivate;
class InlineParser {
public:
    explicit InlineParser(ParseState &state);
    ~InlineParser();
    void parse(const std::vector<Piece> &pieces, const InlineContext &context, std::vector<Node> &out);

private:
    std::unique_ptr<InlineParserPrivate> d;
};

} // namespace md::detail
