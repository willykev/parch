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
        if (layer->maxIncludeDepth) {
            choice.maxIncludeDepth = *layer->maxIncludeDepth;
            choice.maxIncludeDepthFrom = name;
        }
        if (layer->maxIncludeSize) {
            choice.maxIncludeSize = *layer->maxIncludeSize;
            choice.maxIncludeSizeFrom = name;
        }
        if (layer->maxIncludes) {
            choice.maxIncludes = *layer->maxIncludes;
            choice.maxIncludesFrom = name;
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

void Choice::limit(md::Source &source) const
{
    source.maxIncludeDepth = maxIncludeDepth;
    source.maxExpandedBytes = maxIncludeSize;
    source.maxIncludeCount = maxIncludes;
}

QJsonObject Choice::toJson() const
{
    QJsonObject from{{QStringLiteral("profile"), profileFrom},
                     {QStringLiteral("libraryRoot"), libraryRootFrom},
                     {QStringLiteral("bibliography"), bibliographyFrom},
                     {QStringLiteral("citationStyle"), citationStyleFrom},
                     {QStringLiteral("maxIncludeDepth"), maxIncludeDepthFrom},
                     {QStringLiteral("maxIncludeSize"), maxIncludeSizeFrom},
                     {QStringLiteral("maxIncludes"), maxIncludesFrom}};
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
        {QStringLiteral("maxIncludeDepth"), maxIncludeDepth},
        {QStringLiteral("maxIncludeSize"), maxIncludeSize},
        {QStringLiteral("maxIncludes"), maxIncludes},
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
    // The include limits a layer chose, not the engine's own.
    if (maxIncludeDepthFrom != u"default") line += QStringLiteral(", includes at most %1 deep").arg(maxIncludeDepth);
    if (maxIncludeSizeFrom != u"default")
        line += QStringLiteral(", at most %1 with includes").arg(sizeText(maxIncludeSize));
    if (maxIncludesFrom != u"default") line += QStringLiteral(", at most %1 includes").arg(maxIncludes);
    return line;
}

QString sizeText(qint64 bytes)
{
    constexpr qint64 kib = 1024, mib = kib * 1024, gib = mib * 1024;
    if (bytes >= gib && bytes % gib == 0) return QStringLiteral("%1 GiB").arg(bytes / gib);
    if (bytes >= mib && bytes % mib == 0) return QStringLiteral("%1 MiB").arg(bytes / mib);
    if (bytes >= kib && bytes % kib == 0) return QStringLiteral("%1 KiB").arg(bytes / kib);
    return bytes == 1 ? QStringLiteral("1 byte") : QStringLiteral("%1 bytes").arg(bytes);
}

std::optional<qint64> sizeNamed(const QString &written)
{
    const QString text = written.trimmed().toLower();
    qsizetype digits = 0;
    while (digits < text.size() && text[digits] >= u'0' && text[digits] <= u'9') ++digits;
    if (digits == 0 || digits > 12) return std::nullopt;
    const qint64 number = text.left(digits).toLongLong();
    const QString unit = text.mid(digits).trimmed();
    qint64 factor = 0;
    if (unit.isEmpty() || unit == u"b") factor = 1;
    else if (unit == u"k" || unit == u"kb" || unit == u"kib") factor = 1024;
    else if (unit == u"m" || unit == u"mb" || unit == u"mib") factor = 1024 * 1024;
    else if (unit == u"g" || unit == u"gb" || unit == u"gib") factor = 1024ll * 1024 * 1024;
    if (factor == 0 || number <= 0) return std::nullopt;
    return number * factor;
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

QString bibliographyReference(const QString &written, const QString &folder)
{
    return fromFolder(expandHome(written.trimmed()), folder);
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
            const QString path = bibliographyReference(file, folder);
            if (!path.isEmpty()) paths.append(path);
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
