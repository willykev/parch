#pragma once

#include "apppaths.h"
#include "dialectchoice.h"

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

// The Markdown settings parch keeps for documents, for library folders and
// for every document, never in the documents themselves:
// AppPaths::documentSettingsFile(), JSON.
//
//   {
//     "application": {"profile": "writer", "citationStyle": "apa"},
//     "documents": {
//       "<document id>": {"profile": "standard",
//                         "extensions": {"math": true, "footnotes": false},
//                         "libraryRoot": "/full/path",
//                         "bibliography": ["/full/path/refs.bib"],
//                         "citationStyle": "/full/path/style.csl",
//                         "maxIncludeDepth": 8, "maxIncludeSize": 1048576,
//                         "maxIncludes": 100, "remotePictures": ["pictures.example"]}
//     },
//     "libraries": {
//       "/full/path/of/a/library/folder": {"profile": "writer",
//                                          "extensions": {"citations": false}}
//     }
//   }
//
// A document is known by its id, which AppPaths::registryFile() gives its
// canonical path, so its settings follow it when parch sees it renamed. A
// draft has an id and settings of its own before it has a file. Every key of
// an entry is optional; `bibliography` is a list of files, chosen as a whole
// (an empty list chooses none), `citationStyle` is the id of a bundled
// style or the full path of a .csl file, and the three include limits are
// whole numbers within their ranges (dialectchoice.h), the size in bytes;
// one out of its range is not read. `remotePictures` lists the hosts
// approved for this document. An older true value becomes the hosts named
// in the first reading of its preview. The choice belongs to the document
// alone. A library's entry applies to every
// document beneath its folder; the nearest library folder holding a
// document is its library, and also where its `/rooted` includes are found
// unless a setting says otherwise. The application's entry applies to every
// document. What an entry holds that parch does not know is kept when parch
// writes it back.
//
// Commands run from the terminal read these only when asked to, with
// --use-saved-settings (see dialectchoice.h).
namespace DocumentSettings {

struct Saved {
    Dialect::Layer document;
    Dialect::Layer library;
    Dialect::Layer application;
    // The library folder that holds the document, the nearest when several
    // do; empty when none does.
    QString libraryFolder;
};

// What one entry of the file says, and an entry saying it: `entry`'s own
// keys parch does not know are kept.
Dialect::Layer layerOf(const QJsonObject &entry);
QJsonObject entryOf(const Dialect::Layer &layer, QJsonObject entry = {});

// What is kept for the document at `canonicalPath`. Nothing kept is no
// error; a file that is there but cannot be read as settings is, with why.
bool read(const AppPaths &paths, const QString &canonicalPath, Saved *saved, QString *error);

// The whole file, read once and written back whole with each change, in one
// step. A missing file holds nothing; one that cannot be read is never
// written over: the first change moves it aside as <name>.bad (or .bad.2 and
// on) and writes the settings anew.
class Store {
public:
    explicit Store(QString file);

    // Reads the file again. False, with why, when it is there but cannot be
    // read as settings; it then holds nothing until it is changed.
    bool load(QString *error = nullptr);
    QString file() const { return m_file; }

    Dialect::Layer application() const;
    Dialect::Layer document(const QString &documentId) const;
    // A library folder's own entry, written with a final slash or without.
    Dialect::Layer library(const QString &folder) const;
    // The folders that have an entry.
    QStringList libraryFolders() const;

    // Each writes the file; false, with why, when it could not be written,
    // though the change still holds for as long as the store does. An empty
    // layer removes the entry.
    bool setApplication(const Dialect::Layer &layer, QString *error = nullptr);
    bool setDocument(const QString &documentId, const Dialect::Layer &layer, QString *error = nullptr);
    bool setLibrary(const QString &folder, const Dialect::Layer &layer, QString *error = nullptr);
    // Whether the document's pictures from the web are shown, as its entry's
    // `remotePictures` says.
    bool remotePictures(const QString &documentId) const;
    QSet<QString> pictureHosts(const QString &documentId) const;
    bool needsPictureHosts(const QString &documentId) const;
    bool setPictureHosts(const QString &documentId, const QSet<QString> &hosts, QString *error = nullptr);

    // The saved layers of a document: its own by id (none for an empty id),
    // those of the nearest library folder holding `path` among `folders`
    // and the folders with an entry, and the application's. A library
    // folder is the document's library root unless a layer names another.
    Saved savedFor(const QString &documentId, const QString &path, const QStringList &folders) const;

private:
    bool set(const QString &section, const QString &key, const Dialect::Layer &layer, QString *error);
    bool write(QString *error);

    QString m_file;
    QJsonObject m_stored;
    bool m_damaged = false;
};

} // namespace DocumentSettings
