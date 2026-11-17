#pragma once

#include "frontmatter.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

// The Markdown engine: the one interpretation of a document that the editor's
// styling, the outline, the preview, the spellchecker, the exports and the
// command line all read.
//
//     md::Document doc = md::parse(text);                    // one file
//     md::Document doc = md::parse(text, options, source);   // with includes
//
// parse() is a pure function of its arguments. It uses no GUI types and no
// shared state, so it may run on any thread; a Document is plain data that
// can be handed to another thread once built (it moves, it does not copy).
//
// What a Document holds:
//   root         the semantic tree; every node knows the file it came from
//                and its [start, end) range of UTF-16 offsets in that file
//   files        the texts that were parsed: files[0] is the document, the
//                others are the files it includes, with their front matter
//   headings     the outline, which {{TOC}} and anchors are built from
//   footnotes    the endnotes in the order of their numbers
//   bibliography the cited works in the order of their numbers
//   diagnostics  what could not be resolved, per file and range
//
// The dialect is docs/markdown-dialect.md. Two profiles exist: Writer (the
// default, with the Writer extensions) and Standard (CommonMark 0.31.2 with
// GFM tables, strikethrough, task lists and autolinks).
namespace md {

// Bumped whenever the same source can yield a different tree.
inline constexpr int DialectVersion = 1;

// A half-open range of UTF-16 offsets into the text of one source file.
struct Range {
    int start = 0;
    int end = 0;

    int length() const { return end - start; }
    bool isEmpty() const { return end <= start; }
    bool contains(int position) const { return position >= start && position < end; }
    bool operator==(const Range &) const = default;
};

enum class Profile : quint8 { Writer, Standard };

// The profile and every extension switch. The defaults are the Writer
// profile; standard() turns every Writer and additional extension off.
struct Options {
    Profile profile = Profile::Writer;

    // GFM, on in both profiles.
    bool tables = true;
    bool strikethrough = true; // `~~text~~`; in Standard also `~text~`
    bool taskLists = true;
    bool autolinks = true;     // bare www., http://, https://, ftp:// and e-mail

    // Additional extensions, on in Writer and off in Standard.
    bool frontMatter = true;     // YAML between `---` lines at the top
    bool math = true;            // `$inline$`, `$$display$$`
    bool mermaid = true;         // ```mermaid fences are diagrams
    bool definitionLists = true; // term lines followed by `: definition`
    bool footnotes = true;       // `[^label]` with `[^label]: text`; in Writer also `[^an inline note]`
    bool citations = true;       // `[locator][#key]`, `[@key, locator]`, `[#key]: entry`
    bool highlight = true;       // `==text==`
    bool superSubscript = true;  // `x^2`, `y^(a+b)^`, `x~z`, `x~y,z~`
    bool lineComments = true;    // a line that starts at column zero with `//`
    bool pageBreaks = true;      // `+++` alone on a line after a blank line
    bool toc = true;             // `{{TOC}}` alone in a paragraph
    bool variables = true;       // `[%key]` from the front matter
    bool includes = true;        // a path to a .md, .markdown or .txt file alone in a paragraph
    bool mergedCells = true;     // `| joined || third |` spans two columns
    bool headingLabels = true;   // `# Title [Label]`, `[text][Label]`, `[Title][]`

    static Options writer();
    static Options standard();
    static Options forProfile(Profile profile);

    // For reports and command-line switches: the name of the profile, the
    // names of every extension, the ones that are on, and access by name.
    QString profileName() const;
    static QStringList extensionNames();
    QStringList enabledExtensions() const;
    bool extension(QStringView name) const;
    bool setExtension(QStringView name, bool on); // false for an unknown name

    bool operator==(const Options &) const = default;
};

enum class NodeType : quint8 {
    // Blocks
    Document,
    FrontMatter,           // text: the YAML; open/close: the `---` lines
    Paragraph,
    Heading,               // level, setext, label, dest (the slug); open: the `#` run; close: the closing `#` run or the underline
    ThematicBreak,
    BlockQuote,            // markers: every `>`
    List,                  // ordered, tight, number (first number as rendered), sourceNumber (as written), marker
    ListItem,              // number (as rendered), task, checked; open: the bullet or number
    CodeBlock,             // text, label (the info string), fenced; open/close: the fences; target: the info string
    MermaidBlock,          // text: the diagram source; otherwise as CodeBlock
    MathBlock,             // text: the TeX; open/close: the `$$`
    HtmlBlock,             // text: the raw HTML; children: the fragment read under the HTML policy (see mdhtml.h)
    LinkDefinition,        // label, dest, title; target: the destination
    Table,                 // columns; open: the delimiter row; children: TableRow (the first is the header)
    TableRow,              // header; markers: the cell boundaries
    TableCell,             // header, align, column, span
    DefinitionList,        // children: DefinitionTerm and DefinitionDescription in source order
    DefinitionTerm,
    DefinitionDescription, // tight; open: the `:`
    FootnoteDefinition,    // label, number (0 when never referenced); open: `[^label]:`
    CitationDefinition,    // label (the key without `#`), number (0 when never cited); open: `[#key]:`
    PageBreak,
    TableOfContents,       // children: a generated list of links to the headings
    Include,               // dest (the path as written), title (the caption), resolved, includedFile; children: the included blocks
    CommentLine,           // a `//` line; text: its content. A block, or a child of the paragraph, heading or table it is written in

    // Inlines
    Text,                  // text == the source slice
    Escape,                // `\*`; text: the escaped character; open: the backslash
    Entity,                // `&amp;`; text: the character(s) it stands for. Also U+0000, which stands for U+FFFD
    SoftBreak,
    HardBreak,
    Emphasis,
    Strong,
    Strikethrough,
    Highlight,
    Superscript,           // close is empty for the short form `x^2`
    Subscript,
    Code,                  // text: the code
    Link,                  // dest, title, label (of a reference link), resolved; target: destination or label
    Image,                 // as Link; children are the alternative text
    Autolink,              // dest; children: the text shown. open/close: `<` and `>`, empty for a bare one
    HtmlInline,            // text: raw markup that is shown literally unless rendered in passthrough
    HtmlElement,           // label: the tag name, attributes; open/close: the tags; children between them
    HtmlComment,           // text: the comment, omitted from rendering
    MathInline,            // text: the TeX; display for `$$…$$` inside a paragraph
    FootnoteReference,     // label, number, resolved
    InlineNote,            // number; children: the note
    Citation,              // label (the key), text (the locator and what follows it), title (the prefix, as
                           // "see" in `[see @key]`), marker (`-` for `[-@key]`, which leaves the author out;
                           // `@` for `@key` in the text, which names the author there), number, resolved;
                           // children: the Citations of a group, each from its `@` or `-@`
    Variable,              // label (the key), text (the value), resolved
    TaskMarker,            // checked; `[ ]` or `[x]` at the start of a task item
    HeadingLabel,          // label; the `[Label]` that ends a heading, not printed
};

enum class Align : quint8 { None, Left, Center, Right };

struct Attribute {
    QString name;
    QString value;
    bool operator==(const Attribute &) const = default;
};

// One node of the tree.
//
// `range` covers the whole construct in its source file, markers included;
// for a block it runs from its first character to the end of its last line,
// without the line terminator. `open` and `close` are the ranges of the
// punctuation that starts and ends the construct (the `**` of strong text,
// the `[` and `](url)` of a link, the fences of a code block); they are empty
// where there is none. `target` is the range of the construct's argument: a
// link's destination or reference label, a code block's info string.
//
// A Text node's `text` is exactly the source it covers. Text that differs from
// its source is never a Text node: it is an Escape, an Entity or a Variable
// (or it is generated, see below).
//
// Nodes with `generated` set were made by the engine and have no source of
// their own: the list inside a table of contents, and the nodes of a raw
// HTML block whose lines are not one unbroken run of source. Their ranges
// are that of the construct that produced them.
struct Node {
    NodeType type = NodeType::Document;
    Align align = Align::None;
    quint8 level = 0;          // Heading: 1 to 6
    QChar marker;              // List: the bullet, or `.` / `)`; CodeBlock: the fence character

    bool tight = false;        // List, DefinitionDescription
    bool ordered = false;      // List
    bool task = false;         // ListItem
    bool checked = false;      // ListItem, TaskMarker
    bool header = false;       // TableRow, TableCell
    bool fenced = false;       // CodeBlock
    bool setext = false;       // Heading
    bool display = false;      // MathInline
    bool resolved = true;      // false for a reference, note, citation, variable or include that could not be resolved
    bool generated = false;

    int file = 0;              // index into Document::files
    Range range;
    Range open;
    Range close;
    Range target;

    // List: the number its first item is rendered with. ListItem: its number
    // as rendered. FootnoteReference, InlineNote, FootnoteDefinition,
    // Citation, CitationDefinition: the number shown, 0 when there is none.
    int number = 0;
    int sourceNumber = 0;      // List: the first number as written
    int columns = 0;           // Table: the number of columns
    int column = 0;            // TableCell: the first column it covers
    int span = 1;              // TableCell: the columns it covers
    int includedFile = -1;     // Include: index into Document::files, -1 when unresolved

    QString text;
    QString dest;
    QString title;
    QString label;

    std::vector<Attribute> attributes; // HtmlElement: the attributes the HTML policy allows
    std::vector<Range> markers;        // BlockQuote, TableRow
    std::vector<Node> children;

    QString attribute(QLatin1StringView name) const
    {
        for (const Attribute &a : attributes)
            if (a.name == name) return a.value;
        return {};
    }

    bool isBlock() const { return type < NodeType::Text; }
    bool isInline() const { return type >= NodeType::Text; }
    // Text, Escape and Entity: the three nodes whose `text` is prose.
    bool isTextual() const { return type == NodeType::Text || type == NodeType::Escape || type == NodeType::Entity; }
};

const char *typeName(NodeType type);

enum class Severity : quint8 { Info, Warning, Error };

enum class DiagnosticCode : quint8 {
    UnresolvedReference,      // `[text][label]` with no such definition or heading
    UnresolvedAnchor,         // `[text](#slug)` with no such heading
    AmbiguousHeadingReference,
    MissingFootnote,          // `[^label]` with no definition
    DuplicateFootnote,
    UnusedFootnote,
    MissingCitation,
    DuplicateCitation,
    UnresolvedVariable,
    MalformedTableRow,
    UnclosedMath,
    FrontMatter,
    UnsupportedHtml,
    IncludeNotFound,
    IncludeUnreadable,
    IncludeCycle,
    IncludeDepthLimit,
    IncludeSizeLimit,
    IncludeCountLimit,
    IncludeNoLibrary,         // a `/rooted` include without a library root
    IncludeNoBaseFolder,      // a relative include from a document with no folder
    IncludeOutside,           // an include that leads out of the library, or of the document's folder
};

struct Diagnostic {
    int file = 0;
    Range range;
    Severity severity = Severity::Warning;
    DiagnosticCode code = DiagnosticCode::UnresolvedReference;
    QString message;
};

// Stable names for reports: "unresolved-reference", "warning".
const char *codeName(DiagnosticCode code);
const char *severityName(Severity severity);

// One text that was parsed. `text` is shared with the caller's string, not
// copied; every range of a node of this file indexes into it.
struct SourceFile {
    QString path;      // canonical path; empty for a document that has none
    QString directory; // the folder its relative links, images and includes resolve from; may be empty
    QString text;
    FrontMatter frontMatter;
    int includedFrom = -1; // the file that first included it; -1 for the root
};

// A heading of the outline, in document order, included files included.
struct Heading {
    int level = 1;
    QString text;   // without formatting and without the label
    QString slug;   // unique in the document; the target of `#slug` links
    QString label;  // the explicit `[Label]`, empty when there is none
    int file = 0;
    Range range;
    const Node *node = nullptr;
};

// An endnote. Reference notes that share a definition share one Footnote.
struct Footnote {
    int number = 0;
    QString label;             // empty for an inline note
    int file = 0;
    Range range;               // of the definition, or of the inline note
    // The FootnoteDefinition or the InlineNote whose children are the note;
    // with Source::citationNotes, the Citation that is the note.
    const Node *content = nullptr;
    int references = 0;        // how many times the text refers to it
};

// A cited work.
struct BibliographyEntry {
    int number = 0;
    QString key;
    int file = 0;
    const Node *definition = nullptr; // the CitationDefinition; null when the key is external
    bool external = false;            // the key is one of Source::externalCitationKeys
};

// The source a document comes from, for includes and local paths.
class IncludeResolver;
struct Source {
    QString path;         // canonical path of the document; empty when it has none
    QString directory;    // base folder for relative paths; defaults to the folder of `path`
    QString libraryRoot;  // the folder `/rooted` includes are found in; empty when there is none
    // Reads included files. Without one, include lines are recognised but
    // left unexpanded and undiagnosed (the fast path for live styling).
    IncludeResolver *resolver = nullptr;
    int maxIncludeDepth = 32;
    qint64 maxExpandedBytes = 100ll * 1024 * 1024; // UTF-8 bytes of the document and everything it includes
    int maxIncludeCount = 10000;                    // include lines expanded, a guard against fan-out
    // Citation keys an external bibliography provides; a citation of one of
    // these is resolved without a definition in the text.
    QSet<QString> externalCitationKeys;
    // The citation style puts citations in notes: each citation of the
    // running text is a footnote of its own, numbered with the other notes
    // in the order a reader meets them, its content the Citation itself.
    // A citation inside a note stays where it is.
    bool citationNotes = false;
};

struct PlainTextOptions {
    bool code = true;        // code blocks and code spans
    bool notes = true;       // footnote definitions and inline notes
    // Only what is written within this range of the document's own text
    // (file 0), as for a selection; the whole document when unset. Of a
    // construct the range cuts through, the part of its source inside it.
    std::optional<Range> range;
};

class Document {
public:
    Document() = default;
    Document(Document &&) = default;
    Document &operator=(Document &&) = default;
    // The lists below point into the tree, which a copy would not follow.
    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;

    Options options;
    std::vector<SourceFile> files;
    QStringList resourceFolders; // canonical library and root document folders
    Node root;
    std::vector<Heading> headings;
    std::vector<Footnote> footnotes;
    std::vector<BibliographyEntry> bibliography;
    std::vector<Diagnostic> diagnostics;
    // Whether an include could not be read into the document (it is missing,
    // closes a cycle, or crosses a limit): an export must fail rather than
    // write the document as if it were whole.
    bool incomplete = false;

    const FrontMatter &frontMatter() const;

    // The source a range or a node covers.
    QStringView source(int file, Range range) const;
    QStringView source(const Node &node) const { return source(node.file, node.range); }

    // The nodes that contain a position of a file, outermost first; the last
    // is the innermost. A node contains the positions of its range and, when
    // nothing follows it there, the position at its end. Generated nodes are
    // never returned. Empty when the file is not part of the document.
    std::vector<const Node *> pathAt(int position, int file = 0) const;
    const Node *nodeAt(int position, int file = 0) const;

    // The ranges of a file that are prose, in order: the text of paragraphs,
    // headings, lists, tables and notes, without code, math, URLs, comments,
    // front matter, raw HTML and Markdown punctuation. What lies between two
    // ranges is never prose, so a word may continue across a gap of markup
    // (`it**alic**s`). This is what a spellchecker reads.
    std::vector<Range> proseRanges(int file = 0) const;
    void forEachProseRange(int file, const std::function<void(Range)> &visit) const;

    // The text a reader sees, one block per line, for counting words.
    QString plainText(const PlainTextOptions &options = {}) const;
};

// The text of a node and everything in it, without markup: what an image's
// alternative text and a heading's outline entry are.
QString plainText(const Node &node);

// Parses one text. Includes are recognised and left unexpanded.
Document parse(const QString &text, const Options &options = Options());
// Parses a document and assembles the files it includes into its tree.
Document parse(const QString &text, const Options &options, const Source &source);
// How many texts parse() has read since the program started, on any
// thread: for measuring, and for tests that count parses.
quint64 parseCount();

// The slug of a heading's text before duplicates are numbered: lower case,
// punctuation and symbols removed, each run of white space one hyphen.
QString slugify(QStringView headingText);

// A link or image destination, told apart and made usable.
struct Destination {
    enum class Kind { Empty, Anchor, Remote, Mail, OtherScheme, LocalFile };
    Kind kind = Kind::Empty;
    QString scheme;   // lower case, for Remote, Mail and OtherScheme
    QString path;     // LocalFile: the absolute path when a base folder is known, else the relative path, percent-decoded
    QString fragment; // Anchor: the slug; LocalFile: what follows `#`
    bool absolute = false; // LocalFile: `path` is absolute
};
// `baseDirectory` is the folder of the file the destination was written in
// (SourceFile::directory), so that the paths of an included file resolve
// from that file's own folder.
Destination classifyDestination(const QString &destination, const QString &baseDirectory);
Destination classifyDestination(const Document &document, const Node &linkOrImage);

// A file path as a link or image destination can carry it: spaces,
// parentheses, angle brackets, `#`, `%`, `&` (which could start an entity),
// `:` (which could end a scheme), `\`, `"` and control characters are
// percent-encoded, so that `[text](destination)` reads back as the path.
QString encodeDestination(QStringView path);

} // namespace md
