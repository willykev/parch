#include "outline.h"

#include "markdownlines.h"
#include "sourcebuffer.h"

#include <QRegularExpression>
#include <QtConcurrent>
#include <algorithm>
#include <utility>

namespace {
using namespace MarkdownLines;

const QRegularExpression &atxHeading()
{
    static const QRegularExpression expression(QStringLiteral("^ {0,3}(#{1,6})(?:[ \\t]+(.*?))?(?:[ \\t]+#+)?[ \\t]*$"));
    return expression;
}

const QRegularExpression &setextUnderline()
{
    static const QRegularExpression expression(QStringLiteral("^ {0,3}(=+|-+)[ \\t]*$"));
    return expression;
}

const QRegularExpression &thematicBreak()
{
    static const QRegularExpression expression(QStringLiteral("^ {0,3}(?:(?:-[ \\t]*){3,}|(?:\\*[ \\t]*){3,}|(?:_[ \\t]*){3,})$"));
    return expression;
}

// The start of a block quote or a list item, whose headings belong to them.
const QRegularExpression &containerStart()
{
    static const QRegularExpression expression(QStringLiteral("^ {0,3}(?:>|[-*+](?:[ \\t]|$)|\\d{1,9}[.)](?:[ \\t]|$))"));
    return expression;
}

// A heading's text as shown: without a Writer label in brackets at its end,
// which names it for links and is not printed.
QString shownText(QString text)
{
    static const QRegularExpression label(QStringLiteral("[ \\t]*\\[[^\\[\\]]+\\][ \\t]*$"));
    const QRegularExpressionMatch match = label.match(text);
    if (match.hasMatch() && match.capturedStart() > 0) text.truncate(match.capturedStart());
    return text.trimmed();
}
} // namespace

QList<Heading> Outline::headings(const QString &text)
{
    QList<Heading> found;
    const QList<Line> lines = split(text);
    const auto lineAt = [&](qsizetype i) {
        return QStringView(text).sliced(lines.at(i).start, lines.at(i).end - lines.at(i).start);
    };
    qsizetype i = frontMatterLines(text, lines);
    Fence fence;
    bool inComment = false;
    // The paragraph being read, from its first line, and whether a Setext
    // underline can make it a heading.
    qsizetype paragraph = -1;
    bool plain = false;
    for (; i < lines.size(); ++i) {
        const QStringView line = lineAt(i);
        if (fence.length > 0) {
            if (closes(line, fence)) fence = {};
            continue;
        }
        if (inComment) {
            if (line.contains(u"-->")) inComment = false;
            continue;
        }
        if (blank(line)) {
            paragraph = -1;
            continue;
        }
        const int indent = indentOf(line);
        if (paragraph >= 0 && indent <= 3) {
            const QRegularExpressionMatch underline = setextUnderline().matchView(line);
            if (underline.hasMatch()) {
                if (plain) {
                    QStringList parts;
                    for (qsizetype j = paragraph; j < i; ++j) parts << lineAt(j).trimmed().toString();
                    const QString shown = shownText(parts.join(u' '));
                    if (!shown.isEmpty())
                        found.append({underline.captured(1).startsWith(u'=') ? 1 : 2, shown, lines.at(paragraph).start,
                                      int(paragraph)});
                }
                paragraph = -1;
                continue;
            }
        }
        // Indented code, or a line that carries on a paragraph: no heading either way.
        if (indent >= 4) continue;
        if (const Fence opened = fenceOf(line); opened.length > 0) {
            fence = opened;
            paragraph = -1;
            continue;
        }
        const QStringView rest = unindented(line);
        if (rest.startsWith(u"<!--")) {
            inComment = !rest.sliced(4).contains(u"-->");
            paragraph = -1;
            continue;
        }
        if (const QRegularExpressionMatch atx = atxHeading().matchView(line); atx.hasMatch()) {
            const QString shown = shownText(atx.captured(2));
            if (!shown.isEmpty()) found.append({int(atx.capturedLength(1)), shown, lines.at(i).start, int(i)});
            paragraph = -1;
            continue;
        }
        if (thematicBreak().matchView(line).hasMatch()) {
            paragraph = -1;
            continue;
        }
        if (containerStart().matchView(line).hasMatch()) {
            // What follows, lazily, belongs to the quote or the item.
            paragraph = i;
            plain = false;
            continue;
        }
        if (paragraph < 0) {
            paragraph = i;
            plain = true;
        }
    }
    return found;
}

OutlineModel::OutlineModel(QObject *parent) : QAbstractListModel(parent)
{
    m_pool.setMaxThreadCount(1);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &OutlineModel::scan);
}

OutlineModel::~OutlineModel()
{
    m_pool.waitForDone();
}

int OutlineModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_shown.size());
}

QVariant OutlineModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_shown.size()) return {};
    const Heading &heading = m_shown.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case TextRole:
        return heading.text;
    case LevelRole:
        return heading.level;
    case PositionRole:
        return heading.position;
    case LineRole:
        return heading.line;
    default:
        return {};
    }
}

QHash<int, QByteArray> OutlineModel::roleNames() const
{
    return {{TextRole, "heading"}, {LevelRole, "level"}, {PositionRole, "position"}, {LineRole, "line"}};
}

Document *OutlineModel::document() const { return m_document; }
bool OutlineModel::active() const { return m_active; }
QString OutlineModel::filter() const { return m_filter; }
bool OutlineModel::ready() const { return m_ready; }
QList<Heading> OutlineModel::headings() const { return m_shown; }

void OutlineModel::setDocument(Document *document)
{
    if (document == m_document) return;
    disconnect(m_edits);
    m_document = document;
    ++m_generation;
    if (document)
        m_edits = connect(document->buffer(), &SourceBuffer::sourceChanged, this, [this] {
            if (!m_active) return;
            setReady(false);
            schedule(EditSettleMs);
        });
    take({});
    setReady(false);
    emit documentChanged();
    if (m_active) schedule(0);
}

void OutlineModel::setActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    emit activeChanged();
    if (active) {
        schedule(0);
    } else {
        // What it shows may be out of date by the time it is shown again.
        m_timer.stop();
        ++m_generation;
        setReady(false);
    }
}

void OutlineModel::setFilter(const QString &filter)
{
    if (filter == m_filter) return;
    m_filter = filter;
    emit filterChanged();
    show();
}

int OutlineModel::positionAt(int row) const
{
    return row >= 0 && row < m_shown.size() ? m_shown.at(row).position : -1;
}

int OutlineModel::rowAt(int position) const
{
    const auto after = std::upper_bound(m_shown.cbegin(), m_shown.cend(), position,
                                        [](int at, const Heading &heading) { return at < heading.position; });
    return int(after - m_shown.cbegin()) - 1;
}

void OutlineModel::schedule(int delayMs)
{
    m_timer.start(delayMs);
}

void OutlineModel::scan()
{
    if (!m_active || !m_document) return;
    if (m_running) {
        m_scanAgain = true;
        return;
    }
    m_running = true;
    m_scanAgain = false;
    const quint64 generation = m_generation;
    QtConcurrent::run(&m_pool, &OutlineModel::scanJob, m_document->buffer()->snapshot())
        .then(this, [this, generation](const Scan &result) {
            m_running = false;
            const bool again = std::exchange(m_scanAgain, false);
            if (generation == m_generation && !again && m_document
                && result.revision == m_document->buffer()->revision()) {
                take(result.headings);
                setReady(true);
                return;
            }
            // An edit since then asks for its own scan once the text rests.
            if (again || (generation != m_generation && m_active)) scan();
        });
}

OutlineModel::Scan OutlineModel::scanJob(QSharedPointer<const SourceSnapshot> snapshot)
{
    return {Outline::headings(snapshot->text), snapshot->revision};
}

void OutlineModel::take(const QList<Heading> &headings)
{
    m_all = headings;
    show();
}

void OutlineModel::show()
{
    QList<Heading> next;
    for (const Heading &heading : std::as_const(m_all))
        if (m_filter.isEmpty() || heading.text.contains(m_filter, Qt::CaseInsensitive)) next.append(heading);
    // Only the rows that came or went are announced, so that a list keeps
    // its place while headings above and below it stay; the rest may have
    // moved in the text, which a change of their data says.
    const auto same = [](const Heading &a, const Heading &b) { return a.level == b.level && a.text == b.text; };
    const qsizetype before = m_shown.size();
    qsizetype head = 0;
    while (head < before && head < next.size() && same(m_shown.at(head), next.at(head))) ++head;
    qsizetype tail = 0;
    while (tail < before - head && tail < next.size() - head
           && same(m_shown.at(before - 1 - tail), next.at(next.size() - 1 - tail)))
        ++tail;
    if (before - head - tail > 0) {
        beginRemoveRows({}, int(head), int(before - tail - 1));
        m_shown.remove(head, before - head - tail);
        endRemoveRows();
    }
    if (next.size() - head - tail > 0) {
        beginInsertRows({}, int(head), int(next.size() - tail - 1));
        for (qsizetype i = head; i < next.size() - tail; ++i) m_shown.insert(i, next.at(i));
        endInsertRows();
    }
    m_shown = next;
    if (!m_shown.isEmpty()) emit dataChanged(index(0), index(int(m_shown.size()) - 1));
    if (before != m_shown.size()) emit countChanged();
}

void OutlineModel::setReady(bool ready)
{
    if (ready == m_ready) return;
    m_ready = ready;
    emit readyChanged();
}
