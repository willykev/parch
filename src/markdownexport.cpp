#include "markdownexport.h"

#include "exportweb.h"
#include "htmlexport.h"

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
    Assembler(const md::Document &document, const QString &outputFolder, const ImageDestination &image)
        : doc(document), folder(QDir::cleanPath(outputFolder)), imageDestination(image)
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
                if (node.type == md::NodeType::Image && copied(file, node.target, node.dest, edits)) break;
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

    // An image the writer has copied beside the output, at its new place.
    bool copied(int file, md::Range target, const QString &dest, std::vector<Edit> &edits) const
    {
        if (!imageDestination || target.isEmpty() || dest.isEmpty()) return false;
        const md::Destination found = md::classifyDestination(dest, doc.files[size_t(file)].directory);
        if (found.kind != md::Destination::Kind::LocalFile || !found.absolute) return false;
        const QString placed = imageDestination(found.path);
        if (placed.isEmpty()) return false;
        edits.push_back({target.start, target.end, md::encodeDestination(placed)});
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
                if (const QString placed = imageDestination(found.path); !placed.isEmpty())
                    written = QString::fromUtf8(QUrl::toPercentEncoding(placed, "/"));
        }
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
            edits.push_back({start, start + int(found.capturedLength(2)), u'"' + attributeEscaped(written) + u'"'});
            return;
        }
    }

    const md::Document &doc;
    const QString folder;
    const ImageDestination imageDestination;
    Definitions noteDefinitions, linkDefinitions;
    QSet<QString> noteLabels, linkLabels;
    Renames noteRenames, linkRenames;
    QHash<QString, QList<std::pair<int, QString>>> citations;
    QStringList limitations;
};

} // namespace

Assembled assemble(const md::Document &document, const QString &outputFolder, const ImageDestination &image)
{
    return Assembler(document, outputFolder, image).run();
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
                        QStringLiteral("Lead to local images where they are (link), or copy them into a "
                                       "folder beside it, NAME_files (copy); the default is link"),
                        QStringLiteral("HOW")};
    images.label = QStringLiteral("Images");
    images.choices = {QStringLiteral("link"), QStringLiteral("copy")};
    images.defaultValue = QStringLiteral("link");
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
    if (recipe.options.contains(QStringLiteral("as-is"))) {
        result.limitations = limitations(document, recipe, {});
        const QString text = document.files.empty() ? QString() : document.files.front().text;
        if (!Exporters::writeFile(recipe.output, text.toUtf8(), &result.error, recipe.existing)) {
            result.ioFailure = true;
            return result;
        }
        result.written.append(recipe.output);
        result.ok = true;
        return result;
    }
    const bool copy = option(recipe, QStringLiteral("images")) == u"copy";
    const QString folder = HtmlExporter::assetFolder(recipe.output);
    ExportWeb::Assets assets(ExportWeb::Assets::Mode::Copy, QFileInfo(folder).fileName() + u'/', outputFolder);
    QStringList unread;
    MarkdownExport::ImageDestination image;
    if (copy) {
        image = [&](const QString &path) {
            QString error;
            const QString url = assets.file(path, document.resourceFolders, &error);
            if (url.isEmpty()) unread.append(QStringLiteral("%1 (%2)").arg(QFileInfo(path).fileName(), error));
            // The text gives the file's path, not its URL.
            return QUrl::fromPercentEncoding(url.toUtf8());
        };
    }
    const MarkdownExport::Assembled assembled = MarkdownExport::assemble(document, outputFolder, image);
    result.limitations = assembled.limitations;
    if (!unread.isEmpty())
        result.limitations.append(QStringLiteral("Images that could not be read are linked where they were: %1")
                                      .arg(unread.join(QStringLiteral(", "))));
    if (run.stopped()) return Exporters::cancelled();
    // Nothing is written through a link, the folder of pictures included.
    if (!ExportWeb::writePage(recipe, assembled.text.toUtf8(), folder, assets, &result.error)) {
        result.ioFailure = true;
        return result;
    }
    result.written.append(recipe.output);
    for (const ExportWeb::Assets::Asset &asset : assets.assets()) result.written.append(folder + u'/' + asset.name);
    result.ok = true;
    return result;
}

