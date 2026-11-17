#include "frametests.h"

#include "backend.h"
#include "document.h"
#include "editorbinding.h"
#include "editordecorations.h"
#include "markdownstyling.h"
#include "preview.h"
#include "scrollsync.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "spellinglayer.h"
#include "tabmodel.h"
#include "testfiles.h"
#include "windowcontext.h"
#include "windowtesting.h"

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QPointF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QtTest>
#include <algorithm>
#include <cmath>

namespace {

// A dictionary that knows only a few short words, so that every other word
// of the texts below is underlined as misspelled.
void writeFewWords(const QString &folder)
{
    writeFile(folder + QStringLiteral("/en_US.aff"), "SET UTF-8\nTRY esianrtolcdugmphbyfvkwz'\n");
    writeFile(folder + QStringLiteral("/en_US.dic"), "3\nthe\nand\na\n");
}

// Paragraphs of words no dictionary knows, eighty words each, `units` long.
QString unknownProse(qsizetype units)
{
    static const QStringList words = QString::fromLatin1(
                                         "zorblax quibnar flemtoss vardige plinkrut moxwell trandish gorpent "
                                         "skelvin daprout")
                                         .split(u' ');
    QString text;
    text.reserve(units + 16);
    for (int i = 0; text.size() < units; ++i) {
        text += words.at(i % words.size());
        text += (i + 1) % 80 == 0 ? QStringLiteral("\n\n") : QStringLiteral(" ");
    }
    text.truncate(units);
    return text;
}

// Constructs inside one another two hundred deep, as the benchmark nests them.
QString nestedText()
{
    constexpr int depth = 200;
    QString text = QStringLiteral("\n\n") + QStringLiteral("> ").repeated(depth) + QStringLiteral("A quote two hundred deep.\n\n");
    for (int i = 0; i < depth; ++i) text += QString(2 * i, u' ') + QStringLiteral("- a list item\n");
    text += u'\n';
    for (int i = 0; i < depth; ++i) text += i % 2 ? u'_' : u'*';
    text += QStringLiteral("nested emphasis");
    for (int i = depth - 1; i >= 0; --i) text += i % 2 ? u'_' : u'*';
    text += QStringLiteral("\n\n") + QString(depth, u'[') + QStringLiteral("brackets") + QString(depth, u']') + QStringLiteral("\n\n");
    return text;
}

// The points of a wavy underline's path.
int pointsOf(QQuickItem *wave)
{
    int points = 0;
    for (QObject *child : wave->findChildren<QObject *>()) {
        if (!QByteArray(child->metaObject()->className()).contains("PathPolyline")) continue;
        const QVariant path = child->property("path");
        if (path.canConvert<QList<QPointF>>()) points += int(path.value<QList<QPointF>>().size());
        else points += int(path.toList().size());
    }
    return points;
}

// What the editor and the preview of one window place over their text.
struct Scene {
    int waveRows = 0;
    int waves = 0;
    int wavePoints = 0;
    int widestWave = 0;
    int markRows = 0;
    int markItems = 0;
    int islands = 0;

    QString said() const
    {
        return QStringLiteral("wavy underlines %1 listed, %2 drawn with %3 points (at most %4 in one); "
                              "preview marks %5 listed, %6 items, %7 islands")
            .arg(waveRows)
            .arg(waves)
            .arg(wavePoints)
            .arg(widestWave)
            .arg(markRows)
            .arg(markItems)
            .arg(islands);
    }
};

struct AdversarialWindow {
    QTemporaryDir dictionaries;
    std::unique_ptr<ShellFixture> shell;
    QQuickWindow *window = nullptr;
    Document *document = nullptr;
    EditorBinding *binding = nullptr;
    EditorDecorations *decorations = nullptr;
    SpellingLayer *spelling = nullptr;
    MarkdownStyling *styling = nullptr;
    Preview *preview = nullptr;
    QQuickItem *editor = nullptr;
    QQuickItem *editorView = nullptr;
    QQuickItem *surface = nullptr;
    QQuickItem *previewView = nullptr;

    explicit AdversarialWindow(const QString &text)
    {
        writeFewWords(dictionaries.path());
        shell = std::make_unique<ShellFixture>(ShellOptions{.dictionaries = dictionaries.path()});
        if (!shell->ok()) return;
        window = shell->window.get();
        activate(window);
        resizeWindow(window, 1800);
        window->setHeight(800);
        document = shell->document();
        document->buffer()->replace({0, 0}, text);
        binding = shell->binding();
        decorations = qobject_cast<EditorDecorations *>(shell->object(QStringLiteral("decorations")));
        spelling = qobject_cast<SpellingLayer *>(shell->object(QStringLiteral("spelling")));
        styling = qobject_cast<MarkdownStyling *>(shell->object(QStringLiteral("styling")));
        preview = qobject_cast<Preview *>(shell->object(QStringLiteral("preview")));
        editor = shell->item(QStringLiteral("editor"));
        editorView = shell->item(QStringLiteral("flickable"));
        surface = shell->item(QStringLiteral("previewSurface"));
        previewView = shell->item(QStringLiteral("previewFlickable"));
    }
    bool ok() const
    {
        return shell && shell->ok() && binding && decorations && spelling && styling && preview && editor && editorView
               && surface && previewView;
    }
    void view(TabModel::ViewMode mode) { shell->context()->tabs()->setActiveMode(mode); }
    bool previewShown() const { return surface->isVisible(); }

    // Every view has caught up with the text, and what it places is placed.
    bool settled(int timeoutMs = 60000)
    {
        return QTest::qWaitFor(
            [&] {
                if (spelling->checking() || styling->busy()) return false;
                if (previewShown() && (!preview->current() || preview->working())) return false;
                decorations->flush();
                QCoreApplication::processEvents();
                return true;
            },
            timeoutMs);
    }

    // At the end of the text, as one edit, as a paste puts it in.
    void putAtEnd(const QString &text)
    {
        const int end = document->buffer()->length();
        binding->select(end, end);
        const int at = editor->property("cursorPosition").toInt();
        binding->replaceEditorRange(at, at, text);
    }

    // How many of the pixels a view shows are drawn in ink rather than
    // the paper's colour, in a picture of the window as it is drawn now.
    int inkIn(QQuickItem *view) const
    {
        QCoreApplication::processEvents();
        const QImage shot = window->grabWindow();
        const QRectF area = view->mapRectToScene(QRectF(0, 0, view->width(), view->height()));
        const QColor paper = shell->theme->background();
        int ink = 0;
        for (int y = int(area.top()) + 2; y < int(area.bottom()) - 2 && y < shot.height(); y += 2)
            for (int x = int(area.left()) + 2; x < int(area.right()) - 2 && x < shot.width(); x += 2)
                if (colourDistance(shot.pixelColor(x, y), paper) > 120) ++ink;
        return ink;
    }

    Scene scene() const
    {
        Scene scene;
        scene.waveRows = decorations->marks()->rowCount();
        for (QQuickItem *child : editor->childItems()) {
            // Made, whether or not Qt's software renderer is told to show it.
            if (child->objectName() != QLatin1String("wave")) continue;
            ++scene.waves;
            const int points = pointsOf(child);
            scene.wavePoints += points;
            scene.widestWave = std::max(scene.widestWave, points);
        }
        scene.markRows = preview->marks()->rowCount();
        for (QQuickItem *child : surface->childItems()) {
            if (child->objectName() != QLatin1String("previewMark")) continue;
            ++scene.markItems;
            if (findItem(child, QStringLiteral("previewIsland"))) ++scene.islands;
        }
        return scene;
    }

    // Every wavy underline listed lies in the editor's view or within one
    // view's height of it, and every mark of the preview within half a
    // view's height of the preview's view, and no further right than it reaches.
    QString strayMarks() const
    {
        QStringList stray;
        const qreal editorTop = editorView->property("contentY").toReal() - editor->y();
        const qreal editorHeight = editorView->height();
        for (const DecorationMarks::Mark &mark : decorations->marks()->marks())
            if (mark.y + mark.height < editorTop - editorHeight - 64 || mark.y > editorTop + 2 * editorHeight + 64)
                stray << QStringLiteral("wave at %1 for a view from %2").arg(mark.y).arg(editorTop);
        if (previewShown()) {
            const qreal top = preview->viewTop(), height = previewView->height();
            const qreal right = previewView->width() - surface->x();
            QAbstractItemModel *marks = preview->marks();
            for (int row = 0; row < marks->rowCount(); ++row) {
                const QRectF rect = marks->data(marks->index(row, 0), PreviewMarks::RectRole).toRectF();
                if (rect.bottom() < top - height / 2 - 64 || rect.top() > top + 1.5 * height + 64 || rect.left() > right)
                    stray << QStringLiteral("%1 at %2,%3 for a view from %4 to the right at %5")
                                 .arg(marks->data(marks->index(row, 0), PreviewMarks::KindRole).toString())
                                 .arg(rect.left())
                                 .arg(rect.top())
                                 .arg(top)
                                 .arg(right);
            }
        }
        return stray.join(QStringLiteral("; "));
    }
};

} // namespace

namespace FrameTests {

void adversarialEditsDrawOnlyNearTheView()
{
    // A typical document's length of words the dictionary does not know,
    // every one of them underlined, beside its preview.
    AdversarialWindow w(unknownProse(58000));
    QVERIFY2(w.ok(), qPrintable(w.shell ? w.shell->error : QString()));
    QTRY_VERIFY(w.shell->backend->spelling()->active());
    w.view(TabModel::Split);
    QVERIFY(w.settled());
    QTRY_VERIFY(w.decorations->marks()->rowCount() > 0);
    const Scene loaded = w.scene();
    qInfo("loaded: %s", qPrintable(loaded.said()));
    QVERIFY(loaded.waves > 0);
    QCOMPARE(loaded.waves, loaded.waveRows);
    // Both texts are long enough that Qt draws only the blocks in view.
    QVERIFY(w.editor->flags() & QQuickItem::ItemObservesViewport);
    QVERIFY(w.surface->flags() & QQuickItem::ItemObservesViewport);

    // What any one view can show is bounded by the view: as many underlines
    // as a view full of misspelled words has, give or take a part of a
    // view; and an underline is no longer than a line is wide.
    const int waveBound = 2 * loaded.waves + 40;
    const int pointBound = 2 * loaded.wavePoints + 400;
    const auto checkBounded = [&](const char *step) {
        const Scene now = w.scene();
        qInfo("%s: %s", step, qPrintable(now.said()));
        QVERIFY2(now.waves <= waveBound, qPrintable(now.said()));
        QVERIFY2(now.wavePoints <= pointBound, qPrintable(now.said()));
        QVERIFY2(now.widestWave <= loaded.widestWave + 4 || now.widestWave <= 2 * loaded.widestWave, qPrintable(now.said()));
        QVERIFY2(now.markRows <= 80, qPrintable(now.said()));
        QVERIFY2(now.markItems <= now.markRows, qPrintable(now.said()));
        const QString stray = w.strayMarks();
        QVERIFY2(stray.isEmpty(), qPrintable(stray));
        // What each view shows is drawn, where the edit left it.
        QVERIFY2(w.inkIn(w.editorView) > 500, step);
        if (w.previewShown()) QVERIFY2(w.inkIn(w.previewView) > 500, step);
    };

    // A megabyte pasted at the end, at once.
    w.putAtEnd(QStringLiteral("\n\n## A pasted chapter\n\n") + unknownProse(1000000));
    QVERIFY(w.settled());
    QVERIFY(w.document->buffer()->length() > 1000000);
    QVERIFY(w.surface->flags() & QQuickItem::ItemObservesViewport);
    QVERIFY(w.editor->flags() & QQuickItem::ItemObservesViewport);
    checkBounded("1 MB pasted, split");

    // Its undo.
    w.binding->undo();
    QVERIFY(w.settled());
    QCOMPARE(w.document->buffer()->length(), 58000);
    checkBounded("undone, split");

    // Quotes, lists, emphasis and brackets two hundred deep.
    w.putAtEnd(nestedText());
    QVERIFY(w.settled());
    checkBounded("nested 200 deep, split");

    // The same in the editor alone.
    w.view(TabModel::Source);
    QTRY_VERIFY(!w.previewShown());
    w.putAtEnd(QStringLiteral("\n\n## Pasted again\n\n") + unknownProse(1000000));
    QVERIFY(w.settled());
    checkBounded("1 MB pasted, source");
    w.putAtEnd(nestedText());
    QVERIFY(w.settled());
    checkBounded("nested 200 deep, source");
}

void textIsDrawnWhereverTheViewGoes()
{
    // Long enough for both views to draw only what is in view.
    AdversarialWindow w(unknownProse(40000));
    QVERIFY2(w.ok(), qPrintable(w.shell ? w.shell->error : QString()));
    w.view(TabModel::Split);
    QVERIFY(w.settled());
    QVERIFY(w.editor->flags() & QQuickItem::ItemObservesViewport);
    QVERIFY(w.surface->flags() & QQuickItem::ItemObservesViewport);
    const auto inkIn = [&](QQuickItem *view) { return w.inkIn(view); };
    const auto scrollTo = [](QQuickItem *view, qreal fraction) {
        const qreal most = std::max<qreal>(0, view->property("contentHeight").toReal() - view->height());
        view->setProperty("contentY", std::round(most * fraction));
    };
    w.shell->settings()->setScrollSync(false);
    for (const qreal fraction : {0.5, 1.0, 0.0}) {
        scrollTo(w.editorView, fraction);
        scrollTo(w.previewView, fraction);
        QTRY_VERIFY2(inkIn(w.editorView) > 500, qPrintable(QStringLiteral("editor at %1").arg(fraction)));
        QTRY_VERIFY2(inkIn(w.previewView) > 500, qPrintable(QStringLiteral("preview at %1").arg(fraction)));
    }

    // Seen at its end, the text is cut by half: still long, and what each
    // view shows now was out of view before.
    scrollTo(w.editorView, 1.0);
    scrollTo(w.previewView, 1.0);
    QTRY_VERIFY(inkIn(w.editorView) > 500 && inkIn(w.previewView) > 500);
    w.document->buffer()->replace({20000, w.document->buffer()->length() - 20000}, QString());
    QVERIFY(w.settled());
    QVERIFY(w.editor->flags() & QQuickItem::ItemObservesViewport);
    QVERIFY2(inkIn(w.editorView) > 500, "editor cut by half");
    QVERIFY2(inkIn(w.previewView) > 500, "preview cut by half");
    // Drawing the preview again left its colours to the theme.
    replaceTheme(w.shell->themeFolder.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    QTRY_VERIFY(w.shell->theme->dark());
    QTRY_COMPARE(w.surface->property("selectedTextColor").value<QColor>(), w.preview->textColor());
    QTRY_COMPARE(w.surface->property("color").value<QColor>(), w.preview->textColor());

    // Seen at its end, the text is cut short enough for Qt to draw all of
    // it again: what was out of view before is drawn now too.
    scrollTo(w.editorView, 1.0);
    scrollTo(w.previewView, 1.0);
    QTRY_VERIFY(inkIn(w.editorView) > 500 && inkIn(w.previewView) > 500);
    w.document->buffer()->replace({6000, w.document->buffer()->length() - 6000}, QString());
    QVERIFY(w.settled());
    QVERIFY(!(w.editor->flags() & QQuickItem::ItemObservesViewport));
    for (const qreal fraction : {1.0, 0.5, 0.0}) {
        scrollTo(w.editorView, fraction);
        scrollTo(w.previewView, fraction);
        QTRY_VERIFY2(inkIn(w.editorView) > 500, qPrintable(QStringLiteral("editor at %1").arg(fraction)));
        QTRY_VERIFY2(inkIn(w.previewView) > 500, qPrintable(QStringLiteral("preview at %1").arg(fraction)));
    }
}

void keysBesideThePreviewDoNotWaitForIt()
{
    AdversarialWindow w(unknownProse(58000));
    QVERIFY2(w.ok(), qPrintable(w.shell ? w.shell->error : QString()));
    w.view(TabModel::Split);
    QVERIFY(w.settled());
    auto *sync = qobject_cast<ScrollSync *>(w.shell->object(QStringLiteral("scrollSync")));
    QVERIFY(sync);
    QVERIFY(w.shell->settings()->scrollSync());
    QQuickItem *updating = w.shell->item(QStringLiteral("previewUpdating"));
    QVERIFY(updating);
    // The caret in the middle of what the editor shows, both views there.
    w.editorView->setProperty("contentY", w.editorView->property("contentHeight").toReal() / 2);
    QCoreApplication::processEvents();
    const SourceRange shown = w.binding->visibleRange();
    const int middle = shown.start + shown.length / 2;
    w.binding->select(middle, middle);
    w.editor->forceActiveFocus();
    QVERIFY(w.settled());

    // Keys typed one after another, faster than the preview's pause: it
    // follows them, but only every so often, never key by key, and it never
    // says it is late. The scroll sync moves the preview no more often.
    QElapsedTimer clock;
    QList<qint64> updates;
    bool saidLate = false;
    const QMetaObject::Connection seen =
        QObject::connect(w.preview, &Preview::updated, w.preview, [&] { updates << clock.elapsed(); });
    const QMetaObject::Connection late =
        QObject::connect(w.preview, &Preview::lateChanged, w.preview, [&] { saidLate = saidLate || w.preview->late(); });
    const int followedBefore = sync->previewFollowed();
    clock.start();
    constexpr int keyCount = 40;
    for (int i = 0; i < keyCount; ++i) {
        sendKey(w.window, QChar(u'x'));
        QTest::qWait(60);
    }
    const qint64 typing = clock.elapsed();
    const qsizetype whileTyping = updates.size();
    QObject::disconnect(seen);
    QObject::disconnect(late);
    QStringList at;
    for (const qint64 t : std::as_const(updates)) at << QString::number(t);
    const int followed = sync->previewFollowed() - followedBefore;
    qInfo("%d keys in %lld ms: %lld preview updates while typing, at %s ms; the preview followed the text %d times",
          keyCount, typing, qint64(whileTyping), qPrintable(at.join(QStringLiteral(", "))), followed);
    QVERIFY2(whileTyping >= 1, "the preview did not follow the typing");
    QVERIFY2(whileTyping <= typing / Preview::MaxWaitMs + 1, qPrintable(QString::number(whileTyping)));
    QVERIFY(!saidLate);
    QVERIFY(!updating->isVisible());
    QVERIFY2(followed <= 2 * whileTyping + 2, qPrintable(QString::number(followed)));
    // Once typing stops, it catches up after its pause.
    QElapsedTimer caughtUp;
    caughtUp.start();
    QVERIFY(QTest::qWaitFor([&] { return w.preview->current() && !w.preview->working(); }, 10000));
    QVERIFY2(caughtUp.elapsed() <= timeLimit(Preview::PauseMs + 600), qPrintable(QString::number(caughtUp.elapsed())));
    QVERIFY(w.preview->textDocument()->toPlainText().contains(QString(keyCount, u'x')));
}

void newWidthsMoveEachViewOnce()
{
    // As the column narrows and widens a step at a time, the line at the
    // top of the text stays there and the preview beside it stays with it:
    // each new width moves the text at most once, and the preview, which
    // follows it, at most once too. Qt's Flickable rounds a position between
    // pixels whenever what it shows grows or shrinks. A view left between
    // pixels moved by itself at each new width, before its line was put back
    // at the top, and the preview followed it there and back again, drawing
    // all it showed anew each time, its pictures too.
    QString text;
    for (int part = 1; text.size() < 40000; ++part)
        text += QStringLiteral("## Part %1\n\n").arg(part) + unknownProse(1500) + QStringLiteral("\n\n");
    AdversarialWindow w(text);
    QVERIFY2(w.ok(), qPrintable(w.shell ? w.shell->error : QString()));
    // Lines whose height is not a whole number of pixels.
    w.shell->settings()->setLineHeight(1.55);
    w.view(TabModel::Split);
    QVERIFY(w.settled());
    QVERIFY(w.surface->flags() & QQuickItem::ItemObservesViewport);
    QQuickItem *pane = w.shell->item(QStringLiteral("editorPane"));
    QObject *anchors = pane->property("anchors").value<QObject *>();
    QVERIFY(pane && anchors);
    // Narrowed by the room beside the column, so that the column narrows
    // from the first step, with the text in the middle.
    const qreal base = std::floor(pane->width() - 2 * pane->property("margin").toReal()
                                  - pane->property("columnWidth").toReal());
    anchors->setProperty("rightMargin", base);
    const int middle = w.document->buffer()->length() / 2;
    w.binding->restore(middle, middle, middle);
    QVERIFY(w.settled());
    QTest::qWait(ScrollSync::HoldMs + 50);
    const int top = w.binding->topSourcePosition();
    const qreal previewAt = w.previewView->property("contentY").toReal();
    QVERIFY(previewAt > 0);

    QSignalSpy textMoves(w.editorView, SIGNAL(contentYChanged()));
    QSignalSpy previewMoves(w.previewView, SIGNAL(contentYChanged()));
    int mostText = 0, mostPreview = 0, moved = 0;
    for (int step = 1; step <= 120; ++step) {
        textMoves.clear();
        previewMoves.clear();
        // Narrower for sixty steps, and then wider again.
        anchors->setProperty("rightMargin", base + 4 * (step <= 60 ? step : 120 - step));
        mostText = std::max(mostText, int(textMoves.count()));
        mostPreview = std::max(mostPreview, int(previewMoves.count()));
        moved += int(textMoves.count());
        QCOMPARE(w.binding->topSourcePosition(), top);
        if (step % 10 == 0) QCoreApplication::processEvents();
    }
    QVERIFY2(moved >= 5, qPrintable(QString::number(moved)));
    QVERIFY2(mostText <= 1, qPrintable(QString::number(mostText)));
    QVERIFY2(mostPreview <= 1, qPrintable(QString::number(mostPreview)));
    // Back at its first width, the preview is where it was.
    QVERIFY(std::abs(w.previewView->property("contentY").toReal() - previewAt) <= 2);
    anchors->setProperty("rightMargin", 0);
}

} // namespace FrameTests
