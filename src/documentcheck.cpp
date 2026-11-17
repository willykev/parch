#include "documentcheck.h"

#include "documentreading.h"
#include "mdinclude.h"
#include "saferead.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <memory>

namespace DocumentCheck {
namespace {

const qint64 defaultDocumentBytes = md::Source().maxExpandedBytes;

QString kindOf(md::DiagnosticCode code)
{
    using C = md::DiagnosticCode;
    switch (code) {
    case C::UnresolvedReference:
    case C::AmbiguousHeadingReference: return QStringLiteral("reference");
    case C::UnresolvedAnchor: return QStringLiteral("link");
    case C::MissingFootnote:
    case C::DuplicateFootnote:
    case C::UnusedFootnote: return QStringLiteral("footnote");
    case C::MissingCitation:
    case C::DuplicateCitation: return QStringLiteral("citation");
    case C::UnresolvedVariable: return QStringLiteral("variable");
    case C::MalformedTableRow: return QStringLiteral("table");
    case C::UnclosedMath: return QStringLiteral("math");
    case C::FrontMatter: return QStringLiteral("front-matter");
    case C::UnsupportedHtml: return QStringLiteral("html");
    case C::IncludeNotFound:
    case C::IncludeUnreadable:
    case C::IncludeCycle:
    case C::IncludeDepthLimit:
    case C::IncludeSizeLimit:
    case C::IncludeCountLimit:
    case C::IncludeNoLibrary:
    case C::IncludeNoBaseFolder:
    case C::IncludeOutside: return QStringLiteral("include");
    }
    return QStringLiteral("reference");
}

bool isMarkdownFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == u"md" || suffix == u"markdown" || suffix == u"mdown" || suffix == u"mkd";
}

// The checks of the links and images in a tree, which the engine leaves to
// whoever can look at the disk.
class LinkCheck {
public:
    explicit LinkCheck(const Checked &loaded) : m_loaded(loaded) {}

    void walk(const md::Node &node, std::vector<Problem> &found)
    {
        if (!node.generated) visit(node, found);
        for (const md::Node &child : node.children) walk(child, found);
    }

private:
    void visit(const md::Node &node, std::vector<Problem> &found)
    {
        const md::Document &document = m_loaded.document;
        switch (node.type) {
        case md::NodeType::Image:
            image(node, md::classifyDestination(document, node), node.dest, found);
            break;
        case md::NodeType::Link:
            link(node, md::classifyDestination(document, node), node.dest, found);
            break;
        case md::NodeType::HtmlElement: {
            const bool isImage = node.label == u"img";
            const bool isLink = node.label == u"a";
            if (!isImage && !isLink) break;
            for (const md::Attribute &attribute : node.attributes) {
                if (attribute.name != (isImage ? u"src" : u"href")) continue;
                const md::Destination target =
                    md::classifyDestination(attribute.value, document.files[size_t(node.file)].directory);
                if (isImage) image(node, target, attribute.value, found);
                else link(node, target, attribute.value, found);
            }
            break;
        }
        default:
            break;
        }
    }

    static bool checkable(const md::Destination &target)
    {
        return target.kind == md::Destination::Kind::LocalFile && target.absolute && !target.path.isEmpty();
    }

    void image(const md::Node &node, const md::Destination &target, const QString &written,
               std::vector<Problem> &found)
    {
        if (!checkable(target)) return;
        const QString why = SafeRead::pictureRefusal(target.path, m_loaded.document.resourceFolders);
        if (!why.isEmpty()) {
            add(node, QStringLiteral("image-outside-folders"), QStringLiteral("asset"),
                QStringLiteral("The image \"%1\" cannot be read: %2").arg(written, why), found);
            return;
        }
        if (QFileInfo::exists(target.path)) return;
        add(node, QStringLiteral("unresolved-asset"), QStringLiteral("asset"),
            QStringLiteral("The image \"%1\" is not there (%2)").arg(written, target.path), found);
    }

    void link(const md::Node &node, const md::Destination &target, const QString &written,
              std::vector<Problem> &found)
    {
        if (!checkable(target)) return;
        const QFileInfo info(target.path);
        if (!info.exists()) {
            add(node, QStringLiteral("broken-link"), QStringLiteral("link"),
                QStringLiteral("The link \"%1\" leads to a file that is not there (%2)").arg(written, target.path),
                found);
            return;
        }
        if (target.fragment.isEmpty() || !info.isFile() || !isMarkdownFile(target.path)) return;
        const QSet<QString> *slugs = slugsOf(info.canonicalFilePath());
        if (!slugs || slugs->contains(target.fragment)) return;
        add(node, QStringLiteral("broken-link-anchor"), QStringLiteral("link"),
            QStringLiteral("The link \"%1\" leads to a heading %2 does not have").arg(written, info.fileName()), found);
    }

    // The heading anchors of a Markdown file, read once; nothing when the
    // file cannot be read, which is a problem of its own for that file.
    const QSet<QString> *slugsOf(const QString &canonicalPath)
    {
        auto known = m_slugs.find(canonicalPath);
        if (known != m_slugs.end()) return known->get();
        std::unique_ptr<QSet<QString>> slugs;
        if (canonicalPath == m_loaded.path) {
            slugs = std::make_unique<QSet<QString>>();
            for (const md::Heading &heading : m_loaded.document.headings) slugs->insert(heading.slug);
        } else {
            Loaded other;
            QString error;
            if (load(canonicalPath, m_loaded.dialect, &other, &error)) {
                slugs = std::make_unique<QSet<QString>>();
                for (const md::Heading &heading : other.document.headings) slugs->insert(heading.slug);
            }
        }
        return m_slugs.insert(canonicalPath, std::shared_ptr<QSet<QString>>(std::move(slugs)))->get();
    }

    void add(const md::Node &node, const QString &code, const QString &kind, const QString &message,
             std::vector<Problem> &found)
    {
        Problem problem;
        problem.file = node.file;
        problem.range = node.range;
        problem.severity = md::Severity::Warning;
        problem.code = code;
        problem.kind = kind;
        problem.message = message;
        found.push_back(problem);
    }

    const Checked &m_loaded;
    QHash<QString, std::shared_ptr<QSet<QString>>> m_slugs;
};

} // namespace

md::HtmlOptions Loaded::htmlOptions(md::HtmlOptions options) const
{
    options.citations = DocumentCitations::html(formatted);
    return options;
}

bool load(const QString &path, const Dialect::Choice &dialect, Loaded *loaded, QString *error)
{
    // The choice made is the highest layer, so the front matter changes nothing.
    Dialect::Layers layers;
    layers.flags.profile = dialect.options.profile;
    for (const QString &name : md::Options::extensionNames())
        layers.flags.extensions.append({name, dialect.options.extension(name)});
    layers.flags.libraryRoot = dialect.libraryRoot;
    // What a front matter named is no flag: the other document's own front
    // matter chooses, within its own reach.
    if (dialect.bibliographyFrom != u"front-matter") layers.flags.bibliography = dialect.bibliography;
    if (dialect.citationStyleFrom != u"front-matter") layers.flags.citationStyle = dialect.citationStyle;
    layers.flags.maxIncludeDepth = dialect.maxIncludeDepth;
    layers.flags.maxIncludeSize = dialect.maxIncludeSize;
    layers.flags.maxIncludes = dialect.maxIncludes;
    if (!load(path, layers, loaded, error)) return false;
    loaded->dialect = dialect;
    return true;
}

bool load(const QString &path, const Dialect::Layers &layers, Loaded *loaded, QString *error)
{
    md::FileIncludeResolver resolver;
    // A smaller size limit stops what it includes, never the document itself.
    const qint64 maxDocumentBytes = std::max(defaultDocumentBytes, Dialect::effective(layers).maxIncludeSize);
    const md::IncludeResolver::File file = resolver.read(path, maxDocumentBytes);
    if (file.missing) {
        *error = QStringLiteral("%1 does not exist").arg(path);
        return false;
    }
    if (QFileInfo(path).isDir()) {
        *error = QStringLiteral("%1 is a folder, not a Markdown file").arg(path);
        return false;
    }
    if (file.tooLarge) {
        *error = QStringLiteral("%1 is larger than %2").arg(path, Dialect::sizeText(maxDocumentBytes));
        return false;
    }
    if (!file.ok) {
        *error = QStringLiteral("cannot read %1: %2").arg(path, file.error);
        return false;
    }
    md::Source source;
    source.path = file.canonicalPath;
    source.resolver = &resolver;
    DocumentReading::Reading reading = DocumentReading::read(file.text, layers, source);
    loaded->path = file.canonicalPath;
    loaded->dialect = reading.dialect;
    loaded->document = std::move(reading.document);
    loaded->citations = std::move(reading.citations);
    loaded->formatted = std::move(reading.formatted);
    return true;
}

LineIndex::LineIndex(QStringView text) : m_text(text)
{
    m_starts.push_back(0);
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        // "\r\n", "\n" and "\r" each end a line, as in CommonMark.
        if (c == u'\n' || (c == u'\r' && (i + 1 >= text.size() || text[i + 1] != u'\n')))
            m_starts.push_back(int(i + 1));
    }
}

Position LineIndex::at(int offset) const
{
    offset = std::clamp(offset, 0, int(m_text.size()));
    const auto next = std::upper_bound(m_starts.cbegin(), m_starts.cend(), offset);
    const int line = int(next - m_starts.cbegin());
    // Characters, not UTF-16 units: a surrogate pair is one.
    int column = 1;
    for (int i = *(next - 1); i < offset; ++i)
        if (!m_text[i].isLowSurrogate()) ++column;
    return {line, column};
}

Position positionOf(QStringView text, int offset)
{
    return LineIndex(text).at(offset);
}

void order(std::vector<Problem> &problems)
{
    // Those of other files (a bibliography) come last.
    std::stable_sort(problems.begin(), problems.end(), [](const Problem &a, const Problem &b) {
        const unsigned fileA = unsigned(a.file), fileB = unsigned(b.file);
        if (fileA != fileB) return fileA < fileB;
        if (a.path != b.path) return a.path < b.path;
        if (a.file < 0) return a.position.line < b.position.line;
        return a.range.start < b.range.start;
    });
}

std::vector<Problem> problems(const Loaded &loaded, std::vector<Problem> more)
{
    return problems(Checked{loaded.path, loaded.dialect, loaded.document, loaded.formatted}, std::move(more));
}

std::vector<Problem> problems(const Checked &loaded, std::vector<Problem> more)
{
    const md::Document &document = loaded.document;
    std::vector<Problem> found = std::move(more);
    for (const md::Diagnostic &diagnostic : document.diagnostics) {
        Problem problem;
        problem.file = diagnostic.file;
        problem.range = diagnostic.range;
        problem.severity = diagnostic.severity;
        problem.code = QString::fromLatin1(md::codeName(diagnostic.code));
        problem.kind = kindOf(diagnostic.code);
        problem.message = diagnostic.message;
        found.push_back(problem);
    }
    LinkCheck(loaded).walk(document.root, found);
    // What the citation style, the bibliography and the citations against them say.
    for (const DocumentCitations::Problem &cited : loaded.formatted.problems) {
        Problem problem;
        problem.severity = cited.severity;
        problem.code = cited.code;
        problem.message = cited.message;
        if (cited.file >= 0 && size_t(cited.file) < document.files.size()) {
            problem.file = cited.file;
            problem.range = cited.range;
            problem.kind = QStringLiteral("citation");
        } else if (!cited.source.isEmpty()) {
            problem.file = -1;
            problem.path = cited.source;
            problem.position = {cited.line, cited.line > 0 ? 1 : 0};
            problem.kind = cited.code == u"citation-style" ? QStringLiteral("citation") : QStringLiteral("bibliography");
        } else {
            // About the document as a whole: its style or its settings.
            problem.file = 0;
            problem.kind = QStringLiteral("citation");
        }
        found.push_back(problem);
    }

    // A file included twice is in the tree twice; its problems are once.
    order(found);
    QSet<QString> seen;
    std::vector<Problem> unique;
    std::vector<std::unique_ptr<LineIndex>> lines(document.files.size());
    for (Problem &problem : found) {
        const QString key = QStringLiteral("%1:%2:%3:%4:%5:%6")
                                .arg(problem.file)
                                .arg(problem.range.start)
                                .arg(problem.range.end)
                                .arg(problem.path)
                                .arg(problem.position.line)
                                .arg(problem.code + problem.message);
        if (seen.contains(key)) continue;
        seen.insert(key);
        if (problem.file >= 0 && size_t(problem.file) < lines.size()) {
            std::unique_ptr<LineIndex> &index = lines[size_t(problem.file)];
            if (!index) index = std::make_unique<LineIndex>(document.files[size_t(problem.file)].text);
            problem.position = index->at(problem.range.start);
        }
        unique.push_back(std::move(problem));
    }
    return unique;
}

bool anyProblem(const std::vector<Problem> &problems)
{
    return std::any_of(problems.cbegin(), problems.cend(),
                       [](const Problem &problem) { return problem.severity != md::Severity::Info; });
}

} // namespace DocumentCheck
