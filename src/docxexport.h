#pragma once

#include "exporter.h"

// A Word document (Office Open XML), written by hand from the semantic tree
// and packed with libzip, which Word and LibreOffice open as editable text:
//   - paragraph styles Title, Heading 1 to 6, Quote, Code, List Paragraph,
//     Footnote Text, Bibliography, Definition Term and Definition, and the
//     character styles Code Char, Hyperlink and Footnote Reference;
//   - bold, italic, strikethrough, highlighted, superscript and subscript
//     runs, links to the web and to headings (bookmarks), page breaks;
//   - bulleted, numbered and task lists with their levels, each numbered
//     list starting at its own first number;
//   - tables with their alignment, header rows repeated on each page and
//     merged cells (gridSpan, and vMerge for raw HTML rowspan);
//   - notes as Word footnotes, or endnotes when asked; citations and the
//     bibliography as the citation style words them;
//   - images as pictures with their alternative text, and formulas and
//     diagrams as pictures too, the formulas lowered onto the baseline, each
//     described by its TeX or its words;
//   - the document's properties (title, author, subject, keywords, language)
//     from the front matter.
// Formulas are pictures rather than Word equations, which the limitations say.
class DocxExporter : public Exporter {
public:
    QString format() const override { return QStringLiteral("docx"); }
    QString title() const override { return QStringLiteral("DOCX"); }
    QStringList suffixes() const override { return {QStringLiteral("docx")}; }
    QList<ExportOption> options() const override;
    QStringList limitations(const md::Document &document, const ExportRecipe &recipe,
                            const HelperSet &helpers) const override;
    ExportResult write(const md::Document &document, const ExportRecipe &recipe,
                       const ExportRun &run = ExportRun()) const override;
};
