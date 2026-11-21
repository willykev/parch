#pragma once

#include "cslreference.h"
#include "cslrichtext.h"
#include "cslstyle.h"

#include <QList>
#include <QString>

#include <memory>

// One cite within a citation: a reference, optionally with a locator and
// text before and after it.
struct CslCite {
    enum class Mode {
        Normal,
        // "[-@doe]": the author is left out, as when the text names them.
        SuppressAuthor,
        // Only the author, as in a narrative citation's name.
        AuthorOnly,
        // "@doe says": the author, then the rest of the citation, as in
        // "Doe (2006)".
        AuthorInText,
    };

    // The reference, or null for a key no database defines, which is shown
    // as the key and a question mark.
    const CslReference *reference = nullptr;
    QString missingKey;
    QString locator;
    // The locator's type as CSL names it ("page", "chapter", "section"...).
    QString label;
    // Free text before and after the cite; it may hold CSL JSON markup.
    QString prefix;
    QString suffix;
    Mode mode = Mode::Normal;
};

// One citation: what a note or a pair of brackets in the text holds.
struct CslCitation {
    QList<CslCite> cites;
    // The number of the footnote the citation is in, or 0 in running text.
    int noteIndex = 0;
};

struct CslBibliographyEntry {
    // The reference's id.
    QString id;
    CslText text;
    const CslReference *reference = nullptr;
};

// How the style lays out its bibliography.
struct CslBibliographyLayout {
    bool hangingIndent = false;
    // "flush" or "margin" when the first field stands apart, as numbers do.
    QString secondFieldAlign;
    double lineSpacing = 1;
    double entrySpacing = 1;
};

// A CSL 1.0.2 processor. It renders citations and a bibliography from a
// style, a locale and references, all given as plain data. It keeps no
// state between calls and may run on any thread.
class CslEngine {
public:
    struct Result {
        // One text per citation, in the order given.
        QList<CslText> citations;
        QList<CslBibliographyEntry> bibliography;
        CslBibliographyLayout layout;
        bool nestingLimited = false;
        bool renderLimited = false;
    };

    // The locale is the base for the style's language; the style's own
    // locale elements are applied over it.
    CslEngine(std::shared_ptr<const CslStyle> style, const CslLocale &locale);

    // Renders the citations in document order. The bibliography holds every
    // cited reference and, after them, the uncited ones given.
    Result process(const QList<CslCitation> &citations, const QList<const CslReference *> &uncited = {},
                   qsizetype maxCiteChars = 4 * 1024 * 1024) const;

    // Text that a locator holds, such as "p. 23" or "chaps. 2–3", read into
    // a locator type, the locator and what follows it. Without a known label
    // a locator that starts with a number is a page.
    struct Locator {
        QString label;
        QString locator;
        QString suffix;
    };
    static Locator parseLocator(const QString &text, const CslLocale &locale);
    Locator parseLocator(const QString &text) const { return parseLocator(text, m_locale); }

    const CslStyle &style() const { return *m_style; }
    const CslLocale &locale() const { return m_locale; }

private:
    std::shared_ptr<const CslStyle> m_style;
    CslLocale m_locale;
};
