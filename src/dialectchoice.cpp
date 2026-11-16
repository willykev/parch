#include "dialectchoice.h"

#include "citations.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace Dialect {

std::optional<md::Profile> profileNamed(const QString &name)
{
    if (name.compare(QLatin1String("writer"), Qt::CaseInsensitive) == 0) return md::Profile::Writer;
    if (name.compare(QLatin1String("standard"), Qt::CaseInsensitive) == 0) return md::Profile::Standard;
    return std::nullopt;
}

QStringList profileNames()
{
    return {QStringLiteral("writer"), QStringLiteral("standard")};
}

QStringList layerNames()
{
    return {QStringLiteral("flag"),    QStringLiteral("document"),    QStringLiteral("front-matter"),
            QStringLiteral("library"), QStringLiteral("application"), QStringLiteral("default")};
}

Choice effective(const Layers &all)
{
    // Lowest first, as they apply.
    const std::pair<const Layer *, QString> layers[] = {
        {&all.application, QStringLiteral("application")},
        {&all.library, QStringLiteral("library")},
        {&all.frontMatter, QStringLiteral("front-matter")},
        {&all.document, QStringLiteral("document")},
        {&all.flags, QStringLiteral("flag")},
    };
    Choice choice;
    md::Profile profile = md::Profile::Writer;
    for (const auto &[layer, name] : layers) {
        if (layer->profile) {
            profile = *layer->profile;
            choice.profileFrom = name;
        }
        if (!layer->libraryRoot.isEmpty()) {
            choice.libraryRoot = layer->libraryRoot;
            choice.libraryRootFrom = name;
        }
        if (layer->bibliography) {
            choice.bibliography = *layer->bibliography;
            choice.bibliographyFrom = name;
        }
        if (!layer->citationStyle.isEmpty()) {
            choice.citationStyle = layer->citationStyle;
            choice.citationStyleFrom = name;
        }
    }
    choice.options = md::Options::forProfile(profile);
    const md::Options own = choice.options;
    for (const auto &[layer, name] : layers) {
        for (const auto &[extension, on] : layer->extensions) {
            if (!choice.options.setExtension(extension, on)) continue;
            if (own.extension(extension) == on) choice.extensionsFrom.remove(extension);
            else choice.extensionsFrom.insert(extension, name);
        }
    }
    choice.savedSettings = !all.document.isEmpty() || !all.library.isEmpty() || !all.application.isEmpty();
    return choice;
}

Choice choose(const Layer &flags, const Layer &document, const Layer &library)
{
    Layers layers;
    layers.flags = flags;
    layers.document = document;
    layers.library = library;
    return effective(layers);
}

QJsonObject Choice::toJson() const
{
    QJsonObject from{{QStringLiteral("profile"), profileFrom},
                     {QStringLiteral("libraryRoot"), libraryRootFrom},
                     {QStringLiteral("bibliography"), bibliographyFrom},
                     {QStringLiteral("citationStyle"), citationStyleFrom}};
    QJsonObject extensions;
    for (auto it = extensionsFrom.cbegin(); it != extensionsFrom.cend(); ++it) extensions.insert(it.key(), it.value());
    from.insert(QStringLiteral("extensions"), extensions);
    return QJsonObject{
        {QStringLiteral("version"), md::DialectVersion},
        {QStringLiteral("profile"), options.profileName()},
        {QStringLiteral("extensions"), QJsonArray::fromStringList(options.enabledExtensions())},
        {QStringLiteral("libraryRoot"), libraryRoot.isEmpty() ? QJsonValue() : QJsonValue(libraryRoot)},
        {QStringLiteral("bibliography"), QJsonArray::fromStringList(bibliography)},
        {QStringLiteral("citationStyle"), citationStyle.isEmpty() ? QJsonValue() : QJsonValue(citationStyle)},
        {QStringLiteral("savedSettings"), savedSettings},
        {QStringLiteral("from"), from},
    };
}

QString Choice::describe() const
{
    QString line = options.profileName() + QStringLiteral(" profile");
    QStringList with, without;
    // In the order the engine lists them, so the line is the same every time.
    for (const QString &name : md::Options::extensionNames()) {
        if (!extensionsFrom.contains(name)) continue;
        (options.extension(name) ? with : without).append(name);
    }
    if (!with.isEmpty()) line += QStringLiteral(", with ") + with.join(QStringLiteral(", "));
    if (!without.isEmpty()) line += QStringLiteral(", without ") + without.join(QStringLiteral(", "));
    if (!libraryRoot.isEmpty()) line += QStringLiteral(", library ") + libraryRoot;
    if (!citationStyle.isEmpty())
        line += QStringLiteral(", style ") + (isStyleFile(citationStyle) ? QFileInfo(citationStyle).fileName() : citationStyle);
    if (!bibliography.isEmpty()) {
        QStringList names;
        for (const QString &file : bibliography) names.append(QFileInfo(file).fileName());
        line += QStringLiteral(", bibliography ") + names.join(QStringLiteral(", "));
    }
    return line;
}

namespace {

QString fromFolder(const QString &path, const QString &folder)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path) || folder.isEmpty()) return path;
    return QDir::cleanPath(QDir(folder).filePath(path));
}

QString expandHome(const QString &path)
{
    if (path == u"~") return QDir::homePath();
    if (path.startsWith(QLatin1StringView("~/"))) return QDir::homePath() + path.mid(1);
    return path;
}

} // namespace

bool isStyleFile(const QString &style)
{
    return style.startsWith(u'/');
}

QString styleReference(const QString &written, const QString &folder)
{
    const QString name = written.trimmed();
    if (name.isEmpty()) return {};
    const bool looksLikeFile = name.contains(u'/') || name.endsWith(QLatin1StringView(".csl"), Qt::CaseInsensitive)
                               || name.startsWith(u'~');
    if (!looksLikeFile) {
        const QString bundled = CslCatalog::bundledId(name);
        // An unknown name is kept as written, so that reading it says why.
        return bundled.isEmpty() ? name : bundled;
    }
    const QString path = fromFolder(expandHome(name), folder);
    // `apa.csl` with no such file beside the document is the bundled APA.
    if (!QFileInfo::exists(path) && !name.contains(u'/')) {
        const QString bundled = CslCatalog::bundledId(name.chopped(4));
        if (!bundled.isEmpty()) return bundled;
    }
    return path;
}

Layer frontMatterLayer(const md::FrontMatter &frontMatter, const QString &folder)
{
    Layer layer;
    if (!frontMatter.present) return layer;
    if (const md::YamlValue *value = frontMatter.data.find(u"bibliography")) {
        QStringList files;
        if (value->isScalar()) {
            files.append(value->scalar);
        } else if (value->isList()) {
            for (const md::YamlValue &item : value->list)
                if (item.isScalar()) files.append(item.scalar);
        }
        QStringList paths;
        for (const QString &file : std::as_const(files)) {
            const QString trimmed = file.trimmed();
            if (!trimmed.isEmpty()) paths.append(fromFolder(expandHome(trimmed), folder));
        }
        if (!paths.isEmpty()) layer.bibliography = paths;
    }
    if (const std::optional<QString> style = frontMatter.scalar(u"csl"))
        layer.citationStyle = styleReference(*style, folder);
    return layer;
}

bool linkCitations(const md::FrontMatter &frontMatter)
{
    const std::optional<QString> value = frontMatter.scalar(u"link-citations");
    if (!value) return false;
    const QString word = value->trimmed().toLower();
    return word == u"true" || word == u"yes" || word == u"on";
}

} // namespace Dialect
