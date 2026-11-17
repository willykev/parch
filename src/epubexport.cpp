#include "epubexport.h"

#include "exportpictures.h"
#include "exportweb.h"
#include "htmlexport.h"
#include "mdhtml.h"
#include "svgpaper.h"
#include "zippackage.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUuid>

#include <functional>

namespace {

struct Counts {
    int formulas = 0;
    int diagrams = 0;
    int remoteImages = 0;
    int pageBreaks = 0;
};

void count(const md::Document &document, const md::Node &node, Counts &counts)
{
    switch (node.type) {
    case md::NodeType::MathInline:
    case md::NodeType::MathBlock: ++counts.formulas; break;
    case md::NodeType::MermaidBlock: ++counts.diagrams; break;
    case md::NodeType::PageBreak: ++counts.pageBreaks; break;
    case md::NodeType::HtmlElement:
        if (node.label != u"img") break;
        [[fallthrough]];
    case md::NodeType::Image:
        if (md::classifyDestination(document, node).kind == md::Destination::Kind::Remote) ++counts.remoteImages;
        break;
    default: break;
    }
    for (const md::Node &child : node.children) count(document, child, counts);
}

bool missing(const QString &helper) { return helper.isEmpty() || !QFileInfo(helper).isExecutable(); }

QString escaped(const QString &text) { return md::escapeHtml(text); }

QString styleSheet()
{
    QFile css(QStringLiteral(":/export.css"));
    QString sheet = css.open(QIODevice::ReadOnly) ? QString::fromUtf8(css.readAll()) : QString();
    // A reader sets its own page: the text fills it.
    sheet += QStringLiteral("\n/* In a book */\nbody { background: none; }\nmain { max-width: none; padding: 0 0.5em; }\n"
                            "span.task { font-family: sans-serif; }\nspan.image-missing { font-style: italic; color: "
                            "var(--muted); }\n");
    return sheet;
}

// The headings as nested lists: each under the nearest heading above it of
// a lower level.
struct Node {
    const md::Heading *heading = nullptr;
    int level = 0;
    std::vector<Node> children;
};

using FileOf = std::function<QString(const QString &id)>;

void list(const std::vector<Node> &nodes, const FileOf &fileOf, QString &out)
{
    out += QStringLiteral("<ol>\n");
    for (const Node &node : nodes) {
        const QString title = node.heading->text.simplified();
        out += QStringLiteral("<li><a href=\"%1#%2\">%3</a>")
                   .arg(fileOf(node.heading->slug), escaped(node.heading->slug),
                        escaped(title.isEmpty() ? node.heading->slug : title));
        if (!node.children.empty()) {
            out += u'\n';
            list(node.children, fileOf, out);
        }
        out += QStringLiteral("</li>\n");
    }
    out += QStringLiteral("</ol>\n");
}

QString outline(const md::Document &document, const FileOf &fileOf, const QString &first, const QString &title)
{
    Node root;
    std::vector<Node *> path{&root};
    for (const md::Heading &heading : document.headings) {
        while (path.size() > 1 && path.back()->level >= heading.level) path.pop_back();
        path.back()->children.push_back({&heading, heading.level, {}});
        path.push_back(&path.back()->children.back());
    }
    if (root.children.empty())
        return QStringLiteral("<ol>\n<li><a href=\"%1\">%2</a></li>\n</ol>\n").arg(first, escaped(title));
    QString out;
    list(root.children, fileOf, out);
    return out;
}

QString isoDate(const QString &written)
{
    const QDate date = QDate::fromString(written.left(10), Qt::ISODate);
    return date.isValid() ? date.toString(Qt::ISODate) : QString();
}

} // namespace

QList<ExportOption> EpubExporter::options() const
{
    ExportOption title{QStringLiteral("title"),
                       QStringLiteral("The document's title (the default is the front matter's title, the first "
                                      "heading, or the file's name)"),
                       QStringLiteral("TEXT")};
    title.label = QStringLiteral("Title");
    return {title};
}

QStringList EpubExporter::limitations(const md::Document &document, const ExportRecipe &recipe,
                                      const HelperSet &helpers) const
{
    Q_UNUSED(recipe);
    Counts counts;
    count(document, document.root, counts);
    QStringList limitations;
    if (counts.formulas) {
        if (missing(helpers.ratex))
            limitations.append(QStringLiteral("Math is written as its TeX source (%1): the math helper (render-svg) is "
                                              "not installed")
                                   .arg(Exporters::count(counts.formulas, QStringLiteral("formula"), QStringLiteral("formulas"))));
        else
            limitations.append(QStringLiteral("Formulas are SVG pictures named by their TeX, not MathML (%1)")
                                   .arg(Exporters::count(counts.formulas, QStringLiteral("formula"), QStringLiteral("formulas"))));
    }
    if (counts.diagrams) {
        if (missing(helpers.merman))
            limitations.append(QStringLiteral("Mermaid diagrams are written as their source (%1): the diagram helper "
                                              "(merman-cli) is not installed")
                                   .arg(Exporters::count(counts.diagrams, QStringLiteral("diagram"), QStringLiteral("diagrams"))));
        else
            limitations.append(QStringLiteral("Diagrams are SVG pictures described by their words (%1)")
                                   .arg(Exporters::count(counts.diagrams, QStringLiteral("diagram"), QStringLiteral("diagrams"))));
    }
    if (counts.remoteImages)
        limitations.append(QStringLiteral("Images from the web are not packed into the book, so their descriptions "
                                          "stand in their place (%1)")
                               .arg(Exporters::count(counts.remoteImages, QStringLiteral("image"), QStringLiteral("images"))));
    if (counts.pageBreaks)
        limitations.append(QStringLiteral("Page breaks are hints: a reader sets its own pages (%1)")
                               .arg(Exporters::count(counts.pageBreaks, QStringLiteral("page break"), QStringLiteral("page breaks"))));
    return limitations;
}

QString EpubExporter::xhtml(const QString &html)
{
    QString out = html;
    // Alignment is style in XHTML.
    static const QRegularExpression align(QStringLiteral("(<t[dh]\\b[^>]*?)\\salign=\"(left|center|right)\""));
    out.replace(align, QStringLiteral("\\1 style=\"text-align: \\2\""));
    out.replace(QStringLiteral("<input checked=\"\" disabled=\"\" type=\"checkbox\" />"),
                QStringLiteral("<span class=\"task\" role=\"img\" aria-label=\"done\">\u2611</span>"));
    out.replace(QStringLiteral("<input disabled=\"\" type=\"checkbox\" />"),
                QStringLiteral("<span class=\"task\" role=\"img\" aria-label=\"to do\">\u2610</span>"));
    // Characters XML does not allow.
    for (QChar &c : out) {
        const char16_t u = c.unicode();
        if ((u < 0x20 && u != u'\t' && u != u'\n' && u != u'\r') || u == 0xfffe || u == 0xffff) c = QChar(0xfffd);
    }
    return out;
}

QList<EpubExporter::Chapter> EpubExporter::chapters(const QString &body, const md::Document &document)
{
    const QString single = QStringLiteral("text.xhtml");
    // Headings of level 1 at the top level of the text, included files' too;
    // one inside a quote or a list does not count.
    std::function<int(const md::Node &)> topLevelCount = [&](const md::Node &node) {
        int count = 0;
        for (const md::Node &child : node.children) {
            if (child.type == md::NodeType::Heading && child.level == 1) ++count;
            else if (child.type == md::NodeType::Include) count += topLevelCount(child);
        }
        return count;
    };
    const int topLevel = topLevelCount(document.root);
    const QString tag = topLevel >= 2 ? QStringLiteral("h1") : QStringLiteral("h2");
    // Where the headings that start a file begin: at the top level of the
    // text only, never inside a quote, a list or a note. The text is XHTML
    // the renderer wrote, so its tags are found by their brackets: every
    // `<` and `>` within text and attribute values is escaped.
    QList<qsizetype> starts;
    int depth = 0;
    for (qsizetype at = body.indexOf(u'<'); at >= 0; at = body.indexOf(u'<', at)) {
        if (QStringView(body).sliced(at).startsWith(QLatin1String("<!--"))) {
            at = body.indexOf(QLatin1String("-->"), at);
            if (at < 0) break;
            continue;
        }
        const qsizetype close = body.indexOf(u'>', at);
        if (close < 0) break;
        const QStringView inside = QStringView(body).sliced(at + 1, close - at - 1);
        if (inside.startsWith(u'/')) {
            --depth;
        } else if (!inside.startsWith(u'!') && !inside.startsWith(u'?')) {
            qsizetype length = 0;
            while (length < inside.size() && !inside[length].isSpace() && inside[length] != u'/') ++length;
            if (depth == 0 && inside.first(length) == tag) starts.append(at);
            if (!inside.endsWith(u'/')) ++depth;
        }
        at = close + 1;
    }
    if (depth != 0 || starts.isEmpty()) return {{single, body}};
    // What stands before the first is a file of its own when it shows anything.
    if (!body.left(starts.first()).trimmed().isEmpty()) starts.prepend(0);
    else starts.first() = 0;
    QList<Chapter> chapters;
    for (qsizetype i = 0; i < starts.size(); ++i) {
        const qsizetype end = i + 1 < starts.size() ? starts[i + 1] : body.size();
        chapters.append({QStringLiteral("chapter-%1.xhtml").arg(i + 1), body.mid(starts[i], end - starts[i])});
    }
    if (chapters.size() == 1) return {{single, body}};
    // Every target by the file that holds it, and every link to one in
    // another file led there.
    static const QRegularExpression id(QStringLiteral("\\sid=\"([^\"]+)\""));
    QHash<QString, QString> files;
    for (const Chapter &chapter : std::as_const(chapters))
        for (auto match = id.globalMatch(chapter.html); match.hasNext();) files.insert(match.next().captured(1), chapter.name);
    static const QRegularExpression link(QStringLiteral("href=\"#([^\"]*)\""));
    for (Chapter &chapter : chapters) {
        QString out;
        qsizetype at = 0;
        for (auto match = link.globalMatch(chapter.html); match.hasNext();) {
            const QRegularExpressionMatch found = match.next();
            const QString file = files.value(found.captured(1));
            if (file.isEmpty() || file == chapter.name) continue;
            out += QStringView(chapter.html).sliced(at, found.capturedStart() - at);
            out += QStringLiteral("href=\"%1#%2\"").arg(file, found.captured(1));
            at = found.capturedEnd();
        }
        out += QStringView(chapter.html).sliced(at);
        chapter.html = out;
    }
    return chapters;
}

ExportResult EpubExporter::write(const md::Document &document, const ExportRecipe &recipe, const ExportRun &run) const
{
    bool stopped = false;
    const std::unique_ptr<ExportPictures> pictures = Exporters::drawPictures(document, run, 16, 0.8, &stopped);
    if (stopped) return Exporters::cancelled();
    run.report(0.85, QStringLiteral("Packing the book"));

    ExportResult result;
    ExportWeb::Assets assets(ExportWeb::Assets::Mode::Package, QStringLiteral("images/"),
                             QFileInfo(recipe.output).absolutePath());
    const ExportWeb::Body body = ExportWeb::body(document, recipe, pictures.get(), assets, true);
    const QString title = Exporters::title(document, recipe);
    QString language = Exporters::language(document);
    if (language.isEmpty()) language = QStringLiteral("en");
    const QString author = Exporters::frontMatterText(document, QStringLiteral("author"));
    const QString description = Exporters::frontMatterText(document, QStringLiteral("description"));
    const QString date = isoDate(Exporters::frontMatterText(document, QStringLiteral("date")));
    QString identifier = Exporters::frontMatterText(document, QStringLiteral("identifier"));
    if (identifier.isEmpty()) identifier = Exporters::frontMatterText(document, QStringLiteral("isbn"));
    if (identifier.isEmpty())
        identifier = QStringLiteral("urn:uuid:") +
                     QUuid::createUuidV5(QUuid(QStringLiteral("{6f1b0d2e-5a4c-4b8e-9d0a-70617263682e}")),
                                         recipe.source.isEmpty() ? title : recipe.source)
                         .toString(QUuid::WithoutBraces);
    const QString head = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n"
                                        "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
                                        "xmlns:epub=\"http://www.idpf.org/2007/ops\" lang=\"%1\" xml:lang=\"%1\">\n"
                                        "<head>\n<meta charset=\"utf-8\" />\n<title>%2</title>\n"
                                        "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\" />\n</head>\n")
                             .arg(escaped(language), escaped(title));
    // One file for each top-level section, in reading order.
    const QList<Chapter> parts = chapters(xhtml(body.html), document);
    QHash<QString, QString> fileOfId;
    static const QRegularExpression id(QStringLiteral("\\sid=\"([^\"]+)\""));
    for (const Chapter &part : parts)
        for (auto match = id.globalMatch(part.html); match.hasNext();) fileOfId.insert(match.next().captured(1), part.name);
    const QString nav = head + QStringLiteral("<body>\n<nav epub:type=\"toc\" id=\"toc\">\n<h1>%1</h1>\n").arg(escaped(title)) +
                        outline(document, [&](const QString &slug) { return fileOfId.value(slug, parts.first().name); },
                                parts.first().name, title) +
                        QStringLiteral("</nav>\n</body>\n</html>\n");

    QString manifest, spine;
    manifest += QStringLiteral("<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n");
    QList<std::pair<QString, QString>> texts;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const QString text = head + QStringLiteral("<body>\n<section epub:type=\"bodymatter\">\n") + parts[i].html +
                             QStringLiteral("</section>\n</body>\n</html>\n");
        const QString item = QStringLiteral("text-%1").arg(i + 1);
        manifest += QStringLiteral("<item id=\"%1\" href=\"%2\" media-type=\"application/xhtml+xml\"%3/>\n")
                        .arg(item, parts[i].name,
                             text.contains(QLatin1String("<svg")) ? QStringLiteral(" properties=\"svg\"") : QString());
        spine += QStringLiteral("<itemref idref=\"%1\"/>\n").arg(item);
        texts.append({parts[i].name, text});
    }
    manifest += QStringLiteral("<item id=\"style\" href=\"style.css\" media-type=\"text/css\"/>\n");
    int n = 0;
    for (const ExportWeb::Assets::Asset &asset : assets.assets())
        manifest += QStringLiteral("<item id=\"image-%1\" href=\"images/%2\" media-type=\"%3\"/>\n")
                        .arg(++n)
                        .arg(escaped(asset.name), asset.mediaType);
    QString metadata;
    metadata += QStringLiteral("<dc:identifier id=\"book-id\">%1</dc:identifier>\n").arg(escaped(identifier));
    metadata += QStringLiteral("<dc:title>%1</dc:title>\n").arg(escaped(title));
    metadata += QStringLiteral("<dc:language>%1</dc:language>\n").arg(escaped(language));
    if (!author.isEmpty()) metadata += QStringLiteral("<dc:creator>%1</dc:creator>\n").arg(escaped(author));
    if (!description.isEmpty()) metadata += QStringLiteral("<dc:description>%1</dc:description>\n").arg(escaped(description));
    if (!date.isEmpty()) metadata += QStringLiteral("<dc:date>%1</dc:date>\n").arg(date);
    metadata += QStringLiteral("<meta property=\"dcterms:modified\">%1</meta>\n")
                    .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ")));
    metadata += QStringLiteral("<meta name=\"generator\" content=\"parch %1\"/>\n").arg(QStringLiteral(PARCH_VERSION));
    const QString package = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                           "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
                                           "unique-identifier=\"book-id\" xml:lang=\"%1\">\n"
                                           "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n%2</metadata>\n"
                                           "<manifest>\n%3</manifest>\n<spine>\n%4</spine>\n"
                                           "</package>\n")
                                .arg(escaped(language), metadata, manifest, spine);
    const QByteArray container = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                 "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
                                 "<rootfiles>\n<rootfile full-path=\"OEBPS/content.opf\" "
                                 "media-type=\"application/oebps-package+xml\"/>\n</rootfiles>\n</container>\n";

    ZipPackage zip;
    zip.add(QStringLiteral("mimetype"), "application/epub+zip", false);
    zip.add(QStringLiteral("META-INF/container.xml"), container);
    zip.add(QStringLiteral("OEBPS/content.opf"), package.toUtf8());
    zip.add(QStringLiteral("OEBPS/nav.xhtml"), nav.toUtf8());
    for (const auto &[name, text] : std::as_const(texts)) zip.add(QStringLiteral("OEBPS/") + name, text.toUtf8());
    zip.add(QStringLiteral("OEBPS/style.css"), styleSheet().toUtf8());
    for (const ExportWeb::Assets::Asset &asset : assets.assets())
        zip.add(QStringLiteral("OEBPS/images/") + asset.name, asset.bytes, asset.mediaType != u"image/png" &&
                                                                              asset.mediaType != u"image/jpeg");
    if (run.stopped()) return Exporters::cancelled();
    QString error;
    const QByteArray bytes = zip.bytes(&error);
    if (bytes.isEmpty()) {
        result.error = QStringLiteral("the book could not be packed: %1").arg(error);
        return result;
    }

    // What the book does not carry as the document has it.
    ExportWeb::pictureLimitations(document, pictures.get(), &result.limitations);
    Counts counts;
    count(document, document.root, counts);
    if (pictures) {
        const ExportPictures::Counts drawn = pictures->counts(document);
        if (const int formulas = drawn.formulas - drawn.formulasNotDrawn; formulas > 0)
            result.limitations.append(QStringLiteral("Formulas are SVG pictures named by their TeX, not MathML (%1)")
                                          .arg(Exporters::count(formulas, QStringLiteral("formula"), QStringLiteral("formulas"))));
        if (const int diagrams = drawn.diagrams - drawn.diagramsNotDrawn; diagrams > 0)
            result.limitations.append(QStringLiteral("Diagrams are SVG pictures described by their words (%1)")
                                          .arg(Exporters::count(diagrams, QStringLiteral("diagram"), QStringLiteral("diagrams"))));
        result.helpers = pictures->helperReport();
        result.renderers.insert(QStringLiteral("svg"), SvgPaper::version());
    }
    if (body.remoteImages)
        result.limitations.append(QStringLiteral("Images from the web are not packed into the book, so their "
                                                 "descriptions stand in their place (%1)")
                                      .arg(Exporters::count(body.remoteImages, QStringLiteral("image"), QStringLiteral("images"))));
    if (!body.unreadImages.isEmpty())
        result.limitations.append(QStringLiteral("Images that could not be read are left out: %1")
                                      .arg(body.unreadImages.join(QStringLiteral(", "))));
    if (counts.pageBreaks)
        result.limitations.append(QStringLiteral("Page breaks are hints: a reader sets its own pages (%1)")
                                      .arg(Exporters::count(counts.pageBreaks, QStringLiteral("page break"),
                                                            QStringLiteral("page breaks"))));
    result.renderers.insert(QStringLiteral("epub"), QStringLiteral("parch " PARCH_VERSION));
    if (!Exporters::writeFile(recipe.output, bytes, &result.error, recipe.existing)) {
        result.ioFailure = true;
        return result;
    }
    run.report(1, QStringLiteral("Written"));
    result.written.append(recipe.output);
    result.ok = true;
    return result;
}
