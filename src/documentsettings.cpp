#include "documentsettings.h"

#include "diskfile.h"
#include "documentlinks.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace DocumentSettings {
namespace {

const QString applicationKey = QStringLiteral("application");
const QString documentsKey = QStringLiteral("documents");
const QString librariesKey = QStringLiteral("libraries");

// The JSON object in a file: an empty one when there is no file, nothing
// with `error` set when the file cannot be read as one.
std::optional<QJsonObject> readObject(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.exists()) return QJsonObject();
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read %1: %2").arg(path, file.errorString());
        return std::nullopt;
    }
    QJsonParseError parse{};
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !json.isObject()) {
        if (error) *error = QStringLiteral("%1 is not a settings file parch can read").arg(path);
        return std::nullopt;
    }
    return json.object();
}

QJsonObject libraryEntry(const QJsonObject &libraries, const QString &folder)
{
    // A key may be written with a final slash.
    QJsonObject entry = libraries.value(folder).toObject();
    if (entry.isEmpty()) entry = libraries.value(folder + u'/').toObject();
    return entry;
}

QStringList folderKeys(const QJsonObject &libraries)
{
    QStringList folders;
    for (const QString &key : libraries.keys()) {
        if (!key.startsWith(u'/')) continue;
        folders.append(key.size() > 1 && key.endsWith(u'/') ? key.chopped(1) : key);
    }
    return folders;
}

Saved savedFrom(const QJsonObject &store, const QString &documentId, const QString &path, QStringList folders)
{
    Saved saved;
    const QJsonObject libraries = store.value(librariesKey).toObject();
    folders += folderKeys(libraries);
    if (!path.isEmpty()) saved.libraryFolder = DocumentLinks::libraryOf(path, folders);
    if (!saved.libraryFolder.isEmpty()) {
        saved.library = layerOf(libraryEntry(libraries, saved.libraryFolder));
        if (saved.library.libraryRoot.isEmpty()) saved.library.libraryRoot = saved.libraryFolder;
    }
    if (AppPaths::isDocumentId(documentId))
        saved.document = layerOf(store.value(documentsKey).toObject().value(documentId).toObject());
    saved.application = layerOf(store.value(applicationKey).toObject());
    // The application's default applies to every document, so no library
    // root can come from it.
    saved.application.libraryRoot.clear();
    return saved;
}

} // namespace

// One entry of the store. What it says that parch does not know is left
// out, as Settings leaves out what it cannot use.
Dialect::Layer layerOf(const QJsonObject &entry)
{
    Dialect::Layer layer;
    layer.profile = Dialect::profileNamed(entry.value(QStringLiteral("profile")).toString());
    const QJsonObject extensions = entry.value(QStringLiteral("extensions")).toObject();
    const QStringList known = md::Options::extensionNames();
    // In the engine's order, so that a layer read twice is the same layer.
    for (const QString &name : known) {
        const QJsonValue value = extensions.value(name);
        if (value.isBool()) layer.extensions.append({name, value.toBool()});
    }
    const QString root = entry.value(QStringLiteral("libraryRoot")).toString();
    if (root.startsWith(u'/')) layer.libraryRoot = root;
    const QJsonValue bibliography = entry.value(QStringLiteral("bibliography"));
    if (bibliography.isArray()) {
        QStringList files;
        for (const QJsonValue &file : bibliography.toArray())
            if (file.toString().startsWith(u'/')) files.append(file.toString());
        layer.bibliography = files;
    }
    const QString style = entry.value(QStringLiteral("citationStyle")).toString().trimmed();
    if (!style.isEmpty()) layer.citationStyle = style;
    return layer;
}

QJsonObject entryOf(const Dialect::Layer &layer, QJsonObject entry)
{
    const auto put = [&entry](const QString &key, const QJsonValue &value, bool present) {
        if (present) entry.insert(key, value);
        else entry.remove(key);
    };
    put(QStringLiteral("profile"), layer.profile ? md::Options::forProfile(*layer.profile).profileName() : QString(),
        layer.profile.has_value());
    QJsonObject extensions;
    for (const auto &[name, on] : layer.extensions) extensions.insert(name, on);
    put(QStringLiteral("extensions"), extensions, !extensions.isEmpty());
    put(QStringLiteral("libraryRoot"), layer.libraryRoot, !layer.libraryRoot.isEmpty());
    put(QStringLiteral("bibliography"), QJsonArray::fromStringList(layer.bibliography.value_or(QStringList())),
        layer.bibliography.has_value());
    put(QStringLiteral("citationStyle"), layer.citationStyle, !layer.citationStyle.isEmpty());
    return entry;
}

bool read(const AppPaths &paths, const QString &canonicalPath, Saved *saved, QString *error)
{
    *saved = {};
    const auto registry = readObject(paths.registryFile(), error);
    if (!registry) return false;
    const auto settings = readObject(paths.settingsFile(), error);
    if (!settings) return false;
    const auto store = readObject(paths.documentSettingsFile(), error);
    if (!store) return false;

    QStringList folders;
    for (const QJsonValue &folder : settings->value(QStringLiteral("libraryFolders")).toArray())
        if (folder.toString().startsWith(u'/')) folders.append(folder.toString());
    *saved = savedFrom(*store, registry->value(canonicalPath).toString(), canonicalPath, folders);
    return true;
}

Store::Store(QString file) : m_file(std::move(file)) {}

bool Store::load(QString *error)
{
    m_stored = {};
    m_damaged = false;
    QString why;
    const auto stored = readObject(m_file, &why);
    if (!stored) {
        m_damaged = true;
        if (error) *error = why;
        return false;
    }
    m_stored = *stored;
    return true;
}

Dialect::Layer Store::application() const
{
    Dialect::Layer layer = layerOf(m_stored.value(applicationKey).toObject());
    layer.libraryRoot.clear();
    return layer;
}

Dialect::Layer Store::document(const QString &documentId) const
{
    return layerOf(m_stored.value(documentsKey).toObject().value(documentId).toObject());
}

Dialect::Layer Store::library(const QString &folder) const
{
    return layerOf(libraryEntry(m_stored.value(librariesKey).toObject(), folder));
}

QStringList Store::libraryFolders() const
{
    return folderKeys(m_stored.value(librariesKey).toObject());
}

bool Store::setApplication(const Dialect::Layer &layer, QString *error)
{
    Dialect::Layer kept = layer;
    kept.libraryRoot.clear();
    QJsonObject entry = entryOf(kept, m_stored.value(applicationKey).toObject());
    if (entry.isEmpty()) m_stored.remove(applicationKey);
    else m_stored.insert(applicationKey, entry);
    return write(error);
}

bool Store::setDocument(const QString &documentId, const Dialect::Layer &layer, QString *error)
{
    if (!AppPaths::isDocumentId(documentId)) {
        if (error) *error = QStringLiteral("%1 is not a document id").arg(documentId);
        return false;
    }
    return set(documentsKey, documentId, layer, error);
}

bool Store::setLibrary(const QString &folder, const Dialect::Layer &layer, QString *error)
{
    if (!folder.startsWith(u'/')) {
        if (error) *error = QStringLiteral("%1 is not a full path").arg(folder);
        return false;
    }
    // The entry keeps the spelling it already has.
    const QJsonObject libraries = m_stored.value(librariesKey).toObject();
    const QString key = !libraries.contains(folder) && libraries.contains(folder + u'/') ? folder + u'/' : folder;
    return set(librariesKey, key, layer, error);
}

bool Store::set(const QString &section, const QString &key, const Dialect::Layer &layer, QString *error)
{
    QJsonObject entries = m_stored.value(section).toObject();
    const QJsonObject entry = entryOf(layer, entries.value(key).toObject());
    if (entry.isEmpty()) entries.remove(key);
    else entries.insert(key, entry);
    if (entries.isEmpty()) m_stored.remove(section);
    else m_stored.insert(section, entries);
    return write(error);
}

bool Store::write(QString *error)
{
    QString why;
    if (m_damaged) {
        if (DiskFile::moveAside(m_file)) m_damaged = false;
        else why = QStringLiteral("the unreadable file cannot be moved aside");
    }
    if (why.isEmpty() && !QDir().mkpath(QFileInfo(m_file).absolutePath())) {
        why = QStringLiteral("cannot make its folder");
    } else if (why.isEmpty()) {
        QSaveFile file(m_file);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(m_stored).toJson()) < 0 || !file.commit())
            why = file.errorString();
    }
    if (why.isEmpty()) return true;
    if (error) *error = QStringLiteral("The document settings could not be saved to %1: %2").arg(m_file, why);
    return false;
}

Saved Store::savedFor(const QString &documentId, const QString &path, const QStringList &folders) const
{
    return savedFrom(m_stored, documentId, path, folders);
}

} // namespace DocumentSettings
