#pragma once

#include "sourcetypes.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <optional>

// What an editing command does to a document: its replacements, each
// addressing the text before the command, sorted and not overlapping, so
// that SourceBuffer applies them as one undo step; and the selection after
// it, from anchor to cursor, in the text after it. A change with no edits
// only moves the selection.
struct TextChange {
    QList<SourceEdit> edits;
    int anchor = 0;
    int cursor = 0;
};

// The formatting commands and the keys that edit Markdown structure, as
// operations on the source text and a selection. They read lines and
// delimiters directly rather than a parse of the document, so each works on
// any text, finished or not, and changes only what it is for. Positions are
// UTF-16 offsets; lines end with LF, CRLF or a lone CR, and every line break
// a command writes is the document's own.
namespace MarkdownEditing {

struct Options {
    // Headings of levels one and two are underlined (Setext) rather than
    // marked with # (ATX); levels three to six are always marked.
    bool setextHeadings = false;
    // The document's line break.
    QString newline = QStringLiteral("\n");
    // The dialect reads `[^label]` as a note reference, and, as the Writer
    // profile does, `[^a note of its own.]` as an inline note.
    bool footnotes = true;
    bool inlineNotes = true;
    // How the dialect reads a table's pipes: a pipe inside a code span is
    // code (Writer), two pipes side by side merge cells (Writer with merged
    // cells on), and a line starting `//` is a comment.
    bool protectCodePipes = true;
    bool mergedCells = true;
    bool lineComments = true;
};

// Every formatting command's id, in the order a menu lists them.
QStringList commands();
bool isCommand(const QString &id);

// What a formatting command does with the selection from anchor to cursor.
// An inline command (bold, italic, strikethrough, highlight, code, inline
// math, comment) wraps a selection, or each selected line's text, in its
// markers, or removes them when they are already there; without a selection
// it removes the markers around the caret, or inserts a pair with the caret
// between them. A line command (headings, paragraph, quote, lists, task
// checkbox) changes every line the selection touches, and turns itself off
// when every line already has it. Nothing when the command changes nothing.
std::optional<TextChange> format(const QString &command, QStringView text, int anchor, int cursor,
                                 const Options &options = {});

// Enter: in a list item, a new item of the same list (the next number, an
// unchecked box for a task); on an empty item, the end of the list. Nothing
// elsewhere, where Enter is a plain line break.
std::optional<TextChange> newLine(QStringView text, int anchor, int cursor, const Options &options = {});
// Shift+Enter: a Markdown hard line break, two spaces and a line break.
TextChange hardBreak(QStringView text, int anchor, int cursor, const Options &options = {});
// Tab, or Shift+Tab with outdent: indents or outdents a list item, or every
// line of a selection over several lines. Nothing for anything else, where
// Tab types a tab.
std::optional<TextChange> indent(QStringView text, int anchor, int cursor, bool outdent, const Options &options = {});
// Tab, or Shift+Tab with backwards, in a pipe table: selects the text of the
// next or the previous cell. Tab in the last cell adds a row. Nothing
// outside a table.
std::optional<TextChange> moveToCell(QStringView text, int anchor, int cursor, bool backwards,
                                     const Options &options = {});
// A new table of empty cells, `rows` counting the header, written tidy as
// a block of its own after the caret's line, with the caret in its first
// cell.
TextChange insertTable(QStringView text, int position, int columns, int rows, const Options &options = {});
// Aligns the pipes and padding of the pipe table holding the cursor, as its
// delimiter row aligns each column, without changing any cell's text. Pipes
// that are escaped or inside code spans are text; columns are measured in
// display width, so that wide characters line up.
std::optional<TextChange> formatTable(QStringView text, int anchor, int cursor, const Options &options = {});

// The columns a text takes in a monospace font: two for a wide or fullwidth
// character, none for a combining mark, by grapheme.
int displayWidth(QStringView text);

// The label of the note reference `[^label]` at or around a position that
// no `[^label]:` in the text defines: a single word of letters, digits, _,
// - and :, which the dialect reads as a reference that leads nowhere. Empty
// for anything else, a definition's own marker and code among it. The
// commands inlineNote, which makes it an inline note, `[^label.]`, and
// defineNote, which adds `[^label]: ` at the end of the text for its note,
// act on it.
QString undefinedNoteAt(QStringView text, int position);

} // namespace MarkdownEditing
