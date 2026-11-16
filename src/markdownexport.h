#pragma once

#include "exporter.h"

// The assembled Markdown of a document: its own text with every file it
// includes written in place of the include line, so that it reads the same
// as one file. Since the included text is no longer in its own file:
//   - an included file's front matter is left out, and a variable whose
//     value came from it is written as that value;
//   - a footnote or link definition of an included file that another file
//     also defines is given a label of its own, as are the references to it
//     from that file, so that each still leads where it did;
//   - relative links and images are written relative to the folder the
//     Markdown is written to, so they find the same files from there.
// Everything else is the source as written, punctuation and all.
namespace MarkdownExport {

struct Assembled {
    QString text;
    QStringList limitations;
};

Assembled assemble(const md::Document &document, const QString &outputFolder);

} // namespace MarkdownExport

class MarkdownExporter : public Exporter {
public:
    QString format() const override { return QStringLiteral("md"); }
    QString title() const override { return QStringLiteral("Markdown"); }
    QStringList suffixes() const override { return {QStringLiteral("md"), QStringLiteral("markdown")}; }
    ExportResult write(const md::Document &document, const ExportRecipe &recipe) const override;
};
