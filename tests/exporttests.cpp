#include "exporttests.h"

#include "backend.h"
#include "cli.h"
#include "clicommands.h"
#include "codehighlight.h"
#include "commandregistry.h"
#include "document.h"
#include "documentcheck.h"
#include "exporter.h"
#include "exportpictures.h"
#include "exports.h"
#include "filepicker.h"
#include "paperdocument.h"
#include "pdfexport.h"
#include "pdfoutline.h"
#include "previewstyle.h"
#include "printportal.h"
#include "printpreview.h"
#include "rendertesting.h"
#include "svgpaper.h"
#include "testfiles.h"
#include "windowcontext.h"
#include "windowtesting.h"
#include "zippackage.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTimer>
#include <QXmlStreamReader>

#include <thread>

namespace ExportTests {
namespace {

QString canonical(const QString &path) { return QFileInfo(path).canonicalFilePath(); }

// The rich fixture and its square picture, in a folder.
QString writeRich(const QString &folder)
{
    writeFile(folder + QStringLiteral("/rich.md"), readFile(QStringLiteral(PARCH_SOURCE_DIR "/tests/fixtures/export/rich.md")));
    QImage square(64, 64, QImage::Format_ARGB32);
    square.fill(Qt::darkRed);
    square.save(folder + QStringLiteral("/square.png"));
    return canonical(folder + QStringLiteral("/rich.md"));
}

DocumentCheck::Loaded load(const QString &path)
{
    DocumentCheck::Loaded loaded;
    QString error;
    if (!DocumentCheck::load(path, Dialect::choose({}), &loaded, &error)) qWarning() << error;
    return loaded;
}

ExportRecipe recipeFor(const DocumentCheck::Loaded &loaded, const QString &format, const QString &output,
                       const QHash<QString, QString> &options = {})
{
    ExportRecipe recipe;
    recipe.source = loaded.path;
    recipe.format = format;
    recipe.output = output;
    recipe.dialect = loaded.dialect;
    recipe.options = options;
    recipe.citations = std::make_shared<const DocumentCitations::Formatted>(loaded.formatted);
    return recipe;
}

// Stand-in helpers of their own beneath `folder`, and their cache.
RenderService::Options fakeHelpers(const QString &folder)
{
    QDir().mkpath(folder + QStringLiteral("/helpers"));
    writeFakeHelpers(folder + QStringLiteral("/helpers"), folder + QStringLiteral("/helpers.log"));
    return renderOptions(HelperSet::in(folder + QStringLiteral("/helpers")), folder);
}

ExportRun fakeRun(const QString &folder)
{
    ExportRun run;
    run.render = fakeHelpers(folder);
    return run;
}

// No helpers at all: formulas and diagrams stay their source.
ExportRun bareRun()
{
    ExportRun run;
    run.render.findHelpers = false;
    run.render.helpers = {};
    run.render.diskCache = false;
    return run;
}

QString tool(const char *name) { return QStandardPaths::findExecutable(QString::fromLatin1(name)); }

// A page's text as Poppler extracts it; every page's for 0.
QString pdfText(const QString &path, int page = 0)
{
    QProcess process;
    QStringList arguments;
    if (page > 0) arguments << QStringLiteral("-f") << QString::number(page) << QStringLiteral("-l") << QString::number(page);
    arguments << path << QStringLiteral("-");
    process.start(tool("pdftotext"), arguments);
    process.waitForFinished(30000);
    return QString::fromUtf8(process.readAllStandardOutput());
}

int pagesIn(const QByteArray &pdf)
{
    static const QRegularExpression page(QStringLiteral("/Type\\s*/Page[^s]"));
    return int(QString::fromLatin1(pdf).count(page));
}

QSizeF mediaBox(const QByteArray &pdf)
{
    static const QRegularExpression box(QStringLiteral("/MediaBox\\s*\\[\\s*0\\s+0\\s+([\\d.]+)\\s+([\\d.]+)\\s*\\]"));
    const QRegularExpressionMatch match = box.match(QString::fromLatin1(pdf));
    return match.hasMatch() ? QSizeF(match.captured(1).toDouble(), match.captured(2).toDouble()) : QSizeF();
}

QList<ZipPackage::Entry> entriesOf(const QString &path)
{
    QList<ZipPackage::Entry> entries;
    QString error;
    if (!ZipPackage::read(readFile(path), &entries, &error)) qWarning() << error;
    return entries;
}

QByteArray entry(const QList<ZipPackage::Entry> &entries, const QString &name)
{
    for (const ZipPackage::Entry &each : entries)
        if (each.name == name) return each.data;
    return {};
}

QStringList names(const QList<ZipPackage::Entry> &entries)
{
    QStringList list;
    for (const ZipPackage::Entry &each : entries) list.append(each.name);
    return list;
}

QString xmlProblem(const QByteArray &xml)
{
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) reader.readNext();
    return reader.hasError() ? reader.errorString() + QStringLiteral(" at line %1").arg(reader.lineNumber()) : QString();
}

// Every file beneath a folder, with its bytes.
QMap<QString, QByteArray> filesBeneath(const QString &folder)
{
    QMap<QString, QByteArray> found;
    QDirIterator it(folder, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        found.insert(QDir(folder).relativeFilePath(path), readFile(path));
    }
    return found;
}

QStringList entriesIn(const QString &folder)
{
    return QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

bool run(QQuickWindow *window, const char *command)
{
    return QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QString::fromLatin1(command)));
}

// A file dialog that remembers what it was asked and answers when told.
class ExportPicker : public FilePicker {
public:
    int saves = 0;
    QString title, filter;
    QUrl suggested;
    QStringList patterns;

    void openDocuments() override {}
    void openFolder() override {}
    void saveDocument(const QUrl &) override {}
    void saveFile(const QString &asked, const QUrl &suggestion, const QString &filterName, const QStringList &globs) override
    {
        ++saves;
        title = asked;
        suggested = suggestion;
        filter = filterName;
        patterns = globs;
    }
};

// A printer that answers as the desktop's print dialog would, without one.
class FakePrinter : public Printer {
public:
    QStringList prepared;
    QString printedTitle;
    QByteArray pdf;
    quint32 token = 0;
    QVariantMap setup{{QStringLiteral("Width"), 215.9},          {QStringLiteral("Height"), 279.4},
                      {QStringLiteral("Orientation"), QStringLiteral("landscape")}, {QStringLiteral("MarginTop"), 25.0}};
    bool refuse = false;

    void prepare(const QString &title) override
    {
        prepared.append(title);
        QTimer::singleShot(0, this, [this] {
            if (refuse) emit failed(QStringLiteral("no printer here"));
            else emit Printer::prepared(7, setup);
        });
    }
    void print(const QString &title, const QByteArray &bytes, quint32 given) override
    {
        printedTitle = title;
        pdf = bytes;
        token = given;
        QTimer::singleShot(0, this, [this] { emit printed(); });
    }
};

struct Ran {
    int code = -1;
    QString out;
    QString err;
};

Ran runCli(const QString &folder, const QStringList &arguments)
{
    QBuffer out, err;
    out.open(QIODevice::WriteOnly);
    err.open(QIODevice::WriteOnly);
    const Cli::Context context{folder, folder + QStringLiteral("/home"), AppPaths::under(folder + QStringLiteral("/app")),
                               &out, &err};
    Ran ran;
    ran.code = Cli::run(QStringList{QStringLiteral("parch")} + arguments, context);
    ran.out = QString::fromUtf8(out.data());
    ran.err = QString::fromUtf8(err.data());
    return ran;
}

// The colours a PDF fills with, as its page streams set them, each once.
QStringList fillColours(const QByteArray &pdf)
{
    static const QRegularExpression colour(QStringLiteral("(?:^|\\s)((?:[\\d.]+\\s+){3})scn\\b"));
    QStringList colours;
    qsizetype at = 0;
    while ((at = pdf.indexOf("/FlateDecode", at)) >= 0) {
        const qsizetype start = pdf.indexOf("stream\n", at);
        const qsizetype end = start < 0 ? -1 : pdf.indexOf("endstream", start);
        if (end < 0) break;
        QByteArray packed = pdf.mid(start + 7, end - start - 7);
        // qUncompress takes the size it unpacks to first; enough for any page here.
        packed.prepend(QByteArray::fromHex("01000000"));
        const QString stream = QString::fromLatin1(qUncompress(packed));
        for (auto it = colour.globalMatch(stream); it.hasNext();) {
            const QString found = it.next().captured(1).simplified();
            if (!colours.contains(found)) colours.append(found);
        }
        at = end;
    }
    colours.sort();
    return colours;
}

} // namespace

void pdfSetsPagesWithSearchableText()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString path = writeRich(folder.path());
    const DocumentCheck::Loaded loaded = load(path);
    QVERIFY(!loaded.document.incomplete);
    const Exporter *pdf = Exporters::find(QStringLiteral("pdf"));
    QVERIFY(pdf && pdf->available());
    const QString output = folder.path() + QStringLiteral("/out.pdf");
    ExportRun run = fakeRun(folder.path() + QStringLiteral("/work"));
    qreal last = -1;
    bool rising = true;
    QStringList steps;
    run.progress = [&](qreal done, const QString &step) {
        if (done < last) rising = false;
        last = done;
        steps.append(step);
    };
    const ExportResult result = pdf->write(loaded.document, recipeFor(loaded, QStringLiteral("pdf"), output), run);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.written, QStringList{output});
    QVERIFY(rising);
    QVERIFY(steps.contains(QStringLiteral("Drawing formulas and diagrams")));
    QVERIFY(steps.contains(QStringLiteral("Setting the pages")));
    QCOMPARE(last, 1.0);
    const QByteArray bytes = readFile(output);
    QVERIFY(bytes.startsWith("%PDF-"));
    // A4 by default, at 72 points to the inch.
    QCOMPARE(mediaBox(bytes).toSize(), QSize(595, 842));
    // The outline lists the headings, nested by level.
    QCOMPARE(PdfOutline::read(bytes), (QList<std::pair<int, QString>>{{1, QStringLiteral("Opening Heading")},
                                                                    {2, QStringLiteral("Lists Heading")},
                                                                    {2, QStringLiteral("Table Heading")},
                                                                    {2, QStringLiteral("Notes Heading")},
                                                                    {2, QStringLiteral("Pictures Heading")},
                                                                    {1, QStringLiteral("Closing Section")}}));
    // Links lead to the web and within the document.
    QVERIFY(bytes.contains("/URI (https://example.org/page)"));
    QVERIFY(bytes.contains("/Dests"));
    QVERIFY(bytes.contains("/Subtype /Link"));
    QVERIFY(result.renderers.value(QStringLiteral("pdf")).toString().startsWith(QStringLiteral("parch ")));
    QVERIFY(result.helpers.contains(QStringLiteral("render-svg")));
    // The stand-in diagram helper's SVG draws nothing like its picture, so
    // the picture is printed, and the limitations say so.
    QVERIFY2(result.limitations.contains(QStringLiteral("1 diagram is printed as pictures at four times the size, not as "
                                                        "vectors: Qt cannot draw their SVG as Merman does")),
             qPrintable(result.limitations.join(u'\n')));
    QVERIFY(result.limitations.contains(
        QStringLiteral("Images from the web are not fetched, so their descriptions stand in their place (1 image)")));

    // Letter, turned on its side.
    const QString letter = folder.path() + QStringLiteral("/letter.pdf");
    QVERIFY(pdf->write(loaded.document,
                       recipeFor(loaded, QStringLiteral("pdf"), letter,
                                 {{QStringLiteral("page-size"), QStringLiteral("Letter")}, {QStringLiteral("landscape"), {}}}),
                       bareRun())
                .ok);
    QCOMPARE(mediaBox(readFile(letter)).toSize(), QSize(792, 612));

    if (tool("pdftotext").isEmpty()) QSKIP("Poppler's pdftotext is not installed: the PDF's text is not read back");
    const QString all = pdfText(output);
    for (const char *text : {"A paragraph with emphasis, strong text, struck words, highlighted words",
                             "First bullet", "Nested bullet", "Done task", "Nested numbered",
                             "A quoted paragraph that stands apart.", "Term one", "The first definition.",
                             "merged across", "def greet(name):", "return \"hello \" + name",
                             "The first note's text.", "The inline note's text.", "[1, p. 23]",
                             "Doe, Jane. A Book of Examples. Example Press, 2020.", "The last paragraph of the document."})
        QVERIFY2(all.contains(QString::fromUtf8(text)), text);
    // The page break: the pictures start a page, and no line is on two pages.
    int pictures = 0, pages = 0;
    for (int page = 1; page < 20; ++page) {
        const QString text = pdfText(output, page);
        if (text.trimmed().isEmpty()) break;
        pages = page;
        if (text.trimmed().startsWith(QStringLiteral("Pictures Heading"))) pictures = page;
    }
    QVERIFY2(pictures > 1, qPrintable(all));
    QVERIFY2(pdfText(output, pictures - 1).contains(QStringLiteral("here.")),
             qPrintable(QStringLiteral("page %1 of %2: %3").arg(pictures - 1).arg(pages).arg(pdfText(output, pictures - 1))));
    QVERIFY(pages >= pictures);
    QCOMPARE(all.count(QStringLiteral("A sentence with a note.")), 1);
    QCOMPARE(all.count(QStringLiteral("The last paragraph of the document.")), 1);
    if (!tool("qpdf").isEmpty()) {
        QProcess check;
        check.start(tool("qpdf"), {QStringLiteral("--check"), output});
        QVERIFY(check.waitForFinished(30000));
        QVERIFY2(check.exitCode() == 0, check.readAllStandardOutput().constData());
    }
}

void pdfKeepsHeadingsWithTheirText()
{
    // However much comes before it, a heading starts no page without the
    // text it heads, and a page break starts a page.
    for (int lines = 20; lines <= 60; lines += 2) {
        QString text;
        for (int i = 0; i < lines; ++i) text += QStringLiteral("Filler line %1.\n\n").arg(i);
        text += QStringLiteral("## Kept heading\n\nThe text the heading heads.\n\n+++\n\nAfter the break.\n");
        const md::Document document = md::parse(text);
        PaperOptions options;
        options.pageSize = QPageSize(QPageSize::A5);
        PaperDocument paper(document, nullptr, nullptr, options);
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        QPdfWriter writer(&buffer);
        writer.setResolution(96);
        paper.layOut(&writer);
        const QList<PaperDocument::Heading> headings = paper.headings();
        QCOMPARE(headings.size(), 1);
        const QTextDocument *laid = paper.document();
        const auto pageOf = [&](const QString &words) { return paper.pageAt(laid->find(words).position()); };
        QVERIFY2(pageOf(QStringLiteral("The text the heading heads.")) == headings.first().page, qPrintable(QString::number(lines)));
        QCOMPARE(pageOf(QStringLiteral("After the break.")), pageOf(QStringLiteral("The text the heading heads.")) + 1);
        QCOMPARE(paper.pageCount(), pageOf(QStringLiteral("After the break.")) + 1);
    }
}

void pdfTallTableRowsRunOntoTheNextPage()
{
    // A row a few pages tall goes on from page to page, every line of it
    // drawn once, on one page.
    QString cell;
    for (int line = 1; line <= 300; ++line) cell += QStringLiteral("Row line %1<br>").arg(line);
    const md::Document document = md::parse(QStringLiteral("# Tall\n\n| Head | Side |\n|---|---|\n| %1 | short |\n| next | row |\n\n"
                                                           "After the table.\n")
                                                .arg(cell));
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString output = folder.path() + QStringLiteral("/tall.pdf");
    ExportRecipe recipe;
    recipe.format = QStringLiteral("pdf");
    recipe.output = output;
    const ExportResult result = Exporters::find(QStringLiteral("pdf"))->write(document, recipe, bareRun());
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(pagesIn(readFile(output)) >= 4);
    // What follows the table is placed after it, on its last page.
    PaperDocument paper(document, nullptr, nullptr, PaperOptions());
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QPdfWriter writer(&buffer);
    writer.setResolution(96);
    paper.layOut(&writer);
    const QTextBlock after = paper.document()->find(QStringLiteral("After the table.")).block();
    QCOMPARE(after.layout()->lineCount(), 1);
    QCOMPARE(paper.pageAt(after.position()), paper.pageCount() - 1);
    QVERIFY(paper.pageAt(paper.document()->find(QStringLiteral("Row line 300")).position()) > 2);
    if (tool("pdftotext").isEmpty()) QSKIP("Poppler's pdftotext is not installed: the PDF's text is not read back");
    const QString all = pdfText(output);
    for (int line = 1; line <= 300; ++line)
        QVERIFY2(all.count(QRegularExpression(QStringLiteral("\\bRow line %1\\b").arg(line))) == 1, qPrintable(QString::number(line)));
    QVERIFY(all.contains(QStringLiteral("short")));
    QVERIFY(all.contains(QStringLiteral("next")));
    QVERIFY(all.contains(QStringLiteral("After the table.")));
    // Each page holds its own run of lines: the first page the first.
    const QString first = pdfText(output, 1);
    QVERIFY(first.contains(QStringLiteral("Row line 1\n")) || first.contains(QStringLiteral("Row line 1 ")));
    QVERIFY(!first.contains(QStringLiteral("Row line 300")));
}

void pdfSetsALongDocumentWhole()
{
    // Qt lays a long document out a little at a time; every page of it is set.
    QString text;
    for (int chapter = 1; chapter <= 12; ++chapter) {
        text += QStringLiteral("# Chapter %1\n\n").arg(chapter);
        for (int paragraph = 1; paragraph <= 60; ++paragraph)
            text += QStringLiteral("Paragraph %1.%2 of ordinary prose that runs on long enough to wrap across the "
                                   "column at least once, and so to fill the pages it is set on.\n\n")
                        .arg(chapter)
                        .arg(paragraph);
    }
    const md::Document document = md::parse(text);
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    ExportRecipe recipe;
    recipe.format = QStringLiteral("pdf");
    recipe.output = folder.path() + QStringLiteral("/long.pdf");
    QVERIFY(Exporters::find(QStringLiteral("pdf"))->write(document, recipe, bareRun()).ok);
    const QByteArray pdf = readFile(recipe.output);
    QCOMPARE(PdfOutline::read(pdf).size(), 12);
    QVERIFY(pagesIn(pdf) > 30);
    if (tool("pdftotext").isEmpty()) QSKIP("Poppler's pdftotext is not installed: the PDF's text is not read back");
    const QString all = pdfText(recipe.output);
    QCOMPARE(all.count(QRegularExpression(QStringLiteral("Paragraph \\d+\\.\\d+ of ordinary prose"))), 12 * 60);
    QVERIFY(all.contains(QStringLiteral("Paragraph 12.60 of ordinary prose")));
}

void pdfOutlineIsAnIncrementalUpdate()
{
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    {
        QPdfWriter writer(&buffer);
        QPainter painter(&writer);
        painter.drawText(100, 100, QStringLiteral("One"));
        writer.newPage();
        painter.drawText(100, 100, QStringLiteral("Two"));
        writer.newPage();
        painter.drawText(100, 100, QStringLiteral("Three"));
    }
    const QByteArray original = buffer.data();
    QByteArray pdf = original;
    QString error;
    QVERIFY2(PdfOutline::add(pdf, {{1, QStringLiteral("Première"), 0, 10},
                                   {2, QStringLiteral("Second “quoted”"), 1, 20},
                                   {3, QStringLiteral("Deep"), 1, 400},
                                   {1, QStringLiteral("日本語"), 2, 0}},
                             &error),
             qPrintable(error));
    // What the writer wrote is there as it was, the outline after it.
    QVERIFY(pdf.startsWith(original));
    QVERIFY(pdf.size() > original.size());
    QCOMPARE(PdfOutline::read(pdf), (QList<std::pair<int, QString>>{{1, QStringLiteral("Première")},
                                                                  {2, QStringLiteral("Second “quoted”")},
                                                                  {3, QStringLiteral("Deep")},
                                                                  {1, QStringLiteral("日本語")}}));
    QVERIFY(PdfOutline::read(original).isEmpty());
    // What is not a PDF as Qt writes it is left alone.
    QByteArray other = "%PDF-1.4\nnot really\n";
    QVERIFY(!PdfOutline::add(other, {{1, QStringLiteral("x"), 0, 0}}, &error));
    QCOMPARE(other, QByteArray("%PDF-1.4\nnot really\n"));
    QVERIFY(!error.isEmpty());
    if (tool("qpdf").isEmpty()) QSKIP("qpdf is not installed: the outline is not checked by another reader");
    QTemporaryDir folder;
    writeFile(folder.path() + QStringLiteral("/outline.pdf"), pdf);
    QProcess check;
    check.start(tool("qpdf"), {QStringLiteral("--json"), QStringLiteral("--json-key=outlines"), folder.path() + QStringLiteral("/outline.pdf")});
    QVERIFY(check.waitForFinished(30000));
    QCOMPARE(check.exitCode(), 0);
    const QJsonArray outlines = QJsonDocument::fromJson(check.readAllStandardOutput()).object().value(QStringLiteral("outlines")).toArray();
    QCOMPARE(outlines.size(), 2);
    QCOMPARE(outlines[0].toObject().value(QStringLiteral("title")).toString(), QStringLiteral("Première"));
    QCOMPARE(outlines[1].toObject().value(QStringLiteral("destpageposfrom1")).toInt(), 3);
    const QJsonArray kids = outlines[0].toObject().value(QStringLiteral("kids")).toArray();
    QCOMPARE(kids.size(), 1);
    QCOMPARE(kids[0].toObject().value(QStringLiteral("destpageposfrom1")).toInt(), 2);
}

void paperFormulasSitOnTheBaseline()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const md::Document document = md::parse(QStringLiteral(
        "Text with $x$ in it, and a deeper one $\\frac{a}{b}$ too, in a line long enough to wrap onto another line "
        "where $y$ stands as well.\n\n# A heading with $z$\n"));
    ExportPictures::Options options;
    options.service = fakeHelpers(folder.path());
    options.fontSize = 11 * 96.0 / 72.0;
    ExportPictures pictures(options);
    QVERIFY(pictures.draw(document));
    QCOMPARE(pictures.counts(document).formulas, 4);
    const ExportPictures::Picture *x = pictures.formula(QStringLiteral("x"), false);
    QVERIFY(x);
    QVERIFY2(x->drawn, qPrintable(x->diagnostic));
    QCOMPARE(pictures.counts(document).formulasNotDrawn, 0);
    PaperDocument paper(document, nullptr, &pictures, PaperOptions());
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QPdfWriter writer(&buffer);
    writer.setResolution(96);
    paper.layOut(&writer);
    {
        QPainter painter(&writer);
        paper.paint(painter, 0);
    }
    const QList<PaperDocument::Placed> places = paper.formulaPlaces();
    QCOMPARE(places.size(), 4);
    for (const PaperDocument::Placed &place : places)
        QVERIFY2(std::abs(place.baseline - place.lineBaseline) <= 1.0,
                 qPrintable(QStringLiteral("%1: %2 against %3").arg(place.tex).arg(place.baseline).arg(place.lineBaseline)));
    // Drawn as vectors: the formulas are paths in the PDF, not pictures.
    QVERIFY(inspectPdf(buffer.data()).pathOperators > 0);
}

void paperDiagramsAreVectorsWhereQtDrawsThem()
{
    const HelperSet helpers = realHelpers();
    if (helpers.merman.isEmpty()) QSKIP("run bin/fetch-helpers to test them");
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    QString text;
    int valid = 0;
    for (const QJsonValue &fixture : renderFixtures(QStringLiteral("mermaid"))) {
        const QJsonObject object = fixture.toObject();
        text += QStringLiteral("```mermaid\n%1\n```\n\n").arg(object.value(QStringLiteral("source")).toString());
        if (object.value(QStringLiteral("valid")).toBool()) ++valid;
    }
    const md::Document document = md::parse(text);
    ExportPictures::Options options;
    options.service = renderOptions(helpers, folder.path());
    ExportPictures pictures(options);
    QVERIFY(pictures.draw(document));
    const ExportPictures::Counts counts = pictures.counts(document);
    QCOMPARE(counts.diagrams, int(renderFixtures(QStringLiteral("mermaid")).size()));
    QCOMPARE(counts.diagrams - counts.diagramsNotDrawn, valid);
    // librsvg makes Merman's SVG plain enough for Qt: nearly every diagram
    // is a vector drawing, and the rest are printed as Merman's pictures.
    const int vectors = counts.diagrams - counts.diagramsNotDrawn - counts.diagramsAsPictures;
    QVERIFY2(vectors >= valid - 4, qPrintable(QStringLiteral("%1 of %2").arg(vectors).arg(valid)));
    for (const md::Node &node : document.root.children) {
        if (node.type != md::NodeType::MermaidBlock) continue;
        const ExportPictures::Picture *drawn = pictures.diagram(node.text);
        QVERIFY(drawn);
        if (!drawn->drawn) continue;
        QVERIFY(!drawn->png.isEmpty());
        QVERIFY(!drawn->markup.isEmpty());
        if (drawn->vector()) QVERIFY(SvgPaper::difference(drawn->svg, QImage::fromData(drawn->png, "PNG")) <= SvgPaper::kUnlike);
    }
    QVERIFY(SvgPaper::version().startsWith(QStringLiteral("librsvg ")));
}

void docxIsWordprocessingMarkup()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString path = writeRich(folder.path());
    const DocumentCheck::Loaded loaded = load(path);
    const Exporter *docx = Exporters::find(QStringLiteral("docx"));
    QVERIFY(docx && docx->available());
    const QString output = folder.path() + QStringLiteral("/out.docx");
    const ExportResult result =
        docx->write(loaded.document, recipeFor(loaded, QStringLiteral("docx"), output), fakeRun(folder.path() + QStringLiteral("/work")));
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY2(result.limitations.contains(QStringLiteral("1 diagram and 2 formulas become pictures, described by their words "
                                                        "or TeX: Word cannot edit them as equations or diagrams")),
             qPrintable(result.limitations.join(u'\n')));
    const QList<ZipPackage::Entry> entries = entriesOf(output);
    const QStringList parts = names(entries);
    for (const char *part : {"[Content_Types].xml", "_rels/.rels", "word/document.xml", "word/_rels/document.xml.rels",
                             "word/styles.xml", "word/numbering.xml", "word/footnotes.xml", "word/endnotes.xml",
                             "word/settings.xml", "docProps/core.xml", "docProps/app.xml"})
        QVERIFY2(parts.contains(QString::fromLatin1(part)), part);
    // Every part is well-formed XML.
    for (const ZipPackage::Entry &each : entries)
        if (each.name.endsWith(QLatin1String(".xml")) || each.name.endsWith(QLatin1String(".rels")))
            QVERIFY2(xmlProblem(each.data).isEmpty(), qPrintable(each.name + u' ' + xmlProblem(each.data)));
    const QString document = QString::fromUtf8(entry(entries, QStringLiteral("word/document.xml")));
    const QString styles = QString::fromUtf8(entry(entries, QStringLiteral("word/styles.xml")));
    const QString relationships = QString::fromUtf8(entry(entries, QStringLiteral("word/_rels/document.xml.rels")));
    // Real styles.
    for (const char *style : {"Title", "Heading1", "Heading2", "Heading6", "Quote", "Code", "CodeChar", "Hyperlink",
                              "FootnoteText", "FootnoteReference", "ListParagraph", "Bibliography"})
        QVERIFY2(styles.contains(QStringLiteral("w:styleId=\"%1\"").arg(QLatin1StringView(style))), style);
    QVERIFY(document.contains(QStringLiteral("<w:pStyle w:val=\"Title\"/></w:pPr><w:r><w:t xml:space=\"preserve\">The Rich Fixture</w:t>")));
    QVERIFY(document.contains(QStringLiteral("<w:pStyle w:val=\"Heading1\"/>")));
    QVERIFY(document.contains(QStringLiteral("<w:pStyle w:val=\"Heading2\"/>")));
    QVERIFY(document.contains(QStringLiteral("<w:pStyle w:val=\"Quote\"/>")));
    QVERIFY(document.contains(QStringLiteral("<w:pStyle w:val=\"Code\"/></w:pPr><w:r><w:t xml:space=\"preserve\">def greet(name):</w:t><w:br/>")));
    // Runs.
    const auto run = [&](const char *properties, const char *text) {
        return document.contains(QStringLiteral("<w:rPr>%1</w:rPr><w:t xml:space=\"preserve\">%2</w:t>")
                                     .arg(QLatin1StringView(properties), QString::fromUtf8(text)));
    };
    QVERIFY(run("<w:i/>", "emphasis"));
    QVERIFY(run("<w:b/>", "strong text"));
    QVERIFY(run("<w:strike/>", "struck words"));
    QVERIFY(run("<w:highlight w:val=\"yellow\"/>", "highlighted words"));
    QVERIFY(run("<w:vertAlign w:val=\"superscript\"/>", "2"));
    QVERIFY(run("<w:vertAlign w:val=\"subscript\"/>", "2"));
    QVERIFY(run("<w:rStyle w:val=\"CodeChar\"/>", "inline code"));
    // Links: to the web through a relationship, to a heading through its bookmark.
    QRegularExpression web(QStringLiteral("Id=\"(rId\\d+)\" Type=\"[^\"]*/hyperlink\" Target=\"https://example.org/page\" TargetMode=\"External\""));
    const QRegularExpressionMatch link = web.match(relationships);
    QVERIFY2(link.hasMatch(), qPrintable(relationships));
    QVERIFY(document.contains(QStringLiteral("<w:hyperlink r:id=\"%1\"").arg(link.captured(1))));
    QVERIFY(document.contains(QStringLiteral("<w:hyperlink w:anchor=\"h_closing_section\"")));
    QVERIFY(document.contains(QStringLiteral("w:name=\"h_closing_section\"")));
    // Lists, with their levels and the boxes of tasks.
    QVERIFY(document.contains(QStringLiteral("<w:numPr><w:ilvl w:val=\"1\"/>")));
    QVERIFY(document.contains(QStringLiteral("\u2612 ")));
    QVERIFY(document.contains(QStringLiteral("\u2610 ")));
    const QString numbering = QString::fromUtf8(entry(entries, QStringLiteral("word/numbering.xml")));
    QVERIFY(numbering.contains(QStringLiteral("<w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%1.\"/>")));
    QVERIFY(numbering.contains(QStringLiteral("<w:numFmt w:val=\"bullet\"/>")));
    QVERIFY(numbering.contains(QStringLiteral("<w:startOverride w:val=\"1\"/>")));
    // A table with its header, alignment and merged cells.
    QVERIFY(document.contains(QStringLiteral("<w:tblHeader/>")));
    QVERIFY(document.contains(QStringLiteral("<w:gridSpan w:val=\"2\"/>")));
    QVERIFY(document.contains(QStringLiteral("<w:jc w:val=\"center\"/>")));
    QVERIFY(document.contains(QStringLiteral("<w:jc w:val=\"right\"/>")));
    // Notes as Word footnotes, a citation and its bibliography.
    QVERIFY(document.contains(QStringLiteral("<w:footnoteReference w:id=\"1\"/>")));
    QVERIFY(document.contains(QStringLiteral("<w:footnoteReference w:id=\"2\"/>")));
    const QString footnotes = QString::fromUtf8(entry(entries, QStringLiteral("word/footnotes.xml")));
    QVERIFY(footnotes.contains(QStringLiteral("The first note's text.")));
    QVERIFY(footnotes.contains(QStringLiteral("The inline note's text.")));
    QVERIFY(footnotes.contains(QStringLiteral("<w:footnoteRef/>")));
    QVERIFY(document.contains(QStringLiteral("<w:pStyle w:val=\"Bibliography\"/>")));
    QVERIFY(document.contains(QStringLiteral("Doe, Jane. ")));
    // Pictures with their descriptions; the formula in the text lowered onto the baseline.
    QVERIFY(document.contains(QStringLiteral("descr=\"A small square\"")));
    QVERIFY(document.contains(QStringLiteral("descr=\"E = mc^2\"")));
    QVERIFY(document.contains(QStringLiteral("<w:position w:val=\"-9\"/>")));
    int media = 0;
    for (const QString &name : parts)
        if (name.startsWith(QLatin1String("word/media/"))) {
            ++media;
            QVERIFY(QImage::fromData(entry(entries, name)).width() > 0);
        }
    QCOMPARE(media, 4);
    QVERIFY(document.contains(QStringLiteral("<w:br w:type=\"page\"/>")));
    // The document's properties from its front matter.
    const QString core = QString::fromUtf8(entry(entries, QStringLiteral("docProps/core.xml")));
    QVERIFY(core.contains(QStringLiteral("<dc:title>The Rich Fixture</dc:title>")));
    QVERIFY(core.contains(QStringLiteral("<dc:creator>A. Writer</dc:creator>")));
    QVERIFY(core.contains(QStringLiteral("<cp:keywords>export, fixture</cp:keywords>")));
    QVERIFY(core.contains(QStringLiteral("<dc:language>en-GB</dc:language>")));

    // Endnotes, when asked; and no helpers leaves the TeX as code.
    const QString endnotes = folder.path() + QStringLiteral("/endnotes.docx");
    const ExportResult asked = docx->write(loaded.document,
                                           recipeFor(loaded, QStringLiteral("docx"), endnotes, {{QStringLiteral("notes"), QStringLiteral("endnotes")}}),
                                           bareRun());
    QVERIFY(asked.ok);
    const QList<ZipPackage::Entry> other = entriesOf(endnotes);
    QVERIFY(QString::fromUtf8(entry(other, QStringLiteral("word/document.xml"))).contains(QStringLiteral("<w:endnoteReference w:id=\"1\"/>")));
    QVERIFY(QString::fromUtf8(entry(other, QStringLiteral("word/endnotes.xml"))).contains(QStringLiteral("The first note's text.")));
    QVERIFY(QString::fromUtf8(entry(other, QStringLiteral("word/document.xml"))).contains(QStringLiteral(">E = mc^2</w:t>")));
    QVERIFY(asked.limitations.contains(QStringLiteral("Math is written as its TeX source (2 formulas): formulas are not drawn"))
            || asked.limitations.join(u'\n').contains(QStringLiteral("Math is written as its TeX source (2 formulas)")));
}

void docxOpensInLibreOffice()
{
    const QString office = tool("soffice");
    if (office.isEmpty()) QSKIP("LibreOffice is not installed: the DOCX is not opened by a word processor");
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString path = writeRich(folder.path());
    const DocumentCheck::Loaded loaded = load(path);
    const QString output = folder.path() + QStringLiteral("/out.docx");
    QVERIFY(Exporters::find(QStringLiteral("docx"))->write(loaded.document, recipeFor(loaded, QStringLiteral("docx"), output),
                                                         fakeRun(folder.path() + QStringLiteral("/work")))
                .ok);
    QProcess convert;
    convert.setWorkingDirectory(folder.path());
    convert.start(office, {QStringLiteral("-env:UserInstallation=file://") + folder.path() + QStringLiteral("/profile"),
                           QStringLiteral("--headless"), QStringLiteral("--convert-to"), QStringLiteral("txt:Text"),
                           QStringLiteral("--outdir"), folder.path() + QStringLiteral("/text"), output});
    QVERIFY(convert.waitForFinished(180000));
    const QString text = QString::fromUtf8(readFile(folder.path() + QStringLiteral("/text/out.txt")));
    QVERIFY2(!text.isEmpty(), qPrintable(QString::fromUtf8(convert.readAllStandardError())));
    for (const char *words : {"The Rich Fixture", "Opening Heading", "A paragraph with emphasis, strong text, struck words",
                              "First bullet", "Nested bullet", "Done task", "Third numbered", "A quoted paragraph that stands apart.",
                              "Term one", "merged across", "def greet(name):", "Notes Heading", "A citation of a work",
                              "Doe, Jane. A Book of Examples.", "Closing Section", "The last paragraph of the document."})
        QVERIFY2(text.contains(QString::fromUtf8(words)), words);
    // Numbered from the list's own first number, and bulleted.
    QVERIFY2(text.contains(QRegularExpression(QStringLiteral("1\\.\\s+Third numbered"))), qPrintable(text));
    QVERIFY(text.contains(QRegularExpression(QStringLiteral("2\\.\\s+Fourth numbered"))));
    QVERIFY(text.contains(QStringLiteral("\u2022")));
}

void epubFollowsTheContainerRules()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString path = writeRich(folder.path());
    const DocumentCheck::Loaded loaded = load(path);
    const Exporter *epub = Exporters::find(QStringLiteral("epub"));
    const QString output = folder.path() + QStringLiteral("/out.epub");
    const ExportResult result =
        epub->write(loaded.document, recipeFor(loaded, QStringLiteral("epub"), output), fakeRun(folder.path() + QStringLiteral("/work")));
    QVERIFY2(result.ok, qPrintable(result.error));
    const QByteArray bytes = readFile(output);
    // The mimetype first, stored, with no extra field: readable at a fixed offset.
    QVERIFY(bytes.startsWith("PK\x03\x04"));
    QCOMPARE(bytes.mid(30, 8), QByteArray("mimetype"));
    QCOMPARE(bytes.mid(38, 20), QByteArray("application/epub+zip"));
    QCOMPARE(quint8(bytes[8]) | (quint8(bytes[9]) << 8), 0); // stored
    QCOMPARE(quint8(bytes[28]) | (quint8(bytes[29]) << 8), 0); // no extra field
    const QList<ZipPackage::Entry> entries = entriesOf(output);
    QCOMPARE(entries.first().name, QStringLiteral("mimetype"));
    QVERIFY(entries.first().stored);
    // The container leads to the package, which lists every file the book holds.
    const QString container = QString::fromUtf8(entry(entries, QStringLiteral("META-INF/container.xml")));
    QVERIFY(container.contains(QStringLiteral("full-path=\"OEBPS/content.opf\"")));
    const QString package = QString::fromUtf8(entry(entries, QStringLiteral("OEBPS/content.opf")));
    QStringList listed;
    static const QRegularExpression item(QStringLiteral("<item [^>]*href=\"([^\"]+)\" media-type=\"([^\"]+)\""));
    for (auto match = item.globalMatch(package); match.hasNext();) listed.append(QStringLiteral("OEBPS/") + match.next().captured(1));
    QStringList packed;
    for (const ZipPackage::Entry &each : entries)
        if (each.name.startsWith(QLatin1String("OEBPS/")) && each.name != u"OEBPS/content.opf") packed.append(each.name);
    std::sort(listed.begin(), listed.end());
    std::sort(packed.begin(), packed.end());
    QCOMPARE(listed, packed);
    QVERIFY(package.contains(QStringLiteral("properties=\"nav\"")));
    // A file for what comes before the first chapter and one for each of
    // the two chapters; the one with formulas says it holds SVG.
    QVERIFY(package.contains(QStringLiteral("href=\"chapter-1.xhtml\" media-type=\"application/xhtml+xml\"/>")));
    QVERIFY(package.contains(QStringLiteral("href=\"chapter-2.xhtml\" media-type=\"application/xhtml+xml\" properties=\"svg\"")));
    QVERIFY(package.contains(QStringLiteral("<spine>\n<itemref idref=\"text-1\"/>\n<itemref idref=\"text-2\"/>\n"
                                            "<itemref idref=\"text-3\"/>\n</spine>")));
    QVERIFY(package.contains(QStringLiteral("<dc:title>The Rich Fixture</dc:title>")));
    QVERIFY(package.contains(QStringLiteral("<dc:creator>A. Writer</dc:creator>")));
    QVERIFY(package.contains(QStringLiteral("<dc:language>en-GB</dc:language>")));
    QVERIFY(package.contains(QStringLiteral("<dc:date>2026-11-16</dc:date>")));
    QVERIFY(package.contains(QStringLiteral("<dc:identifier id=\"book-id\">urn:uuid:")));
    QVERIFY(package.contains(QRegularExpression(QStringLiteral("<meta property=\"dcterms:modified\">\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\dZ</meta>"))));
    // Every XML file is well formed.
    for (const ZipPackage::Entry &each : entries)
        if (each.name.endsWith(QLatin1String(".xhtml")) || each.name.endsWith(QLatin1String(".opf")) ||
            each.name.endsWith(QLatin1String(".xml")) || each.name.endsWith(QLatin1String(".svg")))
            QVERIFY2(xmlProblem(each.data).isEmpty(), qPrintable(each.name + u' ' + xmlProblem(each.data)));
    // The navigation document lists the headings, nested.
    const QString nav = QString::fromUtf8(entry(entries, QStringLiteral("OEBPS/nav.xhtml")));
    QVERIFY(nav.contains(QStringLiteral("<nav epub:type=\"toc\" id=\"toc\">")));
    QVERIFY(nav.contains(QStringLiteral("<li><a href=\"chapter-2.xhtml#opening-heading\">Opening Heading</a>\n<ol>\n"
                                        "<li><a href=\"chapter-2.xhtml#lists-heading\">Lists Heading</a></li>")));
    QVERIFY(nav.contains(QStringLiteral("<li><a href=\"chapter-3.xhtml#closing-section\">Closing Section</a></li>")));
    // The text: every construct, formulas as SVG named by their TeX, diagrams and images packed.
    QString text;
    for (int chapter = 1; chapter <= 3; ++chapter)
        text += QString::fromUtf8(entry(entries, QStringLiteral("OEBPS/chapter-%1.xhtml").arg(chapter)));
    for (const char *words : {"<h1 id=\"opening-heading\">Opening Heading</h1>", "<em>emphasis</em>", "<strong>strong text</strong>",
                              "<del>struck words</del>", "<mark>highlighted words</mark>", "x<sup>2</sup>", "H<sub>2</sub>O",
                              "<blockquote>", "<dl>", "<td colspan=\"2\" style=\"text-align: left\">merged across</td>",
                              "<th style=\"text-align: center\">Centre</th>", "aria-label=\"E = mc^2\"",
                              "<figure class=\"diagram\"><img src=\"images/diagram.svg\"", "<img src=\"images/square.png\" alt=\"A small square\" />",
                              "<span class=\"image-missing\">A picture from the web</span>", "<section class=\"footnotes\">",
                              "The inline note's text.", "<div class=\"page-break\"></div>", "aria-label=\"done\">\u2611</span>"})
        QVERIFY2(text.contains(QString::fromUtf8(words)), words);
    QVERIFY(!text.contains(QStringLiteral("<input")));
    QVERIFY(!text.contains(QStringLiteral(" align=\"")));
    QVERIFY(result.limitations.contains(QStringLiteral("Formulas are SVG pictures named by their TeX, not MathML (2 formulas)")));
    QVERIFY(result.limitations.contains(QStringLiteral("Page breaks are hints: a reader sets its own pages (1 page break)")));
    QVERIFY(result.limitations.contains(QStringLiteral("Images from the web are not packed into the book, so their descriptions "
                                                       "stand in their place (1 image)")));
    if (tool("epubcheck").isEmpty()) QSKIP("epubcheck is not installed: the book is checked by its structure only");
    QProcess check;
    check.start(tool("epubcheck"), {output});
    QVERIFY(check.waitForFinished(120000));
    QVERIFY2(check.exitCode() == 0, check.readAllStandardOutput().constData());
}

void epubSplitsIntoChapters()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const Exporter *epub = Exporters::find(QStringLiteral("epub"));
    const auto book = [&](const QString &name, const QString &markdown) {
        writeFile(folder.path() + u'/' + name + QStringLiteral(".md"), markdown.toUtf8());
        const DocumentCheck::Loaded loaded = load(canonical(folder.path() + u'/' + name + QStringLiteral(".md")));
        const QString output = folder.path() + u'/' + name + QStringLiteral(".epub");
        const ExportResult result = epub->write(loaded.document, recipeFor(loaded, QStringLiteral("epub"), output), bareRun());
        if (!result.ok) qWarning() << result.error;
        return entriesOf(output);
    };
    const auto file = [](const QList<ZipPackage::Entry> &entries, int chapter) {
        return QString::fromUtf8(entry(entries, QStringLiteral("OEBPS/chapter-%1.xhtml").arg(chapter)));
    };

    // One heading of level 1: a file for each of level 2, the title's own first.
    const QList<ZipPackage::Entry> parts = book(
        QStringLiteral("parts"),
        QStringLiteral("# Book\n\nIntro with a note.[^a] See [the third](#third-part).\n\n"
                       "## First part\n\nText one.[^b]\n\n> # Quoted heading\n>\n> Inside a quote.\n\n"
                       "## Second part\n\nText two, back to [the first](#first-part).\n\n"
                       "## Third part\n\nText three.\n\n[^a]: Note a.\n[^b]: Note b, see [the second](#second-part).\n"));
    QStringList chapters;
    for (const ZipPackage::Entry &each : parts)
        if (each.name.startsWith(QLatin1String("OEBPS/chapter-"))) chapters.append(each.name);
    QCOMPARE(chapters, (QStringList{"OEBPS/chapter-1.xhtml", "OEBPS/chapter-2.xhtml", "OEBPS/chapter-3.xhtml",
                                    "OEBPS/chapter-4.xhtml"}));
    for (int chapter = 1; chapter <= 4; ++chapter)
        QVERIFY2(xmlProblem(file(parts, chapter).toUtf8()).isEmpty(), qPrintable(xmlProblem(file(parts, chapter).toUtf8())));
    QVERIFY(file(parts, 1).contains(QStringLiteral("<h1 id=\"book\">Book</h1>")));
    QVERIFY(file(parts, 2).contains(QStringLiteral("<h2 id=\"first-part\">First part</h2>")));
    // A heading inside a quote starts no file.
    QVERIFY(file(parts, 2).contains(QStringLiteral("Quoted heading")));
    QVERIFY(file(parts, 3).contains(QStringLiteral("<h2 id=\"second-part\">Second part</h2>")));
    QVERIFY(file(parts, 4).contains(QStringLiteral("<h2 id=\"third-part\">Third part</h2>")));
    // The spine reads them in order, and the navigation finds each heading in its file.
    const QString package = QString::fromUtf8(entry(parts, QStringLiteral("OEBPS/content.opf")));
    QVERIFY(package.contains(QStringLiteral("<spine>\n<itemref idref=\"text-1\"/>\n<itemref idref=\"text-2\"/>\n"
                                            "<itemref idref=\"text-3\"/>\n<itemref idref=\"text-4\"/>\n</spine>")));
    for (int chapter = 1; chapter <= 4; ++chapter)
        QVERIFY(package.contains(QStringLiteral("<item id=\"text-%1\" href=\"chapter-%1.xhtml\"").arg(chapter)));
    const QString nav = QString::fromUtf8(entry(parts, QStringLiteral("OEBPS/nav.xhtml")));
    QVERIFY(nav.contains(QStringLiteral("<li><a href=\"chapter-1.xhtml#book\">Book</a>")));
    QVERIFY(nav.contains(QStringLiteral("<li><a href=\"chapter-2.xhtml#first-part\">First part</a></li>")));
    QVERIFY(nav.contains(QStringLiteral("<li><a href=\"chapter-4.xhtml#third-part\">Third part</a></li>")));
    // Links lead into the file that holds their target: headings, notes and the way back.
    QVERIFY(file(parts, 1).contains(QStringLiteral("href=\"chapter-4.xhtml#third-part\"")));
    QVERIFY(file(parts, 1).contains(QStringLiteral("href=\"chapter-4.xhtml#fn-1\" id=\"fnref-1\"")));
    QVERIFY(file(parts, 2).contains(QStringLiteral("href=\"chapter-4.xhtml#fn-2\" id=\"fnref-2\"")));
    QVERIFY(file(parts, 3).contains(QStringLiteral("href=\"chapter-2.xhtml#first-part\"")));
    QVERIFY(file(parts, 4).contains(QStringLiteral("<li id=\"fn-1\">")));
    QVERIFY(file(parts, 4).contains(QStringLiteral("href=\"chapter-1.xhtml#fnref-1\"")));
    QVERIFY(file(parts, 4).contains(QStringLiteral("href=\"chapter-2.xhtml#fnref-2\"")));
    QVERIFY(file(parts, 4).contains(QStringLiteral("href=\"chapter-3.xhtml#second-part\"")));
    // No link is left leading into a file it is not in.
    static const QRegularExpression local(QStringLiteral("href=\"#([^\"]+)\""));
    for (int chapter = 1; chapter <= 4; ++chapter) {
        const QString text = file(parts, chapter);
        for (auto match = local.globalMatch(text); match.hasNext();) {
            const QString id = match.next().captured(1);
            QVERIFY2(text.contains(QStringLiteral("id=\"%1\"").arg(id)), qPrintable(id));
        }
    }

    // Several headings of level 1: a file for each, the part before them its own.
    const QList<ZipPackage::Entry> book2 =
        book(QStringLiteral("two"), QStringLiteral("Before anything.\n\n# One\n\n## Inside one\n\nText.\n\n# Two\n\nMore.\n"));
    QVERIFY(file(book2, 1).contains(QStringLiteral("Before anything.")));
    QVERIFY(file(book2, 2).contains(QStringLiteral("<h2 id=\"inside-one\">Inside one</h2>")));
    QVERIFY(file(book2, 3).contains(QStringLiteral("<h1 id=\"two\">Two</h1>")));
    QVERIFY(file(book2, 4).isEmpty());

    // A short text with no headings stays one file.
    const QList<ZipPackage::Entry> short_ = book(QStringLiteral("short"), QStringLiteral("Just a few words.\n"));
    QVERIFY(QString::fromUtf8(entry(short_, QStringLiteral("OEBPS/text.xhtml"))).contains(QStringLiteral("Just a few words.")));
    QVERIFY(entry(short_, QStringLiteral("OEBPS/chapter-1.xhtml")).isEmpty());
    const QString shortPackage = QString::fromUtf8(entry(short_, QStringLiteral("OEBPS/content.opf")));
    QVERIFY(shortPackage.contains(QStringLiteral("<spine>\n<itemref idref=\"text-1\"/>\n</spine>")));
    QVERIFY(QString::fromUtf8(entry(short_, QStringLiteral("OEBPS/nav.xhtml"))).contains(QStringLiteral("<li><a href=\"text.xhtml\">short</a></li>")));
}

void htmlCarriesItsPictures()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString path = writeRich(folder.path());
    const DocumentCheck::Loaded loaded = load(path);
    const Exporter *html = Exporters::find(QStringLiteral("html"));
    const QString work = folder.path() + QStringLiteral("/work");
    QDir().mkpath(folder.path() + QStringLiteral("/out"));

    // Copied into a folder beside the page, which says an export made it.
    const QString page = folder.path() + QStringLiteral("/out/page.html");
    const ExportResult copied = html->write(loaded.document, recipeFor(loaded, QStringLiteral("html"), page), fakeRun(work));
    QVERIFY2(copied.ok, qPrintable(copied.error));
    const QString shown = QString::fromUtf8(readFile(page));
    QVERIFY(shown.contains(QStringLiteral("<img src=\"page_files/square.png\" alt=\"A small square\" />")));
    QVERIFY(shown.contains(QStringLiteral("<figure class=\"diagram\"><img src=\"page_files/diagram.svg\" alt=\"")));
    QVERIFY(shown.contains(QStringLiteral("role=\"img\" aria-label=\"E = mc^2\" class=\"math-svg\"")));
    QVERIFY(shown.contains(QStringLiteral("<span class=\"math\"><svg")));
    QVERIFY(shown.contains(QStringLiteral("<div class=\"math display\"><svg")));
    QCOMPARE(entriesIn(folder.path() + QStringLiteral("/out/page_files")),
             (QStringList{QStringLiteral(".parch-export"), QStringLiteral("diagram.svg"), QStringLiteral("square.png")}));
    QCOMPARE(readFile(folder.path() + QStringLiteral("/out/page_files/square.png")), readFile(folder.path() + QStringLiteral("/square.png")));
    QVERIFY(copied.written.contains(folder.path() + QStringLiteral("/out/page_files/square.png")));
    QVERIFY2(copied.limitations.isEmpty(), qPrintable(copied.limitations.join(u'\n')));
    // Again: the folder an export made is replaced whole.
    writeFile(folder.path() + QStringLiteral("/out/page_files/stale.png"), "stale");
    QVERIFY(html->write(loaded.document, recipeFor(loaded, QStringLiteral("html"), page), fakeRun(work)).ok);
    QVERIFY(!QFileInfo::exists(folder.path() + QStringLiteral("/out/page_files/stale.png")));
    // A folder an export did not make is never replaced, and the page is not written.
    const QString theirs = folder.path() + QStringLiteral("/out/theirs.html");
    writeFile(folder.path() + QStringLiteral("/out/theirs_files/keep.txt"), "keep");
    const ExportResult refused = html->write(loaded.document, recipeFor(loaded, QStringLiteral("html"), theirs), fakeRun(work));
    QVERIFY(!refused.ok);
    QVERIFY(refused.ioFailure);
    QVERIFY2(refused.error.contains(QStringLiteral("an export did not make it")), qPrintable(refused.error));
    QVERIFY(!QFileInfo::exists(theirs));
    QCOMPARE(readFile(folder.path() + QStringLiteral("/out/theirs_files/keep.txt")), QByteArray("keep"));

    // Inside the page.
    const QString embedded = folder.path() + QStringLiteral("/out/embedded.html");
    QVERIFY(html->write(loaded.document, recipeFor(loaded, QStringLiteral("html"), embedded, {{QStringLiteral("images"), QStringLiteral("embed")}}),
                        fakeRun(work))
                .ok);
    const QString inside = QString::fromUtf8(readFile(embedded));
    QVERIFY(inside.contains(QStringLiteral("<img src=\"data:image/png;base64,")));
    QVERIFY(inside.contains(QStringLiteral("<figure class=\"diagram\"><img src=\"data:image/svg+xml;base64,")));
    QVERIFY(!QFileInfo::exists(folder.path() + QStringLiteral("/out/embedded_files")));
    // Linked where they are, which the limitations say.
    const QString linked = folder.path() + QStringLiteral("/out/linked.html");
    const ExportResult link =
        html->write(loaded.document, recipeFor(loaded, QStringLiteral("html"), linked, {{QStringLiteral("images"), QStringLiteral("link")}}), bareRun());
    QVERIFY(link.ok);
    QVERIFY(QString::fromUtf8(readFile(linked)).contains(QStringLiteral("<img src=\"../square.png\" alt=\"A small square\" />")));
    QVERIFY(link.limitations.contains(QStringLiteral("Images are linked from the page, not inside it (1 image): keep them where "
                                                     "the page finds them")));
    QVERIFY(link.limitations.contains(
        QStringLiteral("Math is written as its TeX source (2 formulas): the math helper (render-svg) is not installed")));
    QVERIFY(QString::fromUtf8(readFile(linked)).contains(QStringLiteral("<span class=\"math\">E = mc^2</span>")));
}

void markdownExportsAssembledOrAsIs()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    writeFile(folder.path() + QStringLiteral("/doc/main.md"), "# Main\n\n![pic](pics/p.png)\n\nchapter.md\n");
    writeFile(folder.path() + QStringLiteral("/doc/chapter.md"), "Chapter text.\n");
    QDir().mkpath(folder.path() + QStringLiteral("/doc/pics"));
    QImage(8, 8, QImage::Format_ARGB32).save(folder.path() + QStringLiteral("/doc/pics/p.png"));
    const DocumentCheck::Loaded loaded = load(canonical(folder.path() + QStringLiteral("/doc/main.md")));
    const Exporter *markdown = Exporters::find(QStringLiteral("md"));
    QDir().mkpath(folder.path() + QStringLiteral("/out"));
    // As it is: the document's own text, its include line and all.
    const QString asIs = folder.path() + QStringLiteral("/out/as-is.md");
    const ExportResult same = markdown->write(loaded.document, recipeFor(loaded, QStringLiteral("md"), asIs, {{QStringLiteral("as-is"), {}}}));
    QVERIFY(same.ok);
    QCOMPARE(readFile(asIs), QByteArray("# Main\n\n![pic](pics/p.png)\n\nchapter.md\n"));
    QVERIFY(same.limitations.contains(QStringLiteral("Included files stay include lines (1 file): keep them where the text finds them")));
    // Assembled, its pictures copied beside it.
    const QString assembled = folder.path() + QStringLiteral("/out/assembled.md");
    const ExportResult whole =
        markdown->write(loaded.document, recipeFor(loaded, QStringLiteral("md"), assembled, {{QStringLiteral("images"), QStringLiteral("copy")}}));
    QVERIFY2(whole.ok, qPrintable(whole.error));
    QCOMPARE(readFile(assembled), QByteArray("# Main\n\n![pic](assembled_files/p.png)\n\nChapter text.\n"));
    QVERIFY(QFileInfo::exists(folder.path() + QStringLiteral("/out/assembled_files/p.png")));
}

void exportsListLimitationsBeforeWriting()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const DocumentCheck::Loaded loaded = load(writeRich(folder.path()));
    const RenderService::Options fakes = fakeHelpers(folder.path() + QStringLiteral("/work"));
    const HelperSet none;
    const auto said = [&](const char *format, const HelperSet &helpers, const QHash<QString, QString> &options = {}) {
        return Exporters::find(QString::fromLatin1(format))
            ->limitations(loaded.document, recipeFor(loaded, QString::fromLatin1(format), folder.path() + QStringLiteral("/x"), options), helpers);
    };
    // Without the helpers, every format that draws says what stays source.
    for (const char *format : {"html", "pdf", "docx", "epub"}) {
        const QString all = said(format, none).join(u'\n');
        QVERIFY2(all.contains(QStringLiteral("(2 formulas): the math helper (render-svg) is not installed")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("(1 diagram): the diagram helper (merman-cli) is not installed")), qPrintable(all));
    }
    // With them, what each format makes of them.
    QVERIFY(said("docx", fakes.helpers).contains(QStringLiteral("1 diagram and 2 formulas become pictures, described by their words "
                                                               "or TeX: Word cannot edit them as equations or diagrams")));
    QVERIFY(said("epub", fakes.helpers).contains(QStringLiteral("Formulas are SVG pictures named by their TeX, not MathML (2 formulas)")));
    QVERIFY(said("epub", fakes.helpers).contains(QStringLiteral("Page breaks are hints: a reader sets its own pages (1 page break)")));
    QVERIFY(said("pdf", fakes.helpers).contains(QStringLiteral("Diagram labels are drawn as shapes, so a reader cannot search or select them (1 diagram)")));
    QVERIFY(said("html", fakes.helpers).isEmpty());
    QVERIFY(said("html", fakes.helpers, {{QStringLiteral("images"), QStringLiteral("link")}}).first().startsWith(QStringLiteral("Images are linked")));
    QVERIFY(said("md", none).isEmpty());
    // What a DOCX or a book says before it is written, it says again after.
    for (const char *format : {"docx", "epub"}) {
        ExportRun run;
        run.render = fakes;
        const ExportResult result = Exporters::find(QString::fromLatin1(format))
                                        ->write(loaded.document,
                                                recipeFor(loaded, QString::fromLatin1(format), folder.path() + QStringLiteral("/out.") + QString::fromLatin1(format)),
                                                run);
        QVERIFY(result.ok);
        for (const QString &line : said(format, fakes.helpers))
            QVERIFY2(result.limitations.contains(line), qPrintable(line + u'\n' + result.limitations.join(u'\n')));
    }
}

void exportsStopWhenCancelled()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const DocumentCheck::Loaded loaded = load(writeRich(folder.path()));
    const QString out = folder.path() + QStringLiteral("/out");
    QDir().mkpath(out);
    for (const char *format : {"md", "html", "pdf", "docx", "epub"}) {
        const QString output = out + QStringLiteral("/old.") + QString::fromLatin1(format);
        writeFile(output, "old");
        const QStringList before = entriesIn(out);
        ExportRun run = fakeRun(folder.path() + QStringLiteral("/work"));
        int reports = 0;
        run.progress = [&](qreal, const QString &) { ++reports; };
        // Stops once it has begun, or at once for a format that says nothing first.
        run.cancelled = [&] { return reports > 0 || QByteArrayView(format) == "md"; };
        const ExportResult result = Exporters::find(QString::fromLatin1(format))
                                        ->write(loaded.document, recipeFor(loaded, QString::fromLatin1(format), output), run);
        QVERIFY2(!result.ok, format);
        QVERIFY2(result.cancelled, format);
        QCOMPARE(result.error, QStringLiteral("the export was cancelled"));
        QVERIFY(result.written.isEmpty());
        // The file that was there is as it was, and nothing else was left behind.
        QCOMPARE(readFile(output), QByteArray("old"));
        QCOMPARE(entriesIn(out), before);
    }
}

void exportsFailWithAReasonAndKeepTheOldFile()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const DocumentCheck::Loaded loaded = load(writeRich(folder.path()));
    const QString locked = folder.path() + QStringLiteral("/locked");
    QDir().mkpath(locked);
    for (const char *format : {"md", "html", "pdf", "docx", "epub"}) writeFile(locked + QStringLiteral("/old.") + QString::fromLatin1(format), "old");
    QFile::setPermissions(locked, QFile::ReadOwner | QFile::ExeOwner);
    const auto unlock = qScopeGuard([&] { QFile::setPermissions(locked, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); });
    if (QFileInfo(locked).isWritable()) QSKIP("this user can write into any folder");
    for (const char *format : {"md", "html", "pdf", "docx", "epub"}) {
        const QString output = locked + QStringLiteral("/old.") + QString::fromLatin1(format);
        const ExportResult result = Exporters::find(QString::fromLatin1(format))
                                        ->write(loaded.document, recipeFor(loaded, QString::fromLatin1(format), output), bareRun());
        QVERIFY2(!result.ok, format);
        QVERIFY2(result.ioFailure, format);
        QVERIFY2(result.error.contains(output), qPrintable(result.error));
        QCOMPARE(readFile(output), QByteArray("old"));
    }
    // Options a format does not take are refused before anything is written.
    const QString elsewhere = folder.path() + QStringLiteral("/never.pdf");
    const ExportResult tabloid = Exporters::find(QStringLiteral("pdf"))->write(
        loaded.document, recipeFor(loaded, QStringLiteral("pdf"), elsewhere, {{QStringLiteral("page-size"), QStringLiteral("Tabloid")}}), bareRun());
    QVERIFY(!tabloid.ok);
    QVERIFY(tabloid.error.startsWith(QStringLiteral("--page-size takes A4, Letter")));
    QVERIFY(!QFileInfo::exists(elsewhere));
    const ExportResult sidenotes = Exporters::find(QStringLiteral("docx"))->write(
        loaded.document, recipeFor(loaded, QStringLiteral("docx"), elsewhere, {{QStringLiteral("notes"), QStringLiteral("sidenotes")}}), bareRun());
    QVERIFY(!sidenotes.ok);
    QCOMPARE(sidenotes.error, QStringLiteral("--notes takes footnotes or endnotes, not \"sidenotes\""));
}

void exportsNeverTouchTheSource()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString book = folder.path() + QStringLiteral("/book");
    writeRich(book);
    writeFile(book + QStringLiteral("/rich.md"), readFile(book + QStringLiteral("/rich.md")) + "\nchapter.md\n");
    // A chapter with Windows line endings and a byte-order mark.
    writeFile(book + QStringLiteral("/chapter.md"), QByteArray("\xef\xbb\xbf## Chapter\r\n\r\nLast words.\r\n"));
    const QMap<QString, QByteArray> before = filesBeneath(book);
    const DocumentCheck::Loaded loaded = load(canonical(book + QStringLiteral("/rich.md")));
    QVERIFY(!loaded.document.incomplete);
    QDir().mkpath(folder.path() + QStringLiteral("/out"));
    for (const Exporter *exporter : Exporters::all()) {
        const QString output = folder.path() + QStringLiteral("/out/book.") + exporter->suffixes().first();
        const ExportResult result =
            exporter->write(loaded.document, recipeFor(loaded, exporter->format(), output), fakeRun(folder.path() + QStringLiteral("/work")));
        QVERIFY2(result.ok, qPrintable(exporter->format() + u' ' + result.error));
        QCOMPARE(filesBeneath(book), before);
    }
    // Written beside the source, it still changes nothing of it.
    const QString beside = book + QStringLiteral("/beside.html");
    QVERIFY(Exporters::find(QStringLiteral("html"))->write(loaded.document, recipeFor(loaded, QStringLiteral("html"), beside), bareRun()).ok);
    QMap<QString, QByteArray> after = filesBeneath(book);
    after.remove(QStringLiteral("beside.html"));
    after.remove(QStringLiteral("beside_files/square.png"));
    after.remove(QStringLiteral("beside_files/.parch-export"));
    QCOMPARE(after, before);
    QVERIFY(Exporters::isSource(loaded.document, book + QStringLiteral("/chapter.md")));
}

void exportDialogListsLimitationsAndExportsInTheBackground()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString path = writeRich(canonical(files.path()));
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1000);
    ExportPicker picker;
    shell.backend->setFilePicker(&picker);
    Exports *exports = shell.backend->exports();
    exports->setRenderOptions(fakeHelpers(files.path() + QStringLiteral("/work")));

    // Commands of the registry, with their keys, in the menu.
    CommandRegistry *commands = shell.backend->commands();
    QCOMPARE(commands->title(QStringLiteral("export")), QStringLiteral("Export…"));
    QCOMPARE(commands->title(QStringLiteral("print")), QStringLiteral("Print…"));
    QCOMPARE(commands->title(QStringLiteral("exportAgain")), QStringLiteral("Export again"));
    for (const char *id : {"export", "print", "exportAgain"}) QCOMPARE(commands->needs(QString::fromLatin1(id)), QStringLiteral("document"));
    QCOMPARE(commands->shortcuts(QStringLiteral("export")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+E"))});
    QCOMPARE(commands->shortcuts(QStringLiteral("print")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+P"))});
    for (const char *name : {"exportEntry", "exportAgainEntry", "printEntry"}) {
        QObject *entry = shell.object(QString::fromLatin1(name));
        QVERIFY2(entry && entry->property("enabled").toBool(), name);
    }

    QObject *dialog = shell.object(QStringLiteral("exportDialog"));
    QVERIFY(dialog);
    QVERIFY(run(window, "export"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(dialog->property("title").toString().startsWith(QStringLiteral("Export “")));
    // It reads the document, then lists what PDF, the first choice, leaves out.
    QTRY_VERIFY(!exports->reading());
    const auto listed = [&] {
        QStringList texts;
        for (QObject *object : everyObjectIn(window))
            if (object->objectName() == u"exportLimitation" && object->property("visible").toBool())
                texts.append(object->property("text").toString());
        return texts;
    };
    QTRY_VERIFY(listed().contains(QStringLiteral("Images from the web are not fetched, so their descriptions stand in their place (1 image)")));
    // DOCX, chosen from the keyboard, lists its own.
    QQuickItem *format = shell.item(QStringLiteral("exportFormat"));
    QVERIFY(format);
    QCOMPARE(window->activeFocusItem(), format);
    QMetaObject::invokeMethod(format, "activated", Q_ARG(int, 3));
    QTRY_COMPARE(dialog->property("format").toString(), QStringLiteral("docx"));
    QTRY_VERIFY(listed().contains(QStringLiteral("1 diagram and 2 formulas become pictures, described by their words or TeX: "
                                                 "Word cannot edit them as equations or diagrams")));
    QVERIFY(shell.item(QStringLiteral("exportOption-notes")));
    QVERIFY(!shell.item(QStringLiteral("exportOption-margins")));
    // Named for assistive technology, within the window, every control reached by Tab.
    QTest::qWait(50);
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    QStringList reached;
    for (int i = 0; i < 30; ++i) {
        QTest::keyClick(window, Qt::Key_Tab);
        QQuickItem *now = window->activeFocusItem();
        if (!now || now == format) break;
        reached << now->objectName();
    }
    for (const char *name : {"exportOption-title", "exportOption-notes", "exportOption-page-size", "cancelExport", "exportGo"})
        QVERIFY2(reached.contains(QString::fromLatin1(name)), qPrintable(reached.join(", ")));

    // Export… asks the desktop's file dialog where, for a DOCX beside the document.
    QMetaObject::invokeMethod(shell.object(QStringLiteral("exportGo")), "clicked");
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QCOMPARE(picker.saves, 1);
    QCOMPARE(picker.suggested.toLocalFile(), QFileInfo(path).absolutePath() + QStringLiteral("/rich.docx"));
    QCOMPARE(picker.patterns, QStringList{QStringLiteral("*.docx")});
    const QString output = canonical(files.path()) + QStringLiteral("/chosen");
    emit picker.saveSelected(QUrl::fromLocalFile(output));
    // The suffix the dialog left off is the format's.
    QTRY_VERIFY_WITH_TIMEOUT(!exports->busy(), 30000);
    QTRY_VERIFY(QFileInfo::exists(output + QStringLiteral(".docx")));
    QCOMPARE(shell.backend->problem(), QStringLiteral("Exported “chosen.docx”"));
    QVERIFY(shell.backend->problemDetail().contains(QStringLiteral("become pictures")));
    QCOMPARE(shell.backend->noticeTone(), Backend::News);
    QCOMPARE(exports->lastFormat(shell.document()), QStringLiteral("docx"));

    // A slow export shows its progress in the notice bar and can be cancelled there.
    const QString slow = canonical(files.path()) + QStringLiteral("/slow.md");
    writeFile(slow, "Slow $slow a$ and $slow b$ and $slow c$ and $slow d$.\n");
    shell.backend->openFiles({slow});
    QTRY_COMPARE(shell.document()->path(), slow);
    const QString never = canonical(files.path()) + QStringLiteral("/never.pdf");
    exports->exportTo(shell.document(), QStringLiteral("pdf"), {}, never);
    QVERIFY(exports->busy());
    QTRY_VERIFY(shell.item(QStringLiteral("exportNotice")) && shell.item(QStringLiteral("exportNotice"))->isVisible());
    QTRY_VERIFY(exports->status().startsWith(QStringLiteral("Exporting “never.pdf”")));
    QQuickItem *cancel = shell.item(QStringLiteral("exportCancel"));
    QVERIFY(cancel);
    QCOMPARE(cancel->property("text").toString(), QStringLiteral("Cancel"));
    QMetaObject::invokeMethod(cancel, "clicked");
    QTRY_VERIFY_WITH_TIMEOUT(!exports->busy(), 30000);
    QCOMPARE(shell.backend->problem(), QStringLiteral("Export cancelled"));
    QVERIFY(!QFileInfo::exists(never));
    // An export never writes over the document itself.
    exports->exportTo(shell.document(), QStringLiteral("md"), {}, slow);
    QTRY_VERIFY_WITH_TIMEOUT(!exports->busy(), 30000);
    QCOMPARE(shell.backend->problem(), QStringLiteral("Not exported"));
    QVERIFY(shell.backend->problemDetail().contains(QStringLiteral("an export never writes over")));
    QCOMPARE(readFile(slow), QByteArray("Slow $slow a$ and $slow b$ and $slow c$ and $slow d$.\n"));
}

void exportsAskBeforeReplacingWhatTheSuffixNames()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonical(files.path());
    const QString path = root + QStringLiteral("/essay.md");
    writeFile(path, "# Essay\n\nFirst words.\n");
    // Another of the writer's documents, by the name the dialog will be given without its suffix.
    const QString other = root + QStringLiteral("/other.md");
    writeFile(other, "Another document.\n");
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    ExportPicker picker;
    shell.backend->setFilePicker(&picker);
    Exports *exports = shell.backend->exports();
    exports->setRenderOptions(fakeHelpers(root + QStringLiteral("/work")));
    QObject *prompt = shell.object(QStringLiteral("exportReplacePrompt"));
    QVERIFY(prompt);
    const auto chooseInDialog = [&](const QString &chosen) {
        const int asked = picker.saves;
        exports->exportDocument(shell.document(), QStringLiteral("md"), {}, shell.context());
        QCOMPARE(picker.saves, asked + 1);
        emit picker.saveSelected(QUrl::fromLocalFile(chosen));
    };

    // The dialog was asked about "other", which is not there; "other.md" is, so the window asks.
    chooseInDialog(root + QStringLiteral("/other"));
    QTRY_VERIFY(prompt->property("opened").toBool());
    QVERIFY(!exports->busy());
    QCOMPARE(prompt->property("title").toString(), QStringLiteral("Replace “other.md”?"));
    QCOMPARE(readFile(other), QByteArray("Another document.\n"));
    // Cancel, the safe choice, has the keys, and leaves the file as it was.
    QQuickItem *keep = shell.item(QStringLiteral("exportReplaceCancel"));
    QVERIFY(keep);
    QTRY_COMPARE(window->activeFocusItem(), keep);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!prompt->property("visible").toBool());
    QTest::qWait(100);
    QVERIFY(!exports->busy());
    QCOMPARE(readFile(other), QByteArray("Another document.\n"));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/other")));

    // Replace writes over it.
    chooseInDialog(root + QStringLiteral("/other"));
    QTRY_VERIFY(prompt->property("opened").toBool());
    QMetaObject::invokeMethod(shell.object(QStringLiteral("exportReplace")), "clicked");
    QTRY_VERIFY_WITH_TIMEOUT(!exports->busy() && readFile(other).startsWith("# Essay"), 30000);
    QCOMPARE(shell.backend->problem(), QStringLiteral("Exported “other.md”"));

    // A name with no file behind it, with or without its suffix, is written without asking.
    chooseInDialog(root + QStringLiteral("/fresh"));
    QTRY_VERIFY_WITH_TIMEOUT(!exports->busy() && QFileInfo::exists(root + QStringLiteral("/fresh.md")), 30000);
    QVERIFY(!prompt->property("visible").toBool());
    // A name the dialog was given whole was asked about there.
    chooseInDialog(root + QStringLiteral("/fresh.md"));
    QTRY_VERIFY_WITH_TIMEOUT(!exports->busy(), 30000);
    QCOMPARE(shell.backend->problem(), QStringLiteral("Exported “fresh.md”"));
    QVERIFY(!prompt->property("visible").toBool());
    QCOMPARE(readFile(path), QByteArray("# Essay\n\nFirst words.\n"));
}

void codeIsColouredWithoutAPreviewShownFirst()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonical(files.path());
    const QString path = root + QStringLiteral("/code.md");
    QByteArray text = "# Code\n\n```python\n";
    for (int i = 0; i < 12; ++i) text += "def f(x):\n    return \"text\"  # note\n";
    text += "```\n";
    writeFile(path, text);
    // What `parch export` makes, on the main thread.
    const Ran cli = runCli(root, {QStringLiteral("export"), QStringLiteral("code.md"), QStringLiteral("-o"), QStringLiteral("cli.pdf")});
    QVERIFY2(cli.code == 0, qPrintable(cli.err));
    const QStringList coloured = fillColours(readFile(root + QStringLiteral("/cli.pdf")));
    // The code's ink, the keywords', the strings' and the comments' at least.
    QVERIFY2(coloured.size() >= 4, qPrintable(coloured.join(u'|')));

    // The window, with its tab showing the source and no preview shown yet.
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    Exports *exports = shell.backend->exports();
    exports->setRenderOptions(fakeHelpers(root + QStringLiteral("/work")));
    Document *document = shell.document();
    const auto asLaunched = [] {
        CodeHighlight::unload();
        // Off the UI thread, code is plain until the languages are loaded.
        bool plain = false;
        std::thread([&plain] { plain = CodeHighlight::spans(QStringLiteral("python"), QStringLiteral("def f(): pass\n")).isEmpty(); })
            .join();
        return plain;
    };
    QVERIFY(asLaunched());
    const QString output = root + QStringLiteral("/window.pdf");
    exports->exportTo(document, QStringLiteral("pdf"), {}, output);
    QTRY_VERIFY_WITH_TIMEOUT(!exports->busy() && QFileInfo::exists(output), 30000);
    QCOMPARE(fillColours(readFile(output)), coloured);

    // The pages the Export dialog and printing show, laid out on a worker too.
    QVERIFY(asLaunched());
    PrintPreview *preview = exports->preview();
    const int before = preview->revision();
    preview->show(document, QStringLiteral("pdf"), {});
    QVERIFY(QTest::qWaitFor([&] { return preview->revision() > before && !preview->busy(); }, 30000));
    QSignalSpy ready(preview, &PrintPreview::pageReady);
    preview->requestPage(0, 1200);
    QVERIFY(ready.wait(30000));
    const QImage page = preview->page(0);
    QVERIFY(!page.isNull());
    QVERIFY(pixelsNear(page, PreviewStyle::paper().ink(PreviewStyle::Keyword), 24) > 20);
    preview->stop();
    CodeHighlight::prepare();
}

void exportAgainRepeatsTheRecipe()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonical(files.path());
    const QString path = root + QStringLiteral("/essay.md");
    writeFile(path, "# Essay\n\nFirst words.\n");
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    Exports *exports = shell.backend->exports();
    exports->setRenderOptions(fakeHelpers(root + QStringLiteral("/work")));
    Document *document = shell.document();
    // Nothing yet to repeat.
    QVERIFY(!exports->exportAgain(document));
    QCOMPARE(shell.backend->problem(), QStringLiteral("Nothing to export again"));
    const QString output = root + QStringLiteral("/essay.pdf");
    exports->exportTo(document, QStringLiteral("pdf"),
                      {{QStringLiteral("page-size"), QStringLiteral("A5")}, {QStringLiteral("no-page-numbers"), true}}, output);
    QTRY_VERIFY_WITH_TIMEOUT(!exports->busy(), 30000);
    QVERIFY(QFileInfo::exists(output));
    // The recipe: format, options, the dialect it was read in and what drew it.
    const QJsonObject recipe = exports->lastRecipe(document);
    QCOMPARE(recipe.value(QStringLiteral("format")).toString(), QStringLiteral("pdf"));
    QCOMPARE(recipe.value(QStringLiteral("output")).toString(), output);
    QCOMPARE(recipe.value(QStringLiteral("options")).toObject(),
             (QJsonObject{{QStringLiteral("page-size"), QStringLiteral("A5")}, {QStringLiteral("no-page-numbers"), true}}));
    QCOMPARE(recipe.value(QStringLiteral("dialect")).toObject().value(QStringLiteral("profile")).toString(), QStringLiteral("writer"));
    QVERIFY(recipe.value(QStringLiteral("renderers")).toObject().contains(QStringLiteral("pdf")));
    QCOMPARE(exports->lastOptions(document).value(QStringLiteral("page-size")).toString(), QStringLiteral("A5"));
    // Kept for the next run of parch.
    QVERIFY(QFileInfo::exists(shell.paths().data + QStringLiteral("/exports.json")));
    Exports later(shell.paths(), nullptr);
    QCOMPARE(later.lastFormat(document), QStringLiteral("pdf"));

    // Export again writes the same file in the same way, with the text as it is now.
    QFile::remove(output);
    document->buffer()->replace(SourceRange{0, 0}, QStringLiteral("Changed. "));
    QVERIFY(QMetaObject::invokeMethod(shell.window.get(), "run", Q_ARG(QVariant, QStringLiteral("exportAgain"))));
    QTRY_VERIFY_WITH_TIMEOUT(!exports->busy() && QFileInfo::exists(output), 30000);
    QCOMPARE(mediaBox(readFile(output)).toSize(), QSize(420, 595));
    if (!tool("pdftotext").isEmpty()) QVERIFY(pdfText(output).contains(QStringLiteral("Changed.")));
    // The document's own file was never written by it.
    QCOMPARE(readFile(path), QByteArray("# Essay\n\nFirst words.\n"));
}

void printingGoesThroughThePortal()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString path = writeRich(canonical(files.path()));
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1000);
    Exports *exports = shell.backend->exports();
    exports->setRenderOptions(fakeHelpers(files.path() + QStringLiteral("/work")));

    // Without the desktop's printing, it says so.
    exports->print(shell.document(), {});
    QCOMPARE(shell.backend->problem(), QStringLiteral("No printing"));

    FakePrinter printer;
    shell.backend->setPrinter(&printer);
    QObject *dialog = shell.object(QStringLiteral("exportDialog"));
    QVERIFY(run(window, "print"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(dialog->property("mode").toString(), QStringLiteral("print"));
    // The print dialog chooses the paper; the type and margins are chosen here.
    QVERIFY(!shell.item(QStringLiteral("exportFormat"))->isVisible());
    QVERIFY(!shell.item(QStringLiteral("exportOption-page-size")));
    QVERIFY(shell.item(QStringLiteral("exportOption-margins")));
    QCOMPARE(shell.object(QStringLiteral("exportGo"))->property("text").toString(), QStringLiteral("Print…"));
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    QMetaObject::invokeMethod(shell.object(QStringLiteral("exportGo")), "clicked");
    QTRY_COMPARE(printer.prepared.size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!printer.pdf.isEmpty(), 30000);
    QCOMPARE(printer.token, quint32(7));
    QVERIFY(printer.pdf.startsWith("%PDF-"));
    // On the paper the dialog chose: Letter, on its side.
    QCOMPARE(mediaBox(printer.pdf).toSize(), QSize(792, 612));
    QTRY_COMPARE(shell.backend->problem(), QStringLiteral("Sent to the printer"));
    // A print dialog that fails says why.
    printer.refuse = true;
    exports->print(shell.document(), {});
    QTRY_COMPARE(shell.backend->problem(), QStringLiteral("Not printed"));
    QCOMPARE(shell.backend->problemDetail(), QStringLiteral("no printer here"));
    // The page setup the portal describes.
    const Printer::Page page = Printer::pageOf({{QStringLiteral("Width"), 297.0}, {QStringLiteral("Height"), 210.0},
                                                {QStringLiteral("Orientation"), QStringLiteral("reverse_landscape")},
                                                {QStringLiteral("MarginLeft"), 6.35}});
    QCOMPARE(page.size, QSizeF(210, 297));
    QVERIFY(page.landscape);
    QCOMPARE(page.margin, 6.35);
}

namespace {

// Lays the document out in the preview and waits for it.
bool previewed(PrintPreview *preview, Document *document, const QString &mode, const QVariantMap &options)
{
    const int before = preview->revision();
    preview->show(document, mode, options);
    return QTest::qWaitFor([&] { return preview->revision() > before && !preview->busy(); }, 30000);
}

} // namespace

void printPreviewShowsThePagesThePdfHas()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonical(files.path());
    const QString path = writeRich(root);
    // The fixture, with enough text after it to fill pages of every size.
    QByteArray text = readFile(path);
    for (int i = 1; i <= 40; ++i)
        text += "\nA closing paragraph, number " + QByteArray::number(i) +
                ", long enough to wrap across the column and so to fill the page it is set on.\n";
    writeFile(path, text);
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1000);
    Exports *exports = shell.backend->exports();
    exports->setRenderOptions(fakeHelpers(root + QStringLiteral("/work")));
    PrintPreview *preview = exports->preview();
    Document *document = shell.document();

    // As many pages as the PDF has, for each page size, and fewer as the paper grows.
    QMap<QString, int> counts;
    for (const char *size : {"A5", "A4", "A3"}) {
        const QVariantMap options{{QStringLiteral("page-size"), QString::fromLatin1(size)}};
        QVERIFY2(previewed(preview, document, QStringLiteral("pdf"), options), size);
        QVERIFY(preview->pageCount() > 0);
        const QString output = root + QStringLiteral("/out-") + QString::fromLatin1(size) + QStringLiteral(".pdf");
        exports->exportTo(document, QStringLiteral("pdf"), options, output);
        QTRY_VERIFY_WITH_TIMEOUT(!exports->busy() && QFileInfo::exists(output), 30000);
        QCOMPARE(pagesIn(readFile(output)), preview->pageCount());
        counts.insert(QString::fromLatin1(size), preview->pageCount());
    }
    QVERIFY2(counts[QStringLiteral("A5")] > counts[QStringLiteral("A4")], qPrintable(QString::number(counts[QStringLiteral("A5")])));
    QVERIFY(counts[QStringLiteral("A4")] >= counts[QStringLiteral("A3")]);
    QVERIFY(qAbs(preview->pageAspect() - 420.0 / 297.0) < 0.01);
    // Turned on its side, and in the compact template.
    QVERIFY(previewed(preview, document, QStringLiteral("pdf"), {{QStringLiteral("landscape"), true}}));
    QVERIFY(preview->pageAspect() < 1);
    QVERIFY(previewed(preview, document, QStringLiteral("pdf"), {{QStringLiteral("template"), QStringLiteral("compact")}}));
    QVERIFY(preview->pageCount() <= counts[QStringLiteral("A4")]);
    QCOMPARE(exports->paperDefaults(QStringLiteral("compact")).value(QStringLiteral("margins")).toDouble(), 15.0);
    // A page drawn as a picture, off the UI thread, white paper with dark text on it.
    QVERIFY(previewed(preview, document, QStringLiteral("pdf"), {}));
    QSignalSpy ready(preview, &PrintPreview::pageReady);
    preview->requestPage(0, 300);
    QVERIFY(ready.wait(30000));
    const QImage first = preview->page(0);
    QCOMPARE(first.width(), 300);
    QCOMPARE(first.height(), qRound(300 * 297.0 / 210.0));
    QVERIFY(pixelsNear(first, Qt::white, 30) > first.width() * first.height() / 2);
    QVERIFY(pixelsNear(first, QColor(26, 26, 26), 120) > 50);

    // In the dialog: shown for PDF and for printing, page by page from the keyboard.
    QVERIFY(run(window, "export"));
    QObject *dialog = shell.object(QStringLiteral("exportDialog"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QQuickItem *section = shell.item(QStringLiteral("printPreview"));
    QVERIFY(section);
    QTRY_VERIFY(section->isVisible());
    QQuickItem *label = shell.item(QStringLiteral("previewPageLabel"));
    QTRY_VERIFY_WITH_TIMEOUT(label->property("text").toString().startsWith(QStringLiteral("Page 1 of ")), 30000);
    const int pages = preview->pageCount();
    QCOMPARE(label->property("text").toString(), QStringLiteral("Page 1 of %1").arg(pages));
    QQuickItem *list = shell.item(QStringLiteral("previewPages"));
    QVERIFY(list);
    QCOMPARE(list->property("count").toInt(), pages);
    QTRY_VERIFY_WITH_TIMEOUT(shell.item(QStringLiteral("previewPage")) &&
                                 shell.item(QStringLiteral("previewPage"))->property("ready").toBool(),
                             30000);
    QTest::qWait(50);
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    bool reached = false;
    for (int i = 0; i < 30 && !reached; ++i) {
        QTest::keyClick(window, Qt::Key_Tab);
        reached = list->hasActiveFocus();
    }
    QVERIFY(reached);
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("Page 2 of %1").arg(pages));
    QTest::keyClick(window, Qt::Key_End);
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("Page %1 of %1").arg(pages));
    // A change of the paper lays the pages out again.
    QMetaObject::invokeMethod(dialog, "setValue", Q_ARG(QVariant, QStringLiteral("page-size")), Q_ARG(QVariant, QStringLiteral("A5")));
    QTRY_VERIFY_WITH_TIMEOUT(!preview->busy() && preview->pageCount() == counts[QStringLiteral("A5")], 30000);
    // Not for a format that has no pages.
    QMetaObject::invokeMethod(shell.item(QStringLiteral("exportFormat")), "activated", Q_ARG(int, 3));
    QTRY_VERIFY(!section->isVisible());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QTRY_COMPARE(preview->pageCount(), 0);
}

namespace {

// Whether all of an item is in sight: inside the frame, and inside every
// item that clips it.
bool inSight(QQuickItem *item, QQuickItem *frame)
{
    if (!item || !frame || !item->isVisible()) return false;
    const QRectF drawn = item->mapRectToScene(QRectF(0, 0, item->width(), item->height())).adjusted(0.5, 0.5, -0.5, -0.5);
    if (!frame->mapRectToScene(QRectF(0, 0, frame->width(), frame->height())).contains(drawn)) return false;
    for (QQuickItem *at = item->parentItem(); at; at = at->parentItem())
        if (at->clip() && !at->mapRectToScene(at->clipRect()).contains(drawn)) return false;
    return true;
}

} // namespace

void exportDialogShowsItsOptionsBesideThePages()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonical(files.path());
    const QString path = writeRich(root);
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->exports()->setRenderOptions(fakeHelpers(root + QStringLiteral("/work")));
    const QStringList asked{QStringLiteral("exportFormat"), QStringLiteral("exportOption-page-size"),
                            QStringLiteral("exportOption-margins"), QStringLiteral("exportOption-font"),
                            QStringLiteral("exportOption-template")};

    // From 960 by 600 up, the format and every option of a PDF are in sight
    // beside its pages, with nothing to scroll.
    for (const QSize size : {QSize(960, 600), QSize(1280, 800)}) {
        window->resize(size);
        QTRY_COMPARE(window->contentItem()->size(), QSizeF(size));
        QVERIFY(run(window, "export"));
        QObject *dialog = shell.object(QStringLiteral("exportDialog"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(dialog->property("format").toString(), QStringLiteral("pdf"));
        QVERIFY(dialog->property("sideBySide").toBool());
        auto *frame = dialog->property("background").value<QQuickItem *>();
        QTRY_VERIFY_WITH_TIMEOUT(shell.item(QStringLiteral("previewPage")) &&
                                     shell.item(QStringLiteral("previewPage"))->property("ready").toBool(),
                                 30000);
        QTest::qWait(50);
        QQuickItem *body = shell.item(QStringLiteral("exportBody"));
        QVERIFY(body->property("contentHeight").toReal() <= body->height());
        QStringList shown;
        for (QQuickItem *control : visibleItems(window)) {
            const QString name = control->objectName();
            if (name != u"exportFormat" && !name.startsWith(u"exportOption-")) continue;
            QVERIFY2(inSight(control, frame), qPrintable(name));
            shown.append(name);
        }
        for (const QString &name : asked) QVERIFY2(shown.contains(name), qPrintable(name + u' ' + shown.join(u' ')));
        QVERIFY(inSight(shell.item(QStringLiteral("previewPage")), frame));
        QVERIFY(inSight(shell.item(QStringLiteral("exportLimitations")), frame));
        QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
        QStringList problems;
        QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
        // The keys reach the pages after the options.
        QQuickItem *list = shell.item(QStringLiteral("previewPages"));
        bool reached = false;
        for (int i = 0; i < 30 && !reached; ++i) {
            QTest::keyClick(window, Qt::Key_Tab);
            reached = list->hasActiveFocus();
        }
        QVERIFY(reached);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("opened").toBool());
    }

    // Narrower, the pages follow the options and scroll with them, and come
    // into sight as the keys reach them.
    window->resize(QSize(700, 600));
    QTRY_COMPARE(window->contentItem()->size(), QSizeF(700, 600));
    QVERIFY(run(window, "export"));
    QObject *dialog = shell.object(QStringLiteral("exportDialog"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(!dialog->property("sideBySide").toBool());
    auto *frame = dialog->property("background").value<QQuickItem *>();
    QTRY_VERIFY_WITH_TIMEOUT(shell.item(QStringLiteral("previewPage")) &&
                                 shell.item(QStringLiteral("previewPage"))->property("ready").toBool(),
                             30000);
    QVERIFY(inSight(shell.item(QStringLiteral("exportFormat")), frame));
    QQuickItem *list = shell.item(QStringLiteral("previewPages"));
    QVERIFY(!inSight(list, frame));
    bool reached = false;
    for (int i = 0; i < 30 && !reached; ++i) {
        QTest::keyClick(window, Qt::Key_Tab);
        reached = list->hasActiveFocus();
    }
    QVERIFY(reached);
    QTRY_VERIFY(inSight(list, frame));
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());
}

void cliExportsEveryFormat()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    writeRich(folder.path());
    // No helpers: what the terminal writes does not hang on what is installed.
    QDir().mkpath(folder.path() + QStringLiteral("/no-helpers"));
    const QByteArray before = qgetenv("PARCH_HELPERS");
    qputenv("PARCH_HELPERS", QFile::encodeName(folder.path() + QStringLiteral("/no-helpers")));
    const auto restore = qScopeGuard([&] {
        if (before.isNull()) qunsetenv("PARCH_HELPERS");
        else qputenv("PARCH_HELPERS", before);
    });
    for (const char *format : {"pdf", "docx", "epub"}) {
        const QString output = QStringLiteral("rich.") + QString::fromLatin1(format);
        const Ran ran = runCli(folder.path(), {QStringLiteral("export"), QStringLiteral("rich.md"), QStringLiteral("-o"), output,
                                               QStringLiteral("--json")});
        QVERIFY2(ran.code == 0, qPrintable(ran.out + ran.err));
        const QJsonObject report = QJsonDocument::fromJson(ran.out.toUtf8()).object();
        QCOMPARE(report.value(QStringLiteral("format")).toString(), QString::fromLatin1(format));
        QVERIFY(report.value(QStringLiteral("renderers")).toObject().contains(QString::fromLatin1(format)));
        QCOMPARE(report.value(QStringLiteral("helpers")).toObject(), QJsonObject());
        const QString limitations = report.value(QStringLiteral("limitations")).toVariant().toStringList().join(u'\n');
        QVERIFY2(limitations.contains(QStringLiteral("the math helper (render-svg) is not installed")), qPrintable(limitations));
        QVERIFY(QFileInfo(folder.path() + u'/' + output).size() > 1000);
    }
    QVERIFY(readFile(folder.path() + QStringLiteral("/rich.pdf")).startsWith("%PDF-"));
    // Each format's own options, checked before anything is read.
    const Ran tabloid = runCli(folder.path(), {QStringLiteral("export"), QStringLiteral("rich.md"), QStringLiteral("-o"),
                                               QStringLiteral("a.pdf"), QStringLiteral("--page-size"), QStringLiteral("Tabloid")});
    QCOMPARE(tabloid.code, 1);
    QVERIFY(tabloid.err.contains(QStringLiteral("--page-size takes A4, Letter, A5, Legal, A3, B5, not \"Tabloid\"")));
    const Ran notes = runCli(folder.path(), {QStringLiteral("export"), QStringLiteral("rich.md"), QStringLiteral("-o"),
                                             QStringLiteral("a.pdf"), QStringLiteral("--notes"), QStringLiteral("endnotes")});
    QCOMPARE(notes.code, 1);
    QVERIFY2(notes.err.contains(QStringLiteral("--notes is an option of DOCX export, not PDF")), qPrintable(notes.err));
    const Ran margins = runCli(folder.path(), {QStringLiteral("export"), QStringLiteral("rich.md"), QStringLiteral("-o"),
                                               QStringLiteral("a.pdf"), QStringLiteral("--margins"), QStringLiteral("2")});
    QCOMPARE(margins.code, 1);
    QVERIFY(margins.err.contains(QStringLiteral("--margins takes a number from 5 to 60, not \"2\"")));
    const Ran images = runCli(folder.path(), {QStringLiteral("export"), QStringLiteral("rich.md"), QStringLiteral("-o"),
                                              QStringLiteral("a.md"), QStringLiteral("--images"), QStringLiteral("embed")});
    QCOMPARE(images.code, 1);
    QVERIFY(images.err.contains(QStringLiteral("--images takes link, copy, not \"embed\"")));
    // A shared option is one option; its help names every format that has it.
    const Ran help = runCli(folder.path(), {QStringLiteral("help"), QStringLiteral("export")});
    QVERIFY(help.out.contains(QStringLiteral("--title TEXT")));
    QVERIFY(help.out.contains(QStringLiteral("HTML, PDF, DOCX, EPUB: The document's title")));
    QVERIFY(help.out.contains(QStringLiteral("PDF, DOCX: The paper, A4, Letter")));
    QVERIFY(!help.out.contains(QStringLiteral("not available yet")));
    QVERIFY(QFileInfo::exists(folder.path() + QStringLiteral("/rich.md")));
}

} // namespace ExportTests
