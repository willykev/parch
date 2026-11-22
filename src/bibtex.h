#pragma once

#include "cslreference.h"

#include <QList>
#include <QString>

// Reads BibTeX and BibLaTeX databases into CSL items: @string macros and
// concatenation, braced and quoted values, LaTeX accents and common commands
// as Unicode and inline markup, names with von and Jr parts, BibLaTeX dates
// and ranges, crossref inheritance, and BibLaTeX's types and fields mapped
// to CSL's.
namespace BibTeX {

struct Entry {
    CslReference reference;
    // The line of the entry's '@', from 1.
    int line = 0;
};

struct Result {
    QList<Entry> entries;
    QList<BibliographyDiagnostic> diagnostics;
};

Result parse(const QString &text, qsizetype maxExpansion = qsizetype(64) << 20);

// The names of a name field, as written in the database ("Doe, John and
// Ludwig van Beethoven"). The von part becomes a non-dropping particle,
// unless useprefix is false, when it becomes a dropping one.
QList<CslName> parseNames(const QString &value, bool usePrefix = true);

// LaTeX text as CSL inline markup: accents and symbols become Unicode,
// \emph and its kind become <i>, <b>, small caps, <sup> and <sub>, and with
// protectCase, braced text becomes a nocase span. The result escapes '<',
// '>' and '&'.
QString toMarkup(const QString &latex, bool protectCase = false);

// A title in title case, such as BibTeX databases hold, in sentence case:
// capitalised words become lowercase, except the first word, the first word
// after a colon and words in nocase spans. Words with other capitals
// ("NASA", "iPhone") keep them.
QString sentenceCase(const QString &markup);

} // namespace BibTeX
