#include "docxexport.h"

#include "exportpictures.h"
#include "exportweb.h"
#include "htmlexport.h"
#include "mdparser_p.h"
#include "paperdocument.h"
#include "saferead.h"
#include "svgpaper.h"
#include "zippackage.h"

#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QSet>

#include <cmath>
#include <functional>

using md::Node;
using md::NodeType;

namespace {

// A pixel at 96 to the inch in English Metric Units, and a point in twips.
constexpr qint64 kEmuPerPixel = 9525;
constexpr int kTwipsPerPixel = 15;

QString escaped(QStringView text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        switch (u) {
        case u'&': out += QStringLiteral("&amp;"); break;
        case u'<': out += QStringLiteral("&lt;"); break;
        case u'>': out += QStringLiteral("&gt;"); break;
        case u'"': out += QStringLiteral("&quot;"); break;
        default:
            // Characters XML refuses.
            if ((u < 0x20 && u != u'\t' && u != u'\n' && u != u'\r') || u == 0xfffe || u == 0xffff) out += QChar(0xfffd);
            else out += c;
        }
    }
    return out;
}

// What a run of text looks like.
struct Look {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool highlight = false;
    bool code = false;
    bool underline = false;
    bool smallCaps = false;
    int script = 0; // 1 above the line, -1 below
    bool linked = false;
};

// What a paragraph looks like.
struct Para {
    QString style;
    bool keepNext = false;
    int numId = 0;
    int level = 0;
    bool rule = false;      // a line beneath it: a thematic break
    int left = -1;          // its indent, in twips; -1 leaves the style's
    int right = -1;
    int hanging = 0;
    QString align;          // "center", "right"
    bool pageBreakBefore = false;
};

// Where blocks are written: the paragraph style they take and how far in
// they are set.
struct Flow {
    QString style = QStringLiteral("Normal");
    int quote = 0;      // quotes it is inside
    int listLevel = -1; // the list level it is inside
    int indent = 0;     // twips, from lists and definitions
    bool toc = false;   // the generated table of contents
};

struct Relationship {
    QString id;
    QString type;
    QString target;
    bool external = false;
};

const QString kHyperlinkType = QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink");
const QString kImageType = QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/image");

// A part of the package with its own relationships: the document, its footnotes or its endnotes.
struct Part {
    QString xml;
    QList<Relationship> relationships;
    int next = 1;

    QString relate(const QString &type, const QString &target, bool external)
    {
        for (const Relationship &r : std::as_const(relationships))
            if (r.type == type && r.target == target) return r.id;
        const QString id = QStringLiteral("rId%1").arg(next++);
        relationships.append({id, type, target, external});
        return id;
    }

    QByteArray relationshipsXml() const
    {
        QString out = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                                     "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
        for (const Relationship &r : relationships)
            out += QStringLiteral("<Relationship Id=\"%1\" Type=\"%2\" Target=\"%3\"%4/>")
                       .arg(r.id, r.type, escaped(r.target), r.external ? QStringLiteral(" TargetMode=\"External\"") : QString());
        out += QStringLiteral("</Relationships>");
        return out.toUtf8();
    }
};

struct Media {
    QString name; // "image3.png"
    QByteArray bytes;
};

class Writer {
public:
    Writer(const md::Document &document, const ExportRecipe &recipe, const ExportPictures *pictures, bool endnotes,
           const QPageSize &page)
        : m_doc(document), m_recipe(recipe), m_pictures(pictures), m_endnotes(endnotes), m_page(page),
          m_citations(recipe.citations && recipe.citations->active ? recipe.citations.get() : nullptr),
          m_destinations(HtmlExporter::destinationsFrom(document, QFileInfo(recipe.output).absolutePath()))
    {
        // Relationships the document part always has, first.
        m_body.next = 1;
        m_body.relate(QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles"),
                      QStringLiteral("styles.xml"), false);
        m_body.relate(QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering"),
                      QStringLiteral("numbering.xml"), false);
        m_body.relate(QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/footnotes"),
                      QStringLiteral("footnotes.xml"), false);
        m_body.relate(QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/endnotes"),
                      QStringLiteral("endnotes.xml"), false);
        m_body.relate(QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings"),
                      QStringLiteral("settings.xml"), false);
        for (const md::Heading &heading : document.headings) bookmark(heading.slug);
        // In a note style, the citations that are notes, by node.
        for (const md::Footnote &note : document.footnotes)
            if (note.content && note.content->type == NodeType::Citation) m_citationNotes.insert(note.content, note.number);
        const QSizeF size = page.size(QPageSize::Millimeter);
        // The width text has between margins of an inch, in pixels.
        m_textWidth = (size.width() / 25.4 - 2) * 96;
    }

    QByteArray package(QString *error);

    QStringList limitations;
    int formulasAsPictures = 0;
    int diagramsAsPictures = 0;
    int remoteImages = 0;
    int notesReferredAgain = 0;
    QStringList unreadImages;

private:
    QString &out() { return m_part->xml; }

    // --- Names ---------------------------------------------------------------
    QString bookmark(const QString &anchor)
    {
        if (const auto known = m_bookmarks.constFind(anchor); known != m_bookmarks.cend()) return *known;
        QString name;
        for (const QChar c : anchor)
            name += (c.isLetterOrNumber() && c.unicode() < 128) ? c : u'_';
        name = QStringLiteral("h_") + name.left(30);
        QString unique = name;
        for (int n = 2; m_bookmarkNames.contains(unique); ++n) unique = name.left(34) + u'_' + QString::number(n);
        m_bookmarkNames.insert(unique);
        m_bookmarks.insert(anchor, unique);
        return unique;
    }

    void bookmarkStart(const QString &anchor)
    {
        out() += QStringLiteral("<w:bookmarkStart w:id=\"%1\" w:name=\"%2\"/><w:bookmarkEnd w:id=\"%1\"/>")
                     .arg(++m_bookmarkId)
                     .arg(bookmark(anchor));
    }

    // --- Paragraphs -----------------------------------------------------------
    void open(const Para &para)
    {
        QString properties;
        if (!para.style.isEmpty()) properties += QStringLiteral("<w:pStyle w:val=\"%1\"/>").arg(para.style);
        if (para.keepNext) properties += QStringLiteral("<w:keepNext/>");
        if (para.pageBreakBefore) properties += QStringLiteral("<w:pageBreakBefore/>");
        if (para.numId > 0)
            properties += QStringLiteral("<w:numPr><w:ilvl w:val=\"%1\"/><w:numId w:val=\"%2\"/></w:numPr>")
                              .arg(std::clamp(para.level, 0, 8))
                              .arg(para.numId);
        if (para.rule)
            properties += QStringLiteral("<w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" "
                                         "w:color=\"BFBFBF\"/></w:pBdr>");
        if (para.left >= 0 || para.right >= 0 || para.hanging > 0) {
            properties += QStringLiteral("<w:ind");
            if (para.left >= 0) properties += QStringLiteral(" w:left=\"%1\"").arg(para.left);
            if (para.right >= 0) properties += QStringLiteral(" w:right=\"%1\"").arg(para.right);
            if (para.hanging > 0) properties += QStringLiteral(" w:hanging=\"%1\"").arg(para.hanging);
            properties += QStringLiteral("/>");
        }
        if (!para.align.isEmpty()) properties += QStringLiteral("<w:jc w:val=\"%1\"/>").arg(para.align);
        out() += QStringLiteral("<w:p>");
        if (!properties.isEmpty()) out() += QStringLiteral("<w:pPr>") + properties + QStringLiteral("</w:pPr>");
        // A note's mark starts the note's first paragraph.
        if (!m_prefix.isEmpty()) out() += std::exchange(m_prefix, QString());
        ++m_paragraphs;
    }
    void close() { out() += QStringLiteral("</w:p>"); }

    Para para(const Flow &flow, const QString &style = {}) const
    {
        Para p;
        p.style = style.isEmpty() ? flow.style : style;
        const int indent = flow.indent + flow.quote * 720;
        if (indent > 0) p.left = indent + (p.style == u"Quote" ? 0 : 0);
        return p;
    }

    // --- Runs -----------------------------------------------------------------
    QString runProperties(const Look &look) const
    {
        QString p;
        if (look.code) p += QStringLiteral("<w:rStyle w:val=\"CodeChar\"/>");
        else if (look.linked) p += QStringLiteral("<w:rStyle w:val=\"Hyperlink\"/>");
        if (look.bold) p += QStringLiteral("<w:b/>");
        if (look.italic) p += QStringLiteral("<w:i/>");
        if (look.smallCaps) p += QStringLiteral("<w:smallCaps/>");
        if (look.strike) p += QStringLiteral("<w:strike/>");
        if (look.highlight) p += QStringLiteral("<w:highlight w:val=\"yellow\"/>");
        if (look.underline) p += QStringLiteral("<w:u w:val=\"single\"/>");
        if (look.script > 0) p += QStringLiteral("<w:vertAlign w:val=\"superscript\"/>");
        else if (look.script < 0) p += QStringLiteral("<w:vertAlign w:val=\"subscript\"/>");
        return p.isEmpty() ? QString() : QStringLiteral("<w:rPr>") + p + QStringLiteral("</w:rPr>");
    }

    void text(const QString &text, const Look &look)
    {
        if (text.isEmpty()) return;
        const QString properties = runProperties(look);
        QString run = QStringLiteral("<w:r>") + properties;
        qsizetype from = 0;
        const auto flush = [&](qsizetype to) {
            if (to > from)
                run += QStringLiteral("<w:t xml:space=\"preserve\">") + escaped(QStringView(text).sliced(from, to - from)) +
                       QStringLiteral("</w:t>");
        };
        for (qsizetype i = 0; i < text.size(); ++i) {
            const QChar c = text.at(i);
            if (c == u'\t' || c == u'\n' || c == QChar::LineSeparator) {
                flush(i);
                run += c == u'\t' ? QStringLiteral("<w:tab/>") : QStringLiteral("<w:br/>");
                from = i + 1;
            }
        }
        flush(text.size());
        out() += run + QStringLiteral("</w:r>");
    }

    void lineBreak() { out() += QStringLiteral("<w:r><w:br/></w:r>"); }

    // A picture in the text, `lower` pixels below the baseline.
    void picture(const QByteArray &png, QSizeF size, const QString &description, qreal lower = 0)
    {
        if (size.width() > m_textWidth) size = QSizeF(m_textWidth, size.height() * m_textWidth / size.width());
        const qreal tallest = m_textWidth * 1.3;
        if (size.height() > tallest) size = QSizeF(size.width() * tallest / size.height(), tallest);
        const QString name = QStringLiteral("image%1.png").arg(m_media.size() + 1);
        m_media.append({name, png});
        const QString id = m_part->relate(kImageType, QStringLiteral("media/") + name, false);
        const qint64 cx = std::max<qint64>(1, qint64(std::round(size.width() * kEmuPerPixel)));
        const qint64 cy = std::max<qint64>(1, qint64(std::round(size.height() * kEmuPerPixel)));
        const int number = ++m_drawings;
        const QString descr = escaped(description.simplified());
        QString run = QStringLiteral("<w:r>");
        // Lowered in half-points, a pixel being three quarters of a point.
        if (lower >= 0.5) run += QStringLiteral("<w:rPr><w:position w:val=\"-%1\"/></w:rPr>").arg(qRound(lower * 1.5));
        run += QStringLiteral(
                   "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"%1\" cy=\"%2\"/>"
                   "<wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/><wp:docPr id=\"%3\" name=\"Picture %3\" descr=\"%4\"/>"
                   "<wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
                   "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                   "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%3\" name=\"%5\" descr=\"%4\"/><pic:cNvPicPr/></pic:nvPicPr>"
                   "<pic:blipFill><a:blip r:embed=\"%6\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                   "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%1\" cy=\"%2\"/></a:xfrm>"
                   "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic>"
                   "</wp:inline></w:drawing>")
                   .arg(cx)
                   .arg(cy)
                   .arg(number)
                   .arg(descr, name, id);
        out() += run + QStringLiteral("</w:r>");
    }

    static QByteArray png(const QImage &image)
    {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "png");
        writer.write(image);
        return bytes;
    }

    // --- Inline content ---------------------------------------------------------
    void inlines(const std::vector<Node> &nodes, const Look &look)
    {
        for (const Node &node : nodes) inl(node, look);
    }

    void link(const QString &destination, const Node &node, const Look &look,
              const std::function<void(const Look &)> &content)
    {
        Look linked = look;
        linked.linked = true;
        QString dest = destination;
        if (dest.startsWith(u'#')) {
            out() += QStringLiteral("<w:hyperlink w:anchor=\"%1\" w:history=\"1\">").arg(bookmark(dest.mid(1)));
        } else {
            dest = m_destinations(node, dest);
            if (dest.isEmpty() || !md::detail::isSafeUrl(dest, false)) {
                content(look);
                return;
            }
            out() += QStringLiteral("<w:hyperlink r:id=\"%1\" w:history=\"1\">").arg(m_part->relate(kHyperlinkType, dest, true));
        }
        content(linked);
        out() += QStringLiteral("</w:hyperlink>");
    }

    void inl(const Node &node, const Look &look)
    {
        Look inner = look;
        switch (node.type) {
        case NodeType::Text:
        case NodeType::Escape:
        case NodeType::Entity: text(node.text, look); break;
        case NodeType::SoftBreak: text(QStringLiteral(" "), look); break;
        case NodeType::HardBreak: lineBreak(); break;
        case NodeType::Emphasis: inner.italic = !look.italic; inlines(node.children, inner); break;
        case NodeType::Strong: inner.bold = true; inlines(node.children, inner); break;
        case NodeType::Strikethrough: inner.strike = true; inlines(node.children, inner); break;
        case NodeType::Highlight: inner.highlight = true; inlines(node.children, inner); break;
        case NodeType::Superscript: inner.script = 1; inlines(node.children, inner); break;
        case NodeType::Subscript: inner.script = -1; inlines(node.children, inner); break;
        case NodeType::Code: inner.code = true; text(node.text, inner); break;
        case NodeType::Link:
        case NodeType::Autolink:
            if (look.linked) {
                inlines(node.children, look);
                break;
            }
            link(node.dest, node, look, [&](const Look &l) { inlines(node.children, l); });
            break;
        case NodeType::Image: image(node, look); break;
        case NodeType::HtmlInline: text(node.text, look); break;
        case NodeType::HtmlComment:
        case NodeType::HeadingLabel:
        case NodeType::CommentLine: break;
        case NodeType::HtmlElement: htmlInline(node, look); break;
        case NodeType::MathInline: formula(node, look); break;
        case NodeType::FootnoteReference:
        case NodeType::InlineNote:
            if (node.number <= 0 || node.number > int(m_doc.footnotes.size())) text(m_doc.source(node).toString(), look);
            else noteReference(node.number, look);
            break;
        case NodeType::Citation: citation(node, look); break;
        case NodeType::Variable: text(node.resolved ? node.text : m_doc.source(node).toString(), look); break;
        case NodeType::TaskMarker: text(node.checked ? QStringLiteral("☒ ") : QStringLiteral("☐ "), look); break;
        default: inlines(node.children, look); break;
        }
    }

    void htmlInline(const Node &node, const Look &look)
    {
        Look inner = look;
        const QString tag = node.label;
        const auto attribute = [&](const char *name) {
            for (const md::Attribute &a : node.attributes)
                if (a.name == QLatin1String(name)) return a.value;
            return QString();
        };
        if (tag == u"br") {
            lineBreak();
            return;
        }
        if (tag == u"img") {
            Node image;
            image.type = NodeType::Image;
            image.file = node.file;
            image.dest = attribute("src");
            Node alt;
            alt.type = NodeType::Text;
            alt.text = attribute("alt");
            image.children.push_back(alt);
            this->image(image, look);
            return;
        }
        if (tag == u"a") {
            const QString href = attribute("href");
            if (!href.isEmpty() && !look.linked) {
                link(href, node, look, [&](const Look &l) { inlines(node.children, l); });
                return;
            }
        }
        if (tag == u"b" || tag == u"strong") inner.bold = true;
        else if (tag == u"i" || tag == u"em") inner.italic = true;
        else if (tag == u"u" || tag == u"ins") inner.underline = true;
        else if (tag == u"s" || tag == u"del") inner.strike = true;
        else if (tag == u"mark") inner.highlight = true;
        else if (tag == u"code" || tag == u"kbd") inner.code = true;
        else if (tag == u"sup") inner.script = 1;
        else if (tag == u"sub") inner.script = -1;
        inlines(node.children, inner);
    }

    void image(const Node &node, const Look &look)
    {
        const QString alt = md::plainText(node).simplified();
        const md::Destination target = md::classifyDestination(m_doc, node);
        const auto missing = [&](const QString &why) {
            Look shown = look;
            shown.italic = true;
            text(alt.isEmpty() ? node.dest : alt, shown);
            if (!why.isEmpty()) unreadImages.append(QStringLiteral("%1 (%2)").arg(node.dest, why));
        };
        if (target.kind == md::Destination::Kind::Remote) {
            ++remoteImages;
            missing({});
            return;
        }
        QByteArray bytes;
        QString why;
        if (target.kind == md::Destination::Kind::LocalFile && target.absolute) {
            const SafeRead::Result read = SafeRead::readPicture(target.path, m_doc.resourceFolders);
            if (!read.ok) why = read.missing ? QStringLiteral("there is no such file") : read.error;
            else bytes = read.bytes;
        } else if (target.kind == md::Destination::Kind::OtherScheme && target.scheme == u"data") {
            const qsizetype comma = node.dest.indexOf(u',');
            if (comma > 0 && node.dest.left(comma).endsWith(QLatin1String(";base64")))
                bytes = QByteArray::fromBase64(node.dest.mid(comma + 1).toLatin1());
        }
        if (bytes.isEmpty()) {
            missing(why.isEmpty() ? QStringLiteral("not a picture parch can read") : why);
            return;
        }
        QImage picture;
        QSizeF size;
        if (ExportWeb::Assets::mediaType(bytes, target.path) == u"image/svg+xml") {
            size = SvgPaper::size(bytes);
            picture = SvgPaper::raster(bytes, (size * 2).toSize());
        } else {
            QBuffer buffer(&bytes);
            QImageReader reader(&buffer);
            reader.setAutoTransform(true);
            const QSize natural = reader.size();
            if (natural.width() > 8192 || natural.height() > 8192)
                reader.setScaledSize(natural.scaled(8192, 8192, Qt::KeepAspectRatio));
            picture = reader.read();
            size = picture.size();
        }
        if (picture.isNull() || size.isEmpty()) {
            missing(QStringLiteral("not a picture parch can read"));
            return;
        }
        // PNG and JPEG go in as they are; anything else as PNG.
        const bool asIs = bytes.startsWith("\x89PNG") && picture.size() == size.toSize();
        this->picture(asIs ? bytes : png(picture), size, alt.isEmpty() ? node.title : alt);
    }

    void formula(const Node &node, const Look &look)
    {
        const bool display = node.type == NodeType::MathBlock || node.display;
        const ExportPictures::Picture *drawn = m_pictures ? m_pictures->formula(node.text, display) : nullptr;
        if (!drawn || !drawn->drawn) {
            Look source = look;
            source.code = true;
            text(node.text, source);
            return;
        }
        const qreal scale = 4;
        const QImage image = SvgPaper::raster(drawn->svg, (drawn->size * scale).toSize());
        if (image.isNull()) {
            Look source = look;
            source.code = true;
            text(node.text, source);
            return;
        }
        ++formulasAsPictures;
        picture(png(image), drawn->size, node.text.simplified(), display ? 0 : drawn->size.height() - drawn->baseline);
    }

    void citation(const Node &node, const Look &look)
    {
        if (const auto note = m_citationNotes.constFind(&node); note != m_citationNotes.cend()) {
            noteReference(*note, look);
            return;
        }
        citationText(node, look);
    }

    void citationText(const Node &node, const Look &look)
    {
        if (m_citations) {
            if (const DocumentCitations::Formatted::Citation *found = m_citations->find(node)) {
                styled(found->text, look);
                return;
            }
        }
        const auto one = [&](const Node &item) {
            if (item.number <= 0) {
                text(m_doc.source(item).toString(), look);
                return;
            }
            link(QStringLiteral("#cite-%1").arg(item.number), item, look,
                 [&](const Look &l) { text(QString::number(item.number), l); });
            if (!item.text.isEmpty()) text(QStringLiteral(", ") + item.text, look);
        };
        if (node.children.empty() && node.number <= 0) {
            text(m_doc.source(node).toString(), look);
            return;
        }
        text(QStringLiteral("["), look);
        if (node.children.empty()) {
            one(node);
        } else {
            bool first = true;
            for (const Node &item : node.children) {
                if (!first) text(QStringLiteral("; "), look);
                first = false;
                one(item);
            }
        }
        text(QStringLiteral("]"), look);
    }

    // Text a citation style formatted.
    void styled(const CslText &words, const Look &look)
    {
        for (const CslSpan &span : words.spans()) {
            Look l = look;
            l.italic = l.italic || span.format.italic;
            l.bold = l.bold || span.format.bold;
            l.smallCaps = span.format.smallCaps;
            l.underline = span.format.underline;
            if (span.format.position == CslFormat::Position::Superscript) l.script = 1;
            else if (span.format.position == CslFormat::Position::Subscript) l.script = -1;
            if (!span.format.link.isEmpty() && !look.linked && md::detail::isSafeUrl(span.format.link, false)) {
                Node none;
                out() += QStringLiteral("<w:hyperlink r:id=\"%1\" w:history=\"1\">")
                             .arg(m_part->relate(kHyperlinkType, span.format.link, true));
                l.linked = true;
                text(span.text, l);
                out() += QStringLiteral("</w:hyperlink>");
            } else {
                text(span.text, l);
            }
        }
    }

    // --- Notes ---------------------------------------------------------------
    void noteReference(int number, const Look &look)
    {
        const QString kind = m_endnotes ? QStringLiteral("endnote") : QStringLiteral("footnote");
        const QString style = m_endnotes ? QStringLiteral("EndnoteReference") : QStringLiteral("FootnoteReference");
        // A note within a note, or one referred to again, is its number.
        if (m_inNote || m_notesWritten.contains(number)) {
            if (m_notesWritten.contains(number) && !m_inNote) ++notesReferredAgain;
            Look mark = look;
            mark.script = 1;
            text(QString::number(number), mark);
            return;
        }
        m_notesWritten.insert(number);
        const int id = int(m_notesWritten.size());
        out() += QStringLiteral("<w:r><w:rPr><w:rStyle w:val=\"%1\"/></w:rPr><w:%2Reference w:id=\"%3\"/></w:r>")
                     .arg(style, kind)
                     .arg(id);
        writeNote(number, id);
    }

    void writeNote(int number, int id)
    {
        const md::Footnote &note = m_doc.footnotes[size_t(number - 1)];
        const QString kind = m_endnotes ? QStringLiteral("endnote") : QStringLiteral("footnote");
        const QString textStyle = m_endnotes ? QStringLiteral("EndnoteText") : QStringLiteral("FootnoteText");
        const QString markStyle = m_endnotes ? QStringLiteral("EndnoteReference") : QStringLiteral("FootnoteReference");
        Part *outer = m_part;
        const QString prefix = std::exchange(m_prefix, QString());
        const bool wasInNote = std::exchange(m_inNote, true);
        m_part = &m_notes;
        out() += QStringLiteral("<w:%1 w:id=\"%2\">").arg(kind).arg(id);
        m_prefix = QStringLiteral("<w:r><w:rPr><w:rStyle w:val=\"%1\"/></w:rPr><w:%2Ref/></w:r><w:r><w:t xml:space=\"preserve\"> </w:t></w:r>")
                       .arg(markStyle, kind);
        Flow flow;
        flow.style = textStyle;
        const int before = m_paragraphs;
        if (note.content && note.content->type == NodeType::FootnoteDefinition) {
            blocks(note.content->children, flow);
        } else if (note.content) {
            open(para(flow));
            if (note.content->type == NodeType::Citation) citationText(*note.content, Look());
            else inlines(note.content->children, Look());
            close();
        }
        if (m_paragraphs == before) {
            open(para(flow));
            close();
        }
        m_prefix.clear();
        out() += QStringLiteral("</w:%1>").arg(kind);
        m_part = outer;
        m_inNote = wasInNote;
        m_prefix = prefix;
    }

    // --- Blocks ---------------------------------------------------------------
    void blocks(const std::vector<Node> &nodes, const Flow &flow)
    {
        for (const Node &node : nodes) block(node, flow);
    }

    void block(const Node &node, const Flow &flow)
    {
        switch (node.type) {
        case NodeType::Document:
        case NodeType::Include: blocks(node.children, flow); break;
        case NodeType::Paragraph: {
            // A picture alone in a paragraph is set apart, in the middle.
            Para p = para(flow);
            if (node.children.size() == 1 && node.children.front().type == NodeType::Image && flow.style == u"Normal") {
                p.style = QStringLiteral("Figure");
            }
            open(p);
            inlines(node.children, Look());
            close();
            break;
        }
        case NodeType::Heading: {
            Para p = para(flow, QStringLiteral("Heading%1").arg(std::clamp(int(node.level), 1, 6)));
            p.left = -1;
            open(p);
            bookmarkStart(node.dest);
            inlines(node.children, Look());
            close();
            break;
        }
        case NodeType::ThematicBreak: {
            Para p = para(flow, QStringLiteral("Rule"));
            p.rule = true;
            open(p);
            close();
            break;
        }
        case NodeType::BlockQuote: {
            Flow inner = flow;
            ++inner.quote;
            if (inner.style == u"Normal") inner.style = QStringLiteral("Quote");
            blocks(node.children, inner);
            break;
        }
        case NodeType::List: list(node, flow); break;
        case NodeType::CodeBlock: code(node.text, flow); break;
        case NodeType::MermaidBlock: diagram(node, flow); break;
        case NodeType::MathBlock: {
            Para p = para(flow, QStringLiteral("Figure"));
            open(p);
            formula(node, Look());
            close();
            break;
        }
        case NodeType::HtmlBlock: htmlBlocks(node.children, flow); break;
        case NodeType::Table: table(node, flow); break;
        case NodeType::DefinitionList:
            for (const Node &child : node.children) {
                if (child.type == NodeType::DefinitionTerm) {
                    Para p = para(flow, QStringLiteral("DefinitionTerm"));
                    open(p);
                    inlines(child.children, Look());
                    close();
                } else if (child.type == NodeType::DefinitionDescription) {
                    Flow inner = flow;
                    inner.indent += 720;
                    if (inner.style == u"Normal") inner.style = QStringLiteral("Definition");
                    blocks(child.children, inner);
                }
            }
            break;
        case NodeType::PageBreak: out() += QStringLiteral("<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>"); break;
        case NodeType::TableOfContents: {
            Flow inner = flow;
            inner.toc = true;
            for (const Node &child : node.children)
                if (child.type == NodeType::List) list(child, inner);
            break;
        }
        case NodeType::FrontMatter:
        case NodeType::LinkDefinition:
        case NodeType::FootnoteDefinition:
        case NodeType::CitationDefinition:
        case NodeType::CommentLine: break;
        default:
            if (node.isInline()) {
                open(para(flow));
                inl(node, Look());
                close();
            } else {
                blocks(node.children, flow);
            }
            break;
        }
    }

    void code(const QString &source, const Flow &flow)
    {
        QString shown = source;
        if (shown.endsWith(u'\n')) shown.chop(1);
        Para p = para(flow, QStringLiteral("Code"));
        open(p);
        text(shown, Look());
        close();
    }

    void diagram(const Node &node, const Flow &flow)
    {
        const ExportPictures::Picture *drawn = m_pictures ? m_pictures->diagram(node.text) : nullptr;
        if (!drawn || !drawn->drawn || drawn->png.isEmpty()) {
            code(node.text, flow);
            return;
        }
        ++diagramsAsPictures;
        Para p = para(flow, QStringLiteral("Figure"));
        open(p);
        picture(drawn->png, drawn->size, drawn->description);
        close();
    }

    // Numbering: one instance per list, of the bulleted, numbered or task kind.
    int numbering(int abstract, int start)
    {
        m_numbers.append({abstract, start});
        return int(m_numbers.size());
    }

    void list(const Node &node, const Flow &flow)
    {
        const int level = std::min(flow.listLevel + 1, 8);
        bool allTasks = !node.children.empty();
        for (const Node &item : node.children)
            if (item.type == NodeType::ListItem && !item.task) allTasks = false;
        const int numId = flow.toc ? 0 : numbering(node.ordered ? 1 : allTasks ? 2 : 0, node.ordered ? std::max(0, node.number) : 1);
        for (const Node &item : node.children) {
            if (item.type != NodeType::ListItem) continue;
            Flow inner = flow;
            inner.listLevel = level;
            bool first = true;
            for (const Node &child : item.children) {
                if (child.type == NodeType::List) {
                    list(child, inner);
                    continue;
                }
                if (first && (child.type == NodeType::Paragraph || child.isInline())) {
                    Para p = para(flow, flow.toc ? QStringLiteral("TOC%1").arg(level + 1) : QStringLiteral("ListParagraph"));
                    if (!flow.toc) {
                        p.numId = numId;
                        p.level = level;
                        p.left = -1;
                        if (flow.quote) p.left = (level + 1) * 720 + flow.quote * 720;
                        if (flow.quote) p.hanging = 360;
                    }
                    open(p);
                    if (child.type == NodeType::Paragraph) inlines(child.children, Look());
                    else inl(child, Look());
                    close();
                    first = false;
                    continue;
                }
                // What follows in an item is set in with its text.
                Flow body = inner;
                body.indent = flow.indent + (level + 1) * 720;
                if (first && !flow.toc) {
                    // An item that starts with something other than text still gets its number.
                    Para p = para(flow, QStringLiteral("ListParagraph"));
                    p.numId = numId;
                    p.level = level;
                    open(p);
                    close();
                }
                first = false;
                block(child, body);
            }
            if (first && !flow.toc) {
                Para p = para(flow, QStringLiteral("ListParagraph"));
                p.numId = numId;
                p.level = level;
                open(p);
                close();
            }
        }
    }

    // --- Tables ---------------------------------------------------------------
    struct Cell {
        const Node *node = nullptr;
        bool header = false;
        md::Align align = md::Align::None;
        int span = 1;
        int rowSpan = 1;
        bool html = false;
    };

    void table(const Node &node, const Flow &flow)
    {
        std::vector<std::vector<Cell>> rows;
        for (const Node &row : node.children) {
            if (row.type != NodeType::TableRow) continue;
            std::vector<Cell> cells;
            for (const Node &cell : row.children)
                if (cell.type == NodeType::TableCell) cells.push_back({&cell, row.header || cell.header, cell.align, std::max(1, cell.span), 1, false});
            rows.push_back(std::move(cells));
        }
        writeTable(rows, std::max(1, node.columns), flow);
    }

    void writeTable(const std::vector<std::vector<Cell>> &rows, int columns, const Flow &flow)
    {
        if (rows.empty()) return;
        // Where rows cover cells of the rows below them, as raw HTML rowspan does.
        std::vector<std::vector<int>> covered(rows.size(), std::vector<int>(size_t(columns), 0));
        const int width = int(m_textWidth * kTwipsPerPixel) - flow.indent - flow.quote * 720;
        const int column = std::max(200, width / columns);
        out() += QStringLiteral("<w:tbl><w:tblPr><w:tblStyle w:val=\"Table\"/><w:tblW w:w=\"%1\" w:type=\"dxa\"/>").arg(column * columns);
        if (flow.indent + flow.quote * 720 > 0)
            out() += QStringLiteral("<w:tblInd w:w=\"%1\" w:type=\"dxa\"/>").arg(flow.indent + flow.quote * 720);
        out() += QStringLiteral("<w:tblLook w:val=\"04A0\" w:firstRow=\"1\" w:lastRow=\"0\" w:firstColumn=\"0\" "
                                "w:lastColumn=\"0\" w:noHBand=\"0\" w:noVBand=\"1\"/></w:tblPr><w:tblGrid>");
        for (int c = 0; c < columns; ++c) out() += QStringLiteral("<w:gridCol w:w=\"%1\"/>").arg(column);
        out() += QStringLiteral("</w:tblGrid>");
        for (size_t r = 0; r < rows.size(); ++r) {
            const bool header = !rows[r].empty() && std::all_of(rows[r].cbegin(), rows[r].cend(), [](const Cell &c) { return c.header; });
            out() += QStringLiteral("<w:tr>");
            if (header && r == 0) out() += QStringLiteral("<w:trPr><w:tblHeader/></w:trPr>");
            int at = 0;
            size_t next = 0;
            while (at < columns) {
                if (covered[r][size_t(at)] > 0) {
                    // A cell merged from above.
                    const int span = covered[r][size_t(at)];
                    out() += QStringLiteral("<w:tc><w:tcPr><w:tcW w:w=\"%1\" w:type=\"dxa\"/>").arg(column * span);
                    if (span > 1) out() += QStringLiteral("<w:gridSpan w:val=\"%1\"/>").arg(span);
                    out() += QStringLiteral("<w:vMerge/></w:tcPr><w:p/></w:tc>");
                    at += span;
                    continue;
                }
                if (next >= rows[r].size()) {
                    out() += QStringLiteral("<w:tc><w:tcPr><w:tcW w:w=\"%1\" w:type=\"dxa\"/></w:tcPr><w:p/></w:tc>").arg(column);
                    ++at;
                    continue;
                }
                const Cell &cell = rows[r][next++];
                const int span = std::clamp(cell.span, 1, columns - at);
                out() += QStringLiteral("<w:tc><w:tcPr><w:tcW w:w=\"%1\" w:type=\"dxa\"/>").arg(column * span);
                if (span > 1) out() += QStringLiteral("<w:gridSpan w:val=\"%1\"/>").arg(span);
                if (cell.rowSpan > 1) {
                    out() += QStringLiteral("<w:vMerge w:val=\"restart\"/>");
                    for (size_t below = r + 1; below < rows.size() && below < r + size_t(cell.rowSpan); ++below)
                        covered[below][size_t(at)] = span;
                }
                out() += QStringLiteral("</w:tcPr>");
                Para p;
                p.style = QStringLiteral("TableText");
                switch (cell.align) {
                case md::Align::Center: p.align = QStringLiteral("center"); break;
                case md::Align::Right: p.align = QStringLiteral("right"); break;
                default: break;
                }
                Look look;
                look.bold = cell.header;
                if (cell.html) {
                    Flow inside;
                    inside.style = QStringLiteral("TableText");
                    const int before = m_paragraphs;
                    htmlBlocks(cell.node->children, inside, look);
                    if (m_paragraphs == before) {
                        open(p);
                        close();
                    }
                } else {
                    open(p);
                    inlines(cell.node->children, look);
                    close();
                }
                out() += QStringLiteral("</w:tc>");
                at += span;
            }
            out() += QStringLiteral("</w:tr>");
        }
        out() += QStringLiteral("</w:tbl>");
        // Word needs a paragraph between two tables, and after one at the end of a cell.
        out() += QStringLiteral("<w:p><w:pPr><w:pStyle w:val=\"TableAfter\"/></w:pPr></w:p>");
    }

    // --- Raw HTML under the engine's policy -----------------------------------
    static QString attributeOf(const Node &node, const char *name)
    {
        for (const md::Attribute &a : node.attributes)
            if (a.name == QLatin1String(name)) return a.value;
        return {};
    }

    static bool isBlockTag(const QString &tag)
    {
        static const QSet<QString> blocks{QStringLiteral("p"),  QStringLiteral("blockquote"), QStringLiteral("pre"),
                                          QStringLiteral("h1"), QStringLiteral("h2"),         QStringLiteral("h3"),
                                          QStringLiteral("h4"), QStringLiteral("h5"),         QStringLiteral("h6"),
                                          QStringLiteral("ul"), QStringLiteral("ol"),         QStringLiteral("li"),
                                          QStringLiteral("dl"), QStringLiteral("dt"),         QStringLiteral("dd"),
                                          QStringLiteral("hr"), QStringLiteral("table"),      QStringLiteral("div")};
        return blocks.contains(tag);
    }

    void htmlBlocks(const std::vector<Node> &nodes, const Flow &flow, const Look &look = Look())
    {
        std::vector<const Node *> run;
        const auto flush = [&] {
            bool visible = false;
            for (const Node *n : run)
                if (!(n->isTextual() && n->text.trimmed().isEmpty()) && n->type != NodeType::SoftBreak) visible = true;
            if (visible) {
                open(para(flow));
                for (const Node *n : run) inl(*n, look);
                close();
            }
            run.clear();
        };
        for (const Node &node : nodes) {
            if (node.type == NodeType::HtmlElement && isBlockTag(node.label)) {
                flush();
                htmlBlock(node, flow, look);
            } else if (node.isBlock()) {
                flush();
                block(node, flow);
            } else {
                run.push_back(&node);
            }
        }
        flush();
    }

    void htmlBlock(const Node &node, const Flow &flow, const Look &look)
    {
        const QString tag = node.label;
        if (tag.size() == 2 && tag[0] == u'h' && tag[1] >= u'1' && tag[1] <= u'6') {
            open(para(flow, QStringLiteral("Heading%1").arg(tag[1])));
            inlines(node.children, look);
            close();
        } else if (tag == u"p" || tag == u"div" || tag == u"dt" || tag == u"li") {
            Flow inner = flow;
            if (tag == u"dt") inner.style = QStringLiteral("DefinitionTerm");
            htmlBlocks(node.children, inner, look);
        } else if (tag == u"blockquote") {
            Flow inner = flow;
            ++inner.quote;
            if (inner.style == u"Normal") inner.style = QStringLiteral("Quote");
            htmlBlocks(node.children, inner, look);
        } else if (tag == u"pre") {
            code(md::plainText(node), flow);
        } else if (tag == u"hr") {
            Para p = para(flow, QStringLiteral("Rule"));
            p.rule = true;
            open(p);
            close();
        } else if (tag == u"ul" || tag == u"ol") {
            const bool ordered = tag == u"ol";
            bool ok = false;
            const int start = attributeOf(node, "start").toInt(&ok);
            const int numId = numbering(ordered ? 1 : 0, ordered && ok ? start : 1);
            const int level = std::min(flow.listLevel + 1, 8);
            for (const Node &child : node.children) {
                if (child.type != NodeType::HtmlElement || child.label != u"li") continue;
                Para p = para(flow, QStringLiteral("ListParagraph"));
                p.numId = numId;
                p.level = level;
                p.left = -1;
                open(p);
                std::vector<const Node *> nested;
                for (const Node &part : child.children) {
                    if (part.type == NodeType::HtmlElement && isBlockTag(part.label)) nested.push_back(&part);
                    else inl(part, look);
                }
                close();
                Flow inner = flow;
                inner.listLevel = level;
                inner.indent = flow.indent + (level + 1) * 720;
                for (const Node *part : nested) {
                    Flow sub = (part->label == u"ul" || part->label == u"ol") ? Flow(flow) : inner;
                    if (part->label == u"ul" || part->label == u"ol") sub.listLevel = level;
                    htmlBlock(*part, sub, look);
                }
            }
        } else if (tag == u"dl") {
            for (const Node &child : node.children) {
                if (child.type != NodeType::HtmlElement) continue;
                Flow inner = flow;
                if (child.label == u"dt") {
                    inner.style = QStringLiteral("DefinitionTerm");
                } else {
                    inner.indent += 720;
                    inner.style = QStringLiteral("Definition");
                }
                htmlBlocks(child.children, inner, look);
            }
        } else if (tag == u"dd") {
            Flow inner = flow;
            inner.indent += 720;
            inner.style = QStringLiteral("Definition");
            htmlBlocks(node.children, inner, look);
        } else if (tag == u"table") {
            htmlTable(node, flow);
        } else {
            htmlBlocks(node.children, flow, look);
        }
    }

    void htmlTable(const Node &node, const Flow &flow)
    {
        std::vector<std::vector<Cell>> rows;
        int columns = 0;
        std::function<void(const Node &)> collect = [&](const Node &n) {
            for (const Node &child : n.children) {
                if (child.type != NodeType::HtmlElement) continue;
                if (child.label == u"tr") {
                    std::vector<Cell> cells;
                    for (const Node &cell : child.children) {
                        if (cell.type != NodeType::HtmlElement || (cell.label != u"td" && cell.label != u"th")) continue;
                        Cell c{&cell, cell.label == u"th", md::Align::None, 1, 1, true};
                        c.span = std::clamp(attributeOf(cell, "colspan").toInt(), 1, 64);
                        c.rowSpan = std::clamp(attributeOf(cell, "rowspan").toInt(), 1, 1000);
                        const QString align = attributeOf(cell, "align");
                        c.align = align == u"center" ? md::Align::Center : align == u"right" ? md::Align::Right : md::Align::None;
                        cells.push_back(c);
                    }
                    rows.push_back(std::move(cells));
                } else if (child.label != u"caption") {
                    collect(child);
                }
            }
        };
        collect(node);
        // The widest row, counting the cells rows above reach into.
        std::vector<int> reaching;
        for (size_t r = 0; r < rows.size(); ++r) {
            int width = 0;
            for (const Cell &cell : rows[r]) width += cell.span;
            for (size_t above = 0; above < r; ++above)
                for (const Cell &cell : rows[above])
                    if (above + size_t(cell.rowSpan) > r) width += cell.span;
            columns = std::max(columns, width);
        }
        writeTable(rows, std::max(1, columns), flow);
    }

    // --- The bibliography --------------------------------------------------------
    void bibliography()
    {
        if (m_citations) {
            if (m_citations->bibliography.isEmpty()) return;
            notesRule();
            for (const CitationBibliographyEntry &entry : m_citations->bibliography) {
                Para p;
                p.style = QStringLiteral("Bibliography");
                if (m_citations->layout.hangingIndent) {
                    p.left = 720;
                    p.hanging = 720;
                }
                open(p);
                bookmarkStart(DocumentCitations::entryId(entry.key));
                styled(entry.text, Look());
                close();
            }
            return;
        }
        if (m_doc.bibliography.empty()) return;
        notesRule();
        for (const md::BibliographyEntry &entry : m_doc.bibliography) {
            Para p;
            p.style = QStringLiteral("Bibliography");
            p.left = 720;
            p.hanging = 720;
            open(p);
            bookmarkStart(QStringLiteral("cite-%1").arg(entry.number));
            text(QStringLiteral("[%1]\t").arg(entry.number), Look());
            if (entry.definition) {
                bool first = true;
                for (const Node &child : entry.definition->children) {
                    if (!first) text(QStringLiteral(" "), Look());
                    first = false;
                    if (child.type == NodeType::Paragraph) inlines(child.children, Look());
                    else text(md::plainText(child), Look());
                }
            } else {
                text(entry.key, Look());
            }
            close();
        }
    }

    void notesRule()
    {
        Para p;
        p.style = QStringLiteral("Rule");
        p.rule = true;
        p.right = int(m_textWidth * kTwipsPerPixel * 0.7);
        open(p);
        close();
    }

    QString styles() const;
    QString numberingXml() const;

    const md::Document &m_doc;
    const ExportRecipe &m_recipe;
    const ExportPictures *m_pictures;
    bool m_endnotes;
    QPageSize m_page;
    const DocumentCitations::Formatted *m_citations;
    std::function<QString(const md::Node &, const QString &)> m_destinations;
    qreal m_textWidth = 600;

    Part m_body;
    Part m_notes;
    Part *m_part = &m_body;
    QString m_prefix;
    bool m_inNote = false;
    int m_paragraphs = 0;
    QSet<int> m_notesWritten;
    QHash<const Node *, int> m_citationNotes;
    QHash<QString, QString> m_bookmarks;
    QSet<QString> m_bookmarkNames;
    int m_bookmarkId = 0;
    int m_drawings = 0;
    QList<Media> m_media;
    QList<std::pair<int, int>> m_numbers; // per numbering instance: its kind and first number
    QString m_language;
};

const QString kMain = QStringLiteral("http://schemas.openxmlformats.org/wordprocessingml/2006/main");

QString Writer::styles() const
{
    const auto paragraph = [](const QString &id, const QString &name, const QString &pPr, const QString &rPr,
                              const QString &extra = {}) {
        return QStringLiteral("<w:style w:type=\"paragraph\" w:styleId=\"%1\"><w:name w:val=\"%2\"/>"
                              "<w:basedOn w:val=\"Normal\"/>%5<w:qFormat/>%3%4</w:style>")
            .arg(id, name, pPr.isEmpty() ? QString() : QStringLiteral("<w:pPr>") + pPr + QStringLiteral("</w:pPr>"),
                 rPr.isEmpty() ? QString() : QStringLiteral("<w:rPr>") + rPr + QStringLiteral("</w:rPr>"), extra);
    };
    const auto character = [](const QString &id, const QString &name, const QString &rPr) {
        return QStringLiteral("<w:style w:type=\"character\" w:styleId=\"%1\"><w:name w:val=\"%2\"/>"
                              "<w:uiPriority w:val=\"99\"/><w:rPr>%3</w:rPr></w:style>")
            .arg(id, name, rPr);
    };
    const QString sans = QStringLiteral("<w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\" w:cs=\"Arial\"/>");
    const QString mono = QStringLiteral("<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:cs=\"Consolas\"/>");
    QString out = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:styles xmlns:w=\"%1\">").arg(kMain);
    out += QStringLiteral("<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Georgia\" w:hAnsi=\"Georgia\" "
                          "w:eastAsia=\"Georgia\" w:cs=\"Georgia\"/><w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/>"
                          "<w:lang w:val=\"%1\"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after=\"160\" "
                          "w:line=\"300\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>")
               .arg(escaped(m_language));
    out += QStringLiteral("<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/>"
                          "<w:qFormat/></w:style>");
    out += paragraph(QStringLiteral("Title"), QStringLiteral("Title"),
                     QStringLiteral("<w:keepNext/><w:spacing w:after=\"300\"/>"),
                     sans + QStringLiteral("<w:b/><w:sz w:val=\"52\"/><w:szCs w:val=\"52\"/>"));
    const int sizes[] = {36, 30, 26, 24, 22, 22};
    for (int level = 1; level <= 6; ++level)
        out += paragraph(QStringLiteral("Heading%1").arg(level), QStringLiteral("heading %1").arg(level),
                         QStringLiteral("<w:keepNext/><w:keepLines/><w:spacing w:before=\"%1\" w:after=\"120\"/>"
                                        "<w:outlineLvl w:val=\"%2\"/>")
                             .arg(level <= 2 ? 360 : 240)
                             .arg(level - 1),
                         sans + QStringLiteral("<w:b/>%1<w:sz w:val=\"%2\"/><w:szCs w:val=\"%2\"/>")
                                    .arg(level == 6 ? QStringLiteral("<w:color w:val=\"595959\"/>") : QString())
                                    .arg(sizes[level - 1]),
                         QStringLiteral("<w:next w:val=\"Normal\"/>"));
    out += paragraph(QStringLiteral("Quote"), QStringLiteral("Quote"),
                     QStringLiteral("<w:pBdr><w:left w:val=\"single\" w:sz=\"18\" w:space=\"8\" w:color=\"BFBFBF\"/></w:pBdr>"
                                    "<w:ind w:left=\"720\"/>"),
                     QStringLiteral("<w:i/><w:color w:val=\"404040\"/>"));
    out += paragraph(QStringLiteral("Code"), QStringLiteral("Code"),
                     QStringLiteral("<w:keepLines/><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F4F4F4\"/>"
                                    "<w:spacing w:after=\"160\" w:line=\"240\" w:lineRule=\"auto\"/>"),
                     mono + QStringLiteral("<w:sz w:val=\"19\"/><w:szCs w:val=\"19\"/>"));
    out += paragraph(QStringLiteral("ListParagraph"), QStringLiteral("List Paragraph"),
                     QStringLiteral("<w:spacing w:after=\"60\"/><w:contextualSpacing/>"), QString());
    out += paragraph(QStringLiteral("Figure"), QStringLiteral("Figure"), QStringLiteral("<w:jc w:val=\"center\"/>"), QString());
    out += paragraph(QStringLiteral("Rule"), QStringLiteral("Rule"), QStringLiteral("<w:spacing w:before=\"120\" w:after=\"240\"/>"),
                     QStringLiteral("<w:sz w:val=\"8\"/>"));
    out += paragraph(QStringLiteral("DefinitionTerm"), QStringLiteral("Definition Term"),
                     QStringLiteral("<w:keepNext/><w:spacing w:after=\"40\"/>"), QStringLiteral("<w:b/>"));
    out += paragraph(QStringLiteral("Definition"), QStringLiteral("Definition"), QStringLiteral("<w:ind w:left=\"720\"/>"), QString());
    out += paragraph(QStringLiteral("TableText"), QStringLiteral("Table Text"),
                     QStringLiteral("<w:spacing w:before=\"40\" w:after=\"40\"/>"), QString());
    out += paragraph(QStringLiteral("TableAfter"), QStringLiteral("After Table"), QStringLiteral("<w:spacing w:after=\"0\"/>"),
                     QStringLiteral("<w:sz w:val=\"8\"/>"));
    out += paragraph(QStringLiteral("Bibliography"), QStringLiteral("Bibliography"), QStringLiteral("<w:spacing w:after=\"120\"/>"), QString());
    out += paragraph(QStringLiteral("FootnoteText"), QStringLiteral("footnote text"),
                     QStringLiteral("<w:spacing w:after=\"60\" w:line=\"240\" w:lineRule=\"auto\"/>"),
                     QStringLiteral("<w:sz w:val=\"18\"/><w:szCs w:val=\"18\"/>"));
    out += paragraph(QStringLiteral("EndnoteText"), QStringLiteral("endnote text"),
                     QStringLiteral("<w:spacing w:after=\"60\" w:line=\"240\" w:lineRule=\"auto\"/>"),
                     QStringLiteral("<w:sz w:val=\"20\"/><w:szCs w:val=\"20\"/>"));
    for (int level = 1; level <= 6; ++level)
        out += paragraph(QStringLiteral("TOC%1").arg(level), QStringLiteral("toc %1").arg(level),
                         QStringLiteral("<w:spacing w:after=\"60\"/><w:ind w:left=\"%1\"/>").arg((level - 1) * 360), QString());
    out += character(QStringLiteral("CodeChar"), QStringLiteral("Code Char"),
                     mono + QStringLiteral("<w:sz w:val=\"19\"/><w:szCs w:val=\"19\"/><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F4F4F4\"/>"));
    out += character(QStringLiteral("Hyperlink"), QStringLiteral("Hyperlink"),
                     QStringLiteral("<w:color w:val=\"0B57D0\"/><w:u w:val=\"single\"/>"));
    out += character(QStringLiteral("FootnoteReference"), QStringLiteral("footnote reference"),
                     QStringLiteral("<w:vertAlign w:val=\"superscript\"/>"));
    out += character(QStringLiteral("EndnoteReference"), QStringLiteral("endnote reference"),
                     QStringLiteral("<w:vertAlign w:val=\"superscript\"/>"));
    out += QStringLiteral("<w:style w:type=\"table\" w:styleId=\"Table\"><w:name w:val=\"Table\"/><w:tblPr>"
                          "<w:tblBorders><w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFBFBF\"/>"
                          "<w:left w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFBFBF\"/>"
                          "<w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFBFBF\"/>"
                          "<w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFBFBF\"/>"
                          "<w:insideH w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFBFBF\"/>"
                          "<w:insideV w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFBFBF\"/></w:tblBorders>"
                          "<w:tblCellMar><w:left w:w=\"100\" w:type=\"dxa\"/><w:right w:w=\"100\" w:type=\"dxa\"/>"
                          "</w:tblCellMar></w:tblPr></w:style>");
    out += QStringLiteral("</w:styles>");
    return out;
}

QString Writer::numberingXml() const
{
    // Three kinds: bullets, numbers and tasks (whose boxes are in their text).
    QString out = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:numbering xmlns:w=\"%1\">").arg(kMain);
    static const QChar bullets[] = {QChar(0x2022), QChar(0x25e6), QChar(0x25aa)};
    static const char *formats[] = {"decimal", "lowerLetter", "lowerRoman"};
    for (int kind = 0; kind < 3; ++kind) {
        out += QStringLiteral("<w:abstractNum w:abstractNumId=\"%1\"><w:multiLevelType w:val=\"multilevel\"/>").arg(kind);
        for (int level = 0; level < 9; ++level) {
            const int left = (level + 1) * 720;
            QString format, text;
            if (kind == 0) {
                format = QStringLiteral("bullet");
                text = bullets[level % 3];
            } else if (kind == 1) {
                format = QString::fromLatin1(formats[level % 3]);
                text = QStringLiteral("%%1.").arg(level + 1);
            } else {
                format = QStringLiteral("none");
            }
            // The level's text holds %1 itself, so it goes in last, by hand.
            out += QStringLiteral("<w:lvl w:ilvl=\"%1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"%2\"/>"
                                  "<w:lvlText w:val=\"")
                       .arg(level)
                       .arg(format) +
                   escaped(text) +
                   QStringLiteral("\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"%1\" w:hanging=\"360\"/></w:pPr></w:lvl>")
                       .arg(left);
        }
        out += QStringLiteral("</w:abstractNum>");
    }
    for (qsizetype i = 0; i < m_numbers.size(); ++i) {
        out += QStringLiteral("<w:num w:numId=\"%1\"><w:abstractNumId w:val=\"%2\"/>").arg(i + 1).arg(m_numbers[i].first);
        // Each list counts from its own first number.
        for (int level = 0; level < 9; ++level)
            out += QStringLiteral("<w:lvlOverride w:ilvl=\"%1\"><w:startOverride w:val=\"%2\"/></w:lvlOverride>")
                       .arg(level)
                       .arg(m_numbers[i].second);
        out += QStringLiteral("</w:num>");
    }
    out += QStringLiteral("</w:numbering>");
    return out;
}

QByteArray Writer::package(QString *error)
{
    m_language = Exporters::language(m_doc);
    if (m_language.isEmpty()) m_language = QStringLiteral("en-US");
    const QString title = Exporters::title(m_doc, m_recipe);
    // The front matter's title opens the document in the Title style.
    if (const QString given = Exporters::frontMatterText(m_doc, QStringLiteral("title")); !given.isEmpty()) {
        Para p;
        p.style = QStringLiteral("Title");
        open(p);
        text(given, Look());
        close();
    }
    Flow flow;
    block(m_doc.root, flow);
    // Notes never referred to are not written, as in every other format.
    bibliography();

    const QSizeF size = m_page.size(QPageSize::Millimeter);
    const int width = qRound(size.width() * 1440 / 25.4), height = qRound(size.height() * 1440 / 25.4);
    const QString namespaces = QStringLiteral(
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"");
    const QString document = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document %1>"
                                            "<w:body>%2<w:sectPr><w:pgSz w:w=\"%3\" w:h=\"%4\"/><w:pgMar w:top=\"1440\" "
                                            "w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"708\" "
                                            "w:footer=\"708\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>")
                                 .arg(namespaces, m_body.xml)
                                 .arg(width)
                                 .arg(height);
    const QString kind = m_endnotes ? QStringLiteral("endnote") : QStringLiteral("footnote");
    const auto notesPart = [&](const QString &which, const QString &content) {
        return QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:%1s %2>"
                              "<w:%1 w:type=\"separator\" w:id=\"-1\"><w:p><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" "
                              "w:lineRule=\"auto\"/></w:pPr><w:r><w:separator/></w:r></w:p></w:%1>"
                              "<w:%1 w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:pPr><w:spacing w:after=\"0\" "
                              "w:line=\"240\" w:lineRule=\"auto\"/></w:pPr><w:r><w:continuationSeparator/></w:r></w:p></w:%1>"
                              "%3</w:%1s>")
            .arg(which, namespaces, content);
    };
    const QString footnotes = notesPart(QStringLiteral("footnote"), m_endnotes ? QString() : m_notes.xml);
    const QString endnotes = notesPart(QStringLiteral("endnote"), m_endnotes ? m_notes.xml : QString());
    Q_UNUSED(kind);
    const QString settings = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:settings xmlns:w=\"%1\">"
        "<w:defaultTabStop w:val=\"720\"/><w:characterSpacingControl w:val=\"doNotCompress\"/>"
        "<w:footnotePr><w:footnote w:id=\"-1\"/><w:footnote w:id=\"0\"/></w:footnotePr>"
        "<w:endnotePr><w:endnote w:id=\"-1\"/><w:endnote w:id=\"0\"/></w:endnotePr>"
        "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" "
        "w:val=\"15\"/></w:compat></w:settings>")
                                 .arg(kMain);

    const QString author = Exporters::frontMatterText(m_doc, QStringLiteral("author"));
    const QString subject = Exporters::frontMatterText(m_doc, QStringLiteral("subject"));
    QString keywords = Exporters::frontMatterText(m_doc, QStringLiteral("keywords"));
    if (keywords.isEmpty()) keywords = Exporters::frontMatterText(m_doc, QStringLiteral("tags"));
    const QString description = Exporters::frontMatterText(m_doc, QStringLiteral("description"));
    const QString now = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
    QString core = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<cp:coreProperties "
        "xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
        "xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">");
    core += QStringLiteral("<dc:title>%1</dc:title>").arg(escaped(title));
    if (!subject.isEmpty()) core += QStringLiteral("<dc:subject>%1</dc:subject>").arg(escaped(subject));
    if (!author.isEmpty()) core += QStringLiteral("<dc:creator>%1</dc:creator>").arg(escaped(author));
    if (!keywords.isEmpty()) core += QStringLiteral("<cp:keywords>%1</cp:keywords>").arg(escaped(keywords));
    if (!description.isEmpty()) core += QStringLiteral("<dc:description>%1</dc:description>").arg(escaped(description));
    core += QStringLiteral("<dc:language>%1</dc:language>").arg(escaped(m_language));
    core += QStringLiteral("<dcterms:created xsi:type=\"dcterms:W3CDTF\">%1</dcterms:created>"
                           "<dcterms:modified xsi:type=\"dcterms:W3CDTF\">%1</dcterms:modified></cp:coreProperties>")
                .arg(now);
    const QString app = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Properties "
                                       "xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
                                       "<Application>parch %1</Application></Properties>")
                            .arg(QStringLiteral(PARCH_VERSION));
    const QString types = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Default Extension=\"png\" ContentType=\"image/png\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
        "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
        "<Override PartName=\"/word/footnotes.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footnotes+xml\"/>"
        "<Override PartName=\"/word/endnotes.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.endnotes+xml\"/>"
        "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>"
        "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
        "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
        "</Types>");
    const QByteArray rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
                            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
                            "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
                            "</Relationships>";

    ZipPackage zip;
    zip.add(QStringLiteral("[Content_Types].xml"), types.toUtf8());
    zip.add(QStringLiteral("_rels/.rels"), rels);
    zip.add(QStringLiteral("word/document.xml"), document.toUtf8());
    zip.add(QStringLiteral("word/_rels/document.xml.rels"), m_body.relationshipsXml());
    zip.add(QStringLiteral("word/styles.xml"), styles().toUtf8());
    zip.add(QStringLiteral("word/numbering.xml"), numberingXml().toUtf8());
    zip.add(QStringLiteral("word/footnotes.xml"), footnotes.toUtf8());
    zip.add(QStringLiteral("word/endnotes.xml"), endnotes.toUtf8());
    zip.add(m_endnotes ? QStringLiteral("word/_rels/endnotes.xml.rels") : QStringLiteral("word/_rels/footnotes.xml.rels"),
            m_notes.relationshipsXml());
    zip.add(QStringLiteral("word/settings.xml"), settings.toUtf8());
    zip.add(QStringLiteral("docProps/core.xml"), core.toUtf8());
    zip.add(QStringLiteral("docProps/app.xml"), app.toUtf8());
    for (const Media &media : std::as_const(m_media)) zip.add(QStringLiteral("word/media/") + media.name, media.bytes, false);
    return zip.bytes(error);
}

} // namespace

QList<ExportOption> DocxExporter::options() const
{
    ExportOption title{QStringLiteral("title"),
                       QStringLiteral("The document's title (the default is the front matter's title, the first "
                                      "heading, or the file's name)"),
                       QStringLiteral("TEXT")};
    title.label = QStringLiteral("Title");
    ExportOption notes{QStringLiteral("notes"),
                       QStringLiteral("Notes as footnotes at the foot of their page, or as endnotes at the end "
                                      "(the default is footnotes)"),
                       QStringLiteral("KIND")};
    notes.label = QStringLiteral("Notes");
    notes.choices = {QStringLiteral("footnotes"), QStringLiteral("endnotes")};
    notes.defaultValue = QStringLiteral("footnotes");
    ExportOption size{QStringLiteral("page-size"),
                      QStringLiteral("The paper, %1 (the default is A4)").arg(PaperOptions::pageSizes().join(QStringLiteral(", "))),
                      QStringLiteral("SIZE")};
    size.label = QStringLiteral("Page size");
    size.choices = PaperOptions::pageSizes();
    size.defaultValue = QStringLiteral("A4");
    return {title, notes, size};
}

QStringList DocxExporter::limitations(const md::Document &document, const ExportRecipe &recipe,
                                      const HelperSet &helpers) const
{
    Q_UNUSED(recipe);
    int formulas = 0, diagrams = 0, remote = 0;
    std::function<void(const Node &)> walk = [&](const Node &node) {
        if (node.type == NodeType::MathInline || node.type == NodeType::MathBlock) ++formulas;
        else if (node.type == NodeType::MermaidBlock) ++diagrams;
        else if (node.type == NodeType::Image && md::classifyDestination(document, node).kind == md::Destination::Kind::Remote)
            ++remote;
        for (const Node &child : node.children) walk(child);
    };
    walk(document.root);
    const auto missing = [](const QString &path) { return path.isEmpty() || !QFileInfo(path).isExecutable(); };
    QStringList limitations;
    QStringList pictures;
    if (diagrams && !missing(helpers.merman))
        pictures.append(Exporters::count(diagrams, QStringLiteral("diagram"), QStringLiteral("diagrams")));
    if (formulas && !missing(helpers.ratex))
        pictures.append(Exporters::count(formulas, QStringLiteral("formula"), QStringLiteral("formulas")));
    if (!pictures.isEmpty())
        limitations.append(QStringLiteral("%1 become pictures, described by their words or TeX: Word cannot edit them "
                                          "as equations or diagrams")
                               .arg(pictures.join(QStringLiteral(" and "))));
    if (formulas && missing(helpers.ratex))
        limitations.append(QStringLiteral("Math is written as its TeX source (%1): the math helper (render-svg) is not "
                                          "installed")
                               .arg(Exporters::count(formulas, QStringLiteral("formula"), QStringLiteral("formulas"))));
    if (diagrams && missing(helpers.merman))
        limitations.append(QStringLiteral("Mermaid diagrams are written as their source (%1): the diagram helper "
                                          "(merman-cli) is not installed")
                               .arg(Exporters::count(diagrams, QStringLiteral("diagram"), QStringLiteral("diagrams"))));
    if (remote)
        limitations.append(QStringLiteral("Images from the web are not fetched, so their descriptions stand in their "
                                          "place (%1)")
                               .arg(Exporters::count(remote, QStringLiteral("image"), QStringLiteral("images"))));
    return limitations;
}

ExportResult DocxExporter::write(const md::Document &document, const ExportRecipe &recipe, const ExportRun &run) const
{
    bool stopped = false;
    // Formulas are drawn at the size of Word's text, 11 points.
    const std::unique_ptr<ExportPictures> pictures = Exporters::drawPictures(document, run, 11 * 96.0 / 72.0, 0.7, &stopped);
    if (stopped) return Exporters::cancelled();
    run.report(0.75, QStringLiteral("Writing the document"));

    ExportResult result;
    const QString size = option(recipe, QStringLiteral("page-size"));
    if (!PaperOptions::pageSizes().contains(size, Qt::CaseInsensitive)) {
        result.error = QStringLiteral("--page-size takes %1, not \"%2\"").arg(PaperOptions::pageSizes().join(QStringLiteral(", ")), size);
        return result;
    }
    const QString notes = option(recipe, QStringLiteral("notes"));
    if (notes != u"footnotes" && notes != u"endnotes") {
        result.error = QStringLiteral("--notes takes footnotes or endnotes, not \"%1\"").arg(notes);
        return result;
    }
    Writer writer(document, recipe, pictures.get(), notes == u"endnotes", PaperOptions::sizeNamed(size));
    QString error;
    const QByteArray bytes = writer.package(&error);
    if (bytes.isEmpty()) {
        result.error = QStringLiteral("the document could not be packed: %1").arg(error);
        return result;
    }
    if (run.stopped()) return Exporters::cancelled();

    QStringList pictured;
    if (writer.diagramsAsPictures)
        pictured.append(Exporters::count(writer.diagramsAsPictures, QStringLiteral("diagram"), QStringLiteral("diagrams")));
    if (writer.formulasAsPictures)
        pictured.append(Exporters::count(writer.formulasAsPictures, QStringLiteral("formula"), QStringLiteral("formulas")));
    if (!pictured.isEmpty())
        result.limitations.append(QStringLiteral("%1 become pictures, described by their words or TeX: Word cannot edit "
                                                 "them as equations or diagrams")
                                      .arg(pictured.join(QStringLiteral(" and "))));
    ExportWeb::pictureLimitations(document, pictures.get(), &result.limitations);
    if (writer.remoteImages)
        result.limitations.append(QStringLiteral("Images from the web are not fetched, so their descriptions stand in "
                                                 "their place (%1)")
                                      .arg(Exporters::count(writer.remoteImages, QStringLiteral("image"), QStringLiteral("images"))));
    if (!writer.unreadImages.isEmpty())
        result.limitations.append(QStringLiteral("Images that could not be read are left out: %1")
                                      .arg(writer.unreadImages.join(QStringLiteral(", "))));
    if (writer.notesReferredAgain)
        result.limitations.append(QStringLiteral("Word links a note from one place only: where a note is referred to "
                                                 "again, its number stands (%1)")
                                      .arg(Exporters::count(writer.notesReferredAgain, QStringLiteral("place"), QStringLiteral("places"))));
    if (pictures) {
        result.helpers = pictures->helperReport();
        result.renderers.insert(QStringLiteral("svg"), SvgPaper::version());
    }
    result.renderers.insert(QStringLiteral("docx"), QStringLiteral("parch " PARCH_VERSION));
    if (!Exporters::writeFile(recipe.output, bytes, &result.error, recipe.existing)) {
        result.ioFailure = true;
        return result;
    }
    run.report(1, QStringLiteral("Written"));
    result.written.append(recipe.output);
    result.ok = true;
    return result;
}
