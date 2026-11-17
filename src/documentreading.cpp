#include "documentreading.h"

#include "frontmatter.h"
#include "mdinclude.h"

namespace DocumentReading {

QStringList referencedFiles(const md::Document &document, const Dialect::Choice &dialect, bool includeLinks)
{
    QStringList paths = dialect.bibliography;
    if (Dialect::isStyleFile(dialect.citationStyle)) paths.append(dialect.citationStyle);
    for (const auto &file : document.files) {
        paths.append(file.path);
        if (file.includedFrom >= 0 && file.frontMatter.present && document.options.citations) {
            const auto layer = Dialect::frontMatterLayer(file.frontMatter, file.directory);
            if (layer.bibliography) paths.append(*layer.bibliography);
        }
    }
    const auto walk = [&](const auto &visit, const md::Node &node) -> void {
        if (node.type == md::NodeType::Include && node.includedFile < 0 && node.file >= 0 && size_t(node.file) < document.files.size()) {
            paths.append(md::includeTarget(node.dest, document.files[size_t(node.file)].directory, dialect.libraryRoot));
        } else if (node.type == md::NodeType::Image || (includeLinks && node.type == md::NodeType::Link)
                   || (node.type == md::NodeType::HtmlElement && (node.label == u"img" || (includeLinks && node.label == u"a")))) {
            const auto dest = md::classifyDestination(document, node);
            if (dest.kind == md::Destination::Kind::LocalFile && dest.absolute) paths.append(dest.path);
        }
        for (const auto &child : node.children) visit(visit, child);
    };
    walk(walk, document.root);
    paths.removeAll(QString());
    paths.removeDuplicates();
    return paths;
}

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
    if (source.libraryRoot.isEmpty()) source.libraryRoot = reading.dialect.libraryRoot;
    // The front matter names files within the library, or the document's
    // folder, as includes are read: never anywhere else.
    reading.citations = DocumentCitations::prepare(reading.dialect, linkCitations,
                                                   DocumentCitations::Reach::of(source.libraryRoot, folder));
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
