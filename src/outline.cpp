#include "outline.h"

#include "mdtree.h"
#include "sourcebuffer.h"
#include "textsearch.h"

#include <QtConcurrent>
#include <algorithm>
#include <utility>

QList<Heading> Outline::headings(const QString &text)
{
    const md::Document document = md::parse(text);
    QList<Heading> found;
    TextSearch::LineCounter lines(text);
    for (const md::Heading &heading : document.headings) {
        if (heading.file != 0 || heading.text.isEmpty()) continue;
        const int line = lines.lineOf(heading.range.start);
        found.append({heading.level, heading.text, lines.lineStart(), line, heading.slug, heading.label});
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
    case SlugRole:
        return heading.slug;
    case LabelRole:
        return heading.label;
    default:
        return {};
    }
}

QHash<int, QByteArray> OutlineModel::roleNames() const
{
    return {{TextRole, "heading"}, {LevelRole, "level"}, {PositionRole, "position"},
            {LineRole, "line"},     {SlugRole, "slug"},   {LabelRole, "label"}};
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
