#include "htmlexport.h"

#include "mdhtml.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

namespace {

struct Counts {
    int formulas = 0;
    int diagrams = 0;
    int localImages = 0;
};

void count(const md::Document &document, const md::Node &node, Counts &counts)
{
    switch (node.type) {
    case md::NodeType::MathInline:
    case md::NodeType::MathBlock: ++counts.formulas; break;
    case md::NodeType::MermaidBlock: ++counts.diagrams; break;
    case md::NodeType::Image:
        if (md::classifyDestination(document, node).kind == md::Destination::Kind::LocalFile) ++counts.localImages;
        break;
    default: break;
    }
    for (const md::Node &child : node.children) count(document, child, counts);
}

QString styleSheet()
{
    QFile css(QStringLiteral(":/export.css"));
    return css.open(QIODevice::ReadOnly) ? QString::fromUtf8(css.readAll()) : QString();
}

QString frontMatterText(const md::Document &document, const char *key)
{
    return document.frontMatter().scalar(QString::fromLatin1(key)).value_or(QString()).simplified();
}

} // namespace

QList<ExportOption> HtmlExporter::options() const
{
    return {{QStringLiteral("title"),
             QStringLiteral("HTML: the page's title (the default is the front matter's title, the first heading, "
                            "or the file's name)"),
             QStringLiteral("TEXT")}};
}

std::function<QString(const md::Node &, const QString &)> HtmlExporter::destinationsFrom(const md::Document &document,
                                                                                        const QString &folder)
{
    return [&document, folder = QDir::cleanPath(folder)](const md::Node &node, const QString &dest) -> QString {
        if (dest.isEmpty() || dest.startsWith(u'/') || dest.startsWith(u'#')) return dest;
        if (node.file < 0 || size_t(node.file) >= document.files.size()) return dest;
        const QString &directory = document.files[size_t(node.file)].directory;
        if (directory.isEmpty() || QDir::cleanPath(directory) == folder) return dest;
        const md::Destination target = md::classifyDestination(dest, directory);
        if (target.kind != md::Destination::Kind::LocalFile || !target.absolute) return dest;
        QString url = QString::fromUtf8(QUrl::toPercentEncoding(Exporters::relativeTo(folder, target.path), "/"));
        if (!target.fragment.isEmpty()) url += u'#' + target.fragment;
        return url;
    };
}

QString HtmlExporter::page(const md::Document &document, const ExportRecipe &recipe, QStringList *limitations)
{
    md::HtmlOptions options;
    options.destination = destinationsFrom(document, QFileInfo(recipe.output).absolutePath());
    const QString body = md::toHtml(document, options);

    QString title = recipe.options.value(QStringLiteral("title")).simplified();
    if (title.isEmpty()) title = frontMatterText(document, "title");
    if (title.isEmpty() && !document.headings.empty()) title = document.headings.front().text.simplified();
    if (title.isEmpty()) title = QFileInfo(recipe.source).completeBaseName();
    QString language = frontMatterText(document, "lang");
    if (language.isEmpty()) language = frontMatterText(document, "language");
    const QString author = frontMatterText(document, "author");
    const QString description = frontMatterText(document, "description");

    QString page;
    page += QStringLiteral("<!DOCTYPE html>\n");
    page += language.isEmpty() ? QStringLiteral("<html>\n")
                               : QStringLiteral("<html lang=\"%1\">\n").arg(md::escapeHtml(language));
    page += QStringLiteral("<head>\n<meta charset=\"utf-8\">\n");
    page += QStringLiteral("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    page += QStringLiteral("<meta name=\"generator\" content=\"parch %1\">\n").arg(QStringLiteral(PARCH_VERSION));
    if (!author.isEmpty()) page += QStringLiteral("<meta name=\"author\" content=\"%1\">\n").arg(md::escapeHtml(author));
    if (!description.isEmpty())
        page += QStringLiteral("<meta name=\"description\" content=\"%1\">\n").arg(md::escapeHtml(description));
    page += QStringLiteral("<title>%1</title>\n").arg(md::escapeHtml(title));
    page += QStringLiteral("<style>\n") + styleSheet() + QStringLiteral("</style>\n</head>\n<body>\n<main>\n");
    page += body;
    page += QStringLiteral("</main>\n</body>\n</html>\n");

    Counts counts;
    count(document, document.root, counts);
    if (counts.formulas)
        limitations->append(QStringLiteral("Math is written as its TeX source (%1 %2)")
                                .arg(counts.formulas)
                                .arg(counts.formulas == 1 ? QStringLiteral("formula") : QStringLiteral("formulas")));
    if (counts.diagrams)
        limitations->append(QStringLiteral("Mermaid diagrams are written as their source (%1 %2)")
                                .arg(counts.diagrams)
                                .arg(counts.diagrams == 1 ? QStringLiteral("diagram") : QStringLiteral("diagrams")));
    if (counts.localImages)
        limitations->append(QStringLiteral("Images are linked from the page, not inside it (%1 %2): keep them where "
                                           "the page finds them")
                                .arg(counts.localImages)
                                .arg(counts.localImages == 1 ? QStringLiteral("image") : QStringLiteral("images")));
    return page;
}

ExportResult HtmlExporter::write(const md::Document &document, const ExportRecipe &recipe) const
{
    ExportResult result;
    const QString html = page(document, recipe, &result.limitations);
    result.renderers.insert(QStringLiteral("html"), QStringLiteral("parch " PARCH_VERSION));
    if (!Exporters::writeFile(recipe.output, html.toUtf8(), &result.error)) {
        result.ioFailure = true;
        return result;
    }
    result.written.append(recipe.output);
    result.ok = true;
    return result;
}
