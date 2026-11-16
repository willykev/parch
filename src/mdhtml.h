#pragma once

#include "mdtree.h"

// HTML from a parsed document: what the HTML export writes, and the form in
// which the engine is checked against the CommonMark and GFM examples.
//
// The output never carries anything a document could run. Text is escaped.
// Link and image URLs that are scripts are emptied. HTML comments are left
// out. Raw HTML follows the policy below.
//
// The raw HTML policy (the parser applies it, so the preview shares it):
//   supported, text level   a b i em strong u s del ins mark code kbd sub sup,
//                           and the empty br and img
//   supported, blocks       p blockquote pre h1-h6 ul ol li dl dt dd hr
//                           table caption thead tbody tfoot tr th td
//   supported attributes    a: href title; img: src alt title width height;
//                           ol: start; td, th: colspan rowspan align
// In the tree a supported element is an HtmlElement node: `label` is its
// name, `attributes` are those the policy allows, `children` is what lies
// between its tags. In a paragraph, the two tags of an element must lie in
// the same parent to pair; in a raw HTML block the block's text is read as a
// fragment, where li, dt, dd, p, tr, td, th and the table sections need no
// closing tag. Everything else (other elements, a tag without its partner,
// processing instructions, declarations) is HtmlInline or Text: markup that
// is shown as the text it is, with an UnsupportedHtml diagnostic.
//
// Beyond CommonMark and GFM the output is:
//   headings          <h2 id="slug">
//   highlight         <mark>, super/subscript <sup> <sub>
//   math              <span class="math">TeX</span>, <span class="math display">,
//                     and <div class="math display">TeX</div> for a block
//   Mermaid           <pre class="mermaid">source</pre>
//   task items        <li><input disabled="" type="checkbox" /> text</li>
//   merged cells      <td colspan="2">
//   definition lists  <dl> <dt> <dd>
//   page breaks       <div class="page-break"></div>
//   contents          <nav class="toc"><ul>…</ul></nav>
//   note references   <sup class="footnote-ref"><a href="#fn-1" id="fnref-1">1</a></sup>
//   notes             <section class="footnotes"><ol><li id="fn-1">… <a href="#fnref-1" class="footnote-backref">↩</a>
//   citations         <span class="citation">[<a href="#cite-1">1</a>, p. 23]</span>
//   bibliography      <section class="bibliography"><ol><li id="cite-1">…
// What could not be resolved (a note without a definition, a variable
// without a value, an include that was not read) is written as its source.
namespace md {

struct HtmlOptions {
    enum class RawHtml {
        Safe,        // the HTML policy
        Passthrough, // raw HTML and URLs exactly as written; only for checking the CommonMark examples
    };
    RawHtml rawHtml = RawHtml::Safe;
    bool headingIds = true;
    bool endnotes = true; // the notes and the bibliography after the text
    // Where a link or an image leads in the output, for a page written
    // somewhere other than beside its source: called with the Link,
    // Autolink or Image node, or the `a` or `img` HtmlElement, and the
    // destination (href or src) as the document has it, it returns the one
    // to write. A script URL it returns is still emptied in Safe mode. When
    // unset, every destination is written as the document has it.
    std::function<QString(const Node &node, const QString &destination)> destination;

    // The plain CommonMark rendering the specification's examples show.
    static HtmlOptions commonMark();
};

QString toHtml(const Document &document, const HtmlOptions &options = HtmlOptions());

QString escapeHtml(QStringView text);
// A URL as an href carries it: characters outside the URL syntax
// percent-encoded as UTF-8, existing escapes kept.
QString encodeUrl(QStringView url);

} // namespace md
