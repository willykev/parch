#pragma once

#include "citations.h"
#include "dialectchoice.h"
#include "mdhtml.h"
#include "mdtree.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <memory>
#include <optional>

// A document's citations as its citation style formats them. The engine's
// tree holds the citations, Writer `[p. 23][#Doe:2006]` and Pandoc
// `[see @doe, p. 23]` alike, and the Writer bibliography written in the
// text; the dialect (dialectchoice.h) names the bibliography files and the
// style, from the document's settings or its front matter. Here they meet:
// prepare() reads the style and the files, format() runs the CSL processor
// over the citations in the order a reader meets them, notes included, and
// html() gives md::toHtml what it writes in place of the engine's own
// numbered citations. The HTML export, `parch render`, `parch check` and the
// native preview all read citations this way; see documentreading.h for the
// whole reading of a document.
//
// Everything here is plain data or a pure function of it, apart from the
// files prepare() reads, so it runs on a worker thread.
namespace DocumentCitations {

using Database = CitationFormatter::Database;

// Where a document's front matter may name files: within its library, or,
// for a document in no library, within its own folder and the folders in
// it. A document that came from someone else so reads none of the writer's
// other files. What the writer chooses in Document settings, in Preferences
// or on the command line is read wherever it is.
struct Reach {
    // Canonical folders.
    QStringList folders;
    // The first of them is the document's library.
    bool library = false;

    // The folders as they are named: the library, when there is one, and
    // the document's folder, each with its links resolved.
    static Reach of(const QString &library, const QString &folder);
    // Whether a front matter's file, a full path, lies within them, its
    // links resolved.
    bool allows(const QString &path) const;
    // Why a file outside them is not read: "it is outside the library".
    QString outside() const;
};

// What is wrong with a document's citations, its style or its bibliography.
struct Problem {
    // "missing-citation", "duplicate-citation-key", "unreadable-bibliography",
    // "bibliography", "citation-style", "no-citation-style",
    // "citation-locale" or "separate-numbering".
    QString code;
    md::Severity severity = md::Severity::Warning;
    QString message;
    QString key;
    // Where in the document: an index into md::Document::files and a range
    // there; -1 when the problem is not at a place in the text.
    int file = -1;
    md::Range range;
    // Otherwise the bibliography or style file it is in, and the line there
    // (0 when unknown).
    QString source;
    int line = 0;
};

// What a document's citations are formatted with: plain data, safe to hand
// to a worker.
struct Setup {
    // Whether a style or a bibliography is chosen at all. Without either,
    // the engine's own numbering of Writer citations stands.
    bool chosen = false;
    std::shared_ptr<const CslStyle> style; // null when none is chosen or it cannot be used
    QString styleName;                     // the style as chosen: an id or a path
    // The root document's bibliography, in order.
    QList<Database> databases;
    // The bibliographies of included files that name their own, by their
    // canonical paths; the others use the root's.
    QHash<QString, QList<Database>> fileDatabases;
    // Front matter `link-citations: true`: citations link to their entries.
    bool linkCitations = false;
    // The style and the files that could not be read, and what is wrong in them.
    QList<Problem> problems;
    // Where the front matter of the document and of the files it includes
    // may name files; unset, anywhere.
    std::optional<Reach> reach;

    bool noteStyle() const { return style && style->isNoteStyle(); }
    // Every key a bibliography defines, for md::Source::externalCitationKeys.
    QSet<QString> keys() const;
};

// Reads the style and the bibliography the dialect names. Each file is read
// once for as long as it is unchanged, by every thread. A file the front
// matter names is read only within `reach`, when it is given; one outside
// it is a problem that says so.
Setup prepare(const Dialect::Choice &dialect, bool linkCitations = false, const std::optional<Reach> &reach = std::nullopt);
// Adds the bibliographies the front matter of included files names, from
// their own folders. True when that brings keys the parse did not know, so
// that the document is to be parsed again with them.
bool addIncludedBibliographies(Setup &setup, const md::Document &document);

// The citations of a document, formatted.
struct Formatted {
    struct Citation {
        // The Citation node: one that stands alone, or the one that holds a
        // group. It points into the document formatted, which must outlive
        // this as long as the pointer is used; `file` and `range` say the
        // same without it.
        const md::Node *node = nullptr;
        int file = 0;
        md::Range range;
        // The keys it cites, in order.
        QStringList keys;
        CslText text;
        // The note it is in, or that it is, from 1; 0 in the running text.
        int note = 0;
    };
    // Whether the style formatted them; false leaves the engine's own.
    bool active = false;
    // In the order a reader meets them, a note's where the note is referred to.
    QList<Citation> citations;
    QList<CitationBibliographyEntry> bibliography;
    CslBibliographyLayout layout;
    bool noteStyle = false;
    bool linkCitations = false;
    QList<Problem> problems;

    // The formatted text of a citation node, by its file and place; null for
    // a node that is no citation of the document formatted.
    const Citation *find(const md::Node &node) const;
    const Citation *find(int file, int start) const;

private:
    friend Formatted format(const md::Document &document, const Setup &setup);
    QHash<const md::Node *, int> m_byNode;
    // By file and place: the first citation there, for a tree of the same text.
    QHash<quint64, int> m_index;
};

Formatted format(const md::Document &document, const Setup &setup);

// The HTML of formatted citations, for md::HtmlOptions::citations: each
// citation as <span class="citation" data-cites="key">…</span>, linked to its
// entry with link-citations, and the bibliography as
// <section class="bibliography csl-bib-body"><div class="csl-entry"
// id="ref-key">…</div>…</section>. Nothing when the style formatted nothing.
std::optional<md::CitationHtml> html(const Formatted &formatted);

// Reader text with the same citations and bibliography as the formatted document.
QString plainText(const md::Document &document, const Formatted &formatted);

// The id of a bibliography entry in HTML: "ref-" and the key, spaces as hyphens.
QString entryId(const QString &key);

// A Writer bibliography definition's text, `[#key]: *Title*. Press.`, as
// formatted text: emphasis, strong text, super- and subscripts and links
// kept, the rest as its reading text.
CslText definitionText(const md::Node &definition);

} // namespace DocumentCitations
