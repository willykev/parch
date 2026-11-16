#include "documentsettings.h"

#include "linktargets.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace DocumentSettings {
namespace {

// The JSON object in a file: an empty one when there is no file, nothing
// with `error` set when the file cannot be read as one.
std::optional<QJsonObject> readObject(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.exists()) return QJsonObject();
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read %1: %2").arg(path, file.errorString());
        return std::nullopt;
    }
    QJsonParseError parse{};
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !json.isObject()) {
        *error = QStringLiteral("%1 is not a settings file parch can read").arg(path);
        return std::nullopt;
    }
    return json.object();
}

// One entry of the store. What it says that parch does not know is left
// out, as Settings leaves out what it cannot use.
Dialect::Layer layerOf(const QJsonObject &entry)
{
    Dialect::Layer layer;
    layer.profile = Dialect::profileNamed(entry.value(QStringLiteral("profile")).toString());
    const QJsonObject extensions = entry.value(QStringLiteral("extensions")).toObject();
    const QStringList known = md::Options::extensionNames();
    for (auto it = extensions.begin(); it != extensions.end(); ++it)
        if (known.contains(it.key()) && it.value().isBool()) layer.extensions.append({it.key(), it.value().toBool()});
    const QString root = entry.value(QStringLiteral("libraryRoot")).toString();
    if (root.startsWith(u'/')) layer.libraryRoot = root;
    return layer;
}

} // namespace

bool read(const AppPaths &paths, const QString &canonicalPath, Saved *saved, QString *error)
{
    *saved = {};
    const auto registry = readObject(paths.registryFile(), error);
    if (!registry) return false;
    const auto settings = readObject(paths.settingsFile(), error);
    if (!settings) return false;
    const auto store = readObject(paths.documentSettingsFile(), error);
    if (!store) return false;

    const QJsonObject libraries = store->value(QStringLiteral("libraries")).toObject();
    QStringList folders = libraries.keys();
    for (const QJsonValue &folder : settings->value(QStringLiteral("libraryFolders")).toArray())
        if (folder.toString().startsWith(u'/')) folders.append(folder.toString());
    saved->libraryFolder = LinkTargets::libraryOf(canonicalPath, folders);
    if (!saved->libraryFolder.isEmpty()) {
        // A key may be written with a final slash.
        QJsonObject entry = libraries.value(saved->libraryFolder).toObject();
        if (entry.isEmpty()) entry = libraries.value(saved->libraryFolder + u'/').toObject();
        saved->library = layerOf(entry);
        if (saved->library.libraryRoot.isEmpty()) saved->library.libraryRoot = saved->libraryFolder;
    }

    const QString id = registry->value(canonicalPath).toString();
    if (AppPaths::isDocumentId(id))
        saved->document = layerOf(store->value(QStringLiteral("documents")).toObject().value(id).toObject());
    return true;
}

} // namespace DocumentSettings
