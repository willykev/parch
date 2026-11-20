#include "editordecorations.h"

#include "document.h"
#include "editorbinding.h"
#include "sourcebuffer.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QVarLengthArray>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
// Edits remembered for moving older results forward; a result older than
// these is stale.
constexpr size_t rememberedEdits = 1024;

// What a layer's format may set: nothing that moves a glyph or changes a
// line's height.
QTextCharFormat withoutGeometry(QTextCharFormat format)
{
    for (const int property :
         {int(QTextFormat::FontFamily), int(QTextFormat::FontFamilies), int(QTextFormat::FontStyleName),
          int(QTextFormat::FontPointSize), int(QTextFormat::FontPixelSize), int(QTextFormat::FontSizeAdjustment),
          int(QTextFormat::FontFixedPitch), int(QTextFormat::FontCapitalization), int(QTextFormat::FontLetterSpacing),
          int(QTextFormat::FontLetterSpacingType), int(QTextFormat::FontWordSpacing), int(QTextFormat::FontStretch),
          int(QTextFormat::FontKerning), int(QTextFormat::FontStyleHint), int(QTextFormat::FontStyleStrategy),
          int(QTextFormat::FontHintingPreference), int(QTextFormat::TextVerticalAlignment),
          int(QTextFormat::TextSuperScriptBaseline), int(QTextFormat::TextSubScriptBaseline),
          int(QTextFormat::TextBaselineOffset)})
        format.clearProperty(property);
    return format;
}

// Where a position of the source goes when an edit replaces `removed` units
// at `start` with `inserted`: before it, it stays; after it, it moves by the
// change of length; inside what was replaced, it goes to the end of what came
// instead. A start at the edit's own start stays, so that a range keeps what
// replaced its first units; an end there stays, so that a range does not
// grow by what is typed just after it.
struct EditMap {
    int start, oldEnd, newEnd, delta;
    int startOf(int position) const
    {
        if (position >= oldEnd) return position + delta;
        return position <= start ? position : newEnd;
    }
    int endOf(int position) const
    {
        if (position <= start) return position;
        return position >= oldEnd ? position + delta : newEnd;
    }
    // A range that ends or starts where the edit starts grows with it, as
    // the caret's sentence grows with what is typed at either end of it.
    int inclusiveEndOf(int position) const { return position == start ? newEnd : endOf(position); }
    int inclusiveStartOf(int position) const { return position == start ? position : startOf(position); }
};

// The first span that ends after `position`, in spans that are in order and apart.
std::vector<DecorationSpan>::const_iterator firstEndingAfter(const std::vector<DecorationSpan> &spans, int position)
{
    return std::partition_point(spans.begin(), spans.end(),
                                [position](const DecorationSpan &span) { return span.end <= position; });
}

// Spans in order and apart, inside [0, length), each with a style the layer has.
std::vector<DecorationSpan> tidy(std::vector<DecorationSpan> spans, int low, int high, int styles)
{
    std::stable_sort(spans.begin(), spans.end(),
                     [](const DecorationSpan &a, const DecorationSpan &b) { return a.start < b.start; });
    std::vector<DecorationSpan> kept;
    kept.reserve(spans.size());
    int reached = low;
    for (DecorationSpan span : spans) {
        if (span.style < 0 || span.style >= styles) continue;
        span.start = qMax(span.start, reached);
        span.end = qMin(span.end, high);
        if (span.start >= span.end) continue;
        reached = span.end;
        kept.push_back(span);
    }
    return kept;
}
} // namespace

int DecorationMarks::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_marks.size());
}

QVariant DecorationMarks::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_marks.size())) return {};
    const Mark &mark = m_marks[size_t(index.row())];
    switch (role) {
    case XRole:
        return mark.x;
    case YRole:
        return mark.y;
    case WidthRole:
        return mark.width;
    case HeightRole:
        return mark.height;
    case ColorRole:
        return mark.color;
    default:
        return {};
    }
}

QHash<int, QByteArray> DecorationMarks::roleNames() const
{
    return {{XRole, "markX"}, {YRole, "markY"}, {WidthRole, "markWidth"}, {HeightRole, "markHeight"},
            {ColorRole, "markColor"}};
}

void DecorationMarks::update(const std::vector<Mark> &marks)
{
    const size_t common = std::min(marks.size(), m_marks.size());
    if (marks.size() < m_marks.size()) {
        beginRemoveRows({}, int(marks.size()), int(m_marks.size()) - 1);
        m_marks.resize(marks.size());
        endRemoveRows();
    }
    for (size_t i = 0; i < common; ++i) {
        if (m_marks[i] == marks[i]) continue;
        m_marks[i] = marks[i];
        emit dataChanged(index(int(i)), index(int(i)));
    }
    if (marks.size() > m_marks.size()) {
        beginInsertRows({}, int(m_marks.size()), int(marks.size()) - 1);
        m_marks.insert(m_marks.end(), marks.begin() + qsizetype(m_marks.size()), marks.end());
        endInsertRows();
    }
}

EditorDecorations::EditorDecorations(QObject *parent) : QSyntaxHighlighter(parent), m_waves(new DecorationMarks(this))
{
    m_slices.setInterval(0);
    connect(&m_slices, &QTimer::timeout, this, &EditorDecorations::formatSlice);
    m_waveTimer.setSingleShot(true);
    m_waveTimer.setInterval(0);
    connect(&m_waveTimer, &QTimer::timeout, this, &EditorDecorations::refreshWaves);
}

EditorDecorations::~EditorDecorations()
{
    for (const QMetaObject::Connection &connection : std::as_const(m_connections)) disconnect(connection);
}

EditorBinding *EditorDecorations::binding() const { return m_binding; }

QAbstractItemModel *EditorDecorations::waves() const { return m_waves; }

void EditorDecorations::setBinding(EditorBinding *binding)
{
    if (m_binding == binding) return;
    for (const QMetaObject::Connection &connection : std::as_const(m_connections)) disconnect(connection);
    m_connections.clear();
    m_binding = binding;
    if (binding) {
        // The view's grids draw their cells with these layers too.
        binding->tables()->setDecorations(this);
        m_connections = {
            connect(binding, &EditorBinding::sourceEdited, this, &EditorDecorations::sourceEdited),
            connect(binding, &EditorBinding::projectionLoaded, this, &EditorDecorations::projectionLoaded),
            connect(binding, &EditorBinding::documentChanged, this, &EditorDecorations::attach),
            connect(binding, &EditorBinding::editorChanged, this, &EditorDecorations::attach),
        };
    }
    attach();
    emit bindingChanged();
}

void EditorDecorations::attach()
{
    // Whatever the layers held was about the text shown before.
    for (Layer &layer : m_layers) layer.spans.clear();
    m_edits.clear();
    m_dirty.clear();
    m_slices.stop();
    m_loaded = false;
    m_visibleKnown = false;
    QTextDocument *document = nullptr;
    if (m_binding && m_binding->editor())
        if (auto *quick = m_binding->editor()->property("textDocument").value<QQuickTextDocument *>())
            document = quick->textDocument();
    if (document != m_document) {
        for (const QMetaObject::Connection &connection : std::as_const(m_documentConnections)) disconnect(connection);
        m_documentConnections.clear();
        m_document = document;
        // Attached before the binding fills it, Qt formats each paragraph as
        // it is made, rather than all of them again afterwards. A view whose
        // TextEdit is going takes its text document with it, which is left
        // alone: clearing its formatting then would reach into the TextEdit.
        if (document) setDocument(document);
        if (document) {
            m_documentConnections = {
                // After an edit, once Qt has laid it out: the underlines in
                // view move with the text at once.
                connect(document, &QTextDocument::contentsChanged, this,
                        [this] {
                            if (m_formatting) scheduleWaves();
                            else if (m_waves->rowCount() > 0) refreshWaves();
                        }),
                connect(document->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
                        [this] {
                            m_visibleKnown = false;
                            scheduleWaves();
                        }),
            };
        }
    }
    m_waves->update({});
    if (m_binding && m_binding->textDocument()) projectionLoaded();
}

bool EditorDecorations::ready() const { return m_loaded && m_binding && m_document && m_binding->textDocument(); }

SourceRange EditorDecorations::visible()
{
    if (!m_visibleKnown) {
        m_visible = m_binding->visibleRange();
        m_visibleKnown = true;
    }
    return m_visible;
}

int EditorDecorations::sourceLength() const
{
    return m_binding && m_binding->document() ? m_binding->document()->buffer()->length() : 0;
}

void EditorDecorations::projectionLoaded()
{
    if (!m_binding || !m_binding->document()) return;
    const Revision now = m_binding->document()->buffer()->revision();
    const bool fresh = !m_loaded || now != m_revision || m_shown != m_binding->document();
    m_loaded = true;
    m_shown = m_binding->document();
    m_visibleKnown = false;
    if (fresh) {
        for (Layer &layer : m_layers) layer.spans.clear();
        m_edits.clear();
        m_dirty.clear();
        m_revision = now;
        if (m_focusing) markDirty(m_focus.start, m_focus.end);
        emit layersReset();
    } else {
        // The same text again: every paragraph with formatting is formatted anew.
        for (const Layer &layer : m_layers)
            for (const DecorationSpan &span : layer.spans) markDirty(span.start, span.end);
        if (m_focusing) markDirty(m_focus.start, m_focus.end);
    }
    schedule();
    refreshWaves();
}

void EditorDecorations::sourceEdited(Revision revision, SourceRange range, int inserted)
{
    if (!m_loaded) return;
    const EditMap map{range.start, range.start + range.length, range.start + inserted, inserted - range.length};
    // Text typed in, replacing nothing, which a growing range at either side takes.
    const bool typedIn = range.length == 0 && inserted > 0;
    for (Layer &layer : m_layers) {
        auto &spans = layer.spans;
        // Spans that end before the edit are untouched; those that start
        // after it only move, which is all a long document's many spans do.
        // Only those between are changed one by one, with those that start
        // where text is typed in when they may grow to take it.
        const auto first = spans.begin() + (firstEndingAfter(spans, range.start - 1) - spans.cbegin());
        const bool dropping = layer.options.dropEdited;
        const bool growing = typedIn && !dropping && !layer.options.growAtEdges.isEmpty();
        const auto after = std::partition_point(first, spans.end(), [&](const DecorationSpan &span) {
            return dropping || growing ? span.start <= map.oldEnd : span.start < map.oldEnd;
        });
        for (auto span = after; span != spans.end(); ++span) {
            span->start += map.delta;
            span->end += map.delta;
        }
        auto out = first;
        for (auto in = first; in != after; ++in) {
            DecorationSpan span = *in;
            if (dropping && span.start <= map.oldEnd && span.end >= map.start) continue;
            const int start = span.start, end = span.end;
            span.start = map.startOf(start);
            span.end = map.endOf(end);
            if (growing && layer.grows(span.style)) {
                const auto next = in + 1;
                if (start == range.start) span.start = start;
                else if (end == range.start && !(next != after && next->start == range.start && layer.grows(next->style)))
                    span.end = map.newEnd;
            }
            if (span.start < span.end) *out++ = span;
        }
        spans.erase(out, after);
    }
    // What waits to be formatted moves too, and stays in order; what the
    // edit made meet is joined.
    size_t kept = 0;
    for (Range waiting : m_dirty) {
        waiting.start = map.startOf(waiting.start);
        waiting.end = qMax(waiting.start, map.endOf(waiting.end));
        if (kept > 0 && waiting.start <= m_dirty[kept - 1].end) m_dirty[kept - 1].end = qMax(m_dirty[kept - 1].end, waiting.end);
        else m_dirty[kept++] = waiting;
    }
    m_dirty.resize(kept);
    if (m_focusing) {
        m_focus.start = map.inclusiveStartOf(m_focus.start);
        m_focus.end = qMax(m_focus.start, map.inclusiveEndOf(m_focus.end));
    }
    m_visibleKnown = false;
    m_edits.push_back({revision, range.start, range.length, inserted});
    if (m_edits.size() > rememberedEdits) m_edits.pop_front();
    m_revision = revision;
}

EditorDecorations::Layer *EditorDecorations::layer(const QString &name)
{
    for (Layer &layer : m_layers)
        if (layer.name == name) return &layer;
    return nullptr;
}

const EditorDecorations::Layer *EditorDecorations::layer(const QString &name) const
{
    for (const Layer &layer : m_layers)
        if (layer.name == name) return &layer;
    return nullptr;
}

bool EditorDecorations::hasLayer(const QString &name) const { return layer(name) != nullptr; }

void EditorDecorations::defineLayer(const QString &name, const QList<QTextCharFormat> &styles, LayerOptions options)
{
    Layer made;
    made.name = name;
    made.options = options;
    for (QTextCharFormat style : styles) {
        style = withoutGeometry(style);
        QColor wave;
        if (style.underlineStyle() == QTextCharFormat::WaveUnderline) {
            wave = style.underlineColor().isValid() ? style.underlineColor() : style.foreground().color();
            style.clearProperty(QTextFormat::TextUnderlineStyle);
            style.clearProperty(QTextFormat::TextUnderlineColor);
            style.clearProperty(QTextFormat::FontUnderline);
        }
        made.drawn.append(style);
        made.waves.append(wave);
    }
    if (Layer *existing = layer(name)) {
        made.spans = tidy(std::move(existing->spans), 0, sourceLength(), int(styles.size()));
        m_layers.erase(m_layers.begin() + (existing - m_layers.data()));
    }
    for (const DecorationSpan &span : made.spans) markDirty(span.start, span.end);
    const auto at = std::upper_bound(m_layers.begin(), m_layers.end(), options.z,
                                     [](int z, const Layer &other) { return z < other.options.z; });
    m_layers.insert(at, std::move(made));
    schedule();
    scheduleWaves();
}

bool EditorDecorations::forward(Revision revision, Range &range, int &shift) const
{
    shift = 0;
    if (revision == m_revision) return true;
    if (revision > m_revision) return false;
    // Every edit since the result was found must be remembered.
    if (m_edits.empty() || m_edits.front().revision > revision + 1) return false;
    for (const Edit &edit : m_edits) {
        if (edit.revision <= revision) continue;
        // An edit at either edge touches the range too: it may join a word to it.
        if (edit.start <= range.end && edit.start + edit.removed >= range.start) return false;
        if (edit.start < range.start) {
            const int delta = edit.inserted - edit.removed;
            range.start += delta;
            range.end += delta;
            shift += delta;
        }
    }
    return true;
}

bool EditorDecorations::follow(Revision revision, SourceRange *range) const
{
    Range moved{range->start, range->start + range->length};
    int shift = 0;
    if (!forward(revision, moved, shift)) return false;
    *range = {moved.start, moved.end - moved.start};
    return true;
}

bool EditorDecorations::replaceSpans(const QString &name, Revision revision, SourceRange given,
                                     std::vector<DecorationSpan> spans)
{
    Layer *target = layer(name);
    if (!target || !ready()) return false;
    Range range{given.start, given.start + given.length};
    int shift = 0;
    if (!forward(revision, range, shift)) return false;
    const int length = sourceLength();
    range.start = qBound(0, range.start, length);
    range.end = qBound(range.start, range.end, length);
    for (DecorationSpan &span : spans) {
        span.start += shift;
        span.end += shift;
    }
    std::vector<DecorationSpan> fresh = tidy(std::move(spans), range.start, range.end, int(target->drawn.size()));
    auto &old = target->spans;
    // The spans the range reaches, with the parts of those straddling its
    // edges that lie outside it kept.
    const auto first = old.begin() + (firstEndingAfter(old, range.start) - old.cbegin());
    auto last = first;
    while (last != old.end() && last->start < range.end) ++last;
    std::vector<DecorationSpan> before, after, inside;
    for (auto it = first; it != last; ++it) {
        if (it->start < range.start) before.push_back({it->start, range.start, it->style});
        if (it->end > range.end) after.push_back({range.end, it->end, it->style});
        inside.push_back({qMax(it->start, range.start), qMin(it->end, range.end), it->style});
    }
    if (inside == fresh && before.empty() && after.empty()) return true;
    // Only where the old and the new spans differ is anything formatted again.
    size_t i = 0, j = 0;
    while (i < inside.size() || j < fresh.size()) {
        if (i < inside.size() && j < fresh.size() && inside[i] == fresh[j]) {
            ++i;
            ++j;
        } else if (j >= fresh.size() || (i < inside.size() && inside[i].start <= fresh[j].start)) {
            markDirty(inside[i].start, inside[i].end);
            ++i;
        } else {
            markDirty(fresh[j].start, fresh[j].end);
            ++j;
        }
    }
    std::vector<DecorationSpan> replacement;
    replacement.reserve(before.size() + fresh.size() + after.size());
    replacement.insert(replacement.end(), before.begin(), before.end());
    replacement.insert(replacement.end(), fresh.begin(), fresh.end());
    replacement.insert(replacement.end(), after.begin(), after.end());
    const auto at = old.erase(first, last);
    old.insert(at, replacement.begin(), replacement.end());
    schedule();
    if (target->waves.size() > 0) scheduleWaves();
    return true;
}

bool EditorDecorations::setSpans(const QString &name, Revision revision, std::vector<DecorationSpan> spans)
{
    // All of the source as it was then, which every edit since touches.
    return replaceSpans(name, revision, {0, std::numeric_limits<int>::max() / 2}, std::move(spans));
}

void EditorDecorations::clearLayer(const QString &name)
{
    Layer *target = layer(name);
    if (!target || target->spans.empty()) return;
    for (const DecorationSpan &span : target->spans) markDirty(span.start, span.end);
    target->spans.clear();
    schedule();
    scheduleWaves();
}

std::vector<DecorationSpan> EditorDecorations::spans(const QString &name) const
{
    const Layer *found = layer(name);
    return found ? found->spans : std::vector<DecorationSpan>{};
}

const DecorationSpan *EditorDecorations::spanAt(const QString &name, int position) const
{
    const Layer *found = layer(name);
    if (!found) return nullptr;
    const auto it = firstEndingAfter(found->spans, position - 1);
    if (it == found->spans.end() || it->start > position) return nullptr;
    return &*it;
}

void EditorDecorations::setFocusColors(const QColor &dim, const QColor &normal)
{
    if (dim == m_dim && normal == m_normal) return;
    m_dim = dim;
    m_normal = normal;
    if (!m_focusing) return;
    markDirty(m_focus.start, m_focus.end);
    for (const Layer &layer : m_layers)
        for (const DecorationSpan &span : layer.spans) markDirty(span.start, span.end);
    schedule();
}

void EditorDecorations::setFocus(SourceRange range)
{
    const Range focus{range.start, range.start + qMax(0, range.length)};
    if (m_focusing && focus.start == m_focus.start && focus.end == m_focus.end) return;
    if (m_focusing) {
        markDirty(m_focus.start, m_focus.end);
    } else {
        // Dimming starts: whatever has a colour of its own is dimmed too.
        for (const Layer &layer : m_layers)
            if (std::any_of(layer.drawn.cbegin(), layer.drawn.cend(),
                            [](const QTextCharFormat &style) { return style.hasProperty(QTextFormat::ForegroundBrush); }))
                for (const DecorationSpan &span : layer.spans) markDirty(span.start, span.end);
    }
    const bool started = !m_focusing;
    m_focusing = true;
    m_focus = focus;
    markDirty(focus.start, focus.end);
    schedule();
    if (started) emit focusChanged();
}

void EditorDecorations::clearFocus()
{
    if (!m_focusing) return;
    markDirty(m_focus.start, m_focus.end);
    for (const Layer &layer : m_layers)
        if (std::any_of(layer.drawn.cbegin(), layer.drawn.cend(),
                        [](const QTextCharFormat &style) { return style.hasProperty(QTextFormat::ForegroundBrush); }))
            for (const DecorationSpan &span : layer.spans) markDirty(span.start, span.end);
    m_focusing = false;
    schedule();
    emit focusChanged();
}

void EditorDecorations::markDirty(int start, int end)
{
    Range range{start, qMax(start, end)};
    emit formatsChanged(range.start, range.end);
    // In order and apart: the new range swallows those it meets.
    auto it = std::partition_point(m_dirty.begin(), m_dirty.end(),
                                   [&](const Range &waiting) { return waiting.end < range.start; });
    auto last = it;
    while (last != m_dirty.end() && last->start <= range.end) {
        range.start = qMin(range.start, last->start);
        range.end = qMax(range.end, last->end);
        ++last;
    }
    it = m_dirty.erase(it, last);
    m_dirty.insert(it, range);
}

void EditorDecorations::reshown(int start, int end)
{
    markDirty(start, end);
    schedule();
}

void EditorDecorations::schedule()
{
    if (m_dirty.empty() || !ready() || m_formatting) return;
    formatVisible();
    if (!m_dirty.empty() && !m_slices.isActive()) m_slices.start();
}

void EditorDecorations::formatVisible()
{
    const SourceRange shown = visible();
    const Range view{shown.start, shown.start + shown.length};
    std::vector<Range> now, later;
    for (const Range &range : std::as_const(m_dirty)) {
        if (range.end < view.start || range.start > view.end) {
            later.push_back(range);
            continue;
        }
        const Range inside{qMax(range.start, view.start), qMin(range.end, view.end)};
        if (range.start < inside.start) later.push_back({range.start, inside.start});
        now.push_back(inside);
        if (inside.end < range.end) later.push_back({inside.end, range.end});
    }
    if (now.empty()) return;
    m_dirty = std::move(later);
    formatRanges(now, nullptr);
    scheduleWaves();
}

void EditorDecorations::formatSlice()
{
    if (!ready()) {
        m_slices.stop();
        return;
    }
    QElapsedTimer budget;
    budget.start();
    // What scrolled into view meanwhile goes first.
    formatVisible();
    formatRanges(m_dirty, &budget);
    if (m_dirty.empty()) m_slices.stop();
    scheduleWaves();
}

void EditorDecorations::flush()
{
    if (!ready()) return;
    formatVisible();
    formatRanges(m_dirty, nullptr);
    m_slices.stop();
    refreshWaves();
}

void EditorDecorations::formatRanges(std::vector<Range> &ranges, const QElapsedTimer *budget)
{
    // Where the paragraphs formatted last end: many ranges in one paragraph
    // format it once.
    int formattedTo = -1;
    size_t done = 0;
    for (; done < ranges.size(); ++done) {
        Range range = ranges[done];
        if (range.start < formattedTo) {
            if (range.end <= formattedTo) continue;
            range.start = formattedTo;
        }
        int reached = range.end;
        if (!format(range, budget, &reached)) {
            ranges[done].start = reached;
            break;
        }
        formattedTo = reached;
        if (budget && budget->elapsed() >= SliceMs) {
            ++done;
            break;
        }
    }
    ranges.erase(ranges.begin(), ranges.begin() + qsizetype(done));
}

bool EditorDecorations::format(Range range, const QElapsedTimer *budget, int *reached)
{
    const int length = sourceLength();
    const int start = m_binding->sourceToEditor(qBound(0, range.start, length));
    const int end = m_binding->sourceToEditor(qBound(0, range.end, length));
    if (start < 0 || end < 0) return true;
    const bool formatting = std::exchange(m_formatting, true);
    QTextBlock block = m_document->findBlock(start);
    bool whole = true;
    // Every paragraph the range reaches into, and the one an empty range is
    // in. A hidden one, a table's row that its grid draws, waits until it
    // is shown.
    while (block.isValid() && (block.position() < end || block.position() <= start)) {
        if (block.isVisible()) rehighlightBlock(block);
        block = block.next();
        if (budget && budget->elapsed() >= SliceMs && block.isValid() && block.position() < end) {
            whole = false;
            break;
        }
    }
    // The start of the next paragraph, up to which all are formatted.
    *reached = block.isValid() ? m_binding->editorToSource(block.position()) : length;
    m_formatting = formatting;
    return whole;
}

void EditorDecorations::viewMoved()
{
    m_visibleKnown = false;
    if (!ready() || m_formatting) return;
    if (!m_dirty.empty()) schedule();
    if (!wavy()) return;
    const SourceRange shown = visible();
    if (shown.start < m_waveWindow.start || shown.start + shown.length > m_waveWindow.end) scheduleWaves();
}

bool EditorDecorations::wavy() const
{
    return std::any_of(m_layers.cbegin(), m_layers.cend(), [](const Layer &layer) {
        return !layer.spans.empty()
            && std::any_of(layer.waves.cbegin(), layer.waves.cend(), [](const QColor &wave) { return wave.isValid(); });
    });
}

void EditorDecorations::scheduleWaves() { m_waveTimer.start(); }

void EditorDecorations::refreshWaves()
{
    m_waveTimer.stop();
    std::vector<DecorationMarks::Mark> marks;
    if (ready() && !m_formatting && wavy()) {
        // The view and as much again above and below it, so that a scroll
        // finds its underlines there already: as high as the view, however
        // much or little text lies there.
        const SourceRange near = m_binding->visibleRange(1);
        m_waveWindow = {near.start, near.start + near.length};
        const QFont font = m_binding->editor()->property("font").value<QFont>();
        const QFontMetricsF metrics(font);
        const qreal pixels = font.pixelSize() > 0 ? font.pixelSize() : metrics.height();
        const qreal height = qMax<qreal>(2, std::round(pixels / 6));
        const qreal below = qMax<qreal>(1, std::round(metrics.underlinePos() - height / 2));
        QAbstractTextDocumentLayout *documentLayout = m_document->documentLayout();
        for (const Layer &layer : m_layers) {
            if (std::none_of(layer.waves.cbegin(), layer.waves.cend(), [](const QColor &wave) { return wave.isValid(); }))
                continue;
            for (auto span = firstEndingAfter(layer.spans, m_waveWindow.start);
                 span != layer.spans.cend() && span->start < m_waveWindow.end; ++span) {
                const QColor color = layer.waves.value(span->style);
                if (!color.isValid()) continue;
                const int start = m_binding->sourceToEditor(span->start);
                const int end = m_binding->sourceToEditor(span->end);
                if (start < 0 || end < 0) continue;
                // One mark for each line the span covers, from the line
                // it starts on, however long its paragraph is.
                for (QTextBlock block = m_document->findBlock(start); block.isValid() && block.position() < end;
                     block = block.next()) {
                    QTextLayout *layout = block.layout();
                    if (!layout || layout->lineCount() == 0) continue;
                    const QPointF origin = documentLayout->blockBoundingRect(block).topLeft();
                    const QTextLine first = layout->lineForTextPosition(qMax(0, start - block.position()));
                    for (int i = first.isValid() ? first.lineNumber() : 0; i < layout->lineCount(); ++i) {
                        const QTextLine line = layout->lineAt(i);
                        const int lineStart = block.position() + line.textStart();
                        if (lineStart >= end) break;
                        const int from = qMax(start, lineStart);
                        const int to = qMin(end, lineStart + line.textLength());
                        if (from >= to) continue;
                        const qreal left = line.cursorToX(from - block.position());
                        const qreal right = line.cursorToX(to - block.position());
                        marks.push_back({origin.x() + qMin(left, right),
                                         origin.y() + line.y() + line.ascent() + below, qAbs(right - left), height,
                                         color});
                    }
                }
            }
        }
    }
    m_waves->update(marks);
}

void EditorDecorations::highlightBlock(const QString &text)
{
    ++m_formatted;
    if (!m_loaded || !m_binding) return;
    // A hidden paragraph, a table's row that its grid draws, is formatted
    // when it is shown again.
    if (!currentBlock().isVisible()) return;
    const int sourceStart = m_binding->editorToSource(currentBlock().position());
    if (sourceStart < 0) return;
    // Inside a paragraph the source and the projection agree unit for unit:
    // only line breaks differ, and they lie between paragraphs.
    for (const QTextLayout::FormatRange &range : formatsFor(sourceStart, int(text.size())))
        setFormat(range.start, range.length, range.format);
}

QList<QTextLayout::FormatRange> EditorDecorations::formatsFor(int sourceStart, int length) const
{
    QList<QTextLayout::FormatRange> formats;
    const int sourceEnd = sourceStart + length;
    const auto local = [&](int position) { return qBound(0, position - sourceStart, length); };

    QVarLengthArray<int, 64> cuts{0, length};
    QVarLengthArray<std::vector<DecorationSpan>::const_iterator, 8> from;
    for (const Layer &layer : m_layers) {
        auto span = firstEndingAfter(layer.spans, sourceStart);
        from.append(span);
        for (; span != layer.spans.cend() && span->start < sourceEnd; ++span) {
            cuts.append(local(span->start));
            cuts.append(local(span->end));
        }
    }
    if (m_focusing) {
        cuts.append(local(m_focus.start));
        cuts.append(local(m_focus.end));
    }
    if (cuts.size() == 2 && !m_focusing) return formats;
    std::sort(cuts.begin(), cuts.end());
    const auto unique = std::unique(cuts.begin(), cuts.end());
    for (auto cut = cuts.begin(); cut + 1 < unique; ++cut) {
        const int start = *cut, end = *(cut + 1);
        const int at = sourceStart + start;
        QTextCharFormat format;
        for (qsizetype i = 0; i < from.size(); ++i) {
            const Layer &layer = m_layers[size_t(i)];
            auto &span = from[i];
            while (span != layer.spans.cend() && span->end <= at) ++span;
            if (span != layer.spans.cend() && span->start <= at) format.merge(layer.drawn.at(span->style));
        }
        if (m_focusing) {
            const bool inside = at >= m_focus.start && at < m_focus.end;
            const bool coloured = format.hasProperty(QTextFormat::ForegroundBrush);
            if (inside && !coloured && m_normal.isValid()) format.setForeground(m_normal);
            else if (!inside && coloured && m_dim.isValid()) format.setForeground(m_dim);
        }
        if (format.propertyCount() > 0) formats.append({start, end - start, format});
    }
    return formats;
}

QList<EditorDecorations::Wave> EditorDecorations::wavesFor(int sourceStart, int length) const
{
    QList<Wave> waves;
    const int sourceEnd = sourceStart + length;
    for (const Layer &layer : m_layers) {
        if (std::none_of(layer.waves.cbegin(), layer.waves.cend(), [](const QColor &wave) { return wave.isValid(); }))
            continue;
        for (auto span = firstEndingAfter(layer.spans, sourceStart); span != layer.spans.cend() && span->start < sourceEnd;
             ++span) {
            const QColor color = layer.waves.value(span->style);
            if (!color.isValid()) continue;
            waves.append({qMax(span->start, sourceStart), qMin(span->end, sourceEnd), color});
        }
    }
    return waves;
}
