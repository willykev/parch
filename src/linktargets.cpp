#include "linktargets.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

namespace {

// How far a link's text may reach for its closing bracket, so that a line of
// nothing but brackets costs no more than its length times this.
constexpr int maxLinkText = 2000;

struct Line {
    qsizetype start;
    QString text;
};

QList<Line> linesOf(const QString &text)
{
    QList<Line> lines;
    qsizetype start = 0;
    const qsizetype n = text.size();
    for (qsizetype i = 0; i < n; ++i) {
        if (text[i] != u'\n' && text[i] != u'\r') continue;
        lines.append({start, text.mid(start, i - start)});
        if (text[i] == u'\r' && i + 1 < n && text[i + 1] == u'\n') ++i;
        start = i + 1;
    }
    lines.append({start, text.mid(start)});
    return lines;
}

// Columns of indentation, a tab reaching the next multiple of four.
int indentation(const QString &line, qsizetype *firstText = nullptr)
{
    int columns = 0;
    qsizetype i = 0;
    for (; i < line.size(); ++i) {
        if (line[i] == u' ') ++columns;
        else if (line[i] == u'\t') columns += 4 - columns % 4;
        else break;
    }
    if (firstText) *firstText = i;
    return columns;
}

bool isBlank(const QString &line)
{
    return line.trimmed().isEmpty();
}

// The fence a line opens, as its character and length; none when it opens none.
bool opensFence(const QString &line, QChar *fence, int *length)
{
    qsizetype at = 0;
    if (indentation(line, &at) > 3 || at >= line.size()) return false;
    const QChar c = line[at];
    if (c != u'`' && c != u'~') return false;
    int run = 0;
    while (at + run < line.size() && line[at + run] == c) ++run;
    if (run < 3) return false;
    // A backtick fence's info string has no backticks.
    if (c == u'`' && line.mid(at + run).contains(u'`')) return false;
    *fence = c;
    *length = run;
    return true;
}

bool closesFence(const QString &line, QChar fence, int length)
{
    qsizetype at = 0;
    if (indentation(line, &at) > 3) return false;
    int run = 0;
    while (at + run < line.size() && line[at + run] == fence) ++run;
    return run >= length && line.mid(at + run).trimmed().isEmpty();
}

bool startsListItem(const QString &line)
{
    static const QRegularExpression item(QStringLiteral("^ {0,3}(?:[-+*]|\\d{1,9}[.)])(?:[ \\t]|$)"));
    return item.match(line).hasMatch();
}

// The line with its code spans blanked out, so that nothing inside them is
// taken for a link; positions stay where they were.
QString withoutCodeSpans(const QString &line)
{
    QString masked = line;
    qsizetype i = 0;
    const qsizetype n = line.size();
    while (i < n) {
        if (line[i] == u'\\') {
            i += 2;
            continue;
        }
        if (line[i] != u'`') {
            ++i;
            continue;
        }
        qsizetype run = 0;
        while (i + run < n && line[i + run] == u'`') ++run;
        // The closing run is the next one exactly as long.
        qsizetype k = i + run;
        qsizetype close = -1;
        while (k < n) {
            if (line[k] != u'`') {
                ++k;
                continue;
            }
            qsizetype other = 0;
            while (k + other < n && line[k + other] == u'`') ++other;
            if (other == run) {
                close = k;
                break;
            }
            k += other;
        }
        if (close < 0) {
            i += run;
            continue;
        }
        for (qsizetype m = i; m < close + run; ++m) masked[m] = u' ';
        i = close + run;
    }
    return masked;
}

// The line with its HTML comments blanked out; `open` says whether a
// comment runs on from the line before, and on into the next.
QString withoutComments(const QString &line, bool &open)
{
    QString masked = line;
    qsizetype i = 0;
    while (i < masked.size()) {
        if (open) {
            const qsizetype end = masked.indexOf(QStringLiteral("-->"), i);
            const qsizetype stop = end < 0 ? masked.size() : end + 3;
            for (qsizetype m = i; m < stop; ++m) masked[m] = u' ';
            if (end < 0) return masked;
            open = false;
            i = stop;
            continue;
        }
        const qsizetype start = masked.indexOf(QStringLiteral("<!--"), i);
        if (start < 0) break;
        open = true;
        i = start;
    }
    return masked;
}

bool hasScheme(const QString &destination)
{
    const qsizetype colon = destination.indexOf(u':');
    if (colon < 2) return false;
    for (qsizetype i = 0; i < colon; ++i) {
        const QChar c = destination[i];
        const bool letter = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
        if (!(letter || (i > 0 && (c.isDigit() || c == u'+' || c == u'.' || c == u'-')))) return false;
    }
    return true;
}

// The destination with its backslash escapes taken as the characters they escape.
QString unescaped(const QString &destination)
{
    QString out;
    for (qsizetype i = 0; i < destination.size(); ++i) {
        if (destination[i] == u'\\' && i + 1 < destination.size() && destination[i + 1].isPunct()) ++i;
        else if (destination[i] == u'\\' && i + 1 < destination.size() && destination[i + 1].isSymbol()) ++i;
        out += destination[i];
    }
    out.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return out;
}

class Scanner {
public:
    Scanner(const QString &folder, const QString &library) : m_folder(folder), m_library(library) {}

    QList<LinkTargets::Target> targets;

    void add(LinkTargets::Target::Kind kind, const QString &destination, qsizetype position, int line)
    {
        LinkTargets::Target target;
        target.kind = kind;
        target.destination = destination;
        target.position = int(position);
        target.line = line;
        if (kind == LinkTargets::Target::Include) {
            if (destination.startsWith(u'/')) target.path = m_library.isEmpty() ? QString() : QDir::cleanPath(m_library + destination);
            else target.path = QDir::cleanPath(m_folder + u'/' + destination);
            targets.append(target);
            return;
        }
        QString path = unescaped(destination.trimmed());
        if (path.isEmpty() || path.startsWith(u'#')) return;
        if (hasScheme(path)) {
            if (!path.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) return;
            path = QUrl(path).toLocalFile();
            if (path.isEmpty()) return;
        } else {
            // A link into a file keeps to the file.
            const qsizetype cut = path.indexOf(QRegularExpression(QStringLiteral("[#?]")));
            if (cut == 0) return;
            if (cut > 0) path.truncate(cut);
            path = QUrl::fromPercentEncoding(path.toUtf8());
        }
        target.path = path.startsWith(u'/') ? QDir::cleanPath(path) : QDir::cleanPath(m_folder + u'/' + path);
        targets.append(target);
    }

    // Inline links and images, and the src and href of HTML, on one line.
    void scanInline(const QString &line, qsizetype offset, int number)
    {
        const qsizetype n = line.size();
        for (qsizetype i = 0; i < n; ++i) {
            if (line[i] == u'\\') {
                ++i;
                continue;
            }
            if (line[i] != u'[') continue;
            // Its closing bracket, past nested pairs and escapes.
            int depth = 0;
            qsizetype close = -1;
            for (qsizetype k = i + 1; k < n && k - i < maxLinkText; ++k) {
                if (line[k] == u'\\') {
                    ++k;
                    continue;
                }
                if (line[k] == u'[') ++depth;
                if (line[k] == u']' && depth-- == 0) {
                    close = k;
                    break;
                }
            }
            if (close < 0 || close + 1 >= n || line[close + 1] != u'(') continue;
            qsizetype start = close + 2;
            while (start < n && (line[start] == u' ' || line[start] == u'\t')) ++start;
            QString destination;
            qsizetype at = start;
            if (start < n && line[start] == u'<') {
                const qsizetype end = line.indexOf(u'>', start + 1);
                if (end < 0) continue;
                destination = line.mid(start + 1, end - start - 1);
                at = start + 1;
            } else {
                int parens = 0;
                qsizetype k = start;
                while (k < n) {
                    const QChar c = line[k];
                    if (c == u'\\') {
                        k += 2;
                        continue;
                    }
                    if (c.isSpace()) break;
                    if (c == u'(') ++parens;
                    if (c == u')' && parens-- == 0) break;
                    ++k;
                }
                destination = line.mid(start, std::min(k, n) - start);
            }
            const bool image = i > 0 && line[i - 1] == u'!' && !(i > 1 && line[i - 2] == u'\\');
            add(image ? LinkTargets::Target::Image : LinkTargets::Target::Link, destination, offset + at, number);
        }
        static const QRegularExpression html(
            QStringLiteral("<(img|a|source)\\b[^>]*?\\s(src|href)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"'>]+))"),
            QRegularExpression::CaseInsensitiveOption);
        for (auto match = html.globalMatch(line); match.hasNext();) {
            const QRegularExpressionMatch found = match.next();
            int group = 3;
            while (group <= 5 && found.capturedStart(group) < 0) ++group;
            const bool image = found.captured(1).compare(QStringLiteral("a"), Qt::CaseInsensitive) != 0;
            add(image ? LinkTargets::Target::Image : LinkTargets::Target::Link, found.captured(group),
                offset + found.capturedStart(group), number);
        }
    }

private:
    QString m_folder;
    QString m_library;
};

} // namespace

QList<LinkTargets::Target> LinkTargets::local(const QString &markdown, const QString &folder, const QString &library)
{
    Scanner scanner(folder, library);
    const QList<Line> lines = linesOf(markdown);
    static const QRegularExpression definition(QStringLiteral("^ {0,3}\\[(?!\\^)(?:[^\\]\\\\]|\\\\.)+\\]:[ \\t]*(<[^>]*>|\\S+)"));
    static const QRegularExpression include(
        QStringLiteral("^ {0,3}(?:\"([^\"]+\\.(?:md|markdown|txt))\"|([^\\s\"]+\\.(?:md|markdown|txt)))(?:[ \\t]+\"[^\"]*\")?\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    qsizetype i = 0;
    // Front matter, from a first line of three hyphens to the next such line.
    if (!lines.isEmpty() && (lines[0].text == u"---" || lines[0].text == QStringLiteral("\ufeff---"))) {
        for (qsizetype k = 1; k < lines.size(); ++k) {
            if (lines[k].text == u"---" || lines[k].text == u"...") {
                i = k + 1;
                break;
            }
        }
    }
    bool inFence = false, inComment = false, previousBlank = true, inList = false, inCode = false;
    QChar fence;
    int fenceLength = 0;
    // The paragraph so far: its first line and how many lines it has.
    qsizetype paragraphLine = -1;
    int paragraphLines = 0;
    const auto endParagraph = [&] {
        if (paragraphLines == 1) {
            const Line &only = lines[paragraphLine];
            const QRegularExpressionMatch found = include.match(only.text);
            if (found.hasMatch()) {
                const int group = found.capturedStart(1) >= 0 ? 1 : 2;
                scanner.add(Target::Include, found.captured(group), only.start + found.capturedStart(group),
                            int(paragraphLine) + 1);
            }
        }
        paragraphLine = -1;
        paragraphLines = 0;
    };
    for (; i < lines.size(); ++i) {
        const Line &line = lines[i];
        const int number = int(i) + 1;
        if (inFence) {
            if (closesFence(line.text, fence, fenceLength)) inFence = false;
            continue;
        }
        if (isBlank(line.text) && !inComment) {
            endParagraph();
            previousBlank = true;
            continue;
        }
        const int indent = indentation(line.text);
        if (!inComment && opensFence(line.text, &fence, &fenceLength)) {
            endParagraph();
            inFence = true;
            previousBlank = false;
            continue;
        }
        // Indented code: after a blank line, outside a list, or going on from more of it.
        if (!inComment && indent >= 4 && ((previousBlank && !inList) || inCode)) {
            inCode = true;
            previousBlank = false;
            continue;
        }
        inCode = false;
        // A line that starts with // is a comment in the Writer-compatible profile.
        if (!inComment && line.text.startsWith(QStringLiteral("//"))) {
            previousBlank = false;
            continue;
        }
        if (startsListItem(line.text)) inList = true;
        else if (indent == 0 && previousBlank) inList = false;
        const QString text = withoutCodeSpans(withoutComments(line.text, inComment));
        if (isBlank(text)) {
            previousBlank = false;
            continue;
        }
        const QRegularExpressionMatch defined = definition.match(text);
        if (defined.hasMatch()) {
            QString destination = defined.captured(1);
            qsizetype at = defined.capturedStart(1);
            if (destination.startsWith(u'<')) {
                destination = destination.mid(1, destination.size() - 2);
                ++at;
            }
            scanner.add(Target::Link, destination, line.start + at, number);
            endParagraph();
        } else {
            scanner.scanInline(text, line.start, number);
            // A heading, a list item or a quote is no paragraph an include could be.
            static const QRegularExpression ownBlock(QStringLiteral("^ {0,3}(?:#{1,6}(?:[ \\t]|$)|>)"));
            if (ownBlock.match(text).hasMatch() || startsListItem(text)) {
                endParagraph();
            } else {
                if (paragraphLines == 0) paragraphLine = i;
                ++paragraphLines;
            }
        }
        previousBlank = false;
    }
    endParagraph();
    // The include of a paragraph is found when the paragraph ends, after the
    // links of later lines; the targets go back in the document's order.
    QList<Target> targets = scanner.targets;
    std::stable_sort(targets.begin(), targets.end(), [](const Target &a, const Target &b) { return a.position < b.position; });
    return targets;
}

QList<LinkTargets::Target> LinkTargets::missing(const QString &markdown, const QString &folder, const QString &library)
{
    QList<Target> absent;
    for (const Target &target : local(markdown, folder, library))
        if (target.path.isEmpty() || !QFileInfo::exists(target.path)) absent.append(target);
    return absent;
}

QString LinkTargets::libraryOf(const QString &path, const QStringList &libraries)
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
