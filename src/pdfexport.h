#pragma once

#include "exporter.h"
#include "exportpictures.h"
#include "paperdocument.h"

#include <QBuffer>
#include <QPdfWriter>

// A PDF of the document on paper (PaperDocument): real pages of the size and
// margins chosen, in a type of its own whatever the desktop's theme, with
// searchable and selectable text, links, an outline of the headings, page
// numbers, formulas and diagrams as vectors where they can be, and images
// whole. Printing prints the same PDF.
class PdfExporter : public Exporter {
public:
    QString format() const override { return QStringLiteral("pdf"); }
    QString title() const override { return QStringLiteral("PDF"); }
    QStringList suffixes() const override { return {QStringLiteral("pdf")}; }
    QList<ExportOption> options() const override;
    QStringList limitations(const md::Document &document, const ExportRecipe &recipe,
                            const HelperSet &helpers) const override;
    ExportResult write(const md::Document &document, const ExportRecipe &recipe,
                       const ExportRun &run = ExportRun()) const override;

    // The PDF's bytes, for an export or for printing; empty, with the
    // reason in `result`, when it could not be made or was cancelled.
    QByteArray render(const md::Document &document, const ExportRecipe &recipe, const ExportRun &run,
                      ExportResult *result) const;

    // The document set on pages exactly as render() sets them, before they
    // are drawn: what a print preview shows. Null, with the reason in
    // `result`, when the options are wrong or the run was stopped.
    struct Pages {
        std::unique_ptr<ExportPictures> pictures;
        QBuffer buffer;
        // The paint device the pages were laid out for.
        std::unique_ptr<QPdfWriter> writer;
        std::unique_ptr<PaperDocument> paper;
    };
    std::unique_ptr<Pages> setPages(const md::Document &document, const ExportRecipe &recipe, const ExportRun &run,
                                    ExportResult *result) const;
};
