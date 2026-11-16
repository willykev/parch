#include "markdownexport.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

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
    Assembler(const md::Document &document, const QString &outputFolder)
        : doc(document), folder(QDir::cleanPath(outputFolder))
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
            case md::NodeType::Heading:
                // A new link label must not take the name of a heading's.
                if (!child.label.isEmpty()) linkLabels.insert(label);
                linkLabels.insert(normalized(md::plainText(child)));
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
        collect(file, container, edits);
        return applied(doc.files[size_t(file)].text, std::move(edits));
    }

    void collect(int file, const md::Node &node, std::vector<Edit> &edits)
    {
        for (const md::Node &child : node.children) {
            if (child.generated || child.file != file) continue;
            if (child.type == md::NodeType::Include) {
                if (child.resolved && child.includedFile >= 0) include(file, child, edits);
                continue;
            }
            visit(file, child, edits);
            collect(file, child, edits);
        }
    }

    void include(int file, const md::Node &node, std::vector<Edit> &edits)
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
        case md::NodeType::LinkDefinition:
            if (const QString fresh = linkRenames.value(file).value(normalized(node.label)); !fresh.isEmpty())
                edits.push_back({node.open.start, node.open.end, QStringLiteral("[%1]:").arg(fresh)});
            rebase(file, node.target, node.dest, edits);
            break;
        case md::NodeType::Link:
        case md::NodeType::Image:
            if (node.label.isEmpty()) {
                rebase(file, node.target, node.dest, edits);
            } else if (const QString fresh = linkRenames.value(file).value(normalized(node.label));
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
        const QString written = rebased(file, dest);
        if (written.isEmpty()) return;
        static const QRegularExpression value(
            QStringLiteral("\\s(src|href)\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s\"'=<>`]+)"),
            QRegularExpression::CaseInsensitiveOption);
        const QStringView tag = doc.source(node.file, node.open);
        for (auto match = value.globalMatchView(tag); match.hasNext();) {
            const QRegularExpressionMatch found = match.next();
            if (found.captured(1).compare(name, Qt::CaseInsensitive) != 0) continue;
            const int start = node.open.start + int(found.capturedStart(2));
            edits.push_back({start, start + int(found.capturedLength(2)), u'"' + attributeEscaped(written) + u'"'});
            return;
        }
    }

    const md::Document &doc;
    const QString folder;
    Definitions noteDefinitions, linkDefinitions;
    QSet<QString> noteLabels, linkLabels;
    Renames noteRenames, linkRenames;
    QHash<QString, QList<std::pair<int, QString>>> citations;
    QStringList limitations;
};

} // namespace

Assembled assemble(const md::Document &document, const QString &outputFolder)
{
    return Assembler(document, outputFolder).run();
}

} // namespace MarkdownExport

ExportResult MarkdownExporter::write(const md::Document &document, const ExportRecipe &recipe) const
{
    ExportResult result;
    const MarkdownExport::Assembled assembled =
        MarkdownExport::assemble(document, QFileInfo(recipe.output).absolutePath());
    result.limitations = assembled.limitations;
    result.renderers.insert(QStringLiteral("markdown"), QStringLiteral("parch " PARCH_VERSION));
    if (!Exporters::writeFile(recipe.output, assembled.text.toUtf8(), &result.error)) {
        result.ioFailure = true;
        return result;
    }
    result.written.append(recipe.output);
    result.ok = true;
    return result;
}
