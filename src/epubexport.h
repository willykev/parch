#pragma once

#include "exporter.h"

// An EPUB 3 book: the document as XHTML from the engine's safe HTML
// renderer, a file for each chapter (see chapters()) in the spine's order,
// a navigation document listing its headings in their files, the export
// typography as its style sheet, its images, formulas as inline SVG named by
// their TeX and diagrams as plain SVG pictures (librsvg's shapes and
// outlines of Merman's drawing, which any reader draws without CSS), and
// metadata from the front matter: title,
// author, language, description, date and an identifier. The archive holds
// `mimetype` first and stored, as the container format requires.
//
// Pictures from the web are not packed, so their descriptions stand in their
// place; page breaks are hints, as a reader sets its own pages.
class EpubExporter : public Exporter {
public:
    QString format() const override { return QStringLiteral("epub"); }
    QString title() const override { return QStringLiteral("EPUB"); }
    QStringList suffixes() const override { return {QStringLiteral("epub")}; }
    QList<ExportOption> options() const override;
    QStringList limitations(const md::Document &document, const ExportRecipe &recipe,
                            const HelperSet &helpers) const override;
    ExportResult write(const md::Document &document, const ExportRecipe &recipe,
                       const ExportRun &run = ExportRun()) const override;

    // HTML from the renderer as XHTML a book holds: alignment as style,
    // task boxes as characters, characters XML refuses left out.
    static QString xhtml(const QString &html);

    // The text as the book's files, one per top-level section: a file starts
    // at each heading of level 1 that stands at the top level of the text,
    // or of level 2 when the document has one level-1 heading or none. What
    // comes before the first is a file of its own. Links within the text
    // lead into whichever file holds their target. A text with no such
    // heading is one file, text.xhtml.
    struct Chapter {
        QString name; // "chapter-2.xhtml"
        QString html; // the body's content
    };
    static QList<Chapter> chapters(const QString &body, const md::Document &document);
};
