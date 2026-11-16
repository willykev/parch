#pragma once

#include <QList>
#include <QString>
#include <QStringView>

// The line rules the outline and the reference finder share until the
// dialect's parser gives them a tree: lines, their indent, code fences, and
// which lines are code.
namespace MarkdownLines {
// A line of the text, without its line break, which is CR, LF or CRLF.
struct Line {
    int start = 0;
    int end = 0;
};

QList<Line> split(const QString &text);
bool blank(QStringView line);
// How far the line is indented, a tab reaching the next multiple of four.
int indentOf(QStringView line);
// The line without its indent.
QStringView unindented(QStringView line);

// A code fence: three or more backticks or tildes, indented at most three spaces.
struct Fence {
    QChar mark;
    qsizetype length = 0;
};
// The fence the line opens; one of length 0 for none.
Fence fenceOf(QStringView line);
bool closes(QStringView line, const Fence &fence);

// The lines front matter takes at the start of a text: a first line of ---
// and the lines up to the next --- or ..., both included; 0 for none.
int frontMatterLines(const QString &text, const QList<Line> &lines);

// For each line, whether it is code: a fence, the lines inside one, or an
// indented line that does not carry on a paragraph.
QList<bool> codeLines(const QString &text, const QList<Line> &lines);
} // namespace MarkdownLines
