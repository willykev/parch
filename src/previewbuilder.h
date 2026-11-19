#pragma once

#include "mdtree.h"
#include "previewstyle.h"

#include <QImage>
#include <QList>
#include <QPointer>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QHash>
#include <functional>
#include <vector>

class CslText;
class QTextTable;
namespace DocumentCitations {
struct Formatted;
}

// Builds the reading form of a parsed document: a QTextDocument made with a
// cursor and formats from the Markdown engine's tree, in the typography and
// colours of a PreviewStyle. The preview shows it; PDF export and printing
// build the same document in a paper style. Nothing here parses Markdown,
// and nothing here knows what a theme is.
//
// A document is built in parts: each top-level block of the tree is one,
// and after them a rule, each note, another rule and each bibliography
// entry. A part is built at a cursor and returns a Section, which says how
// much text it made and what that text is: where it came from in the
// source, the names links can go to, and what is to be drawn or placed
// over it. A part has a key made of everything that decides how it reads,
// so that a part whose key did not change need not be built again.
//
// Citations are the document's own numbered ones, `[1, p. 23]` with the
// bibliography the text defines, until a citation style formats them
// (DocumentReading::read gives the tree and its DocumentCitations::Formatted
// together): then each citation reads as the style words it, in a note
// style as a note of its own, and the bibliography is the style's. The
// parts, their keys and what is built all take the same Formatted.
//
// On screen (BuildOptions::islands) the blocks that may be wider than the
// column, which are tables, code blocks and display math, are built as
// small documents of their own, "islands": the main document holds only an
// empty block of their height, and a view shows each island over its block
// with its own sideways scrolling. On paper they are built in place.
namespace preview {

// Block format properties.
enum Property {
    // An empty block that holds the place of an island: its number in the
    // section, from 1.
    IslandProperty = QTextFormat::UserProperty + 0x510,
    // A character format property: a zero-width space the builder put in a
    // word too long to break, so that it can break there. Text copied from
    // the preview leaves it out.
    BreakProperty = QTextFormat::UserProperty + 0x511,
};

// A word longer than this many characters is given a place to break after
// every so many of them: Qt finds where to break a word that has none in
// time that grows with the square of its length.
inline constexpr qsizetype LongWord = 512;
// A table with more columns or cells than these is not laid out as a table,
// which would take minutes, but shown as it is written, in a code island.
inline constexpr int MaxTableColumns = 128;
inline constexpr qsizetype MaxTableCells = 20000;

// Where a piece of built text came from. `position` counts from the start
// of the section or island the anchor belongs to, and `source` from the
// start of the section's source. For `length` characters the two run side
// by side; an anchor without length marks one place.
struct Anchor {
    int position = 0;
    int source = 0;
    int length = 0;
    // The anchor is where a block begins.
    bool block = false;
    // Text an include brought in: the file it came from, as an index into
    // the document's files, with `source` counted from that file's start.
    int file = 0;
};

// A place links go to: a heading's slug, "~fn-3" for a note, "~fnref-3" for a
// reference to it (each reference has one), "~cite-2" or "~ref-key" for a
// bibliography entry. Heading slugs cannot contain a tilde.
struct NamedAnchor {
    QString name;
    int position = 0;
    // The island it is in; the position is then the island's block.
    int island = -1;
};

// A formula, a diagram or an image as far as it has come.
struct Picture {
    enum State { Pending, Ready, Failed };
    State state = Pending;
    // What names it among the resources and, as an image, in a document.
    QString key;
    QImage image;
    // On the page, in logical pixels.
    QSizeF size;
    // A formula's baseline, from its top, in logical pixels.
    qreal baseline = 0;
    // What it shows, for assistive technology.
    QString description;
    // Why it could not be drawn.
    QString diagnostic;
};

// Where the builder gets what it does not make itself. Each call answers
// at once, with a picture that may still be pending; the builder then
// writes the source in its place, and whoever owns the resources builds the
// section again when the picture arrives.
class Resources {
public:
    virtual ~Resources() = default;
    virtual Picture math(const QString &tex, bool display, qreal fontSize) = 0;
    virtual Picture diagram(const QString &source) = 0;
    virtual Picture image(const md::Destination &destination, const QString &written, const QStringList &folders) = 0;
};

// Something drawn or placed over the text, where the layout puts its blocks.
struct Mark {
    enum Kind {
        QuoteBar,   // beside the blocks first to last
        Rule,       // a thematic break, across the column
        NotesRule,  // the short rule above notes and above the bibliography
        PageBreak,
        Heading,    // nothing drawn: told to assistive technology with its level
        Image,      // nothing drawn: told to assistive technology with its description
    };
    Kind kind = Rule;
    // The positions of its first and last block, from the start of the section.
    int first = 0;
    int last = 0;
    // From the left edge of the column, in pixels.
    qreal x = 0;
    int level = 0;
    QString text;
};

struct Island {
    enum Kind { Code, Table, Math };
    Kind kind = Code;
    // The island's text. It belongs to BuildOptions::islandParent until a
    // view takes it.
    QPointer<QTextDocument> document;
    // The position of the block that holds its place, from the start of the section.
    int placeholder = 0;
    // From the left edge of the column, in pixels.
    qreal indent = 0;
    // Laid out to the width it has, as a table is; otherwise it is as wide
    // as its longest line.
    bool wraps = false;
    // Shown in the middle of the column when it is narrower.
    bool centered = false;
    // Drawn on the tint code has.
    bool tinted = false;
    // The source it covers, from the start of the section's source.
    int sourceStart = 0;
    int sourceEnd = 0;
    QList<Anchor> anchors;
    // Where the text included files brought in came from.
    QList<Anchor> included;
    // What it is, for assistive technology.
    QString description;
    // Table: how many rows are header rows.
    int headerRows = 0;
};

struct Part {
    enum Kind { Block, NotesRule, Note, BibliographyRule, Entry };
    Kind kind = Block;
    // Block: the top-level node.
    const md::Node *node = nullptr;
    // Note, Entry: its index among the document's notes or cited works; an
    // entry's among the style's bibliography when a style formats it.
    int index = 0;
    quint64 key = 0;
};

// What a document's preview marks as leading nowhere, in the danger ink
// with the engine's diagnostic as its tooltip: an include that could not
// be read in (for a cycle, the chain of files), and a variable, a reference,
// a note or an anchor that nothing defines. An unresolved reference reads
// as the text it was written as, so the text it covers is marked. Plain
// data made once for a document, for any thread.
class Unresolved {
public:
    explicit Unresolved(const md::Document &document);
    // What is said of a node: its own diagnostic, or for text, that of the
    // unresolved references it is part of; empty when there is none.
    QString of(const md::Node &node) const;
    // The unresolved references a piece of text (a Text, Escape or Entity
    // node) runs into, in order: a piece of text may run on before and
    // after one.
    std::vector<std::pair<md::Range, QString>> references(const md::Node &node) const;
    // Each of them, in file and source order.
    QStringList messages() const { return m_messages; }

private:
    QHash<quint64, QString> m_nodes;
    // By file, in source order: the ranges of unresolved references.
    std::vector<std::vector<std::pair<md::Range, QString>>> m_references;
    QStringList m_messages;
};

// The parts of a document, in the order they are read; `citations` when a
// citation style formatted the document, whose words are then part of each
// key and whose bibliography is the one listed. A pure function of the
// two, for any thread.
std::vector<Part> parts(const md::Document &document, const DocumentCitations::Formatted *citations = nullptr);
// The range of the document's own text a part was made from; -1 and -1
// when it has none.
void sourceOf(const md::Document &document, const Part &part, int *start, int *end,
              const DocumentCitations::Formatted *citations = nullptr);
// Reads the code of every code block of a document, so that the colouring
// is at hand when the document is built; for a worker thread.
void warm(const md::Document &document);
// Code as a code block shows it: without its last line break, and without
// what would break a line of its own accord.
QString shownCode(const QString &code);

struct Section {
    Part::Kind kind = Part::Block;
    quint64 key = 0;
    // The range of the document's own text it was made from; empty at the
    // place it stands for when it has none of its own.
    int sourceStart = 0;
    int sourceEnd = 0;
    // How many characters it has in the document, block separators included.
    int length = 0;
    QList<Anchor> anchors;
    // Where the text included files brought in came from.
    QList<Anchor> included;
    QList<NamedAnchor> names;
    QList<Mark> marks;
    std::vector<Island> islands;
    // The keys of the pictures it asked for.
    QStringList pictures;
};

struct BuildOptions {
    // Tables, code blocks and display math as islands rather than in place.
    bool islands = true;
    // Whom island documents belong to until a view takes them.
    QObject *islandParent = nullptr;
    // Task boxes, links and notes can be followed: they are anchors.
    bool interactive = true;
    // For a document that leaves this computer (paper, a PDF): a link to a
    // file the document includes leads to where that file is in it, and a
    // link to any other file is written as the document has it, never as
    // the full path on this computer.
    bool portableLinks = false;
    // The document's citations as its citation style formats them: each
    // citation's words, such as "(Doe 2020, p. 3)", in place of its number
    // and locator in brackets, and the style's bibliography in place of the
    // one the text defines. It is the Formatted of the document being built
    // and outlives the builder. Null, or one that is not active, leaves the
    // document's own.
    const DocumentCitations::Formatted *citations = nullptr;
};

class Builder {
public:
    // A note has a way back to each of the first this many places that
    // refer to it, and says how many more there are.
    static constexpr int MaxWaysBack = 100;

    Builder(const md::Document &document, const PreviewStyle &style, Resources *resources,
            const BuildOptions &options = {});

    // Builds a part after the block the cursor is in, which is left as it
    // is, and leaves the cursor at the end of what was built.
    Section build(const Part &part, QTextCursor &cursor);
    // Every part into an empty document, which is given the style's body
    // font; the sections in order. The document then starts with one empty
    // block, as a preview's does.
    // `going`, when given, is told how far building has come, from 0 to 1,
    // part by part, and stops it, with what is built so far, by answering false.
    std::vector<Section> buildAll(QTextDocument *document, const std::function<bool(qreal done)> &going = {});
    // Empties a document down to the one empty block every built document
    // starts with, a pixel high, after which parts are built.
    static void prepare(QTextDocument *document);

    // The images the builder draws itself, registered with a document under
    // these names: a task's box, empty and checked.
    static QString taskImageName(bool checked);
    static QImage taskImage(const PreviewStyle &style, bool checked, qreal size);
    // The link a task's box is: "parch:task/<offset>", the offset of its
    // marker from the start of the section's source.
    static QString taskLink(int offset);

private:
    struct Flow;
    struct Char;
    struct Out;
    struct Marker;

    QTextBlockFormat blockFormat(const Flow &flow, qreal fontSize, qreal top, qreal bottom) const;
    QTextCharFormat charFormat(const Char &state) const;
    Char bodyChar(const Flow &flow) const;

    // Starts a block of text; a list marker that waits is written first.
    void openBlock(const Flow &flow, qreal fontSize, qreal top, qreal bottom, const md::Node *source,
                   const Char &markerState, Qt::Alignment alignment = {});
    // An empty block of a height, for a rule or an island.
    void emptyBlock(const Flow &flow, qreal height, qreal top, qreal bottom, const md::Node *source, int island = 0);
    void flushMarker(const Flow &flow);
    void raiseBottomMargin(qreal margin);
    int here() const;
    // Notes that what is written next came from a node: from its start, or
    // from `source` when that is given.
    void anchor(const md::Node &node, int length, bool block = false, int source = -1);
    void name(const QString &name);
    void text(const QString &text, const Char &state);

    void blocks(const std::vector<md::Node> &nodes, const Flow &flow);
    void block(const md::Node &node, const Flow &flow);
    void paragraph(const md::Node &node, const Flow &flow);
    void heading(const md::Node &node, const Flow &flow, int level, const std::vector<md::Node> &children);
    void quote(const std::vector<md::Node> &children, const Flow &flow, bool html);
    void list(const md::Node &node, const Flow &flow);
    void definitionList(const md::Node &node, const Flow &flow);
    void contents(const md::Node &node, const Flow &flow, int depth);
    void rule(const Flow &flow, const md::Node *node, Mark::Kind kind);
    void codeBlock(const md::Node &node, const Flow &flow, const QString &code, const QString &language,
                   const QString &caption, PreviewStyle::Ink captionInk, PreviewStyle::Ink ink);
    void table(const md::Node &node, const Flow &flow);
    void mathBlock(const md::Node &node, const Flow &flow);
    void diagram(const md::Node &node, const Flow &flow);
    void htmlBlock(const md::Node &node, const Flow &flow);
    void htmlFlow(const std::vector<md::Node> &nodes, const Flow &flow);
    void htmlElementBlock(const md::Node &node, const Flow &flow);
    void htmlTable(const md::Node &node, const Flow &flow);
    void note(const Part &part);
    void entry(const Part &part);

    void inlines(const std::vector<md::Node> &nodes, const Char &state);
    // Where a link to a local file leads (see BuildOptions::portableLinks);
    // `written` is the destination as the document has it.
    QString localHref(const md::Destination &target, const QString &written) const;
    void inl(const md::Node &node, const Char &state);
    void htmlInline(const md::Node &node, const Char &state);
    void image(const md::Node &node, const QString &destination, const QString &alt, const QString &title,
               const Char &state);
    void picture(const Picture &picture, const QSizeF &size, QTextCharFormat::VerticalAlignment alignment,
                 const Char &state, bool fitColumn);
    void mathInline(const md::Node &node, const Char &state);
    void noteReference(const md::Node &node, const Char &state);
    // The raised number of a note, which goes to the note.
    void noteMark(int number, const Char &state);
    void citation(const md::Node &node, const Char &state);
    // A citation's words: the style's, or else its number and locator.
    void citationText(const md::Node &node, const Char &state);
    // Text a citation style formatted, run by run. With `lines`, a run the
    // style sets apart stands on a line of its own.
    void styledText(const CslText &words, const Char &state, bool lines = false);
    void styledEntry(const Part &part);
    // Whether a citation style formatted the document.
    bool styled() const;
    // The number of the note a citation is, in a note style; 0 for a
    // citation that stands in the text.
    int citationNote(const md::Node &node);

    // Fills a table with rows of cells, each cell {node, header, align, column, span}.
    struct Cell;
    QTextTable *fillTable(QTextCursor &cursor, const std::vector<std::vector<Cell>> &rows, int columns, const Flow &flow);
    Island &addIsland(Island::Kind kind, const Flow &flow, const md::Node &node, const QFont &font);
    void placeIsland(const Flow &flow, const md::Node &node, qreal top, qreal bottom);
    void uses(const Picture &picture);

    const md::Document &m_document;
    PreviewStyle m_style;
    Resources *m_resources;
    BuildOptions m_options;
    Unresolved m_unresolved;

    // The section being built and where its text goes.
    Section m_section;
    Out *m_out = nullptr;
    int m_sourceBase = 0;
    // Whether anchors are kept: for nodes of the document's own text.
    Marker *m_marker = nullptr;
    // The cursor stands in an empty block that the next block takes over.
    bool m_reuseBlock = false;
    // The citations that are notes of their own, read when one is first met.
    QHash<const md::Node *, int> m_citationNotes;
    bool m_citationNotesRead = false;
    // How wide the first field of a bibliography entry is at most, where
    // the style sets it apart; -1 until it is measured.
    qreal m_firstField = -1;
};

} // namespace preview
