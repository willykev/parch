#pragma once

#include "sourcetypes.h"

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringView>
#include <atomic>
#include <functional>
#include <optional>

// What to look for in a text: what was typed, and how it is matched. Find in
// a document and search across the library share it.
struct TextQuery {
    QString pattern;
    bool caseSensitive = false;
    // Only where the match is not part of a longer word, as Unicode counts
    // word characters.
    bool wholeWord = false;
    // The pattern is a regular expression, as QRegularExpression reads one,
    // with ^ and $ at the start and end of every line; otherwise it is found
    // as written.
    bool regex = false;
    bool operator==(const TextQuery &) const = default;
};

namespace TextSearch {
// The most matches one search of one text keeps.
constexpr int MaxMatches = 1000000;

// The expression the query is matched with, or none: for an empty pattern,
// which looks for nothing, and for a regular expression that is not one,
// when `error` says why in a few words.
std::optional<QRegularExpression> compile(const TextQuery &query, QString *error = nullptr);

// Every match in the text, in order and apart, each starting and ending
// between graphemes so that it can be selected and replaced whole: a match
// that would split one, as an accent apart from its letter or a CR apart
// from its LF, is passed over. At most `limit` of them; `truncated` says
// whether there were more. A search that `cancel` stops ends at once, with
// what it found so far.
QList<SourceRange> findAll(const QString &text, const QRegularExpression &expression, int limit = MaxMatches,
                           bool *truncated = nullptr, const std::atomic<bool> *cancel = nullptr);

// Calls `each` with every match findAll() would keep, groups and all, in
// order, until it returns false or `cancel` is set. One pass, so that the
// text is checked as UTF-16 once, however many matches it holds.
void forEachMatch(const QString &text, const QRegularExpression &expression,
                  const std::function<bool(const QRegularExpressionMatch &)> &each,
                  const std::atomic<bool> *cancel = nullptr);

// The match of the expression at exactly this range of the text, as findAll()
// found it there; one with no match when the text there no longer matches so.
// Each call reads the whole text again, so it is for one match at a time.
QRegularExpressionMatch matchAt(const QString &text, const QRegularExpression &expression, SourceRange range);

// What a match is replaced with. For a regular expression, $1 to $99 are the
// groups it captured, ${name} a named one, $0 and $& the whole match and $$
// a dollar sign; a group that did not take part is empty. Otherwise the
// replacement is taken as written.
QString expand(const QString &replacement, const QRegularExpressionMatch &match, bool regex);

// Where a match found earlier is in the text now: at the same place if the
// text there still starts with what matched; else the same text on the same
// line, counted from 1; else the same text nearest that place; else nothing
// selected at the start of that line.
SourceRange relocate(const QString &text, int start, int length, int line, const QString &matched);

// The start of the line `lines` lines above the one holding the position,
// or of the first line, as a place to show the position a little below
// the top of a view.
int lineStartAbove(const QString &text, int position, int lines);

// One change of a text: its range, in the text as it was, and what goes there.
struct Edit {
    SourceRange range;
    QString text;
};

// The changes, which are in order and apart, as one: the range from the
// first to the end of the last, and what that range becomes. A text can
// take them as a single edit, which a single undo reverts.
Edit combine(const QString &text, const QList<Edit> &edits);
} // namespace TextSearch
