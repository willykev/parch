#pragma once

#include "apppaths.h"
#include "diskfile.h"
#include "sourcetypes.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

// One recovery file per document, holding the newest text that is not yet in
// the document's own file. A file is replaced atomically, so a crash while
// writing leaves the previous entry intact. Every method works on plain
// values and is safe to call from any thread.
class Recovery {
public:
    explicit Recovery(const AppPaths &paths);

    struct Entry {
        QString documentId;
        // The canonical path of a named file; empty for a draft.
        QString path;
        QString text;
        Revision revision = 0;
        QDateTime written;
        // What the document's own file held when the text last matched it, so a
        // restart can tell whether the file has changed since, with the
        // file's permission bits, which a file made again in its place is
        // given until the file is read again; 0 for an entry that did not
        // record them.
        DiskVersion base;
        // False for an entry from a format that did not record the base.
        bool hasBase = false;
        // The document's line ending and byte-order mark, which a save
        // writes the text with; the defaults for an entry that did not
        // record them.
        TextConvention convention;
        bool hasConvention = false;
    };

    bool write(const QString &documentId, const QString &path, const SourceSnapshot &snapshot,
               const DiskVersion &base, QString *error = nullptr) const;
    // The same, with the document's line ending and byte-order mark, for a
    // restart that cannot read them from the document's file.
    bool write(const QString &documentId, const QString &path, const SourceSnapshot &snapshot,
               const DiskVersion &base, const TextConvention &convention, QString *error = nullptr) const;
    void discard(const QString &documentId) const;
    // Every entry that can be read, oldest first; damaged files are skipped.
    QList<Entry> pending() const;

    // parch's own saves of a document, each as the content it replaced and
    // the content it wrote, by their SHA-256 (empty for a file that did not
    // exist), and the revision of the text it wrote. A save notes itself
    // before it writes, so that whenever its file holds the new content, a
    // restart can tell that parch wrote it: an entry whose base is older
    // than the file is then no sign of another program's change. Only the
    // newest few are kept. False when the note cannot be written, which
    // leaves the save to go on without it.
    bool noteSave(const QString &documentId, const QByteArray &from, const QByteArray &to, Revision revision) const;
    // The newest save, noted with this content and revision, reached its
    // file. Until then the file may still hold what that save replaced.
    bool noteLanded(const QString &documentId, const QByteArray &to, Revision revision) const;
    // The newest save, noted with this content and revision, failed and left
    // its file as it was: it is forgotten, and the saves before it still tell.
    void dropSave(const QString &documentId, const QByteArray &to, Revision revision) const;
    // Whether the content `to` is what parch's newest save of the document
    // wrote, reached from the content `from` through its saves one after
    // another; or, while that save has not landed, what it was replacing,
    // which an earlier save wrote. Content an earlier save wrote is not
    // otherwise enough, since another program can have put it back since.
    bool savedSince(const QString &documentId, const QByteArray &from, const QByteArray &to) const;
    // Whether the content is what parch's newest save of the document
    // wrote, or what it was replacing while it has not landed.
    bool savedLast(const QString &documentId, const QByteArray &content) const;
    // A save of a document: the content it wrote, empty for a file it
    // removed, and the revision of the text it wrote.
    struct Save {
        QByteArray content;
        Revision revision = 0;
    };
    // parch's newest save of a document, landed or not.
    std::optional<Save> lastSave(const QString &documentId) const;
    // The save whose content the file holds, when savedLast() says the file
    // holds parch's: the newest, or the one before it while the newest has
    // not landed.
    std::optional<Save> saveHeld(const QString &documentId, const QByteArray &content) const;
    // Forgets the document's saves: once it is closed and its file holds its
    // text, and whenever it meets content parch did not write, after which
    // its earlier saves tell nothing about what is in its file. While the
    // document is open they outlive its entry, since a save can be landing.
    void forgetSaves(const QString &documentId) const;
    // Forgets the saves of every document that has neither an entry nor an
    // id among the open ones, as those of a document whose file held its
    // text when parch ended.
    void sweepSaves(const QStringList &openIds) const;

private:
    bool writeEntry(const QString &documentId, const QString &path, const SourceSnapshot &snapshot,
                    const DiskVersion &base, const std::optional<TextConvention> &convention, QString *error) const;
    // A noted save, with its two hashes in hex, "-" for none, and whether it
    // is known to have reached its file.
    struct Link {
        QByteArray from;
        QByteArray to;
        Revision revision = 0;
        bool landed = true;
    };
    QString fileFor(const QString &documentId) const;
    QString savesFor(const QString &documentId) const;
    // The noted saves, oldest first.
    QList<Link> saves(const QString &documentId) const;
    bool writeSaves(const QString &documentId, const QList<Link> &links) const;
    // The newest link whose content the file can hold now, by `content`'s hash in hex.
    static std::optional<qsizetype> heldBy(const QList<Link> &links, const QByteArray &content);

    AppPaths m_paths;
};
