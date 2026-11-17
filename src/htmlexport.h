#pragma once

#include "exporter.h"
#include "exportpictures.h"
#include "exportweb.h"

// A standalone HTML page: the engine's safe HTML (mdhtml.h) in a page of its
// own, with export typography that does not depend on the desktop's theme
// (export.css, inside the page). Its title is the front matter's `title`,
// else the first heading, else the file's name; `author`, `description` and
// `lang` from the front matter go into the page's head. Relative links lead
// from the page's folder to the files the document means.
//
// Formulas are inline SVG, sized in ems and named by their TeX for
// assistive technology; diagrams are SVG pictures described by their words.
// Local images are copied into a folder beside the page (NAME_files), or
// written into the page itself, or linked where they are, as the `images`
// option says. Without the helpers, formulas and diagrams are written as
// their source, which the limitations say.
class HtmlExporter : public Exporter {
public:
    QString format() const override { return QStringLiteral("html"); }
    QString title() const override { return QStringLiteral("HTML"); }
    QStringList suffixes() const override { return {QStringLiteral("html"), QStringLiteral("htm")}; }
    QList<ExportOption> options() const override;
    QStringList limitations(const md::Document &document, const ExportRecipe &recipe,
                            const HelperSet &helpers) const override;
    ExportResult write(const md::Document &document, const ExportRecipe &recipe,
                       const ExportRun &run = ExportRun()) const override;

    // The folder a page's pictures are copied into: "notes_files" beside "notes.html".
    static QString assetFolder(const QString &output);

    // For md::HtmlOptions::destination: relative links and images as HTML
    // in `folder` reaches the files the document means, whichever of its
    // files they are written in. The document must outlive it.
    static std::function<QString(const md::Node &, const QString &)> destinationsFrom(const md::Document &document,
                                                                                     const QString &folder);
};

namespace ExportWeb {

// The body of a page or a book's text: the engine's HTML with formulas,
// diagrams and images as `pictures` and `assets` give them, and counts what
// it could not include. For a book, pictures from the web are left out and
// diagrams are plain SVG (SvgPaper::plain), which any reader can draw.
struct Body {
    QString html;
    int imagesLinked = 0;
    int remoteImages = 0;
    QStringList unreadImages;
    QStringList fallbackLinks;
};
Body body(const md::Document &document, const ExportRecipe &recipe, const ExportPictures *pictures, Assets &assets,
          bool book);

// What the document's formulas and diagrams come to in a format that draws
// them as the helpers do: none drawn without a helper, some refused.
void pictureLimitations(const md::Document &document, const ExportPictures *pictures, QStringList *limitations);

} // namespace ExportWeb
