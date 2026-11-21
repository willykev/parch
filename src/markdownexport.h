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

// Where an image the text names, by its absolute path, is written to lead:
// a path relative to the output's folder, or empty to lead where it is.
using ImageDestination = std::function<QString(const QString &absolutePath)>;

// Places the same bytes that were read as a bibliography or a CSL style,
// returning their path from the assembled document's folder.
using FileDestination = std::function<QString(const QByteArray &bytes, const QString &name)>;
Assembled assemble(const md::Document &document, const QString &outputFolder, const Dialect::Choice &dialect,
                   const ImageDestination &image = {},
                   const FileDestination &citationFile = {});

} // namespace MarkdownExport

// The document as Markdown again: assembled (as above), its local images
// led to where they are or copied into a folder beside it, or its own text
// as it is (the `as-is` option), include lines and all.
class MarkdownExporter : public Exporter {
public:
    QString format() const override { return QStringLiteral("md"); }
    QString title() const override { return QStringLiteral("Markdown"); }
    QStringList suffixes() const override { return {QStringLiteral("md"), QStringLiteral("markdown")}; }
    QList<ExportOption> options() const override;
    QStringList limitations(const md::Document &document, const ExportRecipe &recipe,
                            const HelperSet &helpers) const override;
    ExportResult write(const md::Document &document, const ExportRecipe &recipe,
                       const ExportRun &run = ExportRun()) const override;
};
