#include "pdfexport.h"

#include "exportpictures.h"
#include "pdfoutline.h"
#include "svgpaper.h"

#include <QBuffer>
#include <QFileInfo>
#include <QPainter>
#include <QPdfWriter>

namespace {

struct Counts {
    int formulas = 0;
    int diagrams = 0;
    int remoteImages = 0;
};

void count(const md::Document &document, const md::Node &node, Counts &counts)
{
    switch (node.type) {
    case md::NodeType::MathInline:
    case md::NodeType::MathBlock: ++counts.formulas; break;
    case md::NodeType::MermaidBlock: ++counts.diagrams; break;
    case md::NodeType::Image:
        if (md::classifyDestination(document, node).kind == md::Destination::Kind::Remote) ++counts.remoteImages;
        break;
    default: break;
    }
    for (const md::Node &child : node.children) count(document, child, counts);
}

bool missing(const QString &helper) { return helper.isEmpty() || !QFileInfo(helper).isExecutable(); }

QString formulas(int n) { return Exporters::count(n, QStringLiteral("formula"), QStringLiteral("formulas")); }
QString diagrams(int n) { return Exporters::count(n, QStringLiteral("diagram"), QStringLiteral("diagrams")); }

} // namespace

QList<ExportOption> PdfExporter::options() const
{
    ExportOption title{QStringLiteral("title"),
                       QStringLiteral("The document's title (the default is the front matter's title, the first "
                                      "heading, or the file's name)"),
                       QStringLiteral("TEXT")};
    title.label = QStringLiteral("Title");
    ExportOption size{QStringLiteral("page-size"),
                      QStringLiteral("The paper, %1 (the default is A4)").arg(PaperOptions::pageSizes().join(QStringLiteral(", "))),
                      QStringLiteral("SIZE")};
    size.label = QStringLiteral("Page size");
    size.choices = PaperOptions::pageSizes();
    size.defaultValue = QStringLiteral("A4");
    ExportOption landscape{QStringLiteral("landscape"), QStringLiteral("Pages wider than they are tall"), QString()};
    landscape.label = QStringLiteral("Landscape");
    ExportOption margins{QStringLiteral("margins"),
                         QStringLiteral("The margin on every side, in millimetres from 5 to 60 (the default is the template's, 20)"),
                         QStringLiteral("MM")};
    margins.label = QStringLiteral("Margins (mm)");
    margins.number = true;
    margins.minimum = 5;
    margins.maximum = 60;
    margins.defaultValue = QStringLiteral("20");
    ExportOption font{QStringLiteral("font"),
                      QStringLiteral("The family the text is set in (the default is a face made for reading)"),
                      QStringLiteral("FAMILY")};
    font.label = QStringLiteral("Font");
    ExportOption fontSize{QStringLiteral("font-size"),
                          QStringLiteral("The size of the text in points, from 6 to 36 (the default is the template's, 11)"),
                          QStringLiteral("PT")};
    fontSize.label = QStringLiteral("Text size (pt)");
    fontSize.number = true;
    fontSize.minimum = 6;
    fontSize.maximum = 36;
    fontSize.defaultValue = QStringLiteral("11");
    ExportOption numbers{QStringLiteral("no-page-numbers"), QStringLiteral("Leave the pages unnumbered"), QString()};
    numbers.label = QStringLiteral("Leave the pages unnumbered");
    ExportOption look{QStringLiteral("template"),
                      QStringLiteral("How the pages are set: classic (a serif face, 11 points), modern (sans-serif, 10 "
                                     "points) or compact (a serif face, 10 points, close lines, 15 mm margins); the font, its "
                                     "size and the margins chosen go over it (the default is classic)"),
                      QStringLiteral("NAME")};
    look.label = QStringLiteral("Template");
    look.choices = PaperOptions::templates();
    look.defaultValue = QStringLiteral("classic");
    return {title, look, size, landscape, margins, font, fontSize, numbers};
}

QStringList PdfExporter::limitations(const md::Document &document, const ExportRecipe &recipe,
                                     const HelperSet &helpers) const
{
    Q_UNUSED(recipe);
    Counts counts;
    count(document, document.root, counts);
    QStringList limitations;
    if (counts.formulas && missing(helpers.ratex))
        limitations.append(QStringLiteral("Math is printed as its TeX source (%1): the math helper (render-svg) is "
                                          "not installed")
                               .arg(formulas(counts.formulas)));
    if (counts.diagrams && missing(helpers.merman))
        limitations.append(QStringLiteral("Mermaid diagrams are printed as their source (%1): the diagram helper "
                                          "(merman-cli) is not installed")
                               .arg(diagrams(counts.diagrams)));
    if (counts.diagrams && !missing(helpers.merman))
        limitations.append(QStringLiteral("Diagram labels are drawn as shapes, so a reader cannot search or select "
                                          "them (%1)")
                               .arg(diagrams(counts.diagrams)));
    if (counts.remoteImages)
        limitations.append(QStringLiteral("Images from the web are not fetched, so their descriptions stand in their "
                                          "place (%1)")
                               .arg(Exporters::count(counts.remoteImages, QStringLiteral("image"), QStringLiteral("images"))));
    return limitations;
}

std::unique_ptr<PdfExporter::Pages> PdfExporter::setPages(const md::Document &document, const ExportRecipe &recipe,
                                                          const ExportRun &run, ExportResult *result) const
{
    QString why;
    const PaperOptions paper = PaperOptions::from(*this, recipe, &why);
    if (!why.isEmpty()) {
        result->error = why;
        return nullptr;
    }
    bool stopped = false;
    auto pages = std::make_unique<Pages>();
    const qreal fontSize = paper.fontSize * 96.0 / 72.0;
    pages->pictures = Exporters::drawPictures(document, run, fontSize, 0.6, &stopped);
    if (stopped) {
        *result = Exporters::cancelled();
        return nullptr;
    }
    run.report(0.6, QStringLiteral("Laying out the pages"));
    pages->buffer.open(QIODevice::WriteOnly);
    pages->writer = std::make_unique<QPdfWriter>(&pages->buffer);
    QPdfWriter &writer = *pages->writer;
    writer.setResolution(96);
    writer.setPageLayout(QPageLayout(paper.pageSize, paper.orientation, QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter));
    writer.setTitle(Exporters::title(document, recipe));
    writer.setAuthor(Exporters::frontMatterText(document, QStringLiteral("author")));
    writer.setCreator(QStringLiteral("parch " PARCH_VERSION));
    pages->paper = std::make_unique<PaperDocument>(document, recipe.citations.get(), pages->pictures.get(), paper);
    // Laid out block by block, with its progress, and stopped when asked.
    const bool laidOut = pages->paper->layOut(&writer, [&run](qreal done) {
        if (run.stopped()) return false;
        run.report(0.6 + 0.15 * done, QStringLiteral("Laying out the pages"));
        return true;
    });
    if (!laidOut || run.stopped()) {
        *result = Exporters::cancelled();
        return nullptr;
    }
    return pages;
}

QByteArray PdfExporter::render(const md::Document &document, const ExportRecipe &recipe, const ExportRun &run,
                               ExportResult *result) const
{
    const std::unique_ptr<Pages> set = setPages(document, recipe, run, result);
    if (!set) return {};
    QPdfWriter &writer = *set->writer;
    QBuffer &buffer = set->buffer;
    PaperDocument &sheets = *set->paper;
    const std::unique_ptr<ExportPictures> &pictures = set->pictures;
    const int pages = sheets.pageCount();
    {
        QPainter painter;
        if (!painter.begin(&writer)) {
            result->error = QStringLiteral("the PDF could not be started");
            return {};
        }
        for (int page = 0; page < pages; ++page) {
            if (run.stopped()) {
                painter.end();
                *result = Exporters::cancelled();
                return {};
            }
            if (page > 0) writer.newPage();
            sheets.paint(painter, page);
            run.report(0.75 + 0.2 * (page + 1) / pages, QStringLiteral("Setting the pages"));
        }
        painter.end();
    }
    buffer.close();
    QByteArray pdf = buffer.data();

    QList<PdfOutline::Entry> outline;
    for (const PaperDocument::Heading &heading : sheets.headings())
        outline.append({heading.level, heading.text, heading.page, heading.top * 72.0 / 96.0});
    QString outlineError;
    if (!PdfOutline::add(pdf, outline, &outlineError))
        result->limitations.append(QStringLiteral("The PDF has no outline of its headings: %1").arg(outlineError));

    if (pictures) {
        const ExportPictures::Counts drawn = pictures->counts(document);
        if (drawn.formulasNotDrawn)
            result->limitations.append(
                drawn.formulasNotDrawn == drawn.formulas && !pictures->missingMath().isEmpty()
                    ? QStringLiteral("Math is printed as its TeX source (%1): %2").arg(formulas(drawn.formulasNotDrawn), pictures->missingMath())
                    : QStringLiteral("%1 could not be drawn and %2 printed as TeX source")
                          .arg(formulas(drawn.formulasNotDrawn), drawn.formulasNotDrawn == 1 ? QStringLiteral("is") : QStringLiteral("are")));
        if (drawn.diagramsNotDrawn)
            result->limitations.append(
                drawn.diagramsNotDrawn == drawn.diagrams && !pictures->missingDiagrams().isEmpty()
                    ? QStringLiteral("Mermaid diagrams are printed as their source (%1): %2")
                          .arg(diagrams(drawn.diagramsNotDrawn), pictures->missingDiagrams())
                    : QStringLiteral("%1 could not be drawn and %2 printed as source")
                          .arg(diagrams(drawn.diagramsNotDrawn), drawn.diagramsNotDrawn == 1 ? QStringLiteral("is") : QStringLiteral("are")));
        if (drawn.diagramsAsPictures)
            result->limitations.append(QStringLiteral("%1 printed as pictures at four times the size, not as vectors: "
                                                      "Qt cannot draw their SVG as Merman does")
                                           .arg(drawn.diagramsAsPictures == 1 ? QStringLiteral("1 diagram is")
                                                                              : QStringLiteral("%1 diagrams are").arg(drawn.diagramsAsPictures)));
        if (const int vectors = drawn.diagrams - drawn.diagramsNotDrawn - drawn.diagramsAsPictures; vectors > 0)
            result->limitations.append(QStringLiteral("Diagram labels are drawn as shapes, so a reader cannot search or "
                                                      "select them (%1)")
                                           .arg(diagrams(vectors)));
        result->helpers = pictures->helperReport();
        result->renderers.insert(QStringLiteral("svg"), SvgPaper::version());
    } else {
        Counts counts;
        count(document, document.root, counts);
        if (counts.formulas)
            result->limitations.append(QStringLiteral("Math is printed as its TeX source (%1): formulas are not drawn")
                                           .arg(formulas(counts.formulas)));
        if (counts.diagrams)
            result->limitations.append(QStringLiteral("Mermaid diagrams are printed as their source (%1): diagrams are not drawn")
                                           .arg(diagrams(counts.diagrams)));
    }
    result->limitations += sheets.limitations();
    result->renderers.insert(QStringLiteral("pdf"), QStringLiteral("parch %1, Qt %2").arg(QStringLiteral(PARCH_VERSION), QString::fromLatin1(qVersion())));
    return pdf;
}

ExportResult PdfExporter::write(const md::Document &document, const ExportRecipe &recipe, const ExportRun &run) const
{
    ExportResult result;
    const QByteArray pdf = render(document, recipe, run, &result);
    if (pdf.isEmpty()) {
        if (result.error.isEmpty()) result.error = QStringLiteral("the PDF could not be made");
        return result;
    }
    if (run.stopped()) return Exporters::cancelled();
    if (!Exporters::writeFile(recipe.output, pdf, &result.error, recipe.replaceLink)) {
        result.ioFailure = true;
        return result;
    }
    run.report(1, QStringLiteral("Written"));
    result.written.append(recipe.output);
    result.ok = true;
    return result;
}
