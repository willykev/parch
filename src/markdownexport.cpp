#include "markdownexport.h"

#include "dialectchoice.h"
#include "documentcitations.h"
#include "exportweb.h"
#include "htmlexport.h"
#include "saferead.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <map>

namespace MarkdownExport {
namespace {

// Labels match as CommonMark matches them: case and white space folded.
QString normalized(QStringView label)
{
    return label.toString().simplified().toCaseFolded();
}

// Text that reads back as itself: every ASCII punctuation mark escaped.
QString escaped(QStringView text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        if (c.unicode() < 128 && c.isPunct()) out.append(u'\\');
        else if (c.unicode() < 128 && c.isSymbol()) out.append(u'\\');
        out.append(c);
    }
    return out;
}

// A YAML scalar that reads back as `text`, whatever it holds.
QString yamlQuoted(const QString &text)
{
    QString out = text;
    out.replace(u'\\', QStringLiteral("\\\\")).replace(u'"', QStringLiteral("\\\""));
    out.replace(u'\n', QStringLiteral("\\n")).replace(u'\r', QStringLiteral("\\r")).replace(u'\t', QStringLiteral("\\t"));
    return u'"' + out + u'"';
}

QString yamlValue(const md::YamlValue &value)
{
    if (value.isScalar()) return yamlQuoted(value.scalar);
    QStringList items;
    if (value.isList()) {
        for (const auto &item : value.list) items << yamlValue(item);
        return u'[' + items.join(QStringLiteral(", ")) + u']';
    }
    if (value.isMap()) {
        for (const auto &[key, item] : value.map) items << yamlQuoted(key) + QStringLiteral(": ") + yamlValue(item);
        return u'{' + items.join(QStringLiteral(", ")) + u'}';
    }
    return QStringLiteral("null");
}

// The YAML without its top-level entries for `keys`: each key's line and the
// lines that carry on its value (indented, or a list item, blank or a comment
// among them).
QString withoutKeys(const md::FrontMatter &frontMatter, const QStringList &keys)
{
    static const QRegularExpression entry(QStringLiteral("^[\"']?([^\\s:\"'#][^:\"']*)[\"']?\\s*:(\\s|$)"));
    QString out;
    bool dropping = false;
    const QStringList lines = frontMatter.source.split(u'\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString &line = lines[i];
        const bool goesOn = line.isEmpty() || line.front().isSpace() || line.startsWith(u'#')
                            || line.startsWith(QLatin1String("- ")) || line == u"-" || line == u"\r";
        if (!goesOn) {
            const QRegularExpressionMatch key = entry.match(line);
            dropping = key.hasMatch() && keys.contains(key.captured(1).trimmed(), Qt::CaseInsensitive);
        }
        if (dropping) continue;
        out += line;
        if (i + 1 < lines.size()) out += u'\n';
    }
    // Keep the original layout when whole lines can be removed. Flow
    // mappings and escaped keys may instead need to be written from the
    // parsed values so that no old citation settings survive.
    const md::YamlValue kept = md::parseYaml(out);
    if (std::none_of(keys.cbegin(), keys.cend(), [&](const QString &key) { return kept.find(key); })) return out;
    out.clear();
    for (const auto &[key, value] : frontMatter.data.map)
        if (!keys.contains(key, Qt::CaseInsensitive)) out += yamlQuoted(key) + QStringLiteral(": ") + yamlValue(value) + u'\n';
    return out;
}

QString attributeEscaped(QStringView text)
{
    QString out;
    for (const QChar c : text) {
        if (c == u'&') out += QStringLiteral("&amp;");
        else if (c == u'"') out += QStringLiteral("&quot;");
        else if (c == u'<') out += QStringLiteral("&lt;");
        else out.append(c);
    }
    return out;
}

struct Edit {
    int start = 0;
    int end = 0;
    QString text;
};

QString applied(const QString &text, std::vector<Edit> edits)
{
    std::stable_sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.start < b.start; });
    QString out;
    out.reserve(text.size());
    int at = 0;
    for (const Edit &edit : edits) {
        // A file included twice gives its edits twice; overlaps are dropped.
        if (edit.start < at || edit.end > text.size()) continue;
        out.append(QStringView(text).sliced(at, edit.start - at));
        out.append(edit.text);
        at = edit.end;
    }
    out.append(QStringView(text).sliced(at));
    return out;
}

class Assembler {
public:
    Assembler(const md::Document &document, const QString &outputFolder, const Dialect::Choice &choice, const ImageDestination &image,
              const FileDestination &citationFile)
        : doc(document), folder(QDir::cleanPath(outputFolder)), dialect(choice), imageDestination(image), citationDestination(citationFile)
    {
    }

    Assembled run()
    {
        plan(doc.root);
        relabel(noteDefinitions, noteLabels, noteRenames);
        relabel(linkDefinitions, linkLabels, linkRenames);
        for (auto it = citations.cbegin(); it != citations.cend(); ++it) {
            QSet<QString> texts;
            for (const auto &[file, text] : it.value()) texts.insert(text);
            if (texts.size() > 1)
                limitations.append(
                    QStringLiteral("The citation [#%1] is defined differently in more than one file; the assembled "
                                   "Markdown uses the first definition")
                        .arg(it.key()));
        }
        return {assemble(0, doc.root), limitations};
    }

    // What the front matter says of the files citations are formatted with,
    // as the folder written to reaches them, and every bibliography an
    // included file names beside the document's own: the assembled text has
    // no included file of its own to name it. Nothing when nothing changes.
    std::optional<Edit> citationFiles()
    {
        if (!doc.options.frontMatter || doc.files.empty()) return std::nullopt;
        const md::SourceFile &root = doc.files.front();
        const Dialect::Layer own = Dialect::frontMatterLayer(root.frontMatter, root.directory);
        const QStringList original = own.bibliography.value_or(QStringList());
        const auto reach = DocumentCitations::Reach::of(dialect.libraryRoot, root.directory);
        bool outside = false;
        const auto allowed = [&](const QString &path) {
            if (reach.allows(path)) return true;
            outside = true;
            return false;
        };
        const bool ownBibliography = !hasCitations || dialect.bibliographyFrom == u"default";
        const bool authorBibliography = ownBibliography || dialect.bibliographyFrom == u"front-matter";
        QStringList bibliography = ownBibliography ? original : dialect.bibliography;
        QSet<QString> authorFiles;
        if (authorBibliography) {
            bibliography.removeIf([&](const QString &path) { return !allowed(path); });
            for (const QString &path : std::as_const(bibliography)) authorFiles.insert(path);
        }
        if (hasCitations)
            for (size_t i = 1; i < doc.files.size(); ++i) {
                const md::SourceFile &file = doc.files[i];
                if (!file.frontMatter.present || file.directory.isEmpty()) continue;
                const Dialect::Layer layer = Dialect::frontMatterLayer(file.frontMatter, file.directory);
                for (const QString &path : layer.bibliography.value_or(QStringList())) {
                    if (bibliography.contains(path) || !allowed(path)) continue;
                    bibliography.append(path);
                    authorFiles.insert(path);
                }
            }
        const bool ownStyle = !hasCitations || dialect.citationStyleFrom == u"default";
        const bool authorStyle = ownStyle || dialect.citationStyleFrom == u"front-matter";
        QString style = ownStyle ? own.citationStyle : dialect.citationStyle;
        if (authorStyle && Dialect::isStyleFile(style) && !allowed(style)) style.clear();
        if (outside)
            limitations.append(QStringLiteral("Citation files outside the document's allowed folders were omitted"));

        const bool moved = QDir::cleanPath(root.directory) != folder;
        const bool styleFile = Dialect::isStyleFile(style);
        const bool writeBibliography = bibliography != original || (moved && !bibliography.isEmpty());
        const bool writeStyle = style != own.citationStyle || (moved && styleFile);
        if (!writeBibliography && !writeStyle) return std::nullopt;

        // Front matter names files within its document's folder alone: one
        // the folder written to does not hold goes along with the text, as
        // its pictures do.
        bool uncopied = false;
        const auto written = [&](const QString &path, bool fromFrontMatter) {
            const QString relative = Exporters::relativeTo(folder, path);
            const bool within = !QDir::isAbsolutePath(relative) && relative != u".." && !relative.startsWith(QLatin1String("../"));
            if (within || !citationDestination) return relative;
            const QString placed = citationDestination(path, fromFrontMatter ? &reach.folders : nullptr);
            if (placed.isEmpty()) uncopied = true;
            return placed;
        };
        QStringList keys;
        QString entries;
        if (writeBibliography) {
            keys.append(QStringLiteral("bibliography"));
            QString names;
            for (const QString &path : std::as_const(bibliography)) {
                const QString name = written(path, authorFiles.contains(path));
                if (!name.isEmpty()) names += QStringLiteral("  - %1\n").arg(yamlQuoted(name));
            }
            if (!names.isEmpty()) entries += QStringLiteral("bibliography:\n") + names;
        }
        if (writeStyle) {
            keys.append(QStringLiteral("csl"));
            const QString name = styleFile ? written(style, authorStyle) : style;
            if (!name.isEmpty()) entries += QStringLiteral("csl: %1\n").arg(yamlQuoted(name));
        }
        if (uncopied) limitations.append(QStringLiteral("Citation files that could not be copied were omitted"));
        if (!root.frontMatter.present) {
            if (entries.isEmpty()) return std::nullopt;
            const int start = root.text.startsWith(QChar(0xfeff)) ? 1 : 0;
            return Edit{start, start, QStringLiteral("---\n") + entries + QStringLiteral("---\n\n")};
        }
        QString yaml = withoutKeys(root.frontMatter, keys);
        if (!yaml.isEmpty() && !yaml.endsWith(u'\n')) yaml += u'\n';
        return Edit{root.frontMatter.contentStart, root.frontMatter.contentEnd, yaml + entries};
    }

private:
    using Definitions = std::map<QString, std::vector<int>>; // label -> defining files, in file order
    using Renames = QHash<int, QHash<QString, QString>>;     // file -> label -> its new label

    // What every file defines, and every label in use.
    void plan(const md::Node &node)
    {
        for (const md::Node &child : node.children) {
            if (child.generated) continue;
            const QString label = normalized(child.label);
            switch (child.type) {
            case md::NodeType::FootnoteDefinition:
                addDefinition(noteDefinitions, label, child.file);
                noteLabels.insert(label);
                break;
            case md::NodeType::FootnoteReference:
                noteLabels.insert(label);
                break;
            case md::NodeType::LinkDefinition:
                addDefinition(linkDefinitions, label, child.file);
                linkLabels.insert(label);
                break;
            case md::NodeType::Link:
            case md::NodeType::Image:
                if (!child.label.isEmpty()) linkLabels.insert(label);
                break;
            case md::NodeType::Heading: {
                // A new link label must not take the name of a heading's.
                const QString title = normalized(md::plainText(child));
                if (!child.label.isEmpty()) {
                    linkLabels.insert(label);
                    if (label != title) ++headingAliases[label];
                }
                linkLabels.insert(title);
                ++headingAliases[title];
                break;
            }
            case md::NodeType::Citation:
                hasCitations = true;
                break;
            case md::NodeType::CitationDefinition: {
                QString text = doc.source(child).toString();
                text.remove(0, doc.source(child.file, child.open).size());
                citations[child.label.toCaseFolded()].append({child.file, text.simplified()});
                break;
            }
            default:
                break;
            }
            plan(child);
        }
    }

    static void addDefinition(Definitions &definitions, const QString &label, int file)
    {
        std::vector<int> &files = definitions[label];
        if (std::find(files.begin(), files.end(), file) == files.end()) files.push_back(file);
    }

    // A label defined in more than one file stays with the document's own
    // definition, or else the first file's; every other file's gets a new one.
    static void relabel(const Definitions &definitions, QSet<QString> &labels, Renames &renames)
    {
        for (const auto &[label, files] : definitions) {
            if (files.size() < 2) continue;
            const int keeper = std::find(files.begin(), files.end(), 0) != files.end()
                                   ? 0
                                   : *std::min_element(files.begin(), files.end());
            for (const int file : files) {
                if (file == keeper) continue;
                int suffix = 2;
                QString fresh;
                do fresh = label + u'-' + QString::number(suffix++);
                while (labels.contains(fresh));
                labels.insert(fresh);
                renames[file].insert(label, fresh);
            }
        }
    }

    QString assemble(int file, const md::Node &container)
    {
        std::vector<Edit> edits;
        if (file == 0)
            if (const std::optional<Edit> front = citationFiles()) edits.push_back(*front);
        collect(file, container, edits);
        return applied(doc.files[size_t(file)].text, std::move(edits));
    }

    // The block at either edge of an include after its front matter and
    // any nested include lines have been removed.
    static const md::Node *includeEdge(const md::Node &node, bool first)
    {
        if (node.generated || node.type == md::NodeType::FrontMatter) return nullptr;
        if (node.type != md::NodeType::Include || !node.resolved) return &node;
        for (size_t i = 0; i < node.children.size(); ++i) {
            const md::Node &child = node.children[first ? i : node.children.size() - 1 - i];
            if (const md::Node *edge = includeEdge(child, first)) return edge;
        }
        return nullptr;
    }

    static bool listsJoin(const md::Node *before, const md::Node *after)
    {
        return before && after && before->type == md::NodeType::List && after->type == md::NodeType::List
               && before->ordered == after->ordered && before->marker == after->marker;
    }

    void collect(int file, const md::Node &node, std::vector<Edit> &edits)
    {
        const md::Node *previous = nullptr;
        for (size_t i = 0; i < node.children.size(); ++i) {
            const md::Node &child = node.children[i];
            if (child.generated || child.file != file) continue;
            if (child.type == md::NodeType::Include && child.resolved && child.includedFile >= 0) {
                const md::Node *first = includeEdge(child, true);
                const md::Node *last = includeEdge(child, false);
                const md::Node *following = i + 1 < node.children.size() ? &node.children[i + 1] : nullptr;
                const bool before = listsJoin(previous, first);
                if (last) previous = last;
                include(file, child, edits, before, listsJoin(previous, following));
                continue;
            }
            previous = &child;
            visit(file, child, edits);
            collect(file, child, edits);
        }
    }

    void include(int file, const md::Node &node, std::vector<Edit> &edits, bool separateBefore, bool separateAfter)
    {
        QString text = assemble(node.includedFile, node);
        while (text.endsWith(u'\n') || text.endsWith(u'\r')) text.chop(1);
        // A fence the included file leaves open ends with that file.
        if (!node.children.empty()) {
            const md::Node &last = node.children.back();
            if ((last.type == md::NodeType::CodeBlock || last.type == md::NodeType::MermaidBlock) && last.fenced &&
                last.close.isEmpty()) {
                const QStringView open = doc.source(last.file, last.open).trimmed();
                qsizetype run = 0;
                while (run < open.size() && open[run] == last.marker) ++run;
                text += u'\n' + open.first(run).toString();
            }
        }
        // A blank line alone would merge these lists on the next read.
        if (separateBefore) text.prepend(QStringLiteral("\n<!-- -->\n\n"));
        if (separateAfter) text += QStringLiteral("\n\n<!-- -->");
        // What follows the include line starts a block of its own.
        const QString &around = doc.files[size_t(file)].text;
        QStringView rest = QStringView(around).sliced(node.range.end);
        if (rest.startsWith(u"\r\n")) rest = rest.sliced(2);
        else if (rest.startsWith(u'\n') || rest.startsWith(u'\r')) rest = rest.sliced(1);
        if (!rest.isEmpty() && !rest.startsWith(u'\n') && !rest.startsWith(u'\r')) text += u'\n';
        edits.push_back({node.range.start, node.range.end, text});
    }

    void visit(int file, const md::Node &node, std::vector<Edit> &edits)
    {
        switch (node.type) {
        case md::NodeType::FrontMatter:
            if (file != 0) edits.push_back({node.range.start, doc.files[size_t(file)].frontMatter.bodyStart, {}});
            break;
        case md::NodeType::Variable:
            if (file != 0 && node.resolved) {
                const auto own = doc.files[size_t(file)].frontMatter.scalar(node.label);
                if (own && doc.files[0].frontMatter.scalar(node.label) != own)
                    edits.push_back({node.range.start, node.range.end, escaped(node.text)});
            }
            break;
        case md::NodeType::FootnoteDefinition:
            if (const QString fresh = noteRenames.value(file).value(normalized(node.label)); !fresh.isEmpty())
                edits.push_back({node.open.start, node.open.end, QStringLiteral("[^%1]:").arg(fresh)});
            break;
        case md::NodeType::FootnoteReference:
            if (const QString fresh = noteRenames.value(file).value(normalized(node.label)); !fresh.isEmpty())
                edits.push_back({node.range.start, node.range.end, QStringLiteral("[^%1]").arg(fresh)});
            break;
        case md::NodeType::LinkDefinition: {
            if (const QString fresh = linkRenames.value(file).value(normalized(node.label)); !fresh.isEmpty())
                edits.push_back({node.open.start, node.open.end, QStringLiteral("[%1]:").arg(fresh)});
            const QString anchor = file > 0 && node.dest.startsWith(u'#') ? md::anchorInFile(doc, file, node.dest.mid(1)) : QString();
            if (!anchor.isEmpty() && anchor != QUrl::fromPercentEncoding(node.dest.mid(1).toUtf8()))
                edits.push_back({node.target.start, node.target.end, u'#' + anchor});
            else
                rebase(file, node.target, node.dest, edits);
            break;
        }
        case md::NodeType::Link:
        case md::NodeType::Image:
            if (node.type == md::NodeType::Image && copyImage(node, edits)) break;
            if (node.label.isEmpty() && file != 0 && node.type == md::NodeType::Link && node.dest.startsWith(u'#')
                && !node.target.isEmpty()) {
                // An anchor of the included file's own, under the name it
                // has in the whole document (see md::anchorInFile()).
                const QStringView written = doc.source(file, node.target);
                if (QUrl::fromPercentEncoding(written.toString().mid(1).toUtf8()) != node.dest.mid(1))
                    edits.push_back({node.target.start, node.target.end, node.dest});
            } else if (node.label.isEmpty()) {
                rebase(file, node.target, node.dest, edits);
            } else if (node.type == md::NodeType::Link && node.destinationFile < 0 && node.dest.startsWith(u'#')
                       && (headingAliases.value(normalized(node.label)) > 1 || linkDefinitions.contains(normalized(node.label)))) {
                // Flattening the files loses the heading reference's local scope.
                edits.push_back({node.close.start, node.close.end, QStringLiteral("](%1)").arg(node.dest)});
            } else if (const QString fresh = linkRenames.value(node.destinationFile >= 0 ? node.destinationFile : file)
                                               .value(normalized(node.label));
                       !fresh.isEmpty()) {
                // `[text][label]` names its label after the text; `[label][]`
                // and `[label]` are named by their text, which stays.
                if (node.target.start >= node.close.start && !node.close.isEmpty())
                    edits.push_back({node.target.start, node.target.end, fresh});
                else
                    edits.push_back({node.close.start, node.close.end, QStringLiteral("][%1]").arg(fresh)});
            }
            break;
        case md::NodeType::HtmlElement:
            if (node.label == u"a" || node.label == u"img") rebaseAttribute(file, node, edits);
            break;
        default:
            break;
        }
    }

    // A relative destination, as the folder written to reaches the same file.
    QString rebased(int file, const QString &dest) const
    {
        if (dest.isEmpty() || dest.startsWith(u'/') || dest.startsWith(u'#')) return {};
        const QString &directory = doc.files[size_t(file)].directory;
        if (directory.isEmpty() || QDir::cleanPath(directory) == folder) return {};
        const md::Destination target = md::classifyDestination(dest, directory);
        if (target.kind != md::Destination::Kind::LocalFile || !target.absolute) return {};
        QString written = md::encodeDestination(Exporters::relativeTo(folder, target.path));
        if (!target.fragment.isEmpty()) written += u'#' + target.fragment;
        return written;
    }

    // An image the writer has copied beside the output, at its new place.
    bool copyImage(const md::Node &node, std::vector<Edit> &edits) const
    {
        if (!imageDestination) return false;
        const md::Destination found = md::classifyDestination(doc, node);
        if (found.kind != md::Destination::Kind::LocalFile || !found.absolute) return false;
        const QString placed = imageDestination(found.path);
        if (placed.isEmpty()) return false;
        QString dest = md::encodeDestination(placed);
        if (!found.fragment.isEmpty()) dest += u'#' + found.fragment;
        if (node.label.isEmpty()) {
            edits.push_back({node.target.start, node.target.end, dest});
        } else {
            // Inline only the picture. Ordinary links using the same definition keep their destination.
            if (!node.title.isEmpty()) {
                QString title = node.title;
                title.replace(u'\\', QStringLiteral("\\\\")).replace(u'"', QStringLiteral("\\\""));
                dest += QStringLiteral(" \"%1\"").arg(title);
            }
            edits.push_back({node.close.start, node.close.end, QStringLiteral("](%1)").arg(dest)});
        }
        return true;
    }

    void rebase(int file, md::Range target, const QString &dest, std::vector<Edit> &edits) const
    {
        if (target.isEmpty()) return;
        const QString written = rebased(file, dest);
        if (!written.isEmpty()) edits.push_back({target.start, target.end, written});
    }

    void rebaseAttribute(int file, const md::Node &node, std::vector<Edit> &edits) const
    {
        const QLatin1StringView name(node.label == u"img" ? "src" : "href");
        QString dest;
        for (const md::Attribute &attribute : node.attributes)
            if (attribute.name == name) dest = attribute.value;
        QString written;
        if (node.label == u"img" && imageDestination && !dest.isEmpty()) {
            const md::Destination found = md::classifyDestination(dest, doc.files[size_t(file)].directory);
            if (found.kind == md::Destination::Kind::LocalFile && found.absolute)
                if (const QString placed = imageDestination(found.path); !placed.isEmpty()) {
                    written = QString::fromUtf8(QUrl::toPercentEncoding(placed, "/"));
                    if (!found.fragment.isEmpty()) written += u'#' + found.fragment;
                }
        }
        if (written.isEmpty() && file != 0 && node.label == u"a" && node.resolved && dest.startsWith(u'#'))
            written = dest;
        if (written.isEmpty()) written = rebased(file, dest);
        if (written.isEmpty()) return;
        static const QRegularExpression value(
            QStringLiteral("\\s(src|href)\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s\"'=<>`]+)"),
            QRegularExpression::CaseInsensitiveOption);
        const QStringView tag = doc.source(node.file, node.open);
        for (auto match = value.globalMatchView(tag); match.hasNext();) {
            const QRegularExpressionMatch found = match.next();
            if (found.captured(1).compare(name, Qt::CaseInsensitive) != 0) continue;
            const int start = node.open.start + int(found.capturedStart(2));
            const QString replacement = u'"' + attributeEscaped(written) + u'"';
            if (found.captured(2) != replacement)
                edits.push_back({start, start + int(found.capturedLength(2)), replacement});
            return;
        }
    }

    const md::Document &doc;
    const QString folder;
    const Dialect::Choice &dialect;
    const ImageDestination imageDestination;
    const FileDestination citationDestination;
    Definitions noteDefinitions, linkDefinitions;
    QSet<QString> noteLabels, linkLabels;
    QHash<QString, int> headingAliases;
    Renames noteRenames, linkRenames;
    QHash<QString, QList<std::pair<int, QString>>> citations;
    bool hasCitations = false;
    QStringList limitations;
};

} // namespace

Assembled assemble(const md::Document &document, const QString &outputFolder, const Dialect::Choice &dialect,
                   const ImageDestination &image,
                   const FileDestination &citationFile)
{
    return Assembler(document, outputFolder, dialect, image, citationFile).run();
}

} // namespace MarkdownExport

QList<ExportOption> MarkdownExporter::options() const
{
    ExportOption asIs{QStringLiteral("as-is"),
                      QStringLiteral("Write the document's own text as it is, include lines and all, "
                                     "rather than with its includes in place"),
                      QString()};
    asIs.label = QStringLiteral("Write the text as it is, without its includes in place");
    ExportOption images{QStringLiteral("images"),
                        QStringLiteral("Keep images reachable from the new document (auto, the default), "
                                       "link them in place (link), or copy them all into NAME_files (copy)"),
                        QStringLiteral("HOW")};
    images.label = QStringLiteral("Images");
    images.choices = {QStringLiteral("auto"), QStringLiteral("link"), QStringLiteral("copy")};
    images.defaultValue = QStringLiteral("auto");
    return {asIs, images};
}

QStringList MarkdownExporter::limitations(const md::Document &document, const ExportRecipe &recipe,
                                          const HelperSet &) const
{
    QStringList limitations;
    if (recipe.options.contains(QStringLiteral("as-is"))) {
        if (document.files.size() > 1)
            limitations.append(QStringLiteral("Included files stay include lines (%1): keep them where the text "
                                              "finds them")
                                   .arg(Exporters::count(int(document.files.size()) - 1, QStringLiteral("file"),
                                                         QStringLiteral("files"))));
        const QString from = document.files.empty() ? QString() : document.files.front().directory;
        if (!from.isEmpty() && QDir::cleanPath(from) != QFileInfo(recipe.output).absolutePath())
            limitations.append(QStringLiteral("Relative links and images are written as they are, so from another "
                                              "folder they may lead nowhere"));
    }
    return limitations;
}

ExportResult MarkdownExporter::write(const md::Document &document, const ExportRecipe &recipe, const ExportRun &run) const
{
    ExportResult result;
    if (run.stopped()) return Exporters::cancelled();
    result.renderers.insert(QStringLiteral("markdown"), QStringLiteral("parch " PARCH_VERSION));
    const QString outputFolder = QFileInfo(recipe.output).absolutePath();
    const QString folder = HtmlExporter::assetFolder(recipe.output);
    ExportWeb::Assets assets(ExportWeb::Assets::Mode::Copy, QFileInfo(folder).fileName() + u'/', outputFolder);
    if (recipe.options.contains(QStringLiteral("as-is"))) {
        result.limitations = limitations(document, recipe, {});
        const QString text = document.files.empty() ? QString() : document.files.front().text;
        if (!ExportWeb::writePage(document, recipe, text.toUtf8(), folder, assets, &result.error)) {
            result.ioFailure = true;
            return result;
        }
        result.written.append(recipe.output);
        result.ok = true;
        return result;
    }
    const QString images = option(recipe, QStringLiteral("images"));
    QStringList unread;
    MarkdownExport::ImageDestination image;
    if (images != u"link") {
        const QString root = QFileInfo(outputFolder).canonicalFilePath();
        image = [&, root](const QString &path) {
            if (images == u"auto" && SafeRead::pictureRefusal(path, {root}).isEmpty()) return QString();
            QString error;
            const QString url = assets.file(path, document.resourceFolders, &error);
            if (url.isEmpty()) unread.append(QStringLiteral("%1 (%2)").arg(QFileInfo(path).fileName(), error));
            // The text gives the file's path, not its URL.
            return QUrl::fromPercentEncoding(url.toUtf8());
        };
    }
    // A bibliography or style the new folder cannot reach goes along with
    // the text, beside its pictures.
    const MarkdownExport::FileDestination citationFile = [&](const QString &path, const QStringList *folders) {
        const SafeRead::Result file = folders ? SafeRead::readWithin(path, SafeRead::ReferenceBytes, *folders)
                                              : SafeRead::read(path, SafeRead::ReferenceBytes);
        if (!file.ok) return QString();
        return QUrl::fromPercentEncoding(assets.copy(file.bytes, QFileInfo(path).fileName()).toUtf8());
    };
    const MarkdownExport::Assembled assembled = MarkdownExport::assemble(document, outputFolder, recipe.dialect, image, citationFile);
    result.limitations = assembled.limitations;
    if (!unread.isEmpty())
        result.limitations.append(QStringLiteral("Images that could not be read are linked where they were: %1")
                                      .arg(unread.join(QStringLiteral(", "))));
    if (run.stopped()) return Exporters::cancelled();
    // Nothing is written through a link, the folder of pictures included.
    if (!ExportWeb::writePage(document, recipe, assembled.text.toUtf8(), folder, assets, &result.error)) {
        result.ioFailure = true;
        return result;
    }
    result.written.append(recipe.output);
    for (const ExportWeb::Assets::Asset &asset : assets.assets()) result.written.append(folder + u'/' + asset.name);
    result.ok = true;
    return result;
}

