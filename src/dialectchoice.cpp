#include "dialectchoice.h"

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

Choice choose(const Layer &flags, const Layer &document, const Layer &library)
{
    // Lowest first, as they apply.
    const std::pair<const Layer *, QString> layers[] = {
        {&library, QStringLiteral("library")},
        {&document, QStringLiteral("document")},
        {&flags, QStringLiteral("flag")},
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
    choice.savedSettings = !document.isEmpty() || !library.isEmpty();
    return choice;
}

QJsonObject Choice::toJson() const
{
    QJsonObject from{{QStringLiteral("profile"), profileFrom}, {QStringLiteral("libraryRoot"), libraryRootFrom}};
    QJsonObject extensions;
    for (auto it = extensionsFrom.cbegin(); it != extensionsFrom.cend(); ++it) extensions.insert(it.key(), it.value());
    from.insert(QStringLiteral("extensions"), extensions);
    return QJsonObject{
        {QStringLiteral("version"), md::DialectVersion},
        {QStringLiteral("profile"), options.profileName()},
        {QStringLiteral("extensions"), QJsonArray::fromStringList(options.enabledExtensions())},
        {QStringLiteral("libraryRoot"), libraryRoot.isEmpty() ? QJsonValue() : QJsonValue(libraryRoot)},
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
    return line;
}

} // namespace Dialect
