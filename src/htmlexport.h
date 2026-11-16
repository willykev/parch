#pragma once

#include "exporter.h"

// A standalone HTML page: the engine's safe HTML (mdhtml.h) in a page of its
// own, with export typography that does not depend on the desktop's theme
// (export.css, inside the page). Its title is the front matter's `title`,
// else the first heading, else the file's name; `author`, `description` and
// `lang` from the front matter go into the page's head. Relative links and
// images lead from the page's folder to the files the document means.
// Math is written as its TeX and Mermaid diagrams as their source, each in
// its own class, and said among the limitations.
class HtmlExporter : public Exporter {
public:
    QString format() const override { return QStringLiteral("html"); }
    QString title() const override { return QStringLiteral("HTML"); }
    QStringList suffixes() const override { return {QStringLiteral("html"), QStringLiteral("htm")}; }
    QList<ExportOption> options() const override;
    ExportResult write(const md::Document &document, const ExportRecipe &recipe) const override;

    // The page, for a file written into `outputFolder`.
    static QString page(const md::Document &document, const ExportRecipe &recipe, QStringList *limitations);

    // For md::HtmlOptions::destination: relative links and images as HTML
    // in `folder` reaches the files the document means, whichever of its
    // files they are written in. The document must outlive it.
    static std::function<QString(const md::Node &, const QString &)> destinationsFrom(const md::Document &document,
                                                                                     const QString &folder);
};
