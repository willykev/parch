#include "documentlinks.h"

#include "librarywalk.h"
#include "mdinclude.h"
#include "pathtext.h"
#include "textsearch.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>

namespace {
using DocumentLinks::Link;
using Kind = DocumentLinks::Link::Kind;

// How much of a line a change shows on either side of it.
constexpr int contextUnits = 60;
// The largest Markdown file whose headings a link into it is checked against.
constexpr qint64 maxLinkedBytes = 16ll * 1024 * 1024;

class Collector {
public:
    Collector(QStringView text, const QString &folder, const QString &library)
        : m_text(text), m_folder(folder), m_library(library)
    {
    }

    QList<Link> links;

    void visit(const md::Node &node)
    {
        // An include's children, and what the engine made up, are written
        // somewhere else or nowhere.
        if (node.file != 0 || (node.generated && node.type != md::NodeType::HtmlElement)) return;
        switch (node.type) {
        case md::NodeType::Link:
        case md::NodeType::Image:
            // A reference link leads where its definition does, and is
            // found there.
            if (node.label.isEmpty()) add(node.type == md::NodeType::Image ? Kind::Image : Kind::Link, node);
            break;
        case md::NodeType::LinkDefinition:
            add(Kind::Definition, node);
            return;
        case md::NodeType::Include:
            add(Kind::Include, node);
            return;
        case md::NodeType::HtmlElement:
            addHtml(node);
            break;
        case md::NodeType::TableOfContents:
            return;
        default:
            break;
        }
        for (const md::Node &child : node.children) visit(child);
    }

private:
    void add(Kind kind, const md::Node &node)
    {
        if (node.target.isEmpty() || node.target.end > m_text.size()) return;
        Link link;
        link.kind = kind;
        link.range = {node.target.start, node.target.length()};
        link.written = m_text.sliced(node.target.start, node.target.length()).toString();
        link.dest = node.dest;
        const QChar before = node.target.start > 0 ? m_text[node.target.start - 1] : QChar();
        if (kind == Kind::Include) {
            link.enclosed = before == u'"' || before == u'\'';
            link.destination.kind = md::Destination::Kind::LocalFile;
            link.destination.path = md::includeTarget(node.dest, m_folder, m_library);
            link.destination.absolute = !link.destination.path.isEmpty();
        } else {
            link.enclosed = before == u'<';
            link.destination = md::classifyDestination(node.dest, m_folder);
        }
        links.append(link);
    }

    void addHtml(const md::Node &node)
    {
        const bool image = node.label.compare(QLatin1StringView("img"), Qt::CaseInsensitive) == 0;
        const QLatin1StringView wanted = image ? QLatin1StringView("src") : QLatin1StringView("href");
        if (!image && node.label.compare(QLatin1StringView("a"), Qt::CaseInsensitive) != 0) return;
        for (const md::Attribute &attribute : node.attributes) {
            if (attribute.name.compare(wanted, Qt::CaseInsensitive) != 0 || attribute.value.isEmpty()) continue;
            Link link;
            link.kind = image ? Kind::Image : Kind::Link;
            link.html = true;
            link.written = attribute.value;
            link.dest = attribute.value;
            link.range = {node.range.start, (node.open.isEmpty() ? node.range : node.open).end - node.range.start};
            link.destination = md::classifyDestination(attribute.value, m_folder);
            links.append(link);
        }
    }

    QStringView m_text;
    QString m_folder;
    QString m_library;
};

QSet<QString> slugsOf(const md::Document &document)
{
    QSet<QString> slugs;
    for (const md::Heading &heading : document.headings) slugs.insert(heading.slug);
    return slugs;
}

// The library folder that holds a path, if this one does.
QString libraryHolding(const QString &path, const QString &library)
{
    return !library.isEmpty() && path.startsWith(library + u'/') ? library : QString();
}

// The headings another Markdown document has, by slug, with its own includes.
QSet<QString> slugsOfFile(const QString &path, const QString &library, const md::Options &options)
{
    md::FileIncludeResolver resolver;
    const md::IncludeResolver::File file = resolver.read(path, maxLinkedBytes);
    if (!file.ok) return {};
    md::Source source;
    source.path = file.canonicalPath;
    source.libraryRoot = libraryHolding(file.canonicalPath, library);
    source.resolver = &resolver;
    return slugsOf(md::parse(file.text, options, source));
}

// A path written as a destination, percent-encoding characters beyond ASCII
// as UTF-8 when the destination it replaces did.
QString encodedLike(const QString &path, const QString &written)
{
    static const QRegularExpression beyondAscii(QStringLiteral("%[89A-Fa-f][0-9A-Fa-f]"));
    const QString encoded = md::encodeDestination(path);
    if (!beyondAscii.match(written).hasMatch()) return encoded;
    QString result;
    for (const QChar c : encoded) {
        if (c.unicode() < 0x80) {
            result.append(c);
            continue;
        }
        for (const char byte : QString(c).toUtf8())
            result.append(u'%' + QString::number(uchar(byte), 16).toUpper().rightJustified(2, u'0'));
    }
    return result;
}

QString shownAround(QStringView line, qsizetype start, qsizetype end, const QString &with)
{
    const qsizetype from = std::max<qsizetype>(0, start - contextUnits);
    const qsizetype until = std::min(line.size(), end + contextUnits);
    return (from > 0 ? QStringLiteral("…") : QString()) + line.sliced(from, start - from).toString() + with
           + line.sliced(end, until - end).toString() + (until < line.size() ? QStringLiteral("…") : QString());
}
} // namespace

QList<Link> DocumentLinks::find(const md::Document &document, const QString &folder, const QString &library)
{
    if (document.files.empty()) return {};
    const QString &text = document.files.front().text;
    Collector collector(text, folder, library);
    collector.visit(document.root);
    QList<Link> links = std::move(collector.links);
    std::stable_sort(links.begin(), links.end(), [](const Link &a, const Link &b) { return a.range.start < b.range.start; });
    TextSearch::LineCounter lines(text);
    for (Link &link : links) link.line = lines.lineOf(link.range.start);
    return links;
}

QList<Link> DocumentLinks::find(const QString &text, const QString &folder, const QString &library,
                                const md::Options &options)
{
    return find(md::parse(text, options), folder, library);
}

DocumentLinks::Checked DocumentLinks::check(const QString &text, const QString &path, const QString &folder,
                                            const QString &library, const md::Options &options)
{
    md::FileIncludeResolver resolver;
    md::Source source;
    source.path = path;
    source.directory = folder;
    source.libraryRoot = library;
    source.resolver = &resolver;
    const md::Document document = md::parse(text, options, source);
    const QSet<QString> own = slugsOf(document);
    const QString self = path.isEmpty() ? QString() : QFileInfo(path).canonicalFilePath();
    QHash<QString, QSet<QString>> others;
    Checked result;
    for (const Link &link : find(document, folder, library)) {
        const md::Destination &destination = link.destination;
        if (destination.kind == md::Destination::Kind::Anchor) {
            ++result.checked;
            if (!own.contains(destination.fragment)) result.missing.append({link, true});
            continue;
        }
        if (destination.kind != md::Destination::Kind::LocalFile) continue;
        ++result.checked;
        const QFileInfo info(destination.path);
        if (!destination.absolute || !info.exists()) {
            result.missing.append({link, false});
            continue;
        }
        // A heading of another document is looked for there.
        if (link.kind == Kind::Include || destination.fragment.isEmpty() || !info.isFile()
            || !LibraryWalk::isTextDocument(info.fileName()))
            continue;
        const QString canonical = info.canonicalFilePath();
        if (canonical == self) {
            if (!own.contains(destination.fragment)) result.missing.append({link, true});
            continue;
        }
        auto slugs = others.find(canonical);
        if (slugs == others.end()) slugs = others.insert(canonical, slugsOfFile(canonical, library, options));
        if (!slugs->contains(destination.fragment)) result.missing.append({link, true});
    }
    return result;
}

QString DocumentLinks::libraryOf(const QString &path, const QStringList &libraries)
{
    QString nearest;
    for (const QString &library : libraries) {
        if (library.isEmpty()) continue;
        const QString root = library.endsWith(u'/') && library.size() > 1 ? library.chopped(1) : library;
        const bool inside = path == root || path.startsWith(root == u"/" ? root : root + u'/');
        if (inside && root.size() > nearest.size()) nearest = root;
    }
    return nearest;
}

QString DocumentLinks::moved(const QString &path, const QList<Move> &moves)
{
    for (const Move &move : moves) {
        if (path == move.from) return move.to;
        if (path.startsWith(move.from + u'/')) return move.to + path.mid(move.from.size());
    }
    return path;
}

QList<DocumentLinks::Change> DocumentLinks::changes(const QString &text, const QString &path, const QString &library,
                                                    const QList<Move> &moves, const md::Options &options)
{
    QList<Change> found;
    const QString newPath = moved(path, moves);
    const QString folder = PathText::folderOf(path);
    const QString newFolder = PathText::folderOf(newPath);
    for (const Link &link : find(text, folder, library, options)) {
        const md::Destination &destination = link.destination;
        if (link.html || destination.kind != md::Destination::Kind::LocalFile || !destination.absolute) continue;
        const bool include = link.kind == Kind::Include;
        const bool fileUrl = destination.scheme == u"file";
        // The path as written, its escapes and percent-encoding read, and
        // the fragment after it, kept as written.
        QString plain;
        QString suffix;
        if (include) {
            plain = link.written;
        } else if (fileUrl) {
            plain = destination.path;
            const qsizetype hash = link.written.indexOf(u'#');
            if (hash >= 0) suffix = link.written.mid(hash);
        } else {
            const qsizetype hash = link.written.indexOf(u'#');
            if (hash >= 0) suffix = link.written.mid(hash);
            const qsizetype mark = link.dest.indexOf(u'#');
            plain = QUrl::fromPercentEncoding((mark >= 0 ? link.dest.first(mark) : link.dest).toUtf8());
        }
        if (plain.isEmpty()) continue;
        const QString target = destination.path;
        const QString newTarget = moved(target, moves);
        if (newTarget == target && newPath == path) continue;
        const bool absolute = fileUrl || plain.startsWith(u'/');
        QString relative;
        if (absolute && include && (newTarget == library || newTarget.startsWith(library + u'/'))) {
            // Library-rooted it was, and library-rooted it stays.
            relative = u'/' + QDir(library).relativeFilePath(newTarget);
        } else if (absolute && !include) {
            relative = newTarget;
        } else {
            relative = QDir(newFolder).relativeFilePath(newTarget);
            if (relative.isEmpty()) relative = QStringLiteral(".");
            if (plain.startsWith(QStringLiteral("./")) && !relative.startsWith(QStringLiteral("..")))
                relative.prepend(QStringLiteral("./"));
        }
        if (plain.endsWith(u'/') && !relative.endsWith(u'/')) relative.append(u'/');
        if (relative == plain) continue;
        QString after;
        if (include) {
            // The range holds the path alone, inside its quotes when it has them.
            after = !link.enclosed && relative.contains(u' ') ? u'"' + relative + u'"' : relative;
        } else if (link.enclosed) {
            const QString written = fileUrl ? QStringLiteral("file://") + relative : relative;
            after = QString(written).replace(u'<', QStringLiteral("\\<")).replace(u'>', QStringLiteral("\\>")) + suffix;
        } else {
            after = (fileUrl ? QStringLiteral("file://") : QString()) + encodedLike(relative, link.written) + suffix;
        }
        if (after == link.written) continue;
        qsizetype lineStart = link.range.start;
        while (lineStart > 0 && text.at(lineStart - 1) != u'\n' && text.at(lineStart - 1) != u'\r') --lineStart;
        qsizetype lineEnd = link.range.start + link.range.length;
        while (lineEnd < text.size() && text.at(lineEnd) != u'\n' && text.at(lineEnd) != u'\r') ++lineEnd;
        const QStringView lineText = QStringView(text).sliced(lineStart, lineEnd - lineStart);
        const qsizetype start = link.range.start - lineStart;
        const qsizetype end = start + link.range.length;
        found.append({link.range, link.written, after, link.line, shownAround(lineText, start, end, link.written),
                      shownAround(lineText, start, end, after)});
    }
    return found;
}
