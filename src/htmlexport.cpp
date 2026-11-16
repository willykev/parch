#include "htmlexport.h"

#include "mdhtml.h"
#include "svgpaper.h"

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

ExportWeb::Assets::Mode modeOf(const QString &images)
{
    if (images == u"embed") return ExportWeb::Assets::Mode::Embed;
    if (images == u"link") return ExportWeb::Assets::Mode::Link;
    return ExportWeb::Assets::Mode::Copy;
}

QString pixels(qreal value) { return QString::number(qRound(value)); }

} // namespace

namespace ExportWeb {

Body body(const md::Document &document, const ExportRecipe &recipe, const ExportPictures *pictures, Assets &assets,
          bool book)
{
    const bool remoteImagesAllowed = !book;
    Body result;
    const QString folder = QFileInfo(recipe.output).absolutePath();
    md::HtmlOptions options;
    options.destination = HtmlExporter::destinationsFrom(document, folder);
    if (recipe.citations) options.citations = DocumentCitations::html(*recipe.citations);
    const qreal fontSize = pictures ? pictures->options().fontSize : 16;
    options.picture = [&](const md::Node &node) -> QString {
        switch (node.type) {
        case md::NodeType::MathInline:
        case md::NodeType::MathBlock: {
            const bool display = node.type == md::NodeType::MathBlock || node.display;
            const ExportPictures::Picture *drawn = pictures ? pictures->formula(node.text, display) : nullptr;
            return drawn ? formula(*drawn, node.text, display, fontSize) : QString();
        }
        case md::NodeType::MermaidBlock: {
            const ExportPictures::Picture *drawn = pictures ? pictures->diagram(node.text) : nullptr;
            if (!drawn || !drawn->drawn) return {};
            // A browser draws Merman's own SVG; a reader may not, so a book
            // gets it plain, shapes and outlines alone.
            const QByteArray svg = !book ? diagramSvg(*drawn)
                                   : drawn->vector() ? drawn->svg
                                                     : SvgPaper::plain(drawn->markup);
            const QString url = !svg.isEmpty() ? assets.bytes(svg, QStringLiteral("image/svg+xml"), QStringLiteral("diagram.svg"))
                                               : assets.bytes(drawn->png, QStringLiteral("image/png"), QStringLiteral("diagram.png"));
            return QStringLiteral("<figure class=\"diagram\"><img src=\"%1\" alt=\"%2\" width=\"%3\" height=\"%4\" /></figure>")
                .arg(md::escapeHtml(url), md::escapeHtml(drawn->description), pixels(drawn->size.width()),
                     pixels(drawn->size.height()));
        }
        case md::NodeType::Image: {
            const md::Destination target = md::classifyDestination(document, node);
            const QString alt = md::plainText(node);
            QString title;
            if (!node.title.isEmpty()) title = QStringLiteral(" title=\"%1\"").arg(md::escapeHtml(node.title));
            if (target.kind == md::Destination::Kind::Remote && !remoteImagesAllowed) {
                ++result.remoteImages;
                // A book carries every picture it shows: one from the web is
                // its description.
                return QStringLiteral("<span class=\"image-missing\">%1</span>")
                    .arg(md::escapeHtml(alt.isEmpty() ? node.dest : alt));
            }
            if (target.kind != md::Destination::Kind::LocalFile || !target.absolute) return {};
            if (assets.mode() == Assets::Mode::Link) {
                ++result.imagesLinked;
                return {};
            }
            QString error;
            const QString url = assets.file(target.path, &error);
            if (url.isEmpty()) {
                result.unreadImages.append(QStringLiteral("%1 (%2)").arg(QFileInfo(target.path).fileName(), error));
                if (!remoteImagesAllowed)
                    return QStringLiteral("<span class=\"image-missing\">%1</span>")
                        .arg(md::escapeHtml(alt.isEmpty() ? node.dest : alt));
                return {};
            }
            return QStringLiteral("<img src=\"%1\" alt=\"%2\"%3 />").arg(md::escapeHtml(url), md::escapeHtml(alt), title);
        }
        default: return {};
        }
    };
    result.html = md::toHtml(document, options);
    return result;
}

void pictureLimitations(const md::Document &document, const ExportPictures *pictures, QStringList *limitations)
{
    Counts counts;
    count(document, document.root, counts);
    if (!counts.formulas && !counts.diagrams) return;
    ExportPictures::Counts drawn;
    if (pictures) {
        drawn = pictures->counts(document);
    } else {
        drawn.formulas = drawn.formulasNotDrawn = counts.formulas;
        drawn.diagrams = drawn.diagramsNotDrawn = counts.diagrams;
    }
    const QString noMath = pictures ? pictures->missingMath() : QStringLiteral("formulas are not drawn");
    const QString noDiagrams = pictures ? pictures->missingDiagrams() : QStringLiteral("diagrams are not drawn");
    if (drawn.formulasNotDrawn) {
        limitations->append(drawn.formulasNotDrawn == drawn.formulas && !noMath.isEmpty()
                                ? QStringLiteral("Math is written as its TeX source (%1): %2")
                                      .arg(Exporters::count(drawn.formulasNotDrawn, QStringLiteral("formula"),
                                                            QStringLiteral("formulas")),
                                           noMath)
                                : QStringLiteral("%1 could not be drawn and %2 written as TeX source")
                                      .arg(Exporters::count(drawn.formulasNotDrawn, QStringLiteral("formula"),
                                                            QStringLiteral("formulas")),
                                           drawn.formulasNotDrawn == 1 ? QStringLiteral("is") : QStringLiteral("are")));
    }
    if (drawn.diagramsNotDrawn) {
        limitations->append(drawn.diagramsNotDrawn == drawn.diagrams && !noDiagrams.isEmpty()
                                ? QStringLiteral("Mermaid diagrams are written as their source (%1): %2")
                                      .arg(Exporters::count(drawn.diagramsNotDrawn, QStringLiteral("diagram"),
                                                            QStringLiteral("diagrams")),
                                           noDiagrams)
                                : QStringLiteral("%1 could not be drawn and %2 written as source")
                                      .arg(Exporters::count(drawn.diagramsNotDrawn, QStringLiteral("diagram"),
                                                            QStringLiteral("diagrams")),
                                           drawn.diagramsNotDrawn == 1 ? QStringLiteral("is") : QStringLiteral("are")));
    }
}

} // namespace ExportWeb

QList<ExportOption> HtmlExporter::options() const
{
    ExportOption title{QStringLiteral("title"),
                       QStringLiteral("The document's title (the default is the front matter's title, the first "
                                      "heading, or the file's name)"),
                       QStringLiteral("TEXT")};
    title.label = QStringLiteral("Title");
    ExportOption images{QStringLiteral("images"),
                        QStringLiteral("Copy local images into a folder beside the page (NAME_files), embed them "
                                       "in it, or link them where they are: copy, embed or link (the default is copy)"),
                        QStringLiteral("HOW")};
    images.label = QStringLiteral("Images");
    images.choices = {QStringLiteral("copy"), QStringLiteral("embed"), QStringLiteral("link")};
    images.defaultValue = QStringLiteral("copy");
    return {title, images};
}

QString HtmlExporter::assetFolder(const QString &output)
{
    const QFileInfo info(output);
    return info.absolutePath() + u'/' + info.completeBaseName() + QStringLiteral("_files");
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

QStringList HtmlExporter::limitations(const md::Document &document, const ExportRecipe &recipe,
                                      const HelperSet &helpers) const
{
    QStringList limitations;
    Counts counts;
    count(document, document.root, counts);
    const auto missing = [](const QString &path) { return path.isEmpty() || !QFileInfo(path).isExecutable(); };
    if (counts.formulas && missing(helpers.ratex))
        limitations.append(QStringLiteral("Math is written as its TeX source (%1): the math helper (render-svg) is "
                                          "not installed")
                               .arg(Exporters::count(counts.formulas, QStringLiteral("formula"), QStringLiteral("formulas"))));
    if (counts.diagrams && missing(helpers.merman))
        limitations.append(QStringLiteral("Mermaid diagrams are written as their source (%1): the diagram helper "
                                          "(merman-cli) is not installed")
                               .arg(Exporters::count(counts.diagrams, QStringLiteral("diagram"), QStringLiteral("diagrams"))));
    if (counts.localImages && option(recipe, QStringLiteral("images")) == u"link")
        limitations.append(QStringLiteral("Images are linked from the page, not inside it (%1): keep them where "
                                          "the page finds them")
                               .arg(Exporters::count(counts.localImages, QStringLiteral("image"), QStringLiteral("images"))));
    return limitations;
}

ExportResult HtmlExporter::write(const md::Document &document, const ExportRecipe &recipe, const ExportRun &run) const
{
    bool stopped = false;
    const std::unique_ptr<ExportPictures> pictures = Exporters::drawPictures(document, run, 18, 0.8, &stopped);
    if (stopped) return Exporters::cancelled();
    run.report(0.85, QStringLiteral("Writing the page"));

    ExportResult result;
    const QString folder = assetFolder(recipe.output);
    const QString images = option(recipe, QStringLiteral("images"));
    ExportWeb::Assets assets(modeOf(images), QFileInfo(folder).fileName() + u'/', QFileInfo(recipe.output).absolutePath());
    const ExportWeb::Body body = ExportWeb::body(document, recipe, pictures.get(), assets, false);

    const QString title = Exporters::title(document, recipe);
    const QString language = Exporters::language(document);
    const QString author = Exporters::frontMatterText(document, QStringLiteral("author"));
    const QString description = Exporters::frontMatterText(document, QStringLiteral("description"));
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
    page += body.html;
    page += QStringLiteral("</main>\n</body>\n</html>\n");

    ExportWeb::pictureLimitations(document, pictures.get(), &result.limitations);
    if (body.imagesLinked)
        result.limitations.append(QStringLiteral("Images are linked from the page, not inside it (%1): keep them where "
                                                 "the page finds them")
                                      .arg(Exporters::count(body.imagesLinked, QStringLiteral("image"),
                                                            QStringLiteral("images"))));
    if (!body.unreadImages.isEmpty())
        result.limitations.append(QStringLiteral("Images that could not be read are linked where they were: %1")
                                      .arg(body.unreadImages.join(QStringLiteral(", "))));
    result.renderers.insert(QStringLiteral("html"), QStringLiteral("parch " PARCH_VERSION));
    if (pictures) {
        result.renderers.insert(QStringLiteral("svg"), SvgPaper::version());
        result.helpers = pictures->helperReport();
    }
    if (run.stopped()) return Exporters::cancelled();

    // The page is held back until its pictures are in place, and nothing
    // is written through a link, the folder of pictures included.
    Exporters::PendingFile file(recipe.output, recipe.replaceLink);
    if (!file.write(page.toUtf8(), &result.error)) {
        result.ioFailure = true;
        return result;
    }
    if (assets.mode() == ExportWeb::Assets::Mode::Copy && !assets.assets().isEmpty()) {
        if (Exporters::isSource(document, folder) || !ExportWeb::replaceFolder(folder, assets.assets(), &result.error)) {
            if (result.error.isEmpty()) result.error = QStringLiteral("%1 is one of the document's own files").arg(folder);
            result.ioFailure = true;
            return result;
        }
    }
    if (!file.commit(&result.error)) {
        result.ioFailure = true;
        return result;
    }
    result.written.append(recipe.output);
    for (const ExportWeb::Assets::Asset &asset : assets.assets())
        if (assets.mode() == ExportWeb::Assets::Mode::Copy) result.written.append(folder + u'/' + asset.name);
    result.ok = true;
    return result;
}
