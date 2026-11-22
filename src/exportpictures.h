#pragma once

#include "mdtree.h"
#include "renderrequest.h"
#include "renderservice.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QSizeF>
#include <QString>

#include <functional>

// The formulas and diagrams accepted for a document, drawn once each for
// an export by the same helpers and render service as
// the preview, in paper's colours whatever the desktop's theme. An export
// draws them all before it writes anything, on its own thread, which waits
// for them in an event loop of its own and stops at once when the export is
// cancelled.
//
// A formula comes back as RaTeX's SVG, which every format can use as it is
// or as a picture. A diagram comes back as Merman's picture at print scale
// and, when Qt draws the plain SVG librsvg makes of it (SvgPaper) as Merman
// drew it, as that SVG too, which a PDF holds as vectors.
class ExportPictures {
public:
    struct Picture {
        // False when there is no helper, or it refused or failed: then
        // `diagnostic` says why, and the source stands in its place.
        bool drawn = false;
        QString diagnostic;
        // What it shows, for alternative text: a diagram's words, a formula's TeX.
        QString description;
        // A formula: its SVG. A diagram: its plain SVG, when Qt draws it right.
        QByteArray svg;
        // A diagram as Merman drew it, at print scale.
        QByteArray png;
        // Merman's source drawing for a web page or book to turn into plain SVG.
        QByteArray markup;
        // In logical pixels, at the font size the pictures are drawn for.
        QSizeF size;
        // A formula's baseline from its top, in logical pixels.
        qreal baseline = 0;
        bool vector() const { return !svg.isEmpty(); }
    };

    struct Options {
        // Where the helpers are and the cache is; found as the application
        // finds them unless set.
        RenderService::Options service;
        RenderColors colors = paperColors();
        // The size of body text, in logical pixels: what formulas are drawn
        // at (a formula in a heading is scaled from it) and diagram labels too.
        qreal fontSize = 16;
        // The family diagram labels are set in; empty is the desktop's sans-serif.
        QString fontFamily;
    };

    // Dark ink on white, as on paper.
    static RenderColors paperColors();

    ExportPictures();
    explicit ExportPictures(const Options &options);

    // Draws everything the document needs; `progress` is told how many of
    // how many are done. False when `cancelled` said to stop first.
    bool draw(const md::Document &document, const std::function<bool()> &cancelled = {},
              const std::function<void(int done, int total)> &progress = {});

    // What was drawn for a formula or a diagram; null for one the document
    // did not hold.
    const Picture *formula(const QString &tex, bool display) const;
    const Picture *diagram(const QString &source) const;

    const Options &options() const { return m_options; }
    HelperSet helpers() const { return m_helpers; }
    // The helpers that drew, with their identities, for an export's report.
    QJsonObject helperReport() const;

    // How many formulas and diagrams the document holds, each counted once
    // for every place it stands.
    struct Counts {
        int formulas = 0;
        int formulasNotDrawn = 0;
        int diagrams = 0;
        int diagramsNotDrawn = 0;
        // Drawn, but printed as Merman's picture since Qt draws its SVG wrong.
        int diagramsAsPictures = 0;
    };
    Counts counts(const md::Document &document) const;
    static void noteDrawingLimit(const md::Document &document, QStringList &limitations);

    // Why every formula or every diagram stays its source, when a helper is
    // missing: "the math helper (render-svg) is not installed"; empty otherwise.
    QString missingMath() const;
    QString missingDiagrams() const;

private:
    static QString key(int kind, const QString &text, bool display);

    Options m_options;
    HelperSet m_helpers;
    QHash<QString, Picture> m_pictures;
};
