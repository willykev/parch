#include "markdownstyling.h"

#include "document.h"
#include "sourcebuffer.h"
#include "theme.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHashFunctions>
#include <QTextDocument>
#include <QTextLayout>
#include <QThread>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {

using md::Node;
using md::NodeType;
using md::Range;

MarkdownStyle withColor(MarkdownStyle style, MarkdownStyle::Color color)
{
    style.color = color;
    return style;
}

// What lies between a node's opening and closing punctuation.
Range contentOf(const Node &node)
{
    const int start = node.open.isEmpty() ? node.range.start : node.open.end;
    const int end = node.close.isEmpty() ? node.range.end : node.close.start;
    return {start, qMax(start, end)};
}

// Paints the style of every unit of the text, a node over the nodes that
// hold it, and reads the runs off at the end. A node paints only what it
// changes: its markers, and its content where its style differs from the
// style it is in.
class Painter {
public:
    explicit Painter(const md::Document &document)
        : m_styles(document.files.empty() ? 0 : size_t(document.files.front().text.size()), quint8(0))
    {
    }

    void walk(const Node &node, MarkdownStyle base)
    {
        // Generated nodes have no source of their own, and the nodes of an
        // included file are not in this text.
        if (node.generated || node.file != 0) return;
        const MarkdownStyle marker = withColor(base, MarkdownStyle::Marker);
        MarkdownStyle inner = base;
        switch (node.type) {
        case NodeType::FrontMatter:
            paint(node.range, withColor(base, MarkdownStyle::Faded));
            paintMarkers(node, marker);
            return;
        case NodeType::Heading:
            inner.bold = true;
            paint(contentOf(node), inner);
            paintMarkers(node, marker);
            break;
        case NodeType::ThematicBreak:
        case NodeType::PageBreak:
        case NodeType::HtmlInline:
        case NodeType::HeadingLabel:
            paint(node.range, marker);
            return;
        case NodeType::TableOfContents:
            // Its list is generated, and has no source to style.
            paint(node.range, marker);
            return;
        case NodeType::BlockQuote:
        case NodeType::TableRow:
            for (const Range &mark : node.markers) paint(mark, marker);
            break;
        case NodeType::ListItem:
        case NodeType::Table:
        case NodeType::DefinitionDescription:
        case NodeType::FootnoteDefinition:
        case NodeType::CitationDefinition:
        case NodeType::Escape:
        case NodeType::Superscript:
        case NodeType::Subscript:
        case NodeType::InlineNote:
        case NodeType::HtmlElement:
            paintMarkers(node, marker);
            break;
        case NodeType::CodeBlock:
        case NodeType::MermaidBlock:
            paint(node.range, withColor(base, MarkdownStyle::Code));
            paintMarkers(node, marker);
            paint(node.target, marker);
            return;
        case NodeType::MathBlock:
        case NodeType::MathInline:
            paint(contentOf(node), withColor(base, MarkdownStyle::Math));
            paintMarkers(node, marker);
            return;
        case NodeType::HtmlBlock:
            // A raw block whose lines are not one run of source has only
            // generated nodes: it is dimmed whole.
            if (std::any_of(node.children.cbegin(), node.children.cend(), [](const Node &child) { return child.generated; })) {
                paint(node.range, withColor(base, MarkdownStyle::Faded));
                return;
            }
            break;
        case NodeType::LinkDefinition:
            paint(node.range, withColor(base, MarkdownStyle::Faded));
            paintMarkers(node, marker);
            return;
        case NodeType::Include:
            // The path as a link, the caption dimmed; what it includes is another file's.
            paint(node.range, withColor(base, MarkdownStyle::Faded));
            paint(node.target, withColor(base, MarkdownStyle::Link));
            return;
        case NodeType::CommentLine:
        case NodeType::HtmlComment:
            paint(node.range, withColor(base, MarkdownStyle::Faded));
            return;
        case NodeType::Emphasis:
            inner.italic = true;
            paint(contentOf(node), inner);
            paintMarkers(node, marker);
            break;
        case NodeType::Strong:
            inner.bold = true;
            paint(contentOf(node), inner);
            paintMarkers(node, marker);
            break;
        case NodeType::Strikethrough:
            inner.strike = true;
            paint(contentOf(node), inner);
            paintMarkers(node, marker);
            break;
        case NodeType::Highlight:
            inner.fill = MarkdownStyle::MarkFill;
            paint(contentOf(node), inner);
            paintMarkers(node, marker);
            break;
        case NodeType::Code: {
            // The backticks share the code's fill, so that the span reads as one.
            MarkdownStyle code = withColor(base, MarkdownStyle::Code);
            code.fill = MarkdownStyle::CodeFill;
            paint(contentOf(node), code);
            paintMarkers(node, withColor(code, MarkdownStyle::Marker));
            return;
        }
        case NodeType::Link:
        case NodeType::Image:
        case NodeType::Autolink:
        case NodeType::FootnoteReference:
        case NodeType::Citation:
        case NodeType::Variable:
            inner.color = MarkdownStyle::Link;
            paint(contentOf(node), inner);
            paintMarkers(node, marker);
            break;
        case NodeType::TaskMarker:
            paint(node.range, marker);
            // The tick of a done task in the accent, between its dimmed brackets.
            if (node.checked && node.range.length() > 2)
                paint({node.range.start + 1, node.range.end - 1}, withColor(base, MarkdownStyle::Link));
            return;
        default:
            break;
        }
        for (const Node &child : node.children) walk(child, inner);
    }

    std::vector<DecorationSpan> runs() const
    {
        std::vector<DecorationSpan> runs;
        const int length = int(m_styles.size());
        for (int start = 0; start < length;) {
            const quint8 style = m_styles[size_t(start)];
            int end = start + 1;
            while (end < length && m_styles[size_t(end)] == style) ++end;
            if (style != 0) runs.push_back({start, end, style});
            start = end;
        }
        return runs;
    }

private:
    void paint(Range range, const MarkdownStyle &style)
    {
        const int length = int(m_styles.size());
        const int start = qBound(0, range.start, length), end = qBound(start, range.end, length);
        std::fill(m_styles.begin() + start, m_styles.begin() + end, quint8(style.index()));
    }

    void paintMarkers(const Node &node, const MarkdownStyle &marker)
    {
        paint(node.open, marker);
        paint(node.close, marker);
    }

    std::vector<quint8> m_styles;
};

int lineStart(const QString &text, int position)
{
    position = qBound(0, position, int(text.size()));
    while (position > 0 && text[position - 1] != u'\n' && text[position - 1] != u'\r') --position;
    return position;
}

void collectParts(const QString &text, const Node &node, std::vector<int> &starts)
{
    for (const Node &child : node.children) {
        if (!child.isBlock() || child.generated || child.file != 0) continue;
        starts.push_back(lineStart(text, child.range.start));
        collectParts(text, child, starts);
    }
}

} // namespace

MarkdownStyle MarkdownStyle::at(int index)
{
    MarkdownStyle style;
    if (index <= 0 || index >= Count) return style;
    style.bold = index & 1;
    style.italic = index & 2;
    style.strike = index & 4;
    style.color = Color(index / 8 % 6);
    style.fill = Fill(index / 48);
    return style;
}

std::vector<DecorationSpan> markdownStyleRuns(const md::Document &document)
{
    if (document.files.empty()) return {};
    Painter painter(document);
    painter.walk(document.root, {});
    return painter.runs();
}

std::vector<int> markdownStyleParts(const md::Document &document)
{
    std::vector<int> starts{0};
    if (!document.files.empty()) collectParts(document.files.front().text, document.root, starts);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    return starts;
}

bool keepsAdvances(const QFont &font, bool designMetrics, const QTextCharFormat &format)
{
    // The characters a writer's monospace face has itself: ASCII, Latin,
    // Greek and Cyrillic letters, and typographic punctuation. Characters
    // drawn from fallback faces are not the editor's font to judge.
    static const QString sample = [] {
        QString text;
        for (char16_t c = 0x21; c < 0x7f; ++c) text += QChar(c);
        return text + QStringLiteral(" àéîõüçñßæøåœ ÀÉÎÕÜÇÑ ‘’“”–—…•· αβγδλω ΑΩ абвгджя АЯ");
    }();
    const auto positions = [&](const QList<QTextLayout::FormatRange> &formats) {
        QTextLayout layout(sample, font);
        QTextOption option;
        option.setUseDesignMetrics(designMetrics);
        option.setWrapMode(QTextOption::NoWrap);
        layout.setTextOption(option);
        layout.setFormats(formats);
        layout.beginLayout();
        const QTextLine line = layout.createLine();
        layout.endLayout();
        std::vector<qreal> found;
        found.reserve(size_t(sample.size()) + 1);
        for (int i = 0; i <= sample.size(); ++i) found.push_back(line.cursorToX(i));
        return found;
    };
    const std::vector<qreal> plain = positions({});
    const std::vector<qreal> styled = positions({{0, int(sample.size()), format}});
    for (size_t i = 0; i < plain.size(); ++i)
        if (std::abs(plain[i] - styled[i]) > 0.01) return false;
    return true;
}

namespace {

// Passes run here, off the view's thread, a few at a time for every view.
QThreadPool *stylingPool()
{
    static QPointer<QThreadPool> pool;
    if (!pool) {
        pool = new QThreadPool(QCoreApplication::instance());
        pool->setObjectName(QStringLiteral("styling"));
        pool->setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 2, 2));
    }
    return pool;
}

using Part = MarkdownStyling::Part;

// The text in parts, each with the runs in it and a key that says what it
// is: its length and its runs, from its own start.
std::vector<Part> partsOf(const md::Document &document, int length)
{
    const std::vector<DecorationSpan> runs = markdownStyleRuns(document);
    const std::vector<int> starts = markdownStyleParts(document);
    std::vector<Part> parts;
    parts.reserve(starts.size());
    size_t next = 0;
    for (size_t i = 0; i < starts.size(); ++i) {
        const int start = starts[i];
        const int end = i + 1 < starts.size() ? starts[i + 1] : length;
        if (end <= start) continue;
        Part part{{start, end - start}, {}, qHash(end - start)};
        // A run that crosses into the next part is split between them.
        for (size_t k = next; k < runs.size() && runs[k].start < end; ++k) {
            const DecorationSpan run{qMax(runs[k].start, start), qMin(runs[k].end, end), runs[k].style};
            part.runs.push_back(run);
            part.key = qHashMulti(part.key, run.start - start, run.end - start, run.style);
        }
        while (next < runs.size() && runs[next].end <= end) ++next;
        parts.push_back(std::move(part));
    }
    return parts;
}

} // namespace

struct MarkdownStyling::Pass {
    quint64 number = 0;
    Revision revision = 0;
    // A copy of the text of its own, so that no later edit has to copy the buffer's.
    QString text;
    SourceRange visible{0, 0};
    std::vector<Applied> applied;
    std::shared_ptr<std::atomic_bool> cancelled;
    QPointer<MarkdownStyling> owner;
};

// A pass, on the worker: parses the text, styles it, and hands the parts
// over that the view does not have, those in view first.
void MarkdownStyling::run(Pass &pass)
{
    if (pass.cancelled->load()) return;
    const md::Document document = md::parse(pass.text);
    if (pass.cancelled->load()) return;
    std::vector<Part> parts = partsOf(document, int(pass.text.size()));

    // A part the view has as it is, which no edit has touched since, stays.
    const auto unchanged = [&](const Part &part) {
        const auto it = std::partition_point(pass.applied.cbegin(), pass.applied.cend(),
                                             [&](const Applied &applied) { return applied.start < part.range.start; });
        return it != pass.applied.cend() && it->start == part.range.start
            && it->end == part.range.start + part.range.length && it->key == part.key;
    };
    const int top = pass.visible.start, bottom = pass.visible.start + pass.visible.length;
    const auto shown = std::stable_partition(parts.begin(), parts.end(), [&](const Part &part) {
        return part.range.start <= bottom && part.range.start + part.range.length >= top;
    });
    const size_t inView = size_t(shown - parts.begin());

    std::vector<Part> batch;
    int kept = 0;
    QElapsedTimer sinceSent;
    sinceSent.start();
    const auto send = [&](bool visible, bool last) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [owner = pass.owner, number = pass.number, revision = pass.revision, found = std::move(batch), visible, last,
             kept]() mutable {
                if (owner) owner->take(number, revision, std::move(found), visible, last, kept);
            },
            Qt::QueuedConnection);
        batch = {};
        kept = 0;
        sinceSent.restart();
    };
    if (parts.empty()) send(true, true);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (pass.cancelled->load()) return;
        if (unchanged(parts[i])) ++kept;
        else batch.push_back(std::move(parts[i]));
        // What is in view goes at once; the rest a few dozen parts at a time.
        const bool last = i + 1 == parts.size();
        if (i + 1 == inView) send(true, last);
        else if (last || (i >= inView && (batch.size() >= 64 || sinceSent.elapsed() >= 16))) send(false, last);
    }
}

MarkdownStyling::MarkdownStyling(QObject *parent) : QObject(parent)
{
    m_settle.setSingleShot(true);
    connect(&m_settle, &QTimer::timeout, this, &MarkdownStyling::start);
    // Colours and the font arrive one by one; the formats are made once.
    m_reformat.setSingleShot(true);
    m_reformat.setInterval(0);
    connect(&m_reformat, &QTimer::timeout, this, &MarkdownStyling::defineLayer);
}

MarkdownStyling::~MarkdownStyling()
{
    if (m_cancelled) m_cancelled->store(true);
}

void MarkdownStyling::setDecorations(EditorDecorations *decorations)
{
    if (m_decorations == decorations) return;
    if (m_decorations) m_decorations->clearLayer(layerName());
    for (const QMetaObject::Connection &connection : std::as_const(m_decorationConnections)) disconnect(connection);
    m_decorationConnections.clear();
    m_decorations = decorations;
    if (decorations) {
        m_decorationConnections = {
            connect(decorations, &EditorDecorations::bindingChanged, this, &MarkdownStyling::connectBinding),
            // The view shows its text anew, with every layer empty: all of
            // it is styled at once.
            connect(decorations, &EditorDecorations::layersReset, this,
                    [this] {
                        m_applied.clear();
                        measureFont();
                        cancel();
                        schedule(0);
                    }),
        };
    }
    connectBinding();
    if (decorations) defineLayer();
    emit decorationsChanged();
}

void MarkdownStyling::setEnabled(bool enabled)
{
    if (enabled == m_enabled) return;
    m_enabled = enabled;
    refresh();
    emit enabledChanged();
}

void MarkdownStyling::setFont(const QFont &font)
{
    if (font == m_font) return;
    m_font = font;
    measureFont();
    emit fontChanged();
}

void MarkdownStyling::setColor(QColor &member, const QColor &color)
{
    if (member == color) return;
    member = color;
    if (m_decorations) m_reformat.start();
    emit colorsChanged();
}

void MarkdownStyling::connectBinding()
{
    for (const QMetaObject::Connection &connection : std::as_const(m_bindingConnections)) disconnect(connection);
    m_bindingConnections.clear();
    m_binding = m_decorations ? m_decorations->binding() : nullptr;
    if (m_binding)
        m_bindingConnections = {connect(m_binding, &EditorBinding::sourceEdited, this, &MarkdownStyling::edited)};
    m_applied.clear();
    measureFont();
    refresh();
}

bool MarkdownStyling::active() const
{
    return m_enabled && m_decorations && m_binding && m_binding->document() && m_binding->textDocument();
}

void MarkdownStyling::refresh()
{
    if (active()) {
        schedule(0);
        return;
    }
    cancel();
    m_applied.clear();
    if (m_decorations) m_decorations->clearLayer(layerName());
}

void MarkdownStyling::measureFont()
{
    // As the view's text document lays its text out: its font, and its metrics.
    QFont font = m_font;
    bool designMetrics = false;
    if (m_binding && m_binding->textDocument()) {
        font = m_binding->textDocument()->defaultFont();
        designMetrics = m_binding->textDocument()->useDesignMetrics();
    }
    QTextCharFormat bold, italic, both;
    bold.setFontWeight(QFont::Bold);
    italic.setFontItalic(true);
    both.setFontWeight(QFont::Bold);
    both.setFontItalic(true);
    const bool boldShown = keepsAdvances(font, designMetrics, bold);
    const bool italicShown =
        keepsAdvances(font, designMetrics, italic) && (!boldShown || keepsAdvances(font, designMetrics, both));
    if (boldShown == m_boldShown && italicShown == m_italicShown) return;
    m_boldShown = boldShown;
    m_italicShown = italicShown;
    if (m_decorations) m_reformat.start();
    emit formatsChanged();
}

QColor MarkdownStyling::readable(const QColor &color, const QColor &foreground, const QList<QColor> &backgrounds)
{
    if (!color.isValid() || !foreground.isValid()) return color;
    const auto contrast = [](const QColor &a, const QColor &b) {
        const double one = Theme::luminance(a), two = Theme::luminance(b);
        return (std::max(one, two) + .05) / (std::min(one, two) + .05);
    };
    const auto reads = [&](const QColor &candidate) {
        return std::all_of(backgrounds.cbegin(), backgrounds.cend(), [&](const QColor &background) {
            return !background.isValid() || contrast(candidate, background) >= std::min(4.5, contrast(foreground, background));
        });
    };
    for (int step = 0; step < 10; ++step) {
        const QColor candidate = Theme::mix(color, foreground, step / 10.0);
        if (reads(candidate)) return step == 0 ? color : candidate;
    }
    return foreground;
}

QTextCharFormat MarkdownStyling::format(const MarkdownStyle &style) const
{
    QTextCharFormat format;
    if (style.bold && m_boldShown) format.setFontWeight(QFont::Bold);
    if (style.italic && m_italicShown) format.setFontItalic(true);
    if (style.strike) format.setFontStrikeOut(true);
    QColor color;
    switch (style.color) {
    case MarkdownStyle::Marker:
    case MarkdownStyle::Faded:
        // Dimmed on purpose.
        color = m_marker;
        break;
    case MarkdownStyle::Link:
        color = readable(m_link, m_foreground, {m_background});
        break;
    case MarkdownStyle::Code:
        color = readable(m_code, m_foreground, {m_background, m_codeFill});
        break;
    case MarkdownStyle::Math:
        color = readable(m_math, m_foreground, {m_background});
        break;
    case MarkdownStyle::Plain:
        break;
    }
    // A weight or slant the font cannot draw at its own width is a colour:
    // strong text and headings in the link colour, emphasis halfway to it.
    if (!color.isValid() && style.bold && !m_boldShown) color = readable(m_link, m_foreground, {m_background});
    else if (!color.isValid() && style.italic && !m_italicShown && m_foreground.isValid() && m_link.isValid())
        color = readable(Theme::mix(m_foreground, m_link, .5), m_foreground, {m_background});
    if (color.isValid()) format.setForeground(color);
    if (style.fill == MarkdownStyle::CodeFill && m_codeFill.isValid()) format.setBackground(m_codeFill);
    // The highlighter laid thinly enough over the background that the text stays readable.
    if (style.fill == MarkdownStyle::MarkFill && m_mark.isValid() && m_background.isValid())
        format.setBackground(Theme::mix(m_background, m_mark, Theme::luminance(m_background) < .179 ? .3 : .45));
    return format;
}

void MarkdownStyling::defineLayer()
{
    m_reformat.stop();
    if (!m_decorations) return;
    QList<QTextCharFormat> styles;
    QList<bool> grows;
    styles.reserve(MarkdownStyle::Count);
    grows.reserve(MarkdownStyle::Count);
    for (int i = 0; i < MarkdownStyle::Count; ++i) {
        const MarkdownStyle style = MarkdownStyle::at(i);
        styles.append(format(style));
        grows.append(style.growsAtEdges());
    }
    m_decorations->defineLayer(layerName(), styles, {.growAtEdges = grows});
}

void MarkdownStyling::schedule(int delayMs)
{
    if (!active()) return;
    m_settle.start(delayMs);
    setRunning(m_running);
}

void MarkdownStyling::setRunning(bool running)
{
    m_running = running;
    if (busy() == m_wasBusy) return;
    m_wasBusy = busy();
    emit busyChanged();
}

void MarkdownStyling::cancel()
{
    if (m_cancelled) m_cancelled->store(true);
    m_cancelled.reset();
    m_settle.stop();
    m_stale = false;
    setRunning(false);
}

void MarkdownStyling::edited(Revision, SourceRange range, int inserted)
{
    // The parts an edit touches, at their edges too, are no longer what the
    // view was given; those after it move.
    const int oldEnd = range.start + range.length, delta = inserted - range.length;
    const auto first = std::partition_point(m_applied.begin(), m_applied.end(),
                                            [&](const Applied &applied) { return applied.end < range.start; });
    auto last = first;
    while (last != m_applied.end() && last->start <= oldEnd) ++last;
    for (auto it = m_applied.erase(first, last); it != m_applied.end(); ++it) {
        it->start += delta;
        it->end += delta;
    }
    if (active()) schedule(SettleMs);
}

void MarkdownStyling::start()
{
    m_settle.stop();
    if (!active()) {
        setRunning(m_running);
        return;
    }
    if (m_reformat.isActive()) defineLayer();
    // A pass still running reads text that has changed since.
    if (m_cancelled) m_cancelled->store(true);
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    m_stale = false;
    SourceBuffer *buffer = m_binding->document()->buffer();
    auto pass = std::make_shared<Pass>();
    pass->number = ++m_pass;
    pass->revision = buffer->revision();
    {
        const QString shared = buffer->text();
        pass->text = QString(shared.constData(), shared.size());
    }
    pass->visible = m_binding->visibleRange();
    pass->applied = m_applied;
    pass->cancelled = m_cancelled;
    pass->owner = this;
    ++m_stats.passes;
    setRunning(true);
    stylingPool()->start([pass] { run(*pass); });
}

void MarkdownStyling::take(quint64 pass, Revision revision, std::vector<Part> parts, bool visible, bool last, int kept)
{
    // Found by a pass that a later one has replaced.
    if (pass != m_pass || !m_running || !m_decorations) return;
    QElapsedTimer timer;
    timer.start();
    bool stale = false;
    for (Part &part : parts) {
        SourceRange range = part.range;
        if (!m_decorations->replaceSpans(layerName(), revision, range, std::move(part.runs))) {
            stale = true;
            continue;
        }
        if (m_decorations->follow(revision, &range)) remember(range, part.key);
    }
    m_stats.partsSent += int(parts.size());
    m_stats.partsKept += kept;
    m_stats.applyNanoseconds.push_back(timer.nsecsElapsed());
    m_stale = m_stale || stale;
    if (visible && !stale && m_binding && m_binding->document() && revision == m_binding->document()->buffer()->revision())
        emit visibleStyled(revision);
    if (!last) return;
    m_cancelled.reset();
    setRunning(false);
    // Parts edited meanwhile are styled again once typing pauses.
    if (std::exchange(m_stale, false)) schedule(SettleMs);
}

void MarkdownStyling::remember(SourceRange range, size_t key)
{
    const Applied applied{range.start, range.start + range.length, key};
    const auto first = std::partition_point(m_applied.begin(), m_applied.end(),
                                            [&](const Applied &other) { return other.end <= applied.start; });
    auto last = first;
    while (last != m_applied.end() && last->start < applied.end) ++last;
    m_applied.insert(m_applied.erase(first, last), applied);
}
