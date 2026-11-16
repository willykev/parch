#pragma once

#include "dialectchoice.h"
#include "mdtree.h"

#include <QString>

#include <vector>

// Documents read from disk for the commands run from the terminal, and what
// is wrong in them. Plain functions of their arguments and the files they
// read; nothing here needs the window or a display.
namespace DocumentCheck {

// A document as a command reads it: its file, with every file it includes
// assembled, in the dialect chosen for it.
struct Loaded {
    QString path; // canonical
    Dialect::Choice dialect;
    md::Document document;
};

// Reads the Markdown file at `path` (absolute) and parses it in `dialect`,
// its includes read from disk. False, with why, when the file cannot be
// read: it is missing, a folder, not a regular file, or unreadable.
bool load(const QString &path, const Dialect::Choice &dialect, Loaded *loaded, QString *error);

// A place in a file: its line from 1, and its column from 1 in characters
// (code points), as editors count them.
struct Position {
    int line = 1;
    int column = 1;
};
Position positionOf(QStringView text, int offset);

// The lines of a text, for many positions in it. The text must outlive it.
class LineIndex {
public:
    explicit LineIndex(QStringView text);
    Position at(int offset) const;

private:
    QStringView m_text;
    std::vector<int> m_starts;
};

struct Problem {
    int file = 0; // index into md::Document::files
    md::Range range;
    Position position;
    md::Severity severity = md::Severity::Warning;
    // The engine's diagnostic code, or one of the checks made here:
    // "unresolved-asset" (an image whose file is not there), "broken-link"
    // (a link to a file that is not there) and "broken-link-anchor" (a link
    // to a heading another Markdown file does not have).
    QString code;
    // What kind of problem: include, reference, link, asset, footnote,
    // citation, variable, table, math, front-matter or html.
    QString kind;
    QString message;
};

// Everything wrong in a document, in file and source order: the engine's
// diagnostics, then images whose files are missing and links to files or
// headings that are not there. Each problem is reported once, even in a
// file included twice. Linked Markdown files are read to check the heading a
// link names, in the document's own dialect.
std::vector<Problem> problems(const Loaded &loaded);

// Whether anything is a warning or an error. Info alone is not a problem.
bool anyProblem(const std::vector<Problem> &problems);

} // namespace DocumentCheck
