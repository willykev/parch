#include "documentreading.h"

#include "frontmatter.h"

namespace DocumentReading {

md::HtmlOptions Reading::htmlOptions(md::HtmlOptions options) const
{
    options.citations = DocumentCitations::html(formatted);
    return options;
}

Dialect::Layers withFrontMatter(Dialect::Layers layers, const QString &text, const QString &folder)
{
    layers.frontMatter = {};
    if (!Dialect::effective(layers).options.frontMatter) return layers;
    layers.frontMatter = Dialect::frontMatterLayer(md::parseFrontMatter(text), folder);
    return layers;
}

Reading read(const QString &text, const Dialect::Layers &layers, md::Source source)
{
    Reading reading = parse(text, layers, source);
    reading.formatted = DocumentCitations::format(reading.document, reading.citations);
    return reading;
}

Reading parse(const QString &text, const Dialect::Layers &layers, md::Source source)
{
    Reading reading;
    const QString folder = !source.directory.isEmpty() ? source.directory
                           : source.path.contains(u'/') ? source.path.left(source.path.lastIndexOf(u'/'))
                                                         : QString();
    const Dialect::Layers all = withFrontMatter(layers, text, folder);
    reading.dialect = Dialect::effective(all);
    const bool linkCitations =
        reading.dialect.options.frontMatter && Dialect::linkCitations(md::parseFrontMatter(text));
    reading.citations = DocumentCitations::prepare(reading.dialect, linkCitations);
    if (source.libraryRoot.isEmpty()) source.libraryRoot = reading.dialect.libraryRoot;
    reading.dialect.limit(source);
    source.externalCitationKeys = reading.citations.keys();
    source.citationNotes = reading.citations.noteStyle();
    reading.document = md::parse(text, reading.dialect.options, source);
    // Included chapters may name bibliographies of their own.
    if (reading.dialect.options.citations && reading.dialect.options.frontMatter
        && DocumentCitations::addIncludedBibliographies(reading.citations, reading.document)) {
        source.externalCitationKeys = reading.citations.keys();
        reading.document = md::parse(text, reading.dialect.options, source);
    }
    return reading;
}

} // namespace DocumentReading
