#include "documentlinks.h"

#include "dialectchoice.h"
#include "documentcitations.h"
#include "librarywalk.h"
#include "mdhtml.h"
#include "mdinclude.h"
#include "mdparser_p.h"
#include "pathtext.h"
#include "textsearch.h"

#include <QCoreApplication>
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
            const md::Range value = attribute.valueRange;
            link.editable = !value.isEmpty() && value.end <= m_text.size();
            link.quote = attribute.quote;
            link.written = link.editable ? m_text.sliced(value.start, value.length()).toString() : attribute.value;
            link.dest = attribute.value;
            link.range = link.editable ? SourceRange{value.start, value.length()} : SourceRange{node.range.start, 0};
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
QString encodedLike(const QString &encoded, const QString &written)
{
    static const QRegularExpression beyondAscii(QStringLiteral("%[89A-Fa-f][0-9A-Fa-f]"));
    if (!beyondAscii.match(written).hasMatch()) return encoded;
    QString result;
    for (const char byte : encoded.toUtf8()) {
        if (uchar(byte) < 0x80) {
            result.append(QLatin1Char(byte));
            continue;
        }
        result.append(u'%' + QString::number(uchar(byte), 16).toUpper().rightJustified(2, u'0'));
    }
    return result;
}

// The query and fragment belong to the same spelling as before, including
// Markdown escapes and entities that stand for structural delimiters.
QString markdownSuffix(const DocumentLinks::Link &link)
{
    const auto &destination = link.destination;
    if (!destination.query && !destination.encodedFragment) return {};
    const QString &raw = link.written;
    for (int i = 0; i < raw.size();) {
        const int start = i;
        QChar c = raw.at(i++);
        if (c == u'\\' && i < raw.size() && md::detail::isAsciiPunct(raw.at(i).unicode())) c = raw.at(i++);
        else if (c == u'&') {
            QString entity;
            if (const int n = md::detail::scanEntity(raw.constData(), int(raw.size()), start, &entity)) {
                i = start + n;
                c = entity.size() == 1 ? entity.front() : QChar();
            }
        }
        if (c == u'?' || c == u'#') return raw.mid(start);
    }
    return md::destinationSuffix(destination, md::DestinationSyntax::Markdown);
}

QString shownAround(QStringView line, qsizetype start, qsizetype end, const QString &with)
{
    const qsizetype from = std::max<qsizetype>(0, start - contextUnits);
    const qsizetype until = std::min(line.size(), end + contextUnits);
    return (from > 0 ? QStringLiteral("…") : QString()) + line.sliced(from, start - from).toString() + with
           + line.sliced(end, until - end).toString() + (until < line.size() ? QStringLiteral("…") : QString());
}

DocumentLinks::Change reviewedChange(QStringView text, SourceRange range, int line, const QString &before,
                                     const QString &after, const QString &problem = {})
{
    int lineStart = range.start, lineEnd = range.start + range.length;
    while (lineStart > 0 && text.at(lineStart - 1) != u'\n' && text.at(lineStart - 1) != u'\r') --lineStart;
    while (lineEnd < text.size() && text.at(lineEnd) != u'\n' && text.at(lineEnd) != u'\r') ++lineEnd;
    const QStringView lineText = text.sliced(lineStart, lineEnd - lineStart);
    const int start = range.start - lineStart;
    const int end = start + range.length;
    return {range, before, after, line, shownAround(lineText, start, end, before),
            problem.isEmpty() ? shownAround(lineText, start, end, after) : problem, problem};
}

QString yamlPath(const QString &path, QChar quote, bool inFlow)
{
    const bool controls = std::any_of(path.begin(), path.end(), [](QChar c) {
        return c.unicode() < 0x20 || c.unicode() == 0x7f;
    });
    if (controls || quote == u'"') {
        QString escaped;
        for (const QChar c : path) {
            if (c == u'"' || c == u'\\') escaped += u'\\' + QString(c);
            else if (c.unicode() < 0x20 || c.unicode() == 0x7f)
                escaped += QStringLiteral("\\x%1").arg(uint(c.unicode()), 2, 16, QLatin1Char('0'));
            else escaped += c;
        }
        return u'"' + escaped + u'"';
    }
    static const QRegularExpression plainSpecial(QStringLiteral("^(?:[-?:](?:\\s|$)|[#!&*|>'\"%@`\\[\\]{},])|\\s#|:(?:\\s|$)"));
    static const QRegularExpression flowSpecial(QStringLiteral("[,\\[\\]{}'\"]"));
    if (quote == u'\'' || path.trimmed() != path || path.contains(plainSpecial) || (inFlow && path.contains(flowSpecial))
        || path == u"~" || path.compare(QLatin1StringView("null"), Qt::CaseInsensitive) == 0) {
        QString escaped = path;
        escaped.replace(u'\'', QStringLiteral("''"));
        return u'\'' + escaped + u'\'';
    }
    return path;
}

void citationChanges(const md::FrontMatter &front, const QString &text, const QString &folder, const QString &newFolder,
                     const QString &library, const QString &newLibrary, const QList<DocumentLinks::Move> &moves,
                     QList<DocumentLinks::Change> &found)
{
    const md::YamlValue *bib = front.data.find(u"bibliography");
    const md::YamlValue *style = front.data.find(u"csl");
    if (!bib && !style) return;
    const auto before = DocumentCitations::Reach::of(library, folder);
    const auto after = DocumentCitations::Reach::of(newLibrary, newFolder);
    TextSearch::LineCounter lines(text);
    const auto add = [&](const md::YamlValue &value, const QString &path, bool isStyle) {
        if (path.isEmpty()) return;
        const QString plain = value.scalar.trimmed();
        QString replacement, problem;
        if (isStyle && !Dialect::isStyleFile(path)) {
            // A bundled style may have been written as a filename because
            // there was no file of that name beside the document.
            if (folder == newFolder || !Dialect::isStyleFile(Dialect::styleReference(plain, newFolder))) return;
            replacement = path;
        } else {
            const QString normalized = QDir::cleanPath(path);
            const QString target = DocumentLinks::moved(normalized, moves);
            const bool absolute = QDir::isAbsolutePath(plain);
            const bool fromHome = plain == u"~" || plain.startsWith(QLatin1StringView("~/"));
            if (target == normalized && (folder == newFolder || absolute || fromHome)) return;
            const QString canonical = QFileInfo(path).canonicalFilePath();
            if (!before.allows(path) || !after.allows(target)
                || (!canonical.isEmpty() && !after.allows(DocumentLinks::moved(canonical, moves)))) {
                problem = QCoreApplication::translate("DocumentLinks", "“%1” is outside the allowed folders.").arg(plain);
            } else if (absolute) {
                replacement = target;
            } else if (fromHome) {
                const QString home = QDir::homePath();
                replacement = md::isWithin(target, home) ? QStringLiteral("~/") + QDir(home).relativeFilePath(target) : target;
            } else {
                if (QDir::cleanPath(newFolder + u'/' + plain) == target) return;
                replacement = QDir(newFolder).relativeFilePath(target);
                if (plain.startsWith(QLatin1StringView("./")) && !replacement.startsWith(u'.'))
                    replacement.prepend(QStringLiteral("./"));
            }
        }
        const int offset = value.sourceOffset < 0 ? front.contentStart : value.sourceOffset;
        const SourceRange range{offset, value.sourceLength};
        QString written;
        if (range.length > 0) {
            written = text.mid(range.start, range.length);
            const QChar first = written.front();
            replacement = yamlPath(replacement, first == u'"' || first == u'\'' ? first : QChar(), value.inFlow);
        } else {
            written = plain;
            if (problem.isEmpty())
                problem = QCoreApplication::translate("DocumentLinks", "“%1” has no exact YAML source range.").arg(plain);
        }
        found.append(reviewedChange(text, range, lines.lineOf(offset), written, replacement, problem));
    };
    const auto bibliography = [&](const md::YamlValue &value) {
        if (value.isScalar()) add(value, Dialect::bibliographyReference(value.scalar, folder), false);
    };
    if (bib) {
        if (bib->isList()) {
            for (const md::YamlValue &value : bib->list) bibliography(value);
        } else {
            bibliography(*bib);
        }
    }
    if (style && style->isScalar()) add(*style, Dialect::styleReference(style->scalar, folder), true);
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
                                                    const QList<Move> &moves, const md::Options &options,
                                                    const QString &destinationLibrary)
{
    QList<Change> found;
    const QString newPath = moved(path, moves);
    const QString folder = PathText::folderOf(path);
    const QString newFolder = PathText::folderOf(newPath);
    const md::Document document = md::parse(text, options);
    for (const Link &link : find(document, folder, library)) {
        const md::Destination &destination = link.destination;
        if (destination.kind != md::Destination::Kind::LocalFile || !destination.absolute) continue;
        const bool include = link.kind == Kind::Include;
        const bool fileUrl = destination.scheme == u"file";
        const QString &plain = include ? link.dest : destination.localPath;
        if (plain.isEmpty()) continue;
        const QString target = destination.path;
        const QString newTarget = moved(target, moves);
        if (newTarget == target && newPath == path) continue;
        const bool absolute = fileUrl || plain.startsWith(u'/');
        if (!absolute && QDir::cleanPath(newFolder + u'/' + plain) == newTarget) continue;
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
        if (include && md::isUrlSpelling(relative)) relative.prepend(QStringLiteral("./"));
        if (relative == plain) continue;
        QString after, problem;
        if (include) {
            // The range holds the path alone, inside its quotes when it has them.
            const bool quote = !md::isBareIncludePath(relative);
            const QChar delimiter = link.enclosed ? text.at(link.range.start - 1) : QChar(u'"');
            after = relative;
            if (link.enclosed || quote)
                after.replace(u'\\', QStringLiteral("\\\\")).replace(delimiter, u'\\' + QString(delimiter));
            if (!link.enclosed && quote) after = delimiter + after + delimiter;
        } else if (link.html) {
            QString url = md::encodeUrlPath(relative) + md::destinationSuffix(destination);
            if (fileUrl) url.prepend(QStringLiteral("file://"));
            after = md::htmlAttributeValue(url, link.quote);
            if (!link.editable)
                problem = QCoreApplication::translate("DocumentLinks", "The HTML attribute has no exact source range.");
        } else {
            QString encoded;
            if (link.enclosed) {
                // Angle brackets allow spaces and parentheses. The path
                // still goes through Markdown unescaping and URL decoding.
                encoded = relative.replace(u'%', QStringLiteral("%25")).replace(u'#', QStringLiteral("%23"))
                              .replace(u'?', QStringLiteral("%3F"))
                              .replace(u'&', QStringLiteral("%26")).replace(u'\\', QStringLiteral("%5C"))
                              .replace(u'<', QStringLiteral("\\<")).replace(u'>', QStringLiteral("\\>"));
            } else {
                encoded = md::encodeDestination(relative);
            }
            after = encodedLike(encoded, link.written) + markdownSuffix(link);
            if (fileUrl) after.prepend(QStringLiteral("file://"));
        }
        if (problem.isEmpty() && after == link.written) continue;
        found.append(reviewedChange(text, link.range, link.line, link.written, after, problem));
    }
    if (!document.files.empty() && document.files.front().frontMatter.present)
        citationChanges(document.files.front().frontMatter, text, folder, newFolder, library,
                        newPath == path || destinationLibrary.isEmpty() ? library : destinationLibrary, moves, found);
    std::stable_sort(found.begin(), found.end(), [](const Change &a, const Change &b) { return a.range.start < b.range.start; });
    return found;
}
