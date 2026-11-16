#pragma once

#include "dialectchoice.h"
#include "documentcitations.h"
#include "mdhtml.h"
#include "mdtree.h"

#include <QString>

#include <vector>

// Documents read from disk for the commands run from the terminal, and what
// is wrong in them. Plain functions of their arguments and the files they
// read; nothing here needs the window or a display.
namespace DocumentCheck {

// A document as a command reads it (documentreading.h): its file, with every
// file it includes assembled, in the dialect chosen for it, with its
// citations formatted in its style.
struct Loaded {
    QString path; // canonical
    Dialect::Choice dialect;
    md::Document document;
    DocumentCitations::Setup citations;
    DocumentCitations::Formatted formatted;

    // The renderer's options, with the citations as the style formats them.
    md::HtmlOptions htmlOptions(md::HtmlOptions options = {}) const;
};

// Reads the Markdown file at `path` (absolute) in the dialect its layers and
// its front matter choose, its includes read from disk. False, with why,
// when the file cannot be read: it is missing, a folder, not a regular
// file, or unreadable.
bool load(const QString &path, const Dialect::Layers &layers, Loaded *loaded, QString *error);
// The same in a dialect already chosen, front matter and all.
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
    int file = 0; // index into md::Document::files, or -1 for a file of its own (`path`)
    md::Range range;
    Position position;
    md::Severity severity = md::Severity::Warning;
    // The engine's diagnostic code, or one of the checks made here:
    // "unresolved-asset" (an image whose file is not there), "broken-link"
    // (a link to a file that is not there) and "broken-link-anchor" (a link
    // to a heading another Markdown file does not have); one of the
    // citation style's (DocumentCitations::Problem); or one of a formula's
    // or diagram's that its helper refused or could not draw
    // (rendercheck.h).
    QString code;
    // What kind of problem: include, reference, link, asset, footnote,
    // citation, bibliography, variable, table, math, diagram, front-matter
    // or html.
    QString kind;
    QString message;
    // A bibliography or style the problem is in rather than the document,
    // with the line in `position` (0 when unknown); or the document itself,
    // with no place in it, for the bibliography and style as a whole.
    QString path;
};

// Everything wrong in a document, in file and source order: the engine's
// diagnostics, then images whose files are missing and links to files or
// headings that are not there, then the problems of its citations, its
// style and its bibliography. Each problem is reported once, even in a
// file included twice. Linked Markdown files are read to check the heading a
// link names, in the document's own dialect. `more` are problems found
// otherwise, such as formulas a helper refused, put in their places.
std::vector<Problem> problems(const Loaded &loaded, std::vector<Problem> more = {});

// What problems() reads, wherever it is kept: as a Loaded holds it, or as
// the window's shared parse of a document does (documentparse.h).
struct Checked {
    QString path; // canonical; empty for a draft
    const Dialect::Choice &dialect;
    const md::Document &document;
    const DocumentCitations::Formatted &formatted;
};
std::vector<Problem> problems(const Checked &checked, std::vector<Problem> more = {});

// Puts problems in file and source order, those of other files last.
void order(std::vector<Problem> &problems);

// Whether anything is a warning or an error. Info alone is not a problem.
bool anyProblem(const std::vector<Problem> &problems);

} // namespace DocumentCheck
