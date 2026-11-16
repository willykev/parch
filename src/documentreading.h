#pragma once

#include "dialectchoice.h"
#include "documentcitations.h"
#include "mdhtml.h"
#include "mdtree.h"

#include <QString>

// The one reading of a document that every output makes: the effective
// dialect from its layers and its own front matter, its citation style and
// bibliography read, the text parsed with them, and its citations
// formatted. The command line's check, render, export and stats, the HTML
// export and the native preview read a document through read(); the views
// of the window, which only need the dialect's options, take them from the
// Document (Document::markdownOptions()), which holds the same layers.
//
// A pure function of its arguments and the files it reads, so it runs on a
// worker thread with a snapshot of the text.
namespace DocumentReading {

struct Reading {
    // With the front matter's bibliography and style.
    Dialect::Choice dialect;
    DocumentCitations::Setup citations;
    md::Document document;
    DocumentCitations::Formatted formatted;

    // The renderer's options for this document: its citations as its style
    // formats them, when one or a bibliography is chosen.
    md::HtmlOptions htmlOptions(md::HtmlOptions options = {}) const;
};

// The layers with the front matter's: what `bibliography:` and `csl:` say,
// paths found from `folder`, but only when the dialect the other layers
// choose reads front matter at all.
Dialect::Layers withFrontMatter(Dialect::Layers layers, const QString &text, const QString &folder);

// Reads a text: `source` says where it lives and how its includes are read;
// its library root, unless set, is the dialect's.
Reading read(const QString &text, const Dialect::Layers &layers, md::Source source);

} // namespace DocumentReading
