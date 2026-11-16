#include "exporter.h"

#include "htmlexport.h"
#include "markdownexport.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>

namespace {

// A format this build does not write yet. It is listed, so that --to takes
// its name and says why it cannot be written rather than that it is unknown.
class UnavailableExporter : public Exporter {
public:
    UnavailableExporter(QString format, QString title, QStringList suffixes)
        : m_format(std::move(format)), m_title(std::move(title)), m_suffixes(std::move(suffixes))
    {
    }
    QString format() const override { return m_format; }
    QString title() const override { return m_title; }
    QStringList suffixes() const override { return m_suffixes; }
    bool available(QString *why) const override
    {
        if (why) *why = QStringLiteral("%1 export is not available yet in this version of parch").arg(m_title);
        return false;
    }
    ExportResult write(const md::Document &, const ExportRecipe &) const override
    {
        ExportResult result;
        available(&result.error);
        return result;
    }

private:
    QString m_format;
    QString m_title;
    QStringList m_suffixes;
};

} // namespace

QJsonObject ExportRecipe::toJson() const
{
    QJsonObject given;
    for (auto it = options.cbegin(); it != options.cend(); ++it)
        given.insert(it.key(), it.value().isEmpty() ? QJsonValue(true) : QJsonValue(it.value()));
    return QJsonObject{
        {QStringLiteral("source"), source},
        {QStringLiteral("format"), format},
        {QStringLiteral("output"), output},
        {QStringLiteral("dialect"), dialect.toJson()},
        {QStringLiteral("options"), given},
    };
}

namespace Exporters {

QList<const Exporter *> all()
{
    static const MarkdownExporter markdown;
    static const HtmlExporter html;
    static const UnavailableExporter pdf(QStringLiteral("pdf"), QStringLiteral("PDF"), {QStringLiteral("pdf")});
    static const UnavailableExporter docx(QStringLiteral("docx"), QStringLiteral("DOCX"), {QStringLiteral("docx")});
    static const UnavailableExporter epub(QStringLiteral("epub"), QStringLiteral("EPUB"), {QStringLiteral("epub")});
    return {&markdown, &html, &pdf, &docx, &epub};
}

QStringList formats()
{
    QStringList names;
    for (const Exporter *exporter : all()) names.append(exporter->format());
    return names;
}

const Exporter *find(const QString &format)
{
    for (const Exporter *exporter : all())
        if (exporter->format().compare(format, Qt::CaseInsensitive) == 0) return exporter;
    return nullptr;
}

const Exporter *forFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty()) return nullptr;
    for (const Exporter *exporter : all())
        if (exporter->suffixes().contains(suffix)) return exporter;
    return nullptr;
}

bool isSource(const md::Document &document, const QString &path)
{
    // The file it would replace, through any link, or the path itself when
    // nothing is there yet.
    const QFileInfo info(path);
    const QString target = info.exists() ? info.canonicalFilePath() : QDir::cleanPath(info.absoluteFilePath());
    for (const md::SourceFile &file : document.files)
        if (!file.path.isEmpty() && file.path == target) return true;
    return false;
}

bool writeFile(const QString &path, const QByteArray &bytes, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        *error = QStringLiteral("cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

QString relativeTo(const QString &outputFolder, const QString &absolutePath)
{
    return QDir(outputFolder).relativeFilePath(absolutePath);
}

} // namespace Exporters
