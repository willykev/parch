#include "markdownexport.h"

#include "dialectchoice.h"
#include "documentcitations.h"
#include "exportweb.h"
#include "htmlexport.h"
#include "mdhtml.h"
#include "mdparser_p.h"
#include "mdtable.h"
#include "saferead.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <map>
#include <set>

namespace MarkdownExport {
namespace {

// Labels match as CommonMark matches them: case and white space folded.
QString normalized(QStringView label)
{
    return md::detail::normalizeLabel(label);
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

struct Edit {
    int start = 0;
    int end = 0;
    QString text;
};

QString applied(QStringView text, std::vector<Edit> edits, int start = 0)
{
    std::stable_sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.start < b.start; });
    QString out;
    out.reserve(text.size());
    int at = start;
    for (const Edit &edit : edits) {
        if (edit.start < at || edit.end > start + text.size()) continue;
        out.append(text.sliced(at - start, edit.start - at));
        out.append(edit.text);
        at = edit.end;
    }
    out.append(text.sliced(at - start));
    return out;
}

QStringView lineEnding(QStringView text, int at)
{
    const QStringView rest = text.sliced(at);
    if (rest.startsWith(u"\r\n")) return rest.first(2);
    if (rest.startsWith(u'\n') || rest.startsWith(u'\r')) return rest.first(1);
    return {};
}

QString withTitle(QString destination, const QString &title)
{
    if (title.isEmpty()) return destination;
    QString written = title;
    written.replace(u'\\', QStringLiteral("\\\\")).replace(u'"', QStringLiteral("\\\""))
        .replace(u'&', QStringLiteral("\\&"));
    return destination + QStringLiteral(" \"") + written + u'"';
}

// An inline note's lines remain inline content in its named definition.
void appendNoteLine(QString &out, QStringView line)
{
    static const QRegularExpression blockStart(QStringLiteral(
        "^[ \\t]*(?:(>)|(#)(?=#{0,5}(?:[ \\t]|$))|([-+*:])(?=[ \\t]|$)|[0-9]{1,9}([.)])(?=[ \\t]|$)"
        "|(=+|-+)(?=[ \\t]*$)|(?<rule>[-*_])(?=(?:[ \\t]*\\k<rule>){2,}[ \\t]*$)|(/)(?=/)"
        "|(\\x60)(?=\\x60\\x60[^\\x60]*$)|(~)(?=~~)|\\[[^\\]]+\\](:))"));
    const auto starts = blockStart.matchView(line);
    qsizetype marker = starts.hasMatch() ? starts.capturedStart(starts.lastCapturedIndex()) : -1;
    if (marker < 0 && line.contains(u'|') && MdTable::delimiterRow(line, 0, int(line.size()))) marker = line.indexOf(u'|');
    if (marker >= 0) {
        out.append(line.first(marker));
        out += u'\\';
        line = line.sliced(marker);
    }
    out.append(line);
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
        allocateLabels();
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
        QStringList outside;
        const auto allowed = [&](const QString &path) {
            if (reach.allows(path)) return true;
            outside.append(QFileInfo(path).fileName());
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
        if (!outside.isEmpty()) {
            outside.removeDuplicates();
            limitations.append(QStringLiteral("Citation files outside the document's allowed folders were omitted: %1")
                                   .arg(outside.join(QStringLiteral(", "))));
        }

        const bool moved = QDir::cleanPath(root.directory) != folder;
        const bool styleFile = Dialect::isStyleFile(style);
        const bool rebaseBibliography = bibliography != original || (moved && !bibliography.isEmpty());
        const bool rebaseStyle = style != own.citationStyle || (moved && styleFile);
        // Check the bytes of the opened file before naming it or putting
        // those same bytes alongside the assembled document.
        const auto written = [&](const QString &path, bool fromFrontMatter, bool isStyle) -> QString {
            const QString name = QFileInfo(path).fileName();
            const auto omit = [&](const QString &why) {
                limitations.append(QStringLiteral("Omitted %1 (%2)").arg(name, why));
                return QString();
            };
            const auto format = BibliographyDatabase::formatForPath(path);
            if (isStyle && !name.endsWith(QStringLiteral(".csl"), Qt::CaseInsensitive))
                return omit(QStringLiteral("A citation style needs a .csl filename."));
            if (!isStyle && format == BibliographyDatabase::Format::Auto)
                return omit(QStringLiteral("The filename has no bibliography extension."));
            const auto file = fromFrontMatter ? SafeRead::readWithin(path, SafeRead::ReferenceBytes, reach.folders)
                                              : SafeRead::read(path, SafeRead::ReferenceBytes);
            if (!file.ok) return omit(file.error);
            QString why;
            bool usable;
            if (isStyle) {
                usable = bool(CslStyle::fromXml(file.bytes, &why));
                if (usable) {
                    QXmlStreamReader xml(file.bytes);
                    usable = xml.readNextStartElement() && xml.namespaceUri() == u"http://purl.org/net/xbiblio/csl";
                    if (!usable) why = QStringLiteral("The style needs a CSL root element.");
                }
            } else {
                const auto db = BibliographyDatabase::fromData(file.bytes, format, path);
                usable = db.isValid() && db.hasReferenceEntries();
                if (!usable) {
                    why = QStringLiteral("The bibliography contains no readable entries.");
                    for (const auto &d : db.diagnostics())
                        if (d.severity == BibliographyDiagnostic::Severity::Error) { why = d.message; break; }
                }
            }
            if (!usable) return omit(why);
            const QString relative = Exporters::relativeTo(folder, path);
            const bool within = !QDir::isAbsolutePath(relative) && relative != u".." && !relative.startsWith(QLatin1String("../"));
            if (within || !citationDestination || !(isStyle ? rebaseStyle : rebaseBibliography)) return relative;
            return citationDestination(file.bytes, name);
        };
        QStringList names;
        for (const QString &path : std::as_const(bibliography)) {
            const QString name = written(path, authorFiles.contains(path), false);
            if (!name.isEmpty()) names.append(name);
        }
        const QString styleName = styleFile ? written(style, authorStyle, true) : style;
        const bool writeBibliography = rebaseBibliography || names.size() != bibliography.size();
        const bool writeStyle = rebaseStyle || (styleFile && styleName.isEmpty());
        if (!writeBibliography && !writeStyle) return std::nullopt;
        QStringList keys;
        QString entries;
        if (writeBibliography) {
            keys.append(QStringLiteral("bibliography"));
            if (!names.isEmpty()) {
                entries += QStringLiteral("bibliography:\n");
                for (const QString &name : std::as_const(names))
                    entries += QStringLiteral("  - %1\n").arg(yamlQuoted(name));
            }
        }
        if (writeStyle) {
            keys.append(QStringLiteral("csl"));
            if (!styleName.isEmpty()) entries += QStringLiteral("csl: %1\n").arg(yamlQuoted(styleName));
        }
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
    using Definitions = std::map<QString, std::vector<int>>;
    using Renames = QHash<int, QHash<QString, QString>>;
    using ReferenceKey = std::pair<int, QString>;
    struct Reference {
        QString label;
        bool image = false;
    };

    void plan(const md::Node &node)
    {
        if (node.generated) return;
        using Type = md::NodeType;
        if (node.type == Type::LinkDefinition || node.type == Type::FootnoteDefinition) {
            if (!sourceDefinitions.emplace(node.file, node.range.start).second) {
                duplicateDefinitions.insert(&node);
                return;
            }
        }
        switch (node.type) {
        case md::NodeType::FootnoteDefinition: {
            auto &files = noteDefinitions[normalized(node.label)];
            if (std::find(files.begin(), files.end(), node.file) == files.end()) files.push_back(node.file);
            break;
        }
        case md::NodeType::LinkDefinition:
            references.try_emplace({node.file, normalized(node.label)});
            break;
        case md::NodeType::Image:
            if (node.destinationFile >= 0)
                references[{node.destinationFile, normalized(node.label)}].image = true;
            break;
        case md::NodeType::Heading: {
            const QString title = normalized(md::plainText(node));
            const QString label = normalized(node.label);
            if (!label.isEmpty() && label != title) ++headingAliases[label];
            ++headingAliases[title];
            break;
        }
        case md::NodeType::Citation:
            hasCitations = true;
            break;
        case md::NodeType::CitationDefinition: {
            QString text = doc.source(node).toString();
            text.remove(0, doc.source(node.file, node.open).size());
            citations[node.label.toCaseFolded()].append({node.file, text.simplified()});
            break;
        }
        default:
            break;
        }
        for (const md::Node &child : node.children) plan(child);
    }

    void allocateLabels()
    {
        if (doc.files.size() <= 1) return;
        const auto repeated = [](const md::Footnote &note) {
            return note.references > 1 && note.content && note.content->type == md::NodeType::InlineNote;
        };
        if (std::none_of(references.cbegin(), references.cend(), [](const auto &ref) { return ref.first.first != 0; })
            && std::none_of(noteDefinitions.cbegin(), noteDefinitions.cend(), [](const auto &note) { return note.second.size() > 1; })
            && std::none_of(doc.footnotes.cbegin(), doc.footnotes.cend(), repeated)) return;
        // Reserve a spelling absent from all the source text as the parser
        // folds labels, including brackets that do not resolve to a link.
        static const QRegularExpression prefix(QStringLiteral("PARCH-(?:(\\d+)-)?(?:LINK|NOTE)-"));
        QSet<int> used;
        for (const md::SourceFile &file : doc.files) {
            const QString folded = normalized(file.text);
            for (const auto &match : prefix.globalMatch(folded))
                used.insert(match.captured(1).isEmpty() ? 1 : match.captured(1).toInt());
        }
        int space = 1;
        while (used.contains(space)) ++space;
        const QString stem = space == 1 ? QStringLiteral("parch-") : QStringLiteral("parch-%1-").arg(space);
        int link = 0;
        for (auto &[key, reference] : references)
            if (key.first != 0) reference.label = stem + QStringLiteral("link-") + QString::number(++link);
        int note = 0;
        for (const auto &[label, files] : noteDefinitions) {
            if (files.size() < 2) continue;
            const int keeper = *std::min_element(files.begin(), files.end());
            for (const int file : files)
                if (file != keeper)
                    noteRenames[file].insert(label, stem + QStringLiteral("note-") + QString::number(++note));
        }
        for (const md::Footnote &footnote : doc.footnotes)
            if (repeated(footnote))
                inlineLabels.insert(footnote.number, stem + QStringLiteral("note-") + QString::number(++note));
    }

    const Reference &reference(int file, QStringView label) const
    {
        static const Reference empty;
        const auto found = references.find({file, normalized(label)});
        return found == references.end() ? empty : found->second;
    }

    QString noteDefinitionsForOutput()
    {
        QString definitions;
        for (const md::Footnote &note : doc.footnotes) {
            const auto label = inlineLabels.constFind(note.number);
            if (label == inlineLabels.cend()) continue;
            const md::Node &inlineNote = *note.content;
            const md::Range body{inlineNote.open.end, inlineNote.close.start};
            std::vector<Edit> edits;
            collect(note.file, inlineNote, edits);
            joinCodeSpanLines(inlineNote, edits);
            for (const md::Range prefix : inlineNote.markers) edits.push_back({prefix.start, prefix.end, {}});
            const QString written = applied(doc.source(note.file, body), std::move(edits), body.start).trimmed();
            definitions += QStringLiteral("[^%1]: ").arg(*label);
            // A reference definition opens the paragraph and disappears
            // when it is read, keeping leading display math inside it.
            if (written.startsWith(u"$$")) definitions += QStringLiteral("[%1]: <>\n    ").arg(*label);
            const QStringView text(written);
            for (int start = 0, end = 0; start < written.size(); start = end) {
                end = start;
                while (end < written.size() && text[end] != u'\r' && text[end] != u'\n') ++end;
                appendNoteLine(definitions, text.sliced(start, end - start));
                if (end == written.size()) break;
                const QStringView ending = lineEnding(text, end);
                end += int(ending.size());
                definitions.append(ending);
                definitions += QStringLiteral("    ");
            }
            definitions += QStringLiteral("\n\n");
        }
        return definitions;
    }

    void joinCodeSpanLines(const md::Node &node, std::vector<Edit> &edits) const
    {
        if (node.type != md::NodeType::Code) {
            for (const md::Node &child : node.children) joinCodeSpanLines(child, edits);
            return;
        }
        const QStringView code = doc.source(node);
        for (int i = 0; i < code.size();) {
            const QStringView ending = lineEnding(code, i);
            if (ending.isEmpty()) {
                ++i;
            } else {
                edits.push_back({node.range.start + i, node.range.start + i + int(ending.size()), QStringLiteral(" ")});
                i += int(ending.size());
            }
        }
    }

    QString assemble(int file, const md::Node &container)
    {
        std::vector<Edit> edits;
        const md::SourceFile &source = doc.files[size_t(file)];
        if (file == 0) {
            if (const auto front = citationFiles()) edits.push_back(*front);
            if (!inlineLabels.isEmpty()) {
                // Definitions must stay outside the blocks the book ends with,
                // in particular a fence or HTML block left open on purpose.
                const int at = source.frontMatter.present ? source.frontMatter.bodyStart
                             : source.text.startsWith(QChar(0xfeff)) ? 1 : 0;
                QString written = noteDefinitionsForOutput();
                if (at > 1 && !source.text.startsWith(QChar(0xfeff)) && lineEnding(source.text, at - 1).isEmpty())
                    written.prepend(u'\n');
                edits.push_back({at, at, written});
            }
        }
        collect(file, container, edits);
        return applied(source.text, std::move(edits));
    }

    // The block at either edge of an include after its front matter and
    // any nested include lines have been removed.
    const md::Node *includeEdge(const md::Node &node, bool first) const
    {
        if (node.generated || node.type == md::NodeType::FrontMatter || duplicateDefinitions.contains(&node)) return nullptr;
        if (node.type != md::NodeType::Include || !node.resolved) return &node;
        for (size_t i = 0; i < node.children.size(); ++i) {
            const md::Node &child = node.children[first ? i : node.children.size() - 1 - i];
            if (const md::Node *edge = includeEdge(child, first)) return edge;
        }
        return nullptr;
    }

    static bool joins(const md::Node *before, const md::Node *after)
    {
        if (!before || !after) return false;
        using Type = md::NodeType;
        const auto indented = [](const md::Node &node) { return node.type == Type::CodeBlock && !node.fenced; };
        if (indented(*after))
            return indented(*before) || before->type == Type::List || before->type == Type::DefinitionList;
        if (before->type == Type::DefinitionList) return after->type == Type::DefinitionList;
        return before->type == Type::List && after->type == Type::List
            && before->ordered == after->ordered && before->marker == after->marker;
    }

    void collect(int file, const md::Node &node, std::vector<Edit> &edits)
    {
        const md::Node *previous = nullptr;
        std::optional<size_t> previousSplice;
        for (const md::Node &child : node.children) {
            if (child.file != file || (child.generated && child.type != md::NodeType::HtmlElement)) continue;
            if (duplicateDefinitions.contains(&child)) {
                if (previous) previousSplice = edits.size();
                edits.push_back({child.range.start, child.range.end, {}});
                continue;
            }
            if (child.type == md::NodeType::Include && child.resolved && child.includedFile >= 0) {
                const md::Node *first = includeEdge(child, true);
                const md::Node *last = includeEdge(child, false);
                const size_t at = edits.size();
                edits.push_back(include(file, child, joins(previous, first)));
                if (last) previous = last;
                if (previous) previousSplice = at;
                continue;
            }
            // An include on the right owns the join before it. An ordinary
            // block on the right can instead use the end of the include.
            if (previousSplice && joins(previous, &child)) {
                Edit &preceding = edits[*previousSplice];
                const QStringView ending = lineEnding(doc.files[size_t(file)].text, preceding.end);
                const QString newline = ending.isEmpty() ? QStringLiteral("\n") : ending.toString();
                preceding.text += newline + newline + QStringLiteral("<!-- -->");
            }
            previous = &child;
            previousSplice.reset();

            using Type = md::NodeType;
            if (child.type == Type::InlineNote) {
                const auto label = inlineLabels.constFind(child.number);
                if (label != inlineLabels.cend()) {
                    edits.push_back({child.range.start, child.range.end, QStringLiteral("[^%1]").arg(*label)});
                    continue;
                }
            }
            visit(file, child, edits);
            collect(file, child, edits);
        }
    }

    Edit include(int file, const md::Node &node, bool separateBefore)
    {
        QString text = assemble(node.includedFile, node);
        const QString &around = doc.files[size_t(file)].text;
        const QStringView ending = lineEnding(around, node.range.end);
        const QString newline = ending.isEmpty() ? QStringLiteral("\n") : ending.toString();
        if (!node.children.empty()) {
            const md::Node &last = node.children.back();
            if ((last.type == md::NodeType::CodeBlock || last.type == md::NodeType::MermaidBlock) && last.fenced &&
                last.close.isEmpty()) {
                const QStringView open = doc.source(last.file, last.open).trimmed();
                qsizetype run = 0;
                while (run < open.size() && open[run] == last.marker) ++run;
                if (!text.endsWith(u'\r') && !text.endsWith(u'\n')) text += newline;
                text += open.first(run);
            }
        }
        while (text.endsWith(u'\n') || text.endsWith(u'\r')) text.chop(1);
        if (separateBefore) text.prepend(newline + QStringLiteral("<!-- -->") + newline + newline);
        const int next = node.range.end + int(ending.size());
        if (next < around.size() && lineEnding(around, next).isEmpty()) text += newline;
        // The include is a whole line. Its indentation should not become
        // indentation of the chapter's first block.
        int start = node.range.start;
        while (start > 0 && (around[start - 1] == u' ' || around[start - 1] == u'\t')) --start;
        return {start, node.range.end, std::move(text)};
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
            if (node.number > 0 && size_t(node.number) <= doc.footnotes.size()) {
                const int origin = doc.footnotes[size_t(node.number - 1)].file;
                const QString fresh = noteRenames.value(origin).value(normalized(node.label));
                if (!fresh.isEmpty()) edits.push_back({node.range.start, node.range.end, QStringLiteral("[^%1]").arg(fresh)});
            }
            break;
        case md::NodeType::LinkDefinition: {
            const Reference &ref = reference(file, node.label);
            if (!ref.label.isEmpty())
                edits.push_back({node.open.start, node.open.end, QStringLiteral("[%1]:").arg(ref.label)});
            QString destination;
            if (file > 0 && node.dest.startsWith(u'#')) {
                const QString anchor = md::anchorInFile(doc, file, node.dest.mid(1));
                if (!anchor.isEmpty() && anchor != md::classifyDestination(node.dest, {}).fragment)
                    destination = u'#' + anchor;
            } else {
                destination = rewritten(file, node.dest, !ref.image);
            }
            if (!destination.isEmpty()) edits.push_back({node.target.start, node.target.end, destination});
            break;
        }
        case md::NodeType::Autolink:
            if (const QString destination = rewritten(file, node.dest, true); !destination.isEmpty())
                edits.push_back({node.range.start, node.range.end,
                                 QStringLiteral("[%1](%2)").arg(escaped(md::plainText(node)), destination)});
            break;
        case md::NodeType::Link:
        case md::NodeType::Image:
            if (node.type == md::NodeType::Image && copyImage(node, edits)) break;
            if (node.label.isEmpty()) {
                if (node.target.isEmpty()) break;
                QString destination = rewritten(file, node.dest, node.type == md::NodeType::Link);
                if (file > 0 && node.type == md::NodeType::Link && node.dest.startsWith(u'#')) {
                    const QString written = md::detail::unescape(doc.source(file, node.target));
                    if (md::classifyDestination(written, {}).fragment != md::classifyDestination(node.dest, {}).fragment)
                        destination = node.dest;
                }
                if (!destination.isEmpty()) edits.push_back({node.target.start, node.target.end, destination});
            } else {
                referenceUse(node, edits);
            }
            break;
        case md::NodeType::HtmlElement:
            if (node.label == u"a" || node.label == u"img") rebaseAttribute(file, node, edits);
            break;
        default:
            break;
        }
    }

    void referenceUse(const md::Node &node, std::vector<Edit> &edits) const
    {
        if (node.destinationFile < 0) {
            // More than one heading can have this name. The source has
            // already chosen the one in this occurrence of the chapter.
            if (headingAliases.value(normalized(node.label)) > 1 && node.dest.startsWith(u'#'))
                edits.push_back({node.close.start, node.close.end, QStringLiteral("](%1)").arg(node.dest)});
            return;
        }
        const Reference &ref = reference(node.destinationFile, node.label);
        if (node.type == md::NodeType::Link && ref.image) {
            const QString anchor = md::includedAnchor(doc, md::classifyDestination(doc, node));
            if (!anchor.isEmpty()) {
                // Keep the definition's file destination for the image.
                edits.push_back({node.close.start, node.close.end, QStringLiteral("](%1)").arg(withTitle(u'#' + anchor, node.title))});
                return;
            }
        }
        if (ref.label.isEmpty()) return;
        // A full reference names its label apart from its display text.
        // Collapsed and shortcut references keep the display text as written.
        if (node.target.start >= node.close.start && !node.close.isEmpty())
            edits.push_back({node.target.start, node.target.end, ref.label});
        else
            edits.push_back({node.close.start, node.close.end, QStringLiteral("][%1]").arg(ref.label)});
    }

    QString rewritten(int file, const QString &dest, bool link, md::DestinationSyntax syntax = md::DestinationSyntax::Markdown) const
    {
        if (dest.isEmpty() || dest.startsWith(u'#')) return {};
        const QString &directory = doc.files[size_t(file)].directory;
        const md::Destination target = md::classifyDestination(dest, directory);
        if (target.kind != md::Destination::Kind::LocalFile || !target.absolute) return {};
        if (link)
            if (const QString anchor = md::includedAnchor(doc, target); !anchor.isEmpty()) {
                const QString href = u'#' + anchor;
                return syntax == md::DestinationSyntax::Url ? md::encodeUrl(href) : href;
            }
        if (dest.startsWith(u'/') || directory.isEmpty() || QDir::cleanPath(directory) == folder) return {};
        return Exporters::rebasedDestination(target, folder, syntax);
    }

    // An image the writer has copied beside the output, at its new place.
    bool copyImage(const md::Node &node, std::vector<Edit> &edits) const
    {
        if (!imageDestination) return false;
        const md::Destination found = md::classifyDestination(doc, node);
        if (found.kind != md::Destination::Kind::LocalFile || !found.absolute) return false;
        const QString placed = imageDestination(found.path);
        if (placed.isEmpty()) return false;
        QString dest = md::encodeDestination(placed) + md::destinationSuffix(found, md::DestinationSyntax::Markdown);
        if (node.label.isEmpty()) {
            edits.push_back({node.target.start, node.target.end, dest});
        } else {
            // Inline only the picture. Ordinary links using the same definition keep their destination.
            edits.push_back({node.close.start, node.close.end, QStringLiteral("](%1)").arg(withTitle(dest, node.title))});
        }
        return true;
    }

    void rebaseAttribute(int file, const md::Node &node, std::vector<Edit> &edits) const
    {
        const QLatin1StringView name(node.label == u"img" ? "src" : "href");
        for (const md::Attribute &attribute : node.attributes) {
            const md::Range range = attribute.valueRange;
            if (attribute.name != name || range.isEmpty()) continue;
            const QString &dest = attribute.value;
            QString written;
            if (node.label == u"img" && imageDestination && !dest.isEmpty()) {
                const md::Destination found = md::classifyDestination(dest, doc.files[size_t(file)].directory);
                if (found.kind == md::Destination::Kind::LocalFile && found.absolute)
                    if (const QString placed = imageDestination(found.path); !placed.isEmpty())
                        written = md::encodeUrlPath(placed) + md::destinationSuffix(found);
            }
            if (written.isEmpty() && file != 0 && node.label == u"a" && node.resolved && dest.startsWith(u'#'))
                written = dest;
            if (written.isEmpty()) written = rewritten(file, dest, node.label == u"a", md::DestinationSyntax::Url);
            if (written.isEmpty()) continue;
            const QString replacement = md::htmlAttributeValue(written, attribute.quote);
            if (doc.source(node.file, range) != replacement) edits.push_back({range.start, range.end, replacement});
        }
    }

    const md::Document &doc;
    const QString folder;
    const Dialect::Choice &dialect;
    const ImageDestination imageDestination;
    const FileDestination citationDestination;
    Definitions noteDefinitions;
    std::map<ReferenceKey, Reference> references;
    std::set<std::pair<int, int>> sourceDefinitions;
    QSet<const md::Node *> duplicateDefinitions;
    QHash<QString, int> headingAliases;
    Renames noteRenames;
    QHash<int, QString> inlineLabels;
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
    ExportWeb::Assets assets(ExportWeb::Assets::Mode::Markdown, QFileInfo(folder).fileName() + u'/', outputFolder);
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
    const MarkdownExport::FileDestination citationFile = [&](const QByteArray &bytes, const QString &name) {
        return QUrl::fromPercentEncoding(assets.copy(bytes, name).toUtf8());
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
    assets.listCopies(result);
    result.ok = true;
    return result;
}

