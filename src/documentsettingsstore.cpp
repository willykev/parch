#include "documentsettingsstore.h"

#include "citations.h"
#include "document.h"
#include "documentlinks.h"
#include "documentreading.h"
#include "pathtext.h"
#include "sourcebuffer.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

QString tr(const char *text) { return QCoreApplication::translate("DocumentSettings", text); }

// What the window calls each extension.
QString extensionTitle(const QString &name)
{
    static const QHash<QString, const char *> titles{
        {QStringLiteral("tables"), "Tables"},
        {QStringLiteral("strikethrough"), "Strikethrough"},
        {QStringLiteral("task-lists"), "Task lists"},
        {QStringLiteral("autolinks"), "Bare web and e-mail addresses as links"},
        {QStringLiteral("front-matter"), "Front matter"},
        {QStringLiteral("math"), "Math"},
        {QStringLiteral("mermaid"), "Mermaid diagrams"},
        {QStringLiteral("definition-lists"), "Definition lists"},
        {QStringLiteral("footnotes"), "Footnotes"},
        {QStringLiteral("citations"), "Citations and bibliographies"},
        {QStringLiteral("highlight"), "Highlight (==text==)"},
        {QStringLiteral("super-subscript"), "Superscript and subscript"},
        {QStringLiteral("line-comments"), "Comment lines (//)"},
        {QStringLiteral("page-breaks"), "Page breaks (+++)"},
        {QStringLiteral("toc"), "Table of contents ({{TOC}})"},
        {QStringLiteral("variables"), "Variables ([%key])"},
        {QStringLiteral("includes"), "Included files"},
        {QStringLiteral("merged-cells"), "Merged table cells"},
        {QStringLiteral("heading-labels"), "Heading labels"},
    };
    const char *title = titles.value(name);
    return title ? tr(title) : name;
}

QVariantMap layerMap(const Dialect::Layer &layer)
{
    QVariantMap extensions;
    for (const auto &[name, on] : layer.extensions) extensions.insert(name, on);
    return {
        {QStringLiteral("profile"), layer.profile ? md::Options::forProfile(*layer.profile).profileName() : QString()},
        {QStringLiteral("extensions"), extensions},
        {QStringLiteral("bibliography"), layer.bibliography ? QVariant(*layer.bibliography) : QVariant()},
        {QStringLiteral("citationStyle"), layer.citationStyle},
        {QStringLiteral("libraryRoot"), layer.libraryRoot},
    };
}

QString styleTitle(const QString &style)
{
    if (style.isEmpty()) return {};
    if (Dialect::isStyleFile(style)) return QFileInfo(style).fileName();
    for (const CslCatalog::StyleInfo &info : CslCatalog::bundledStyles())
        if (info.id == style) return info.title;
    return style;
}

QString folderOfDocument(const Document *document)
{
    return document && !document->isDraft() ? PathText::folderOf(document->path()) : QString();
}

} // namespace

DocumentSettingsStore::DocumentSettingsStore(const AppPaths &paths, QObject *parent)
    : QObject(parent), m_store(paths.documentSettingsFile()), m_registryFile(paths.registryFile())
{
    QString error;
    if (!m_store.load(&error))
        qWarning("parch: the document settings cannot be read (%s); none apply until one is changed",
                 qPrintable(error));
}

void DocumentSettingsStore::setLibraryFolders(std::function<QStringList()> folders)
{
    m_libraryFolders = std::move(folders);
    refresh();
}

QStringList DocumentSettingsStore::folders() const
{
    const QStringList given = m_libraryFolders ? m_libraryFolders() : QStringList();
    if (given != m_foldersGiven) {
        // Documents are known by canonical paths, and so are their folders.
        m_foldersGiven = given;
        m_canonicalFolders.clear();
        for (const QString &folder : given) {
            const QString canonical = QFileInfo(folder).canonicalFilePath();
            m_canonicalFolders.append(canonical.isEmpty() ? folder : canonical);
        }
    }
    return m_canonicalFolders;
}

QString DocumentSettingsStore::libraryFolderOf(const Document *document) const
{
    if (!document || document->isDraft()) return {};
    return DocumentLinks::libraryOf(document->path(), folders() + m_store.libraryFolders());
}

Dialect::Layers DocumentSettingsStore::layersFor(const Document *document) const
{
    Dialect::Layers layers;
    if (!document) {
        layers.application = m_store.application();
        return layers;
    }
    const DocumentSettings::Saved saved =
        m_store.savedFor(document->id(), document->isDraft() ? QString() : document->path(), folders());
    layers.document = saved.document;
    layers.library = saved.library;
    layers.application = saved.application;
    return layers;
}

std::function<md::Options(const QString &path)> DocumentSettingsStore::dialectsOfFiles() const
{
    // Which id each file has, as parch keeps it.
    QHash<QString, QString> ids;
    QFile registry(m_registryFile);
    if (registry.open(QIODevice::ReadOnly)) {
        const QJsonObject paths = QJsonDocument::fromJson(registry.readAll()).object();
        for (auto it = paths.begin(); it != paths.end(); ++it) ids.insert(it.key(), it.value().toString());
    }
    return [store = m_store, folders = folders(), ids](const QString &path) {
        const DocumentSettings::Saved saved = store.savedFor(ids.value(path), path, folders);
        Dialect::Layers layers;
        layers.document = saved.document;
        layers.library = saved.library;
        layers.application = saved.application;
        return Dialect::effective(layers).options;
    };
}

void DocumentSettingsStore::watch(Document *document)
{
    if (!document || m_documents.contains(document)) return;
    m_documents.append(document);
    // A document that moves, or a draft that is named, may come into
    // another library folder.
    connect(document, &Document::changed, this, [this, document] { apply(document); });
    connect(document, &QObject::destroyed, this, [this, document] { m_documents.removeAll(document); });
    apply(document);
}

void DocumentSettingsStore::apply(Document *document)
{
    if (document) document->setSavedDialect(layersFor(document));
}

void DocumentSettingsStore::refresh()
{
    m_documents.removeAll(nullptr);
    for (const QPointer<Document> &document : std::as_const(m_documents)) apply(document);
}

QVariantList DocumentSettingsStore::styles() const
{
    QVariantList list;
    for (const CslCatalog::StyleInfo &style : CslCatalog::bundledStyles())
        list.append(QVariantMap{{QStringLiteral("id"), style.id}, {QStringLiteral("title"), style.title}});
    return list;
}

QVariantList DocumentSettingsStore::extensions() const
{
    QVariantList list;
    for (const QString &name : md::Options::extensionNames())
        list.append(QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("title"), extensionTitle(name)}});
    return list;
}

QVariantMap DocumentSettingsStore::describe(Document *document) const
{
    Dialect::Layers layers = layersFor(document);
    const QString library = libraryFolderOf(document);
    if (document)
        layers = DocumentReading::withFrontMatter(layers, document->buffer()->text(), folderOfDocument(document));
    const Dialect::Choice choice = Dialect::effective(layers);
    QVariantList extensionList;
    for (const QString &name : md::Options::extensionNames())
        extensionList.append(QVariantMap{{QStringLiteral("name"), name},
                                         {QStringLiteral("title"), extensionTitle(name)},
                                         {QStringLiteral("on"), choice.options.extension(name)},
                                         {QStringLiteral("from"), choice.extensionsFrom.value(name, QStringLiteral("profile"))}});
    QVariantMap own;
    if (document) own.insert(QStringLiteral("document"), layerMap(layers.document));
    // The library's own entry, without the root its folder gives by default.
    own.insert(QStringLiteral("library"), layerMap(library.isEmpty() ? Dialect::Layer() : m_store.library(library)));
    own.insert(QStringLiteral("application"), layerMap(layers.application));
    return {
        {QStringLiteral("profile"), choice.options.profileName()},
        {QStringLiteral("profileFrom"), choice.profileFrom},
        {QStringLiteral("extensions"), extensionList},
        {QStringLiteral("bibliography"), choice.bibliography},
        {QStringLiteral("bibliographyFrom"), choice.bibliographyFrom},
        {QStringLiteral("citationStyle"), choice.citationStyle},
        {QStringLiteral("citationStyleTitle"), styleTitle(choice.citationStyle)},
        {QStringLiteral("citationStyleFrom"), choice.citationStyleFrom},
        {QStringLiteral("libraryRoot"), choice.libraryRoot},
        {QStringLiteral("libraryRootFrom"), choice.libraryRootFrom},
        {QStringLiteral("library"), library},
        {QStringLiteral("libraryName"), library.isEmpty() ? QString() : PathText::fileName(library)},
        {QStringLiteral("draft"), document ? document->isDraft() : false},
        {QStringLiteral("layers"), own},
    };
}

QVariantMap DocumentSettingsStore::describeApplication() const
{
    return describe(nullptr);
}

bool DocumentSettingsStore::edit(Document *document, const QString &scope,
                                 const std::function<bool(Dialect::Layer &)> &change)
{
    QString error;
    bool kept = false;
    if (scope == u"application") {
        Dialect::Layer layer = m_store.application();
        if (!change(layer)) return false;
        kept = m_store.setApplication(layer, &error);
    } else if (scope == u"document") {
        if (!document) return false;
        Dialect::Layer layer = m_store.document(document->id());
        if (!change(layer)) return false;
        kept = m_store.setDocument(document->id(), layer, &error);
    } else if (scope == u"library") {
        const QString folder = libraryFolderOf(document);
        if (folder.isEmpty()) {
            emit problem(tr("Not in the library"),
                         tr("Only a document in one of the library's folders has library settings."));
            return false;
        }
        Dialect::Layer layer = m_store.library(folder);
        if (!change(layer)) return false;
        kept = m_store.setLibrary(folder, layer, &error);
    } else {
        return false;
    }
    // A change that could not be written still applies while parch runs.
    if (!kept) emit problem(tr("Document settings not saved"), error);
    refresh();
    ++m_revision;
    emit changed();
    return kept;
}

bool DocumentSettingsStore::setProfile(Document *document, const QString &scope, const QString &profile)
{
    if (!profile.isEmpty() && !Dialect::profileNamed(profile)) return false;
    return edit(document, scope, [&](Dialect::Layer &layer) {
        layer.profile = profile.isEmpty() ? std::nullopt : Dialect::profileNamed(profile);
        return true;
    });
}

bool DocumentSettingsStore::setExtension(Document *document, const QString &scope, const QString &name,
                                         const QString &state)
{
    if (!md::Options::extensionNames().contains(name)) return false;
    if (!state.isEmpty() && state != u"on" && state != u"off") return false;
    return edit(document, scope, [&](Dialect::Layer &layer) {
        layer.extensions.removeIf([&](const std::pair<QString, bool> &extension) { return extension.first == name; });
        if (!state.isEmpty()) layer.extensions.append({name, state == u"on"});
        return true;
    });
}

bool DocumentSettingsStore::resetExtensions(Document *document, const QString &scope)
{
    return edit(document, scope, [](Dialect::Layer &layer) {
        layer.extensions.clear();
        return true;
    });
}

bool DocumentSettingsStore::setBibliography(Document *document, const QString &scope, const QStringList &files)
{
    QStringList kept;
    for (const QString &file : files) {
        if (!PathText::isFullPath(file)) return false;
        if (!kept.contains(file)) kept.append(file);
    }
    return edit(document, scope, [&](Dialect::Layer &layer) {
        layer.bibliography = kept;
        return true;
    });
}

bool DocumentSettingsStore::resetBibliography(Document *document, const QString &scope)
{
    return edit(document, scope, [](Dialect::Layer &layer) {
        layer.bibliography.reset();
        return true;
    });
}

bool DocumentSettingsStore::setCitationStyle(Document *document, const QString &scope, const QString &style)
{
    QString chosen = style.trimmed();
    if (!chosen.isEmpty() && !Dialect::isStyleFile(chosen)) {
        chosen = CslCatalog::bundledId(chosen);
        if (chosen.isEmpty()) return false;
    }
    return edit(document, scope, [&](Dialect::Layer &layer) {
        layer.citationStyle = chosen;
        return true;
    });
}

bool DocumentSettingsStore::setLibraryRoot(Document *document, const QString &scope, const QString &folder)
{
    if (scope == u"application") return false;
    if (!folder.isEmpty() && !PathText::isFullPath(folder)) return false;
    return edit(document, scope, [&](Dialect::Layer &layer) {
        layer.libraryRoot = folder;
        return true;
    });
}
