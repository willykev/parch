#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// The local files a Markdown document refers to: its images, its links to
// files, its reference definitions, the src and href of its HTML, and its
// includes. Code blocks, code spans, comments and front matter refer to
// nothing. Remote URLs, other schemes and links within the page are left
// out, since only files on this machine can be checked here. Pure text work
// apart from missing(), which asks the file system; both are safe on any
// thread, for the preview, exports and the Check links command alike.
namespace LinkTargets {

struct Target {
    enum Kind { Image, Link, Include };
    Kind kind = Link;
    // As the document writes it, without angle brackets or title.
    QString destination;
    // The file it names: decoded, and relative to the document's folder,
    // or to the library for an include that starts with "/". Empty for a
    // library-rooted include with no library to resolve it in.
    QString path;
    // Where the destination starts, as a UTF-16 offset, and its line from 1.
    int position = 0;
    int line = 0;
};

// Every local target, in the order the document has them. `folder` is the
// document's own folder; `library`, the library folder that holds it, when
// one does. Paths are resolved but not checked.
QList<Target> local(const QString &markdown, const QString &folder, const QString &library = {});

// The local targets whose file is not there.
QList<Target> missing(const QString &markdown, const QString &folder, const QString &library = {});

// Of these folders, the one that holds the path, the nearest when several
// do; empty when none does. Paths are compared as given.
QString libraryOf(const QString &path, const QStringList &libraries);

} // namespace LinkTargets
