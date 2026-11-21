#pragma once

#include "mdtree.h"
#include "sourcetypes.h"

#include <QList>
#include <QString>
#include <QStringList>

// The links of one Markdown document to files and to headings, as the
// Markdown engine reads them: inline links and images, reference
// definitions, the src and href of supported HTML, and includes. A link may
// be split across lines; code, comments, front matter and anything the
// engine reads as text refer to nothing. check() finds the ones that lead
// nowhere, and moving or renaming a file in the library updates the ones
// that would. Pure text work apart from check(), which reads the file
// system; all of it is safe on any thread.
namespace DocumentLinks {

struct Link {
    enum class Kind { Link, Image, Definition, Include };
    Kind kind = Kind::Link;
    bool html = false;
    bool editable = true;
    QChar quote; // HTML attribute delimiter, if any
    // The destination as the source writes it, without angle brackets or
    // quotes, before escapes, entities and percent-encoding are read; and
    // where that is, with the line it is on, from 0.
    QString written;
    SourceRange range{0, 0};
    int line = 0;
    // Written between < and >, or, for an include, between quotes.
    bool enclosed = false;
    // The destination as the engine reads it, escapes and entities taken.
    QString dest;
    // What it leads to. A local file's path is absolute when the document's
    // folder is known; an include that starts with / is found in the
    // library, and leads nowhere (an empty path) without one.
    md::Destination destination;
};

// The links written in the document's own text (file 0), in order. `folder`
// is the document's folder, `library` the library folder holding it, if any.
QList<Link> find(const md::Document &document, const QString &folder, const QString &library = {});
// The same for a text, parsed as a document of its own.
// `options` is the dialect it is read in.
QList<Link> find(const QString &text, const QString &folder, const QString &library = {},
                 const md::Options &options = {});

// A link that leads nowhere: to a file that is not there, or to a heading
// (`#slug`, `other.md#slug`) that the document or that Markdown file does
// not have.
struct Missing {
    Link link;
    bool heading = false; // the file is there, the heading is not
};
struct Checked {
    QList<Missing> missing;
    // The links to local files and headings that were looked at.
    int checked = 0;
};
// Parses the document with its includes read from disk, so that headings in
// included files count, and looks at every link to a local file or heading.
// `path` is the document's own file, empty for a draft.
// `options` is the dialect the document is read in (Document::markdownOptions()),
// which Markdown files it links to are read in too.
Checked check(const QString &text, const QString &path, const QString &folder, const QString &library = {},
              const md::Options &options = {});

// Of these folders, the one that holds the path, the nearest when several
// do; empty when none does. Paths are compared as given.
QString libraryOf(const QString &path, const QStringList &libraries);

// A file or a folder that moves, as full paths; a folder moves with
// everything in it.
struct Move {
    QString from;
    QString to;
};

// Where a path is after the moves, or the path itself when none takes it.
QString moved(const QString &path, const QList<Move> &moves);

// One link a move changes: where its destination is in the text, the
// destination as it was and as it becomes, both as written, and its line
// (from 0) around it, before and after, to show.
struct Change {
    SourceRange range{0, 0};
    QString before;
    QString after;
    int line = 0;
    QString lineBefore;
    QString lineAfter;
    QString problem; // an identified reference that cannot be updated safely
};

// The links and front-matter citation files a move would break, each given
// a replacement or a reason it cannot be updated. `path` is the old path;
// it may move itself, when the move takes it along, and then its own
// relative links keep leading where they led. Links to other schemes, to
// headings of the document itself, and absolute paths to nothing that moved
// are left alone. A new destination keeps the old one's form: ./ in front,
// < >, quotes, `file:`, percent-encoding of characters beyond ASCII, a
// fragment and a title; otherwise it is written as md::encodeDestination()
// writes paths, and an include that takes a space is quoted.
// `options` is the dialect the text is read in. If the document changes
// libraries, `destinationLibrary` supplies the new reading boundary.
QList<Change> changes(const QString &text, const QString &path, const QString &library, const QList<Move> &moves,
                      const md::Options &options = {}, const QString &destinationLibrary = {});

} // namespace DocumentLinks
