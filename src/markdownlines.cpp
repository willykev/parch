#include "markdownlines.h"

#include <algorithm>

QList<MarkdownLines::Line> MarkdownLines::split(const QString &text)
{
    QList<Line> lines;
    int start = 0;
    const int size = int(text.size());
    for (int at = 0; at < size; ++at) {
        const QChar c = text.at(at);
        if (c != u'\n' && c != u'\r') continue;
        lines.append({start, at});
        if (c == u'\r' && at + 1 < size && text.at(at + 1) == u'\n') ++at;
        start = at + 1;
    }
    lines.append({start, size});
    return lines;
}

bool MarkdownLines::blank(QStringView line)
{
    return std::all_of(line.begin(), line.end(), [](QChar c) { return c == u' ' || c == u'\t'; });
}

int MarkdownLines::indentOf(QStringView line)
{
    int columns = 0;
    for (const QChar c : line) {
        if (c == u' ') ++columns;
        else if (c == u'\t') columns += 4 - columns % 4;
        else break;
    }
    return columns;
}

QStringView MarkdownLines::unindented(QStringView line)
{
    qsizetype at = 0;
    while (at < line.size() && (line.at(at) == u' ' || line.at(at) == u'\t')) ++at;
    return line.sliced(at);
}

MarkdownLines::Fence MarkdownLines::fenceOf(QStringView line)
{
    if (indentOf(line) > 3) return {};
    const QStringView rest = unindented(line);
    if (rest.isEmpty() || (rest.front() != u'`' && rest.front() != u'~')) return {};
    const QChar mark = rest.front();
    qsizetype length = 0;
    while (length < rest.size() && rest.at(length) == mark) ++length;
    if (length < 3) return {};
    // A backtick fence's info string has no backtick in it.
    if (mark == u'`' && rest.sliced(length).contains(u'`')) return {};
    return {mark, length};
}

bool MarkdownLines::closes(QStringView line, const Fence &fence)
{
    if (indentOf(line) > 3) return false;
    const QStringView rest = unindented(line);
    qsizetype length = 0;
    while (length < rest.size() && rest.at(length) == fence.mark) ++length;
    return length >= fence.length && blank(rest.sliced(length));
}

int MarkdownLines::frontMatterLines(const QString &text, const QList<Line> &lines)
{
    const auto lineAt = [&](qsizetype i) {
        return QStringView(text).sliced(lines.at(i).start, lines.at(i).end - lines.at(i).start);
    };
    if (lines.isEmpty() || lineAt(0).trimmed() != u"---") return 0;
    for (qsizetype j = 1; j < lines.size(); ++j) {
        const QStringView closing = lineAt(j).trimmed();
        if (closing == u"---" || closing == u"...") return int(j + 1);
    }
    return 0;
}

QList<bool> MarkdownLines::codeLines(const QString &text, const QList<Line> &lines)
{
    QList<bool> code(lines.size(), false);
    Fence fence;
    bool paragraph = false;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QStringView line = QStringView(text).sliced(lines.at(i).start, lines.at(i).end - lines.at(i).start);
        if (fence.length > 0) {
            code[i] = true;
            if (closes(line, fence)) fence = {};
            continue;
        }
        if (blank(line)) {
            paragraph = false;
            continue;
        }
        if (indentOf(line) >= 4 && !paragraph) {
            code[i] = true;
            continue;
        }
        if (const Fence opened = fenceOf(line); opened.length > 0) {
            fence = opened;
            code[i] = true;
            paragraph = false;
            continue;
        }
        paragraph = true;
    }
    return code;
}
