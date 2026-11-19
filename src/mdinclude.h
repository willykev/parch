#pragma once

#include <QHash>
#include <QString>
#include <QStringView>

// Reading the files a document includes.
//
// An include is a path to a `.md`, `.markdown` or `.txt` file alone in a
// paragraph, as in `chapters/opening.md "Opening"`. A path that starts with
// `/` is found in the library root; any other is found in the folder of the
// file that includes it. md::parse() joins the path, asks the resolver of
// its md::Source for the file, and reads the file's blocks into the tree
// under an Include node, with a file id of their own.
//
// Scope in the assembled document (docs/markdown-dialect.md):
//   - link, footnote and citation definitions and metadata variables are
//     looked up in the file that uses them, then in the root document;
//     definitions of one included file are not seen by another
//   - a footnote is identified by its file and label, so two files may each
//     define `[^1]`; a file included twice contributes each note once
//   - notes, citations and heading slugs are numbered across the whole
//     document, in the order they are first used
//   - relative links and images of an included file resolve from its folder
//
// Confinement: an include is read only from within the document's library
// (Source::libraryRoot) or, for a document in no library, its own folder
// and the folders in it, as its resolver names files, links resolved. One
// that leads out, through "..", a rooted "/../" or a link, is not read and
// its diagnostic, IncludeOutside, says so. A downloaded document so never
// shows, and never exports, the writer's other files.
//
// Guards: a file that includes itself, directly or through others, is not
// expanded and the diagnostic names the chain; nesting deeper than
// Source::maxIncludeDepth, more text than Source::maxExpandedBytes or more
// includes than Source::maxIncludeCount stop the expansion with an Error
// diagnostic and set Document::incomplete.
namespace md {

class IncludeResolver {
public:
    struct File {
        bool ok = false;
        // What identifies the file: two paths to one file must give the
        // same canonical path, since cycles and repeats are told by it.
        QString canonicalPath;
        QString text;
        qint64 bytes = 0;      // its size as UTF-8
        bool missing = false;  // there is no such file
        bool tooLarge = false; // it is larger than `maxBytes`, and was not read
        QString error;         // why it could not be read
    };

    virtual ~IncludeResolver() = default;
    // `path` is absolute. A file larger than `maxBytes` need not be read.
    virtual File read(const QString &path, qint64 maxBytes) = 0;
    // The name that identifies the file or folder at a path, as `File`'s
    // canonical path does, links resolved; empty when nothing is there.
    // Includes are held within their folders by it. By default the path
    // tidied, as for files held in memory.
    virtual QString canonicalPath(const QString &path);
};

// Reads UTF-8 text files from disk, with symbolic links resolved, as
// SafeRead reads them: only a regular file, none of /proc, /sys or /dev,
// and none larger than a document may be (SafeRead::DocumentBytes).
class FileIncludeResolver : public IncludeResolver {
public:
    File read(const QString &path, qint64 maxBytes) override;
    QString canonicalPath(const QString &path) override;
};

// Serves texts held in memory, by the path they were added under.
class MemoryIncludeResolver : public IncludeResolver {
public:
    void add(const QString &path, const QString &text);
    File read(const QString &path, qint64 maxBytes) override;
    int reads() const { return m_reads; }

private:
    QHash<QString, QString> m_files;
    int m_reads = 0;
};

// A URL spelling reserved in include lines, with or without quotes.
bool isUrlSpelling(QStringView path);

// Whether a local path can be written on an include line without quotes.
bool isBareIncludePath(QStringView path);

// The absolute path an include line points at, or an empty string when the
// folder it needs (the library root for `/rooted`, else the including
// file's folder) is not known.
QString includeTarget(const QString &written, const QString &includingDirectory, const QString &libraryRoot);

// Whether a path is the folder or lies in it, at any depth; both spelled
// alike, as canonical paths are.
bool isWithin(const QString &path, const QString &folder);

} // namespace md
