#pragma once

#include "exportpictures.h"
#include "mdtree.h"
#include "previewbuilder.h"
#include "previewstyle.h"

#include <QHash>
#include <QList>
#include <QPageLayout>
#include <QPageSize>
#include <QString>
#include <QStringList>
#include <QTextCursor>
#include <QTextDocument>

#include <functional>
#include <memory>
#include <vector>

class Exporter;
class QPagedPaintDevice;
class QPainter;
struct ExportRecipe;
namespace DocumentCitations {
struct Formatted;
}

// How a document is set on paper: the page, its margins and the type.
struct PaperOptions {
    QPageSize pageSize = QPageSize(QPageSize::A4);
    QPageLayout::Orientation orientation = QPageLayout::Portrait;
    // Every side, in millimetres.
    qreal margin = 20;
    // The reading face; empty takes the paper style's.
    QString family;
    // Body text, in points.
    qreal fontSize = 11;
    // The distance between lines, as a multiple of the text's size.
    qreal lineHeight = 1.5;
    bool pageNumbers = true;
    // Maximum number of decoded picture pixels kept by one paper document.
    qint64 picturePixelBudget = 512'000'000;

    // The page sizes the options offer, by the names they take.
    static QStringList pageSizes();
    // The templates: classic (a serif face made for reading, 11 points,
    // generous lines), modern (the desktop's sans-serif, a little smaller)
    // and compact (10 points, close lines and 15 mm margins). The font, its
    // size and the margins chosen as well go over the template's.
    static QStringList templates();
    static QPageSize sizeNamed(const QString &name);
    // From a PDF recipe's options; `why` says what was given wrong.
    static PaperOptions from(const Exporter &exporter, const ExportRecipe &recipe, QString *why = nullptr);
};

// A document on paper: the preview's builder (preview::Builder) makes it
// in paper's style, with tables, code and display math in place, and here
// it is laid out on pages and drawn, a page at a time, into a PDF or onto a
// printer. What the screen shows over the text is drawn here instead: the
// bars beside quotes and the rules. On paper:
//   - `+++` starts a new page, and a heading is never left at the foot of
//     a page apart from what follows it;
//   - links lead where they lead (other documents and the web), and links
//     within the document, such as to a note or a heading, go there;
//   - formulas and diagrams are vectors where they can be (a diagram whose
//     SVG Qt draws wrongly is Merman's picture at print scale); images are
//     whole, scaled only to fit the page;
//   - a formula sits on the baseline of the text around it, its depth below.
// Everything here may run on a worker thread.
class PaperDocument {
public:
    PaperDocument(const md::Document &document, const DocumentCitations::Formatted *citations,
                  const ExportPictures *pictures, const PaperOptions &options);
    ~PaperDocument();

    // Builds the document and lays it out on pages for the device, whose
    // resolution must be 96 dots per inch: a logical pixel is a device's.
    // `going`, when given, is told how far it has come, from 0 to 1, as it
    // builds and lays out block by block, and stops it by answering false:
    // then nothing is laid out as it should be, and layOut() is false.
    bool layOut(QPagedPaintDevice *device, const std::function<bool(qreal done)> &going = {},
                const QString &outputFolder = {});

    int pageCount() const;
    // The page, from 0, of the line a position of the laid-out text is in.
    int pageAt(int position) const;
    // The whole page, in logical pixels.
    QSizeF pageSize() const;
    // Draws one page, from 0, at the painter's origin.
    void paint(QPainter &painter, int page) const;

    // The document's headings, where they are on paper: for a PDF's outline.
    struct Heading {
        int level = 1;
        QString text;
        int page = 0;
        // From the top of the page, in logical pixels.
        qreal top = 0;
    };
    QList<Heading> headings() const { return m_headings; }

    // What paper could not show as the document has it: images that could
    // not be read or are on the web.
    QStringList limitations() const;

    // Where each formula in the text was drawn, once a page with it has
    // been painted, for tests: its baseline and the baseline of the line it
    // stands in, in logical pixels from the top of the document.
    struct Placed {
        QString tex;
        qreal baseline = 0;
        qreal lineBaseline = 0;
    };
    QList<Placed> formulaPlaces() const;

    const QTextDocument *document() const { return &m_text; }

private:
    class Resources;
    // Cursors, which follow the text as blocks are split between pages.
    struct Rule {
        enum Kind { QuoteBar, Line, ShortLine };
        Kind kind = Line;
        QTextCursor first;
        QTextCursor last;
        qreal x = 0;
    };

    void markNames(const std::vector<preview::Section> &sections);
    void ruleTables();
    void breakPages(const std::vector<preview::Section> &sections);
    void keepHeadingsWithText();
    // A paragraph that runs from one page onto the next becomes two, one on
    // each page, so that each page draws only its own lines.
    void splitAcrossPages();
    void relayOut();
    // Lays the text out block by block, telling `m_going` how far it has
    // come within [from, to] as it goes; false once it says to stop.
    bool layOutBlocks(qreal from, qreal to);
    // Whether `m_going` says to stop, at this much of the whole.
    bool stopping(qreal done);
    void findHeadings();
    int pageOf(qreal y) const;
    qreal blockBottom(const QTextBlock &block) const;

    const md::Document &m_source;
    const DocumentCitations::Formatted *m_citations;
    PaperOptions m_options;
    PreviewStyle m_style;
    QTextDocument m_text;
    std::function<bool(qreal done)> m_going;
    bool m_stopped = false;
    std::unique_ptr<Resources> m_resources;
    // Draws the pictures: formulas and diagrams as vectors where it can.
    std::unique_ptr<QObject> m_objects;
    QList<Rule> m_rules;
    // Heading blocks: where they are, their levels and texts.
    struct HeadingBlock {
        QTextCursor at;
        int level = 1;
        QString text;
    };
    QList<HeadingBlock> m_headingBlocks;
    // The blocks that break the page after them.
    QList<QTextCursor> m_breaks;
    QList<Heading> m_headings;
    QSizeF m_page;
    QRectF m_body;
};
