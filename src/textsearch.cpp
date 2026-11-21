#include "textsearch.h"

#include "sourceboundaries.h"

#include <QCoreApplication>
#include <algorithm>

namespace {
// Line breaks are CR, LF and CRLF alike, so that ^ and $ find the ends of
// lines in every document and never the place between a CR and its LF.
const auto lineBreaks = QStringLiteral("(*ANYCRLF)");

bool breaksLine(const QString &text, qsizetype at)
{
    const QChar c = text.at(at);
    return c == u'\n' || (c == u'\r' && (at + 1 >= text.size() || text.at(at + 1) != u'\n'));
}

// The line, counted from 1, without its line break; the last line for a
// count past the end.
SourceRange lineRange(const QString &text, int line)
{
    qsizetype start = 0;
    int seen = 1;
    for (qsizetype at = 0; at < text.size() && seen < line; ++at) {
        if (!breaksLine(text, at)) continue;
        ++seen;
        start = at + 1;
    }
    qsizetype end = start;
    while (end < text.size() && text.at(end) != u'\n' && text.at(end) != u'\r') ++end;
    return {int(start), int(end - start)};
}
} // namespace

SourceRange TextSearch::relocate(const QString &text, int start, int length, int line, const QString &matched)
{
    const int size = int(text.size());
    if (start >= 0 && start <= size && QStringView(text).sliced(start).startsWith(matched)) {
        const int units = std::min(std::max(length, int(matched.size())), size - start);
        if (sourceGraphemeRange(text, {start, units})) return {start, units};
    }
    const SourceRange within = lineRange(text, line);
    if (!matched.isEmpty()) {
        const qsizetype onLine = QStringView(text).sliced(within.start, within.length).indexOf(matched);
        if (onLine >= 0 && sourceGraphemeRange(text, {within.start + int(onLine), int(matched.size())}))
            return {within.start + int(onLine), int(matched.size())};
        const int from = qBound(0, start, size);
        const qsizetype after = text.indexOf(matched, from);
        const qsizetype before = from > 0 ? text.lastIndexOf(matched, from - 1) : -1;
        const qsizetype nearest = before >= 0 && (after < 0 || from - before < after - from) ? before : after;
        if (nearest >= 0 && sourceGraphemeRange(text, {int(nearest), int(matched.size())}))
            return {int(nearest), int(matched.size())};
    }
    return {within.start, 0};
}

int TextSearch::LineCounter::lineOf(int position)
{
    position = std::clamp(position, 0, int(m_text.size()));
    if (position < m_at) m_at = m_line = m_start = 0;
    for (; m_at < position; ++m_at) {
        const QChar c = m_text[m_at];
        if (c != u'\n' && c != u'\r') continue;
        // A CR before an LF ends no line of its own.
        if (c == u'\r' && m_at + 1 < m_text.size() && m_text[m_at + 1] == u'\n') continue;
        ++m_line;
        m_start = m_at + 1;
    }
    return m_line;
}

int TextSearch::lineStartAbove(const QString &text, int position, int lines)
{
    qsizetype at = std::clamp<qsizetype>(position, 0, text.size());
    // Back to the start of the position's line, and then over as many line
    // breaks again, a CRLF pair counting as one.
    int breaks = 0;
    while (at > 0) {
        const QChar before = text.at(at - 1);
        if (before != u'\n' && before != u'\r') {
            --at;
            continue;
        }
        if (++breaks > lines) break;
        at -= before == u'\n' && at >= 2 && text.at(at - 2) == u'\r' ? 2 : 1;
    }
    return int(at);
}

std::optional<QRegularExpression> TextSearch::compile(const TextQuery &query, QString *error)
{
    if (error) error->clear();
    if (query.pattern.isEmpty()) return std::nullopt;
    QString body = query.regex ? query.pattern : QRegularExpression::escape(query.pattern);
    // Not inside a longer word on either side, whatever the match begins or ends with.
    if (query.wholeWord) body = QStringLiteral("(?<!\\w)(?:") + body + QStringLiteral(")(?!\\w)");
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
    if (query.regex) options |= QRegularExpression::MultilineOption;
    if (!query.caseSensitive) options |= QRegularExpression::CaseInsensitiveOption;
    const QString prefix = query.regex ? lineBreaks : QString();
    QRegularExpression expression(prefix + body, options);
    if (!expression.isValid()) {
        if (error) {
            // Counted in what was typed, not in what it was wrapped in.
            const qsizetype wrapped = prefix.size() + (query.wholeWord ? 10 : 0);
            const qsizetype at = qBound<qsizetype>(0, expression.patternErrorOffset() - wrapped, query.pattern.size());
            *error = QCoreApplication::translate("TextSearch", "Not a valid pattern: %1, at character %2")
                         .arg(expression.errorString(), QString::number(at + 1));
        }
        return std::nullopt;
    }
    expression.optimize();
    return expression;
}

void TextSearch::forEachMatch(const QString &text, const QRegularExpression &expression,
                              const std::function<bool(const QRegularExpressionMatch &)> &each,
                              const std::atomic<bool> *cancel)
{
    QRegularExpressionMatchIterator matches = expression.globalMatch(text);
    int seen = 0;
    while (matches.hasNext()) {
        if (cancel && (++seen & 0xff) == 0 && cancel->load(std::memory_order_relaxed)) return;
        const QRegularExpressionMatch match = matches.next();
        if (!sourceGraphemeRange(text, {int(match.capturedStart()), int(match.capturedLength())})) continue;
        if (!each(match)) return;
    }
}

QList<SourceRange> TextSearch::findAll(const QString &text, const QRegularExpression &expression, int limit,
                                       bool *truncated, const std::atomic<bool> *cancel)
{
    QList<SourceRange> found;
    if (truncated) *truncated = false;
    forEachMatch(text, expression, [&](const QRegularExpressionMatch &match) {
        if (found.size() >= limit) {
            if (truncated) *truncated = true;
            return false;
        }
        found.append({int(match.capturedStart()), int(match.capturedLength())});
        return true;
    }, cancel);
    return found;
}

QRegularExpressionMatch TextSearch::matchAt(const QString &text, const QRegularExpression &expression,
                                            SourceRange range)
{
    if (range.start < 0 || range.start > text.size()) return {};
    QRegularExpressionMatch match = expression.match(text, range.start, QRegularExpression::NormalMatch,
                                                     QRegularExpression::AnchorAtOffsetMatchOption);
    if (!match.hasMatch() || match.capturedStart() != range.start || match.capturedLength() != range.length)
        return {};
    return match;
}

QString TextSearch::expand(const QString &replacement, const QRegularExpressionMatch &match, bool regex)
{
    if (!regex || !replacement.contains(u'$')) return replacement;
    const int groups = match.lastCapturedIndex();
    QString result;
    result.reserve(replacement.size());
    for (qsizetype i = 0; i < replacement.size(); ++i) {
        const QChar c = replacement.at(i);
        if (c != u'$' || i + 1 >= replacement.size()) {
            result.append(c);
            continue;
        }
        const QChar next = replacement.at(i + 1);
        if (next == u'$') {
            result.append(u'$');
            ++i;
        } else if (next == u'&') {
            result.append(match.captured(0));
            ++i;
        } else if (next.isDigit() && next.unicode() < 128) {
            // Two digits name a group when there is one so numbered; otherwise
            // the first digit does, and the second is text.
            int group = next.unicode() - '0';
            qsizetype used = 2;
            if (i + 2 < replacement.size() && replacement.at(i + 2).isDigit() && replacement.at(i + 2).unicode() < 128) {
                const int both = group * 10 + (replacement.at(i + 2).unicode() - '0');
                if (both <= groups) {
                    group = both;
                    used = 3;
                }
            }
            if (group <= groups) result.append(match.captured(group));
            i += used - 1;
        } else if (next == u'{') {
            const qsizetype close = replacement.indexOf(u'}', i + 2);
            if (close < 0) {
                result.append(c);
                continue;
            }
            const QString name = replacement.mid(i + 2, close - i - 2);
            bool number = false;
            const int group = name.toInt(&number);
            if (number) {
                if (group >= 0 && group <= groups) result.append(match.captured(group));
            } else {
                result.append(match.captured(name));
            }
            i = close;
        } else {
            result.append(c);
        }
    }
    return result;
}

TextSearch::Edit TextSearch::combine(const QString &text, const QList<Edit> &edits)
{
    if (edits.isEmpty()) return {{0, 0}, {}};
    const int start = edits.first().range.start;
    const int end = edits.last().range.start + edits.last().range.length;
    QString replaced;
    int at = start;
    for (const Edit &edit : edits) {
        replaced.append(QStringView(text).sliced(at, edit.range.start - at));
        replaced.append(edit.text);
        at = edit.range.start + edit.range.length;
    }
    return {{start, end - start}, replaced};
}
