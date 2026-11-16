#include "outline.h"

#include "mdinclude.h"
#include "mdtree.h"
#include "pathtext.h"
#include "sourcebuffer.h"
#include "textsearch.h"

#include <QHash>
#include <QSet>
#include <QtConcurrent>
#include <algorithm>
#include <utility>

namespace {
// Where each included heading is brought in: the start of the include, in
// the document's own text, that brings in it or what holds it.
void includedFrom(const md::Node &node, int include, const QSet<const md::Node *> &wanted,
                  QHash<const md::Node *, int> &places)
{
    if (include < 0 && node.type == md::NodeType::Include && node.file == 0) include = node.range.start;
    if (include >= 0 && wanted.contains(&node)) places.insert(&node, include);
    for (const md::Node &child : node.children) includedFrom(child, include, wanted, places);
}

QList<Heading> headingsOf(const QString &text, const md::Document &document)
{
    QSet<const md::Node *> wanted;
    for (const md::Heading &heading : document.headings)
        if (heading.file != 0 && heading.node) wanted.insert(heading.node);
    QHash<const md::Node *, int> places;
    if (!wanted.isEmpty()) includedFrom(document.root, -1, wanted, places);
    QList<Heading> found;
    TextSearch::LineCounter lines(text);
    for (const md::Heading &heading : document.headings) {
        if (heading.text.isEmpty()) continue;
        if (heading.file == 0) {
            const int line = lines.lineOf(heading.range.start);
            found.append({heading.level, heading.text, lines.lineStart(), line, heading.slug, heading.label});
            continue;
        }
        const auto place = places.constFind(heading.node);
        if (place == places.cend() || size_t(heading.file) >= document.files.size()) continue;
        const md::SourceFile &file = document.files[size_t(heading.file)];
        if (file.path.isEmpty()) continue;
        const int line = lines.lineOf(*place);
        found.append({heading.level, heading.text, lines.lineStart(), line, heading.slug, heading.label, file.path,
                      TextSearch::LineCounter(file.text).lineOf(heading.range.start)});
    }
    return found;
}
} // namespace

QList<Heading> Outline::headings(const QString &text, const md::Options &options)
{
    return headingsOf(text, md::parse(text, options));
}

QList<Heading> Outline::headings(const QString &text, const md::Options &options, const md::Source &source)
{
    return headingsOf(text, md::parse(text, options, source));
}

QList<Heading> Outline::headings(const md::Document &document, const QString &text)
{
    return headingsOf(text, document);
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
    case FileRole:
        return heading.file;
    case FileLineRole:
        return heading.fileLine;
    default:
        return {};
    }
}

QHash<int, QByteArray> OutlineModel::roleNames() const
{
    return {{TextRole, "heading"}, {LevelRole, "level"}, {PositionRole, "position"},
            {LineRole, "line"},     {SlugRole, "slug"},   {LabelRole, "label"},
            {FileRole, "file"},     {FileLineRole, "fileLine"}};
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
    disconnect(m_dialect);
    disconnect(m_parses);
    m_document = document;
    ++m_generation;
    // A parse asked of the document before never comes.
    if (std::exchange(m_waiting, false)) m_running = false;
    if (document) {
        m_parses = connect(document->parse(), &DocumentParse::parsed, this, [this](const ParsedTextPtr &parse) {
            if (std::exchange(m_waiting, false)) read(parse);
        });
        m_edits = connect(document->buffer(), &SourceBuffer::sourceChanged, this, [this] {
            if (!m_active) return;
            setReady(false);
            schedule(EditSettleMs);
        });
        // Read in another dialect, the same text may have other headings.
        m_dialect = connect(document, &Document::dialectChanged, this, [this] {
            ++m_generation;
            if (!m_active) return;
            setReady(false);
            schedule(0);
        });
    }
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

QString OutlineModel::fileAt(int row) const
{
    return row >= 0 && row < m_shown.size() ? m_shown.at(row).file : QString();
}

int OutlineModel::fileLineAt(int row) const
{
    return row >= 0 && row < m_shown.size() ? m_shown.at(row).fileLine : -1;
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
    // The text as it is now, parsed for every view of the document: at
    // once when it is already, or else once it is.
    m_waiting = true;
    if (const ParsedTextPtr parse = m_document->parse()->request()) {
        m_waiting = false;
        read(parse);
    }
}

void OutlineModel::read(const ParsedTextPtr &parse)
{
    const quint64 generation = m_generation;
    QtConcurrent::run(&m_pool, &OutlineModel::scanJob, parse)
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
        })
        // A scan that fails, as when memory runs out, leaves the outline as
        // it was, ready for the next edit.
        .onFailed(this, [this] {
            m_running = false;
            m_scanAgain = false;
        });
}

OutlineModel::Scan OutlineModel::scanJob(ParsedTextPtr parse)
{
    return {Outline::headings(parse->document(), parse->text), parse->revision};
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
