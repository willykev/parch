#include "focusmode.h"

#include "document.h"
#include "sourcebuffer.h"

#include <QRegularExpression>
#include <QTextBoundaryFinder>
#include <algorithm>

namespace {

bool isBreak(QChar c) { return c == u'\n' || c == u'\r' || c == QChar(0x2029); }

int lineStart(QStringView text, int position)
{
    while (position > 0 && !isBreak(text[position - 1])) --position;
    return position;
}

int lineEnd(QStringView text, int position)
{
    while (position < text.size() && !isBreak(text[position])) ++position;
    return position;
}

// The start of the line before the one starting at `start`, or -1.
int previousLine(QStringView text, int start)
{
    if (start <= 0) return -1;
    int end = start - 1;
    if (text[end] == u'\n' && end > 0 && text[end - 1] == u'\r') --end;
    return lineStart(text, end);
}

// The start of the line after the one ending at `end`, or -1.
int nextLine(QStringView text, int end)
{
    if (end >= text.size()) return -1;
    return text[end] == u'\r' && end + 1 < text.size() && text[end + 1] == u'\n' ? end + 2 : end + 1;
}

bool blank(QStringView line)
{
    return std::all_of(line.begin(), line.end(), [](QChar c) { return c.isSpace(); });
}

bool heading(QStringView line)
{
    static const QRegularExpression pattern(QStringLiteral("^ {0,3}#{1,6}(?:\\s|$)"));
    return pattern.matchView(line).hasMatch();
}

// A line that starts a part of its own: a heading, a list item, a quote, a
// table row or a fence.
bool startsPart(QStringView line)
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\s*(?:#{1,6}(?:\\s|$)|[*+-]\\s|\\d{1,9}[.)]\\s|>|\\||```|~~~)"));
    return pattern.matchView(line).hasMatch();
}

} // namespace

FocusMode::FocusMode(QObject *parent) : QObject(parent)
{
    m_afterEdit.setSingleShot(true);
    m_afterEdit.setInterval(0);
    connect(&m_afterEdit, &QTimer::timeout, this, &FocusMode::refresh);
}

void FocusMode::setDecorations(EditorDecorations *decorations)
{
    if (m_decorations == decorations) return;
    if (m_decorations) m_decorations->clearFocus();
    for (const QMetaObject::Connection &connection : std::as_const(m_connections)) disconnect(connection);
    m_connections.clear();
    m_decorations = decorations;
    if (decorations) {
        m_connections = {
            connect(decorations, &EditorDecorations::bindingChanged, this, &FocusMode::connectBinding),
            connect(decorations, &EditorDecorations::layersReset, this, &FocusMode::refresh),
        };
    }
    connectBinding();
    emit decorationsChanged();
}

void FocusMode::connectBinding()
{
    for (const QMetaObject::Connection &connection : std::as_const(m_bindingConnections)) disconnect(connection);
    m_bindingConnections.clear();
    m_binding = m_decorations ? m_decorations->binding() : nullptr;
    if (m_binding) {
        m_bindingConnections = {
            connect(m_binding, &EditorBinding::cursorChanged, this, &FocusMode::refresh),
            connect(m_binding, &EditorBinding::sourceEdited, this, [this] {
                if (m_enabled) m_afterEdit.start();
            }),
        };
    }
    refresh();
}

void FocusMode::setEnabled(bool enabled)
{
    if (enabled == m_enabled) return;
    m_enabled = enabled;
    refresh();
    emit enabledChanged();
}

QString FocusMode::unit() const
{
    return m_unit == Unit::Paragraph ? QStringLiteral("paragraph") : QStringLiteral("sentence");
}

void FocusMode::setUnit(const QString &unit)
{
    const Unit given = unit == u"paragraph" ? Unit::Paragraph : Unit::Sentence;
    if (given == m_unit) return;
    m_unit = given;
    refresh();
    emit unitChanged();
}

void FocusMode::setDim(const QColor &dim)
{
    if (dim == m_dim) return;
    m_dim = dim;
    if (m_decorations) m_decorations->setFocusColors(m_dim, m_normal);
    emit colorsChanged();
}

void FocusMode::setNormal(const QColor &normal)
{
    if (normal == m_normal) return;
    m_normal = normal;
    if (m_decorations) m_decorations->setFocusColors(m_dim, m_normal);
    emit colorsChanged();
}

void FocusMode::refresh()
{
    if (!m_decorations) return;
    if (!m_enabled || !m_binding || !m_binding->document() || !m_binding->textDocument()) {
        m_decorations->clearFocus();
        return;
    }
    m_decorations->setFocusColors(m_dim, m_normal);
    m_decorations->setFocus(rangeAt(m_binding->document()->buffer()->text(), m_binding->cursorSource(), m_unit));
}

SourceRange FocusMode::rangeAt(QStringView text, int position, Unit unit)
{
    position = qBound(0, position, int(text.size()));
    // Between the CR and LF of a pair is the end of the line before.
    if (position > 0 && position < text.size() && text[position - 1] == u'\r' && text[position] == u'\n') --position;
    const int first = lineStart(text, position);
    const int last = lineEnd(text, position);
    const QStringView line = text.sliced(first, last - first);
    if (blank(line)) return {position, 0};
    int start = first, end = last;
    if (!heading(line)) {
        // Back to the line that starts the paragraph, and on to its last.
        for (QStringView current = line; !startsPart(current) && position - start < ParagraphReach;) {
            const int previous = previousLine(text, start);
            if (previous < 0) break;
            const QStringView before = text.sliced(previous, lineEnd(text, previous) - previous);
            if (blank(before) || heading(before)) break;
            start = previous;
            current = before;
        }
        for (int next = nextLine(text, end); next >= 0 && end - position < ParagraphReach; next = nextLine(text, end)) {
            const QStringView after = text.sliced(next, lineEnd(text, next) - next);
            if (blank(after) || startsPart(after)) break;
            end = next + int(after.size());
        }
    }
    if (unit == Unit::Paragraph) return {start, end - start};

    const int from = qMax(start, position - SentenceReach);
    const int to = qMin(end, position + SentenceReach);
    QString window = text.sliced(from, to - from).toString();
    // A soft line break is a space within a sentence.
    for (QChar &c : window)
        if (isBreak(c) || c == QChar(0x2028)) c = u' ';
    QTextBoundaryFinder finder(QTextBoundaryFinder::Sentence, window);
    const int at = position - from;
    int sentenceStart = 0, sentenceEnd = int(window.size());
    if (at >= window.size()) {
        finder.toEnd();
        sentenceStart = qMax<qsizetype>(0, finder.toPreviousBoundary());
    } else {
        finder.setPosition(at);
        sentenceStart = finder.isAtBoundary() ? at : int(qMax<qsizetype>(0, finder.toPreviousBoundary()));
        finder.setPosition(at);
        const qsizetype next = finder.toNextBoundary();
        sentenceEnd = next < 0 ? int(window.size()) : int(next);
    }
    // The ends of what was looked through are no sentence's ends.
    const int opening = sentenceStart == 0 ? start : from + sentenceStart;
    const int closing = sentenceEnd == window.size() ? end : from + sentenceEnd;
    return {opening, closing - opening};
}
