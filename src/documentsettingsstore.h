#pragma once

#include "apppaths.h"
#include "dialectchoice.h"
#include "documentsettings.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class Document;

// The Markdown settings of documents, of library folders and of every
// document, as the window keeps and edits them: the store of
// documentsettings.h, read once and written back with each change. Every
// open document is given its saved layers (Document::setSavedDialect) as it
// opens, as it moves or is named, as the library's folders change and as
// any setting changes, so that every view reads it in its dialect at once.
//
// For QML, describe() says what applies to a document and where each value
// comes from, and the setters change one layer: "document" (the document's
// own), "library" (the library folder that holds it) or "application"
// (every document, as Preferences shows it). An empty value gives a setting
// back to the layers below. What cannot be written is said through
// problem(), and still applies for as long as parch runs.
class DocumentSettingsStore : public QObject {
    Q_OBJECT
    // One more with every change, for bindings that call describe().
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    // The bundled citation styles, [{id, title}], in the order parch lists them.
    Q_PROPERTY(QVariantList styles READ styles CONSTANT)
    // Every extension, [{name, title}], in the engine's order.
    Q_PROPERTY(QVariantList extensions READ extensions CONSTANT)

public:
    explicit DocumentSettingsStore(const AppPaths &paths, QObject *parent = nullptr);

    // The library's folders, full paths, as the settings list them.
    void setLibraryFolders(std::function<QStringList()> folders);
    // Gives the document its layers now, and keeps them current.
    void watch(Document *document);
    // Gives every document its layers again: the library's folders changed.
    void refresh();

    int revision() const { return m_revision; }
    QVariantList styles() const;
    QVariantList extensions() const;
    const DocumentSettings::Store &store() const { return m_store; }

    // What applies to the document and where each comes from:
    //   profile, profileFrom; extensions [{name, title, on, from}];
    //   bibliography [paths], bibliographyFrom; citationStyle,
    //   citationStyleTitle, citationStyleFrom; libraryRoot, libraryRootFrom;
    //   library (the folder holding it, or empty), draft; and `layers`, what
    //   each of "document", "library" and "application" sets on its own:
    //   {profile, extensions {name: bool}, bibliography [paths] or null,
    //   citationStyle, libraryRoot}.
    // A source is "document", "front-matter", "library", "application",
    // "default", or for an extension also "profile".
    Q_INVOKABLE QVariantMap describe(Document *document) const;
    // The application's own layer alone, as Preferences edits it, in the form
    // of describe()'s, without front matter, document or library.
    Q_INVOKABLE QVariantMap describeApplication() const;

    // `document` may be null for "application". Each returns whether it was kept.
    Q_INVOKABLE bool setProfile(Document *document, const QString &scope, const QString &profile);
    // `state` is "on", "off", or empty to leave the extension to the layers below.
    Q_INVOKABLE bool setExtension(Document *document, const QString &scope, const QString &name, const QString &state);
    Q_INVOKABLE bool resetExtensions(Document *document, const QString &scope);
    // Full paths; the list is chosen as a whole, and an empty list chooses none.
    Q_INVOKABLE bool setBibliography(Document *document, const QString &scope, const QStringList &files);
    Q_INVOKABLE bool resetBibliography(Document *document, const QString &scope);
    // A bundled style's id or a .csl file's full path; empty resets it.
    Q_INVOKABLE bool setCitationStyle(Document *document, const QString &scope, const QString &style);
    // A folder's full path; empty resets it. Not for "application".
    Q_INVOKABLE bool setLibraryRoot(Document *document, const QString &scope, const QString &folder);

    // The library folder that holds the document, the nearest when several
    // do; empty for a draft or a file outside the library.
    QString libraryFolderOf(const Document *document) const;
    // The layers the document is given.
    Dialect::Layers layersFor(const Document *document) const;
    // The dialect of any Markdown file by its canonical path, open or not, as
    // the settings are now: its own settings, found by the id parch keeps for
    // its path, its library folder's and every document's. A copy, safe to
    // use on any thread.
    std::function<md::Options(const QString &path)> dialectsOfFiles() const;

signals:
    void changed();
    void problem(const QString &problem, const QString &detail);

private:
    void apply(Document *document);
    bool edit(Document *document, const QString &scope, const std::function<bool(Dialect::Layer &)> &change);
    QStringList folders() const;

    DocumentSettings::Store m_store;
    QString m_registryFile;
    std::function<QStringList()> m_libraryFolders;
    // The library's folders as canonical paths, found once whenever they change.
    mutable QStringList m_canonicalFolders;
    mutable QStringList m_foldersGiven;
    QList<QPointer<Document>> m_documents;
    // The own settings of documents of parch's own, by id: kept while parch
    // runs, never written, as nothing of such a document is.
    QHash<QString, Dialect::Layer> m_scratch;
    int m_revision = 0;
};
