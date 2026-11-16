#pragma once

#include "sourcetypes.h"

#include <QList>
#include <QString>

// The references from one Markdown document to files: inline links and
// images, reference definitions, and Writer's include lines. Found by line
// rules alone, outside fenced and indented code and inline code, until the
// dialect's parser can give them; this is the one place that finds them.
namespace References {
enum class Kind { Link, Image, Definition, Include };

struct Reference {
    Kind kind = Kind::Link;
    // The destination as written, without the angle brackets or quotes
    // around it, and where that is in the text.
    QString destination;
    SourceRange range;
    // Written between < and >, or, for an include, between quotes.
    bool enclosed = false;
    // The line it is on, from 0.
    int line = 0;
};

QList<Reference> find(const QString &text);

// A file or a folder that moves, as full paths; a folder moves with
// everything in it.
struct Move {
    QString from;
    QString to;
};

// One reference a move changes: where it is in the text, the destination
// as it was and as it becomes, both as written, and the line around it,
// before and after, to show.
struct Change {
    SourceRange range;
    QString before;
    QString after;
    int line = 0;
    QString lineBefore;
    QString lineAfter;
};

// The references in a document that a move breaks, each given the
// destination that leads where it led. `path` is where the document is
// before the move; it may move itself, when the move takes it along, and
// then its own relative references are kept leading where they led.
// Relative paths are resolved from the document's folder, percent-encoding
// and all; an include's path that starts with / from the library's folder.
// A destination with a scheme, a bare fragment or an absolute path that
// leads to nothing moved is left alone. Each new destination keeps the old
// one's form: ./ in front, < >, quotes or percent-encoding, and its
// fragment, query and title; a path with a space is written so that it
// stays one destination.
QList<Change> changes(const QString &text, const QString &path, const QString &library, const QList<Move> &moves);

// Where a path is after the moves, or the path itself when none takes it.
QString moved(const QString &path, const QList<Move> &moves);
} // namespace References
