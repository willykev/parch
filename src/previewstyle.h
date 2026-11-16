#pragma once

#include <QColor>
#include <QFont>
#include <QString>
#include <QStringList>

class Theme;

// The typography and colours a document is set in when it is built for
// reading: on screen, from the live theme and the reading settings, or on
// paper, where the theme has no say. The builder that turns a parsed
// document into a QTextDocument reads nothing else about how it should
// look, so the preview, PDF export and printing differ only in the style
// they hand it.
//
// Colours are named by what they are for. A run of text that is not in the
// plain text colour carries its role with it (PreviewStyle::InkProperty,
// PaperProperty), so that a change of colours alone can be applied to a
// built document without building it again.
struct PreviewStyle {
    // What a run of text is drawn in. Plain text has no role and takes the
    // colour of the surface it is shown on.
    enum Ink : quint8 {
        NoInk,
        Heading,
        Dim,     // list markers, note numbers, captions
        Faint,
        Link,
        Code,    // code spans and code without a language
        Marked,  // highlighted text, on the highlight
        Danger,  // what could not be drawn or found
        Keyword,
        Type,
        String,
        Number,
        Comment,
        Function,
        Variable,
        Operator,
        Preprocessor,
        Constant,
        InkCount,
    };
    // What lies behind a run of text.
    enum Paper : quint8 { NoPaper, CodePaper, MarkPaper, PaperCount };

    // Format properties that name the roles of a run.
    static const int InkProperty;
    static const int PaperProperty;

    // The reading face, the face code is set in, the size of body text in
    // pixels, and the distance between its lines as a multiple of that size.
    QString family;
    QString codeFamily;
    qreal size = 18;
    qreal lineHeight = 1.6;
    // Device pixels per logical pixel, for what the builder draws itself.
    qreal scale = 1;

    bool dark = false;
    QColor text;
    QColor background;
    QColor accent;
    // Rules, table lines and the bars beside quotes.
    QColor rule;
    QColor quoteBar;
    QColor selection;
    QColor inks[InkCount];
    QColor papers[PaperCount];

    QColor ink(Ink role) const { return role > NoInk && role < InkCount ? inks[role] : text; }
    QColor paper(Paper role) const { return role > NoPaper && role < PaperCount ? papers[role] : background; }

    // The live theme's colours, with every derived colour kept readable on
    // what it lies on, and the reading settings.
    static PreviewStyle screen(const Theme &theme, const QString &family, qreal size, qreal lineHeight);
    // Black on white in a serif, whatever the desktop looks like: what
    // exports and prints start from.
    static PreviewStyle paper();
    // The colours of `colors`, with this style's typography.
    PreviewStyle withColorsOf(const PreviewStyle &colors) const;

    // Whether two styles lay text out alike: everything but colour.
    bool sameTypography(const PreviewStyle &other) const;
    bool sameColors(const PreviewStyle &other) const;

    QFont bodyFont() const;
    QFont codeFont() const;
    // Sizes in pixels.
    qreal headingSize(int level) const;
    qreal codeSize() const { return size * 0.88; }
    qreal smallSize() const { return size * 0.88; }
    // The distance between the lines of text of a size.
    qreal lineSpacing(qreal fontSize) const;
    // Between paragraphs.
    qreal gap() const { return size * 0.85; }
    // How far a quote, a list or a description steps in.
    qreal indent() const { return size * 1.5; }
    // Around the text of a code block and the cells of a table.
    qreal padding() const { return size * 0.7; }

    // The reading face used when none is chosen: the first installed of a
    // list of faces made for reading at length, or the desktop's serif.
    static QString defaultFamily();
    // The same from a list of installed families, for tests.
    static QString defaultFamily(const QStringList &installed);

    // The WCAG contrast ratio of two opaque colours, from 1 to 21.
    static qreal contrast(const QColor &a, const QColor &b);
    // The colour, moved towards `towards` until it stands out from
    // `background` by at least `minimum`; as it is when it already does.
    static QColor readable(const QColor &color, const QColor &background, const QColor &towards, qreal minimum = 4.5);
};
