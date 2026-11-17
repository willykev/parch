#include "previewtests.h"

#include "apppaths.h"
#include "codehighlight.h"
#include "documentstore.h"
#include "backend.h"
#include "dialectchoice.h"
#include "documentparse.h"
#include "documentreading.h"
#include "documentsettingsstore.h"
#include "editorbinding.h"
#include "editordecorations.h"
#include "focusmode.h"
#include "mdinclude.h"
#include "mdtree.h"
#include "outline.h"
#include "preview.h"
#include "previewbuilder.h"
#include "previewresources.h"
#include "previewwatch.h"
#include "remotefetcher.h"
#include "renderservice.h"
#include "rendertesting.h"
#include "scrollsync.h"
#include "settings.h"
#include "previewstyle.h"
#include "session.h"
#include "sourcebuffer.h"
#include "tabmodel.h"
#include "testfiles.h"
#include "theme.h"
#include "vimmode.h"
#include "windowcontext.h"
#include "windowtesting.h"

#include <algorithm>
#include <QAbstractTextDocumentLayout>
#include <QAccessible>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextTable>
#include <QUuid>
#include <QtTest>
#include <memory>
#include <unistd.h>

using preview::Anchor;
using preview::Island;
using preview::Mark;
using preview::Part;
using preview::Picture;
using preview::Section;

namespace {

// Pictures that are ready, pending or failed by what their source says.
class TestResources : public preview::Resources {
public:
    Picture math(const QString &tex, bool display, qreal fontSize) override
    {
        ++mathAsked;
        Picture picture = byWord(tex, QStringLiteral("math"));
        picture.key = QStringLiteral("parch:math/%1/%2").arg(display ? "d" : "i").arg(qHash(tex));
        if (picture.state == Picture::Ready) {
            picture.size = QSizeF(10 * tex.size(), fontSize);
            picture.baseline = fontSize * 0.8;
            picture.image = filled(picture.size, Qt::blue);
            picture.description = QStringLiteral("formula ") + tex;
        }
        return picture;
    }
    Picture diagram(const QString &source) override
    {
        Picture picture = byWord(source, QStringLiteral("diagram"));
        picture.key = QStringLiteral("parch:diagram/%1").arg(qHash(source));
        if (picture.state == Picture::Ready) {
            picture.size = QSizeF(300, 120);
            picture.image = filled(picture.size, Qt::green);
            picture.description = QStringLiteral("Flowchart: start, end");
        }
        return picture;
    }
    Picture image(const md::Destination &destination, const QString &written, const QStringList &) override
    {
        asked << (destination.kind == md::Destination::Kind::LocalFile ? destination.path : written);
        Picture picture = byWord(written, QStringLiteral("image"));
        picture.key = QStringLiteral("parch:image/%1").arg(qHash(written));
        if (picture.state == Picture::Ready) {
            picture.size = QSizeF(640, 480);
            picture.image = filled(picture.size, Qt::red);
        }
        return picture;
    }

    QStringList asked;
    int mathAsked = 0;

private:
    static Picture byWord(const QString &source, const QString &what)
    {
        Picture picture;
        if (source.contains(QLatin1String("pending"))) {
            picture.state = Picture::Pending;
        } else if (source.contains(QLatin1String("broken"))) {
            picture.state = Picture::Failed;
            picture.diagnostic = QStringLiteral("no such %1").arg(what);
        } else {
            picture.state = Picture::Ready;
        }
        return picture;
    }
    static QImage filled(const QSizeF &size, const QColor &color)
    {
        QImage image(size.toSize(), QImage::Format_ARGB32_Premultiplied);
        image.fill(color);
        return image;
    }
};

PreviewStyle testStyle()
{
    PreviewStyle style = PreviewStyle::paper();
    style.family = QStringLiteral("serif");
    style.codeFamily = QStringLiteral("monospace");
    style.size = 20;
    style.lineHeight = 1.5;
    return style;
}

// A document built as the preview builds it.
struct Built {
    md::Document tree;
    // For a document read with its settings: its tree and its citations.
    DocumentReading::Reading reading;
    QObject islandOwner;
    QTextDocument document;
    std::vector<Section> sections;
    TestResources resources;

    // Where a section starts in the document.
    int startOf(size_t index) const
    {
        int start = 1;
        for (size_t i = 0; i < index; ++i) start += sections[i].length;
        return start;
    }
    QTextCursor find(const QString &needle, int from = 0) const
    {
        return document.find(needle, from, QTextDocument::FindCaseSensitively);
    }
    // The format of the first character of the first place a text is found.
    QTextCharFormat formatOf(const QString &needle) const
    {
        QTextCursor found = find(needle);
        if (found.isNull()) return {};
        found.setPosition(found.selectionStart() + 1);
        return found.charFormat();
    }
    QTextBlock blockOf(const QString &needle) const { return find(needle).block(); }
    const Island *island(Island::Kind kind, int which = 0) const
    {
        for (const Section &section : sections)
            for (const Island &island : section.islands)
                if (island.kind == kind && which-- == 0) return &island;
        return nullptr;
    }
    QList<Mark> marks(Mark::Kind kind) const
    {
        QList<Mark> found;
        for (const Section &section : sections)
            for (const Mark &mark : section.marks)
                if (mark.kind == kind) found << mark;
        return found;
    }
    // Every name links can go to, with the absolute position it leads to.
    QHash<QString, int> names() const
    {
        QHash<QString, int> found;
        for (size_t i = 0; i < sections.size(); ++i)
            for (const preview::NamedAnchor &name : sections[i].names)
                if (!found.contains(name.name)) found.insert(name.name, startOf(i) + name.position);
        return found;
    }
    // The document's lines of text, one for each block.
    QStringList lines() const
    {
        QStringList lines;
        for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) lines << block.text();
        return lines;
    }
};

std::unique_ptr<Built> build(const QString &markdown, const md::Options &options = md::Options::writer(),
                             const md::Source &source = {}, const preview::BuildOptions &given = {})
{
    auto built = std::make_unique<Built>();
    built->tree = md::parse(markdown, options, source);
    preview::BuildOptions buildOptions = given;
    buildOptions.islandParent = &built->islandOwner;
    preview::Builder builder(built->tree, testStyle(), &built->resources, buildOptions);
    built->sections = builder.buildAll(&built->document);
    return built;
}

// A text read as every output of a document reads it, in the settings
// given and those of its front matter, with its files found in `folder`,
// and built with its citations as its citation style formats them.
std::unique_ptr<Built> buildRead(const QString &markdown, const QString &folder, const Dialect::Layers &layers = {})
{
    auto built = std::make_unique<Built>();
    md::FileIncludeResolver resolver;
    md::Source source;
    source.directory = folder;
    source.resolver = &resolver;
    built->reading = DocumentReading::read(markdown, layers, source);
    preview::BuildOptions options;
    options.islandParent = &built->islandOwner;
    options.citations = &built->reading.formatted;
    preview::Builder builder(built->reading.document, testStyle(), &built->resources, options);
    built->sections = builder.buildAll(&built->document);
    return built;
}

int inkOf(const QTextCharFormat &format) { return format.intProperty(PreviewStyle::InkProperty); }
int paperOf(const QTextCharFormat &format) { return format.intProperty(PreviewStyle::PaperProperty); }

// The image formats of a document, in order.
QList<QTextImageFormat> imagesIn(const QTextDocument &document)
{
    QList<QTextImageFormat> images;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().isImageFormat()) images << it.fragment().charFormat().toImageFormat();
    return images;
}

// Runs a command in a window, as its shortcuts, menu and palette do.
void runIn(QQuickWindow *window, const char *command)
{
    QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QString::fromLatin1(command)));
}

// A window on one document, with what the tests of its preview reach for.
struct PreviewWindow {
    ShellFixture shell;
    QQuickWindow *window = nullptr;
    Document *document = nullptr;
    Preview *preview = nullptr;
    QQuickItem *surface = nullptr;
    QQuickItem *flickable = nullptr;
    QQuickItem *editor = nullptr;
    QQuickItem *editorFlickable = nullptr;
    QQuickItem *previewPane = nullptr;
    QQuickItem *editorPane = nullptr;

    explicit PreviewWindow(const QString &text, int width = 1280, const ShellOptions &options = {}) : shell(options)
    {
        if (!shell.ok()) return;
        window = shell.window.get();
        activate(window);
        resizeWindow(window, width);
        document = shell.document();
        if (!text.isEmpty()) document->buffer()->replace({0, 0}, text);
        preview = qobject_cast<Preview *>(shell.object(QStringLiteral("preview")));
        surface = shell.item(QStringLiteral("previewSurface"));
        flickable = shell.item(QStringLiteral("previewFlickable"));
        editor = shell.item(QStringLiteral("editor"));
        editorFlickable = shell.item(QStringLiteral("flickable"));
        previewPane = shell.item(QStringLiteral("previewPane"));
        editorPane = shell.item(QStringLiteral("editorPane"));
    }
    bool ok() const { return shell.ok() && window && preview && surface && flickable && editor && previewPane && editorPane; }
    TabModel *tabs() const { return shell.context()->tabs(); }
    void run(const char *command) { runIn(window, command); }
    // Waits until the preview shows the text as it is and has nothing left to build or draw.
    bool settled(int timeoutMs = 10000) const
    {
        return QTest::qWaitFor([&] { return preview->current() && !preview->working(); }, timeoutMs);
    }
    QStringList lines() const
    {
        QStringList lines;
        for (QTextBlock block = preview->textDocument()->begin(); block.isValid(); block = block.next()) lines << block.text();
        return lines;
    }
    // Keeps a picture of the window where PARCH_PREVIEW_SHOTS says, for looking at; nowhere otherwise.
    void shot(const char *name) const
    {
        const QString folder = qEnvironmentVariable("PARCH_PREVIEW_SHOTS");
        if (folder.isEmpty()) return;
        QTest::qWait(150);
        window->grabWindow().save(folder + u'/' + QLatin1String(name) + QStringLiteral(".png"));
    }
};

// Where the preview finds the heading a link names, once it has: -2 when
// it says nothing.
int headingFound(Preview *preview, const QString &slug)
{
    QSignalSpy found(preview, &Preview::headingFound);
    const int request = preview->findHeading(slug);
    if (!QTest::qWaitFor([&] { return !found.isEmpty(); }, 10000)) return -2;
    return found.last().at(0).toInt() == request && found.last().at(1).toString() == slug ? found.last().at(2).toInt() : -2;
}

} // namespace

namespace PreviewTests {

void viewModesAreKeptByEachView()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    DocumentStore store(paths);
    TabModel tabs(&store);
    QSignalSpy modes(&tabs, &TabModel::activeModeChanged);
    QSignalSpy changes(&tabs, &TabModel::changed);

    // Without a tab there is the source alone, and nothing to change.
    QCOMPARE(tabs.activeMode(), TabModel::Source);
    tabs.setActiveMode(TabModel::Split);
    QCOMPARE(tabs.activeMode(), TabModel::Source);

    tabs.openDocument(store.newDraft());
    QCOMPARE(tabs.activeMode(), TabModel::Source);
    QCOMPARE(tabs.activeSplit(), 0.5);
    modes.clear();
    changes.clear();
    tabs.setActiveMode(TabModel::Split);
    QCOMPARE(tabs.activeMode(), TabModel::Split);
    QCOMPARE(modes.count(), 1);
    QVERIFY(changes.count() >= 1);
    // The same mode again says nothing.
    tabs.setActiveMode(TabModel::Split);
    QCOMPARE(modes.count(), 1);
    // The divider stays between its limits.
    tabs.setActiveSplit(0.35);
    QCOMPARE(tabs.activeSplit(), 0.35);
    tabs.setActiveSplit(0.01);
    QCOMPARE(tabs.activeSplit(), ViewState::MinSplit);
    tabs.setActiveSplit(7);
    QCOMPARE(tabs.activeSplit(), ViewState::MaxSplit);
    tabs.setActiveSplit(0.35);

    // A new tab starts beside its preview when the tab in front is, at the
    // same divider; each tab then keeps its own.
    tabs.openDocument(store.newDraft());
    QCOMPARE(tabs.activeIndex(), 1);
    QCOMPARE(tabs.activeMode(), TabModel::Split);
    QCOMPARE(tabs.activeSplit(), 0.35);
    tabs.setActiveMode(TabModel::Reading);
    tabs.setActiveSplit(0.6);
    tabs.setActiveIndex(0);
    QCOMPARE(tabs.activeMode(), TabModel::Split);
    QCOMPARE(tabs.activeSplit(), 0.35);
    tabs.setActiveIndex(1);
    QCOMPARE(tabs.activeMode(), TabModel::Reading);
    // A new tab is for writing, so it never starts as the preview alone.
    tabs.openDocument(store.newDraft());
    QCOMPARE(tabs.activeMode(), TabModel::Source);
    QCOMPARE(tabs.activeSplit(), 0.6);
    // A second view of a document starts as the first is.
    tabs.setActiveIndex(1);
    const int second = tabs.newView(1);
    QCOMPARE(tabs.activeIndex(), second);
    QCOMPARE(tabs.activeMode(), TabModel::Reading);

    // The session keeps each view's mode and divider.
    const QString a = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString b = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString c = QUuid::createUuid().toString(QUuid::WithoutBraces);
    SessionState state;
    WindowState window;
    window.tabs = {ViewState{a, {}, 0, 0, 0, QStringLiteral("A"), ViewState::Split, 0.3},
                   ViewState{b, {}, 0, 0, 0, QStringLiteral("B"), ViewState::Reading, 0.5},
                   ViewState{c, {}, 0, 0, 0, QStringLiteral("C")}};
    state.windows = {window};
    QVERIFY(Session::save(paths, state));
    const SessionState loaded = Session::load(paths);
    QCOMPARE(loaded.windows.size(), 1);
    QCOMPARE(loaded.windows.first().tabs.size(), 3);
    QCOMPARE(loaded.windows.first().tabs.at(0).mode, int(ViewState::Split));
    QCOMPARE(loaded.windows.first().tabs.at(0).split, 0.3);
    QCOMPARE(loaded.windows.first().tabs.at(1).mode, int(ViewState::Reading));
    QCOMPARE(loaded.windows.first().tabs.at(2).mode, int(ViewState::Source));
    QCOMPARE(loaded.windows.first().tabs.at(2).split, 0.5);
    // A mode or a divider that is none is the default, and costs no tab.
    writeFile(paths.sessionFile(),
              QStringLiteral("{\"version\":1,\"windows\":[{\"tabs\":["
                             "{\"id\":\"%1\",\"mode\":\"sideways\",\"split\":0.99},"
                             "{\"id\":\"%2\",\"mode\":7,\"split\":\"wide\"}]}]}")
                  .arg(a, b)
                  .toUtf8());
    const SessionState mended = Session::load(paths);
    QCOMPARE(mended.windows.size(), 1);
    QCOMPARE(mended.windows.first().tabs.size(), 2);
    for (const ViewState &view : mended.windows.first().tabs) {
        QCOMPARE(view.mode, int(ViewState::Source));
        QCOMPARE(view.split, 0.5);
    }
    // Restored tabs come back as they were kept.
    TabModel restored(&store);
    QSignalSpy restoredModes(&restored, &TabModel::activeModeChanged);
    restored.restore(loaded.windows.first().tabs, 1);
    QVERIFY(restoredModes.count() >= 1);
    QCOMPARE(restored.activeMode(), TabModel::Reading);
    QCOMPARE(restored.viewState(0).mode, int(ViewState::Split));
    QCOMPARE(restored.viewState(0).split, 0.3);
}

void previewBuildsTextAndInlineFormatting()
{
    const auto built = build(QStringLiteral(
        "# The *first* heading\n\n"
        "Plain words, *leaning*, **heavy**, ~~struck~~, ==marked==, `spanned`,\n"
        "x^2^ and y~i~, an &amp; and a \\* star.  \n"
        "After the break [a link](https://example.com \"Its title\") and <https://auto.example>.\n\n"
        "### Third level ###\n\n"
        "Setext two\n----------\n\n"
        "// a comment line\n\n"
        "Last <!-- hidden --> paragraph.\n"));
    const QTextDocument &document = built->document;
    // The document starts with an empty block, and every block after it is a block of the text.
    QCOMPARE(built->lines(),
             (QStringList{QString(), QStringLiteral("The first heading"),
                          QStringLiteral("Plain words, leaning, heavy, struck, marked, spanned, x2 and yi, an & and a * star.")
                              + QChar::LineSeparator
                              + QStringLiteral("After the break a link and https://auto.example."),
                          QStringLiteral("Third level"), QStringLiteral("Setext two"),
                          QStringLiteral("Last  paragraph.")}));
    // Headings say their level, in type that steps down with it.
    const QTextBlock first = built->blockOf(QStringLiteral("The first"));
    QCOMPARE(first.blockFormat().headingLevel(), 1);
    QCOMPARE(built->blockOf(QStringLiteral("Third level")).blockFormat().headingLevel(), 3);
    QCOMPARE(built->blockOf(QStringLiteral("Setext two")).blockFormat().headingLevel(), 2);
    QCOMPARE(built->blockOf(QStringLiteral("Plain words")).blockFormat().headingLevel(), 0);
    const QTextCharFormat h1 = built->formatOf(QStringLiteral("The "));
    const QTextCharFormat h2 = built->formatOf(QStringLiteral("Setext"));
    const QTextCharFormat h3 = built->formatOf(QStringLiteral("Third"));
    QCOMPARE(h1.intProperty(QTextFormat::FontPixelSize), 40);
    QCOMPARE(h2.intProperty(QTextFormat::FontPixelSize), 30);
    QCOMPARE(h3.intProperty(QTextFormat::FontPixelSize), 25);
    QCOMPARE(h1.fontWeight(), int(QFont::DemiBold));
    QCOMPARE(inkOf(h1), int(PreviewStyle::Heading));
    // Emphasis inside a heading keeps the heading's type.
    const QTextCharFormat leaningHeading = built->formatOf(QStringLiteral("first"));
    QVERIFY(leaningHeading.fontItalic());
    QCOMPARE(leaningHeading.intProperty(QTextFormat::FontPixelSize), 40);
    // Each line of a paragraph is at least the line height apart.
    const QTextBlockFormat paragraph = built->blockOf(QStringLiteral("Plain words")).blockFormat();
    QCOMPARE(paragraph.lineHeightType(), int(QTextBlockFormat::MinimumHeight));
    QCOMPARE(paragraph.lineHeight(), 30.0);
    QVERIFY(paragraph.bottomMargin() > 0);

    // Plain text has no format of its own: it takes the surface's colour and font.
    const QTextCharFormat plain = built->formatOf(QStringLiteral("Plain"));
    QVERIFY(!plain.hasProperty(QTextFormat::ForegroundBrush));
    QVERIFY(!plain.hasProperty(QTextFormat::FontPixelSize));
    QVERIFY(built->formatOf(QStringLiteral("leaning")).fontItalic());
    QCOMPARE(built->formatOf(QStringLiteral("heavy")).fontWeight(), int(QFont::Bold));
    QVERIFY(built->formatOf(QStringLiteral("struck")).fontStrikeOut());
    const QTextCharFormat marked = built->formatOf(QStringLiteral("marked"));
    QCOMPARE(paperOf(marked), int(PreviewStyle::MarkPaper));
    QCOMPARE(marked.background().color(), testStyle().paper(PreviewStyle::MarkPaper));
    const QTextCharFormat spanned = built->formatOf(QStringLiteral("spanned"));
    QCOMPARE(spanned.fontFamilies().toStringList(), QStringList{QStringLiteral("monospace")});
    QCOMPARE(paperOf(spanned), int(PreviewStyle::CodePaper));
    QCOMPARE(inkOf(spanned), int(PreviewStyle::Code));
    QCOMPARE(built->formatOf(QStringLiteral("2 and")).verticalAlignment(), QTextCharFormat::AlignSuperScript);
    QCOMPARE(built->formatOf(QStringLiteral("i, an")).verticalAlignment(), QTextCharFormat::AlignSubScript);

    // Links are anchors, in the link colour and underlined, with where they
    // lead and then their title.
    const QTextCharFormat link = built->formatOf(QStringLiteral("a link"));
    QVERIFY(link.isAnchor());
    QCOMPARE(link.anchorHref(), QStringLiteral("https://example.com"));
    QCOMPARE(inkOf(link), int(PreviewStyle::Link));
    QVERIFY(link.fontUnderline());
    QCOMPARE(link.toolTip(), QStringLiteral("https://example.com\nIts title"));
    QCOMPARE(built->formatOf(QStringLiteral("https://auto")).anchorHref(), QStringLiteral("https://auto.example"));
    QVERIFY(!built->formatOf(QStringLiteral("After")).isAnchor());

    // A heading is marked for assistive technology, with its level and text.
    const QList<Mark> headings = built->marks(Mark::Heading);
    QCOMPARE(headings.size(), 3);
    QCOMPARE(headings.at(0).level, 1);
    QCOMPARE(headings.at(0).text, QStringLiteral("The first heading"));
    QCOMPARE(headings.at(2).level, 2);
    // Each heading can be gone to by its slug.
    const QHash<QString, int> names = built->names();
    QCOMPARE(names.value(QStringLiteral("the-first-heading")), first.position());
    QVERIFY(names.contains(QStringLiteral("third-level")));
    QVERIFY(names.contains(QStringLiteral("setext-two")));
    // Nothing of a comment is built.
    QVERIFY(document.find(QStringLiteral("comment")).isNull());
    QVERIFY(document.find(QStringLiteral("hidden")).isNull());
}

void previewBuildsListsQuotesAndRules()
{
    const QString markdown = QStringLiteral(
        "- one\n- two\n  - nested\n\n"
        "7. seven\n8. eight\n\n"
        "3) loose three\n\n4) loose four\n\n"
        "- [ ] to do\n- [x] done **well**\n\n"
        "> A quote\n> goes on.\n>\n> > Inside it.\n\n"
        "***\n\n"
        "After the rule.\n\n"
        "+++\n\n"
        "Term\n: Its description\n: Another\n\n"
        "The end.\n");
    const auto built = build(markdown);
    const QChar gap(0x2002);
    // Each item starts with its marker; a task's is a box, which is a picture.
    const QStringList lines = built->lines();
    QVERIFY2(lines.contains(QChar(0x2022) + QString(gap) + QStringLiteral("one")), qPrintable(lines.join(u'|')));
    QVERIFY(lines.contains(QChar(0x25e6) + QString(gap) + QStringLiteral("nested")));
    // The Writer profile numbers every list from one, whatever number it
    // was written with, and keeps the mark after the number.
    QVERIFY(lines.contains(QStringLiteral("1.") + gap + QStringLiteral("seven")));
    QVERIFY(lines.contains(QStringLiteral("2.") + gap + QStringLiteral("eight")));
    QVERIFY(lines.contains(QStringLiteral("1)") + gap + QStringLiteral("loose three")));
    QVERIFY(lines.contains(QStringLiteral("2)") + gap + QStringLiteral("loose four")));
    QVERIFY(lines.contains(QChar::ObjectReplacementCharacter + QString(gap) + QStringLiteral("to do")));
    QVERIFY(lines.contains(QChar::ObjectReplacementCharacter + QString(gap) + QStringLiteral("done well")));
    // The Standard profile numbers a list from the number it starts with.
    const auto standard = build(QStringLiteral("5. five\n6. six\n"), md::Options::standard());
    QVERIFY(standard->lines().contains(QStringLiteral("5.") + gap + QStringLiteral("five")));
    QVERIFY(standard->lines().contains(QStringLiteral("6.") + gap + QStringLiteral("six")));

    // The marker hangs before the text: the text of every line of an item
    // starts at the item's left margin, and the first line is pulled back
    // by exactly the marker.
    const QTextBlockFormat one = built->blockOf(QStringLiteral("one")).blockFormat();
    QVERIFY(one.leftMargin() > 0);
    QCOMPARE(one.textIndent(), -one.leftMargin());
    const QTextBlockFormat nested = built->blockOf(QStringLiteral("nested")).blockFormat();
    QVERIFY(nested.leftMargin() > one.leftMargin());
    // Numbers end where the text begins, so they line up on their dots.
    const QTextBlockFormat seven = built->blockOf(QStringLiteral("seven")).blockFormat();
    const QTextBlockFormat eight = built->blockOf(QStringLiteral("eight")).blockFormat();
    QCOMPARE(seven.leftMargin(), eight.leftMargin());
    // Markers are dimmer than the text.
    QCOMPARE(inkOf(built->formatOf(QStringLiteral("2."))), int(PreviewStyle::Dim));
    QCOMPARE(inkOf(built->formatOf(QStringLiteral("seven"))), 0);
    // A tight list keeps its items close and ends as a paragraph does; a
    // loose one sets each item apart.
    QVERIFY(one.bottomMargin() < built->blockOf(QStringLiteral("loose three")).blockFormat().bottomMargin());
    QCOMPARE(built->blockOf(QStringLiteral("nested")).blockFormat().bottomMargin(),
             built->blockOf(QStringLiteral("After the rule")).blockFormat().bottomMargin());

    // A task's box is a link to its marker in the source, and says its state.
    const QList<QTextImageFormat> images = imagesIn(built->document);
    QCOMPARE(images.size(), 2);
    QCOMPARE(images.at(0).name(), preview::Builder::taskImageName(false));
    QCOMPARE(images.at(1).name(), preview::Builder::taskImageName(true));
    QVERIFY(images.at(0).isAnchor());
    // The section's source starts at the list, and the marker is two in.
    const int listStart = int(markdown.indexOf(QStringLiteral("- [ ]")));
    size_t tasks = 0;
    while (tasks < built->sections.size() && built->sections[tasks].sourceStart != listStart) ++tasks;
    QVERIFY(tasks < built->sections.size());
    QCOMPARE(images.at(0).anchorHref(), preview::Builder::taskLink(2));
    QCOMPARE(images.at(1).anchorHref(), preview::Builder::taskLink(int(markdown.indexOf(QStringLiteral("[x]"))) - listStart));
    QCOMPARE(built->document.resource(QTextDocument::ImageResource, QUrl(images.at(0).name())).value<QImage>().isNull(), false);
    QCOMPARE(built->formatOf(QStringLiteral("well")).fontWeight(), int(QFont::Bold));

    // A quote steps in, leans, and has a bar from its first block to its
    // last; a quote inside it has its own, further in.
    const QTextBlock quote = built->blockOf(QStringLiteral("A quote goes on."));
    QVERIFY(quote.isValid());
    QVERIFY(quote.blockFormat().leftMargin() > 0);
    QVERIFY(built->formatOf(QStringLiteral("A quote")).fontItalic());
    const QTextBlock inner = built->blockOf(QStringLiteral("Inside it."));
    QVERIFY(inner.blockFormat().leftMargin() > quote.blockFormat().leftMargin());
    const QList<Mark> bars = built->marks(Mark::QuoteBar);
    QCOMPARE(bars.size(), 2);
    size_t quoted = 0;
    while (quoted < built->sections.size() && built->sections[quoted].marks.isEmpty()) ++quoted;
    const int quoteStart = built->startOf(quoted);
    // The inner quote's bar is found first, since it ends first.
    QCOMPARE(quoteStart + bars.at(0).first, inner.position());
    QCOMPARE(quoteStart + bars.at(0).last, inner.position());
    QCOMPARE(quoteStart + bars.at(1).first, quote.position());
    QCOMPARE(quoteStart + bars.at(1).last, inner.position());
    QVERIFY(bars.at(0).x > bars.at(1).x);

    // A thematic break and a page break are empty blocks with a mark each.
    QCOMPARE(built->marks(Mark::Rule).size(), 1);
    QCOMPARE(built->marks(Mark::PageBreak).size(), 1);
    QVERIFY(built->document.find(QStringLiteral("+++")).isNull());
    QVERIFY(built->document.find(QStringLiteral("***")).isNull());

    // A term is heavier than its descriptions, which step in.
    QCOMPARE(built->formatOf(QStringLiteral("Term")).fontWeight(), int(QFont::DemiBold));
    QVERIFY(built->blockOf(QStringLiteral("Its description")).blockFormat().leftMargin() > 0);
    QCOMPARE(built->blockOf(QStringLiteral("Another")).blockFormat().leftMargin(),
             built->blockOf(QStringLiteral("Its description")).blockFormat().leftMargin());
    QCOMPARE(built->blockOf(QStringLiteral("The end.")).blockFormat().leftMargin(), 0.0);
}

void previewBuildsCodeAndTablesAsIslands()
{
    const QString markdown = QStringLiteral(
        "Before.\n\n"
        "```python\ndef area(r):\n    return 3.14 * r  # a comment\n```\n\n"
        "    indented code\n    second line\n\n"
        "| Name | Amount | Note |\n|:-----|-------:|:----:|\n| Alpha | 12 | *first* |\n| Both columns || last |\n\n"
        "After.\n");
    const auto built = build(markdown);
    // The main document holds one empty block for each island, where its place is.
    QCOMPARE(built->lines(), (QStringList{QString(), QStringLiteral("Before."), QString(), QString(), QString(),
                                          QStringLiteral("After.")}));
    int holes = 0;
    for (QTextBlock block = built->document.begin(); block.isValid(); block = block.next())
        if (block.blockFormat().hasProperty(preview::IslandProperty)) {
            ++holes;
            QCOMPARE(block.blockFormat().lineHeightType(), int(QTextBlockFormat::FixedHeight));
        }
    QCOMPARE(holes, 3);

    // Code: a line of the island for each line of code, in the code face,
    // coloured by what each word is in its language.
    const Island *code = built->island(Island::Code);
    QVERIFY(code && code->document);
    QCOMPARE(code->document->blockCount(), 2);
    QCOMPARE(code->document->begin().text(), QStringLiteral("def area(r):"));
    QCOMPARE(code->document->defaultFont().family(), QStringLiteral("monospace"));
    QVERIFY(code->tinted);
    QVERIFY(!code->wraps);
    QCOMPARE(code->description, QStringLiteral("Code, python"));
    QCOMPARE(code->document->defaultTextOption().wrapMode(), QTextOption::NoWrap);
    const auto codeInk = [&](const QTextDocument *document, const QString &word) {
        QTextCursor found = document->find(word);
        found.setPosition(found.selectionStart() + 1);
        return inkOf(found.charFormat());
    };
    QCOMPARE(codeInk(code->document, QStringLiteral("def")), int(PreviewStyle::Keyword));
    QCOMPARE(codeInk(code->document, QStringLiteral("3.14")), int(PreviewStyle::Number));
    QCOMPARE(codeInk(code->document, QStringLiteral("# a comment")), int(PreviewStyle::Comment));
    // Each line knows the line of the source it was written on.
    QCOMPARE(code->anchors.size(), 2);
    size_t codeSection = 0;
    while (codeSection < built->sections.size() && built->sections[codeSection].islands.empty()) ++codeSection;
    const int codeSource = built->sections[codeSection].sourceStart;
    QCOMPARE(codeSource, int(markdown.indexOf(QStringLiteral("```python"))));
    QCOMPARE(codeSource + code->anchors.at(0).source, int(markdown.indexOf(QStringLiteral("def area"))));
    QCOMPARE(codeSource + code->anchors.at(1).source, int(markdown.indexOf(QStringLiteral("    return"))));
    QCOMPARE(code->sourceStart, 0);
    QCOMPARE(codeSource + code->sourceEnd, int(markdown.indexOf(QStringLiteral("```\n\n")) + 3));

    // Code without a language is plain code.
    const Island *indented = built->island(Island::Code, 1);
    QVERIFY(indented && indented->document);
    QCOMPARE(indented->document->toPlainText(), QStringLiteral("indented code\nsecond line"));
    QCOMPARE(indented->description, QStringLiteral("Code"));
    QCOMPARE(codeInk(indented->document, QStringLiteral("indented")), int(PreviewStyle::Code));

    // The table: its rows and columns, the alignment of each column, the
    // header heavier, and a cell that spans two columns as one cell.
    const Island *island = built->island(Island::Table);
    QVERIFY(island && island->document);
    QVERIFY(island->wraps);
    QCOMPARE(island->headerRows, 1);
    QCOMPARE(island->description, QStringLiteral("Table, 3 columns, 3 rows"));
    QTextTable *table = nullptr;
    for (QTextFrame *frame : island->document->rootFrame()->childFrames())
        if (auto *found = qobject_cast<QTextTable *>(frame)) table = found;
    QVERIFY(table);
    QCOMPARE(table->rows(), 3);
    QCOMPARE(table->columns(), 3);
    const auto cellText = [&](int row, int column) {
        const QTextTableCell cell = table->cellAt(row, column);
        return cell.firstCursorPosition().block().text();
    };
    QCOMPARE(cellText(0, 0), QStringLiteral("Name"));
    QCOMPARE(cellText(1, 1), QStringLiteral("12"));
    QCOMPARE(cellText(1, 2), QStringLiteral("first"));
    QCOMPARE(cellText(2, 0), QStringLiteral("Both columns"));
    QCOMPARE(table->cellAt(2, 0).columnSpan(), 2);
    QCOMPARE(table->cellAt(2, 1).column(), 0);
    QCOMPARE(cellText(2, 2), QStringLiteral("last"));
    const auto alignment = [&](int row, int column) {
        return table->cellAt(row, column).firstCursorPosition().blockFormat().alignment();
    };
    QCOMPARE(alignment(1, 0), Qt::AlignLeft);
    QCOMPARE(alignment(1, 1), Qt::AlignRight);
    QCOMPARE(alignment(1, 2), Qt::AlignHCenter);
    const auto cellFormat = [&](int row, int column) {
        QTextCursor cursor = table->cellAt(row, column).firstCursorPosition();
        cursor.movePosition(QTextCursor::NextCharacter);
        return cursor.charFormat();
    };
    QCOMPARE(cellFormat(0, 0).fontWeight(), int(QFont::DemiBold));
    QVERIFY(cellFormat(1, 0).fontWeight() != int(QFont::DemiBold));
    QVERIFY(cellFormat(1, 2).fontItalic());
    // A cell's text knows where it is in the source.
    bool found = false;
    size_t tableSection = codeSection;
    while (tableSection < built->sections.size()
           && (built->sections[tableSection].islands.empty()
               || built->sections[tableSection].islands.front().kind != Island::Table))
        ++tableSection;
    const int tableSource = built->sections[tableSection].sourceStart;
    for (const Anchor &anchor : island->anchors)
        if (tableSource + anchor.source == int(markdown.indexOf(QStringLiteral("Alpha")))) {
            QCOMPARE(island->document->findBlock(anchor.position).text(), QStringLiteral("Alpha"));
            found = true;
        }
    QVERIFY(found);
}

void previewBuildsNotesCitationsAndContents()
{
    const QString markdown = QStringLiteral(
        "{{TOC}}\n\n"
        "# Notes\n\n"
        "A claim.[^one] Another[^two], and the first again.[^one] An aside[^written in place].\n\n"
        "## Works\n\n"
        "As shown [p. 23][#doe] and in [@roe; @doe].\n\n"
        "A [link to the works][Works] and one [nowhere](#missing).\n\n"
        "[^one]: The first note,\n    on two lines.\n\n"
        "    With a second paragraph.\n"
        "[^two]: The second note.\n\n"
        "[#doe]: Doe, J. *A Book*. 2020.\n"
        "[#roe]: Roe, R. An Article.\n");
    const auto built = build(markdown);
    const QHash<QString, int> names = built->names();

    // The contents: a line for each heading, stepped in by its depth, each
    // a link to its heading.
    const QTextBlock notesLine = built->blockOf(QStringLiteral("Notes"));
    const QTextBlock worksLine = built->blockOf(QStringLiteral("Works"));
    QVERIFY(notesLine.blockFormat().headingLevel() == 0);
    QVERIFY(worksLine.blockFormat().leftMargin() > notesLine.blockFormat().leftMargin());
    QCOMPARE(built->formatOf(QStringLiteral("Notes")).anchorHref(), QStringLiteral("#notes"));
    QCOMPARE(built->formatOf(QStringLiteral("Works")).anchorHref(), QStringLiteral("#works"));
    QVERIFY(names.contains(QStringLiteral("notes")) && names.contains(QStringLiteral("works")));
    QVERIFY(names.value(QStringLiteral("works")) > worksLine.position());
    QVERIFY(built->document.find(QStringLiteral("TOC")).isNull());

    // A reference is the note's number, raised, and leads to the note.
    QVERIFY(built->lines().contains(
        QStringLiteral("A claim.1 Another2, and the first again.1 An aside3.")));
    QTextCursor reference = built->find(QStringLiteral("claim.1"));
    const QTextCharFormat mark = reference.charFormat();
    QCOMPARE(mark.verticalAlignment(), QTextCharFormat::AlignSuperScript);
    QCOMPARE(mark.anchorHref(), QStringLiteral("#fn-1"));
    QCOMPARE(inkOf(mark), int(PreviewStyle::Link));
    // Each reference has a name of its own kind; the first one is where
    // "fnref-1" leads.
    QCOMPARE(names.value(QStringLiteral("fnref-1")), reference.selectionEnd() - 1);
    int references = 0;
    for (const Section &section : built->sections)
        for (const preview::NamedAnchor &name : section.names) references += name.name == QLatin1String("fnref-1");
    QCOMPARE(references, 2);

    // The notes follow the text after a rule, in the order of their
    // numbers, each with its number, its text and the ways back.
    QCOMPARE(built->marks(Mark::NotesRule).size(), 2);
    const QStringList lines = built->lines();
    const QChar gap(0x2002), back(0x21a9);
    const qsizetype first = lines.indexOf(QStringLiteral("1.") + gap + QStringLiteral("The first note, on two lines."));
    QVERIFY2(first > 0, qPrintable(lines.join(u'|')));
    QCOMPARE(lines.at(first + 1), QStringLiteral("With a second paragraph. ") + back + QStringLiteral(" ") + back + QStringLiteral("2"));
    QCOMPARE(lines.at(first + 2), QStringLiteral("2.") + gap + QStringLiteral("The second note. ") + back);
    QCOMPARE(lines.at(first + 3), QStringLiteral("3.") + gap + QStringLiteral("written in place ") + back);
    QCOMPARE(names.value(QStringLiteral("fn-1")), built->blockOf(QStringLiteral("The first note")).position());
    QCOMPARE(names.value(QStringLiteral("fn-3")), built->blockOf(QStringLiteral("written in place")).position());
    QTextCursor ways = built->find(QString(back));
    QCOMPARE(ways.charFormat().anchorHref(), QStringLiteral("#fnref-1"));
    ways = built->find(QString(back) + QStringLiteral("2"));
    QCOMPARE(ways.charFormat().anchorHref(), QStringLiteral("#fnref-1-2"));
    // A note referred to everywhere keeps the ways back to its first places
    // and says how many more there are: its paragraph stays a paragraph.
    {
        QString everywhere;
        const int places = preview::Builder::MaxWaysBack + 50;
        for (int i = 0; i < places; ++i) everywhere += QStringLiteral("A place.[^n]\n\n");
        const auto often = build(everywhere + QStringLiteral("[^n]: The note.\n"));
        const QTextBlock note = often->blockOf(QStringLiteral("The note."));
        QCOMPARE(note.text().count(back), preview::Builder::MaxWaysBack);
        QVERIFY2(note.text().endsWith(QStringLiteral(" and 50 more")), qPrintable(note.text().right(40)));
        QVERIFY(note.text().size() < 1000);
        QCOMPARE(often->formatOf(QStringLiteral("and 50 more")).anchorHref(), QString());
        QCOMPARE(often->find(QString(back) + QString::number(preview::Builder::MaxWaysBack)).charFormat().anchorHref(),
                 QStringLiteral("#fnref-1-%1").arg(preview::Builder::MaxWaysBack));
    }
    // Notes are set smaller than the text.
    QVERIFY(built->formatOf(QStringLiteral("The first note")).intProperty(QTextFormat::FontPixelSize) < 20);
    // The definitions themselves are not part of the text.
    QVERIFY(built->document.find(QStringLiteral("[^one]")).isNull());

    // Citations are numbers in brackets that lead to the bibliography,
    // with their locators; the bibliography lists each work once.
    QVERIFY2(lines.contains(QStringLiteral("As shown [1, p. 23] and in [2; 1].")), qPrintable(lines.join(u'|')));
    QCOMPARE(built->formatOf(QStringLiteral("1, p")).anchorHref(), QStringLiteral("#cite-1"));
    QVERIFY(!built->formatOf(QStringLiteral(", p. 23")).isAnchor());
    QVERIFY(lines.contains(QStringLiteral("[1]") + gap + QStringLiteral("Doe, J. A Book. 2020.")));
    QVERIFY(lines.contains(QStringLiteral("[2]") + gap + QStringLiteral("Roe, R. An Article.")));
    QVERIFY(built->formatOf(QStringLiteral("A Book")).fontItalic());
    QCOMPARE(names.value(QStringLiteral("cite-2")), built->blockOf(QStringLiteral("Roe, R.")).position());

    // A reference to a heading leads to it; one to no heading still reads as a link.
    QCOMPARE(built->formatOf(QStringLiteral("link to the works")).anchorHref(), QStringLiteral("#works"));
    QCOMPARE(built->formatOf(QStringLiteral("nowhere")).anchorHref(), QStringLiteral("#missing"));

    // What cannot be resolved is shown as it was written; a work nothing
    // defines says so, in the colour of what is wrong.
    const auto unresolved = build(QStringLiteral("See[^nothing] and [#nobody] and [%title].\n"));
    QVERIFY2(unresolved->lines().contains(QStringLiteral("See[^nothing] and [#nobody] and [%title].")),
             qPrintable(unresolved->lines().join(u'|')));
    QVERIFY(unresolved->marks(Mark::NotesRule).isEmpty());
    const auto lacking = build(QStringLiteral("See [p. 2][#nobody] and [@nokey] and [@nokey; @other].\n"));
    QVERIFY2(lacking->lines().contains(QStringLiteral("See [p. 2][#nobody] and [@nokey] and [@nokey; @other].")),
             qPrintable(lacking->lines().join(u'|')));
    QCOMPARE(inkOf(lacking->formatOf(QStringLiteral("[p. 2][#nobody]"))), int(PreviewStyle::Danger));
    QVERIFY(lacking->formatOf(QStringLiteral("[p. 2][#nobody]")).toolTip().contains(QStringLiteral("nobody")));
    QCOMPARE(inkOf(lacking->formatOf(QStringLiteral("[@nokey]"))), int(PreviewStyle::Danger));
    QCOMPARE(inkOf(lacking->formatOf(QStringLiteral("@other"))), int(PreviewStyle::Danger));
    QVERIFY(lacking->formatOf(QStringLiteral("@other")).toolTip().contains(QStringLiteral("other")));
    QCOMPARE(inkOf(lacking->formatOf(QStringLiteral("See"))), int(PreviewStyle::NoInk));
    QVERIFY(lacking->marks(Mark::NotesRule).isEmpty());
    // Metadata is not text, and its values stand where they are asked for.
    const auto variables = build(QStringLiteral("---\ntitle: The Title\n---\n\nRead [%title] now.\n"));
    QCOMPARE(variables->lines(), (QStringList{QString(), QStringLiteral("Read The Title now.")}));
}

void previewBuildsPicturesAndTheirStates()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString markdown = QStringLiteral(
        "An image ![a cat](cat.png \"Our cat\") in a line, one still ![coming](pending.png)\n"
        "and one ![lost](broken.png).\n\n"
        "Inline $x^2$ math, $pending$ math and $broken$ math.\n\n"
        "$$\nE = mc^2\n$$\n\n"
        "$$\npending display\n$$\n\n"
        "$$\nbroken display\n$$\n\n"
        "```mermaid\nflowchart LR\n  a --> b\n```\n\n"
        "```mermaid\npending chart\n```\n\n"
        "```mermaid\nbroken chart\n```\n");
    md::Source source;
    source.path = folder.filePath(QStringLiteral("doc.md"));
    source.directory = folder.path();
    const auto built = build(markdown, md::Options::writer(), source);
    const QList<QTextImageFormat> images = imagesIn(built->document);
    // In the main document: the image, the inline formula and the diagram.
    QCOMPARE(images.size(), 3);

    // An image is asked for by the file it names, beside the document, and
    // never drawn wider than the column; its alternative text is told.
    QVERIFY(built->resources.asked.contains(folder.filePath(QStringLiteral("cat.png"))));
    const QTextImageFormat cat = images.at(0);
    QCOMPARE(cat.width(), 640.0);
    QVERIFY(!cat.hasProperty(QTextFormat::ImageHeight));
    QCOMPARE(cat.maximumWidth().type(), QTextLength::PercentageLength);
    QCOMPARE(cat.maximumWidth().rawValue(), 100.0);
    QCOMPARE(cat.toolTip(), QStringLiteral("Our cat"));
    QVERIFY(!built->document.resource(QTextDocument::ImageResource, QUrl(cat.name())).value<QImage>().isNull());
    // The picture, the formula in the line and the diagram are each told
    // to assistive technology with what they show.
    const QList<Mark> described = built->marks(Mark::Image);
    QCOMPARE(described.size(), 3);
    QCOMPARE(described.at(0).text, QStringLiteral("a cat"));
    QCOMPARE(described.at(1).text, QStringLiteral("Formula: formula x^2"));
    // While it loads, and when it cannot be shown, words stand in its place.
    QVERIFY(!built->find(QStringLiteral("Loading image: coming")).isNull());
    QCOMPARE(inkOf(built->formatOf(QStringLiteral("Loading image"))), int(PreviewStyle::Dim));
    QVERIFY(!built->find(QStringLiteral("Image not shown: lost (no such image)")).isNull());
    QCOMPARE(inkOf(built->formatOf(QStringLiteral("Image not shown"))), int(PreviewStyle::Danger));

    // An inline formula is a picture set on the baseline: drawn from the
    // bottom of the line, with room beneath it for the text's descent.
    const QTextImageFormat formula = images.at(1);
    QCOMPARE(formula.verticalAlignment(), QTextCharFormat::AlignBottom);
    QCOMPARE(formula.width(), 30.0);
    QVERIFY(formula.height() >= 20.0);
    QCOMPARE(formula.toolTip(), QStringLiteral("formula x^2"));
    // Its source stands in while it is drawn, and is marked when it cannot be.
    QCOMPARE(inkOf(built->formatOf(QStringLiteral("pending math")).isValid() ? built->formatOf(QStringLiteral("pending")) : QTextCharFormat()),
             int(PreviewStyle::Dim));
    const QTextCharFormat refused = built->formatOf(QStringLiteral("broken"));
    QCOMPARE(inkOf(refused), int(PreviewStyle::Danger));
    QCOMPARE(refused.toolTip(), QStringLiteral("no such math"));

    // A display formula is an island that keeps to the middle of the column.
    const Island *display = built->island(Island::Math);
    QVERIFY(display && display->document);
    QVERIFY(display->centered);
    QCOMPARE(display->description, QStringLiteral("Formula: formula E = mc^2"));
    QCOMPARE(imagesIn(*display->document).size(), 1);
    QCOMPARE(imagesIn(*display->document).first().height(), 20.0);
    // Its source while it is drawn; its source and what went wrong when it cannot be.
    const Island *waiting = built->island(Island::Code, 0);
    QVERIFY(waiting && waiting->document);
    QCOMPARE(waiting->document->toPlainText(), QStringLiteral("pending display"));
    const Island *failed = built->island(Island::Code, 1);
    QVERIFY(failed && failed->document);
    QCOMPARE(failed->document->toPlainText(), QStringLiteral("broken display\nno such math"));

    // A diagram is a picture in the middle of the column, with what it says.
    const QTextImageFormat chart = images.at(2);
    QCOMPARE(chart.width(), 300.0);
    QCOMPARE(chart.maximumWidth().type(), QTextLength::PercentageLength);
    QCOMPARE(described.at(2).text, QStringLiteral("Flowchart: start, end"));
    bool centred = false;
    for (QTextBlock block = built->document.begin(); block.isValid(); block = block.next())
        if (block.text() == QString(QChar::ObjectReplacementCharacter)) centred = block.blockFormat().alignment() & Qt::AlignHCenter;
    QVERIFY(centred);
    const Island *drawing = built->island(Island::Code, 2);
    QVERIFY(drawing && drawing->document);
    QCOMPARE(drawing->document->toPlainText(), QStringLiteral("pending chart\nDrawing the diagram…"));
    const Island *undrawn = built->island(Island::Code, 3);
    QVERIFY(undrawn && undrawn->document);
    QCOMPARE(undrawn->document->toPlainText(), QStringLiteral("broken chart\nno such diagram"));

    // Every section names the pictures it asked for.
    QStringList keys;
    for (const Section &section : built->sections) keys << section.pictures;
    QCOMPARE(keys.size(), 12);
    // Without anything to draw them, formulas and diagrams show their source.
    md::Document tree = md::parse(QStringLiteral("$a$\n\n```mermaid\nchart\n```\n"));
    QTextDocument plain;
    QObject owner;
    preview::BuildOptions options;
    options.islandParent = &owner;
    const std::vector<Section> sections = preview::Builder(tree, testStyle(), nullptr, options).buildAll(&plain);
    QCOMPARE(plain.begin().next().text(), QStringLiteral("a"));
    QCOMPARE(sections.back().islands.size(), size_t(1));
}

void previewShowsRawHtmlUnderThePolicy()
{
    const auto built = build(QStringLiteral(
        "Inline <b>bold</b>, <i>leaning</i>, <u>under</u>, <s>out</s>, <mark>marked</mark>, <kbd>Ctrl</kbd>,\n"
        "H<sub>2</sub>O, x<sup>2</sup>, a <a href=\"https://example.com\" title=\"T\">link</a>, a break<br>here\n"
        "and <span class=\"x\">a span</span> <script>alert(1)</script>.\n\n"
        "<h2>An HTML heading</h2>\n"
        "<p>A   paragraph\n   with folded   space.</p>\n"
        "<blockquote><p>Quoted.</p></blockquote>\n"
        "<ol start=\"4\"><li>four<li>five</ol>\n"
        "<ul><li>bullet</li></ul>\n"
        "<dl><dt>Term<dd>Described</dl>\n"
        "<pre>keep   this\n  as it is</pre>\n"
        "<hr>\n"
        "<table><caption>Sizes</caption><thead><tr><th>A<th>B</tr></thead>\n"
        "<tr><td colspan=\"2\" align=\"center\">both</td></tr><tr><td rowspan=\"2\">tall<td>x</tr><tr><td>y</tr></table>\n"
        "<video src=\"x.mp4\"></video>\n"));
    const QStringList lines = built->lines();
    const QChar gap(0x2002);
    // Supported elements are formatted as their Markdown counterparts are.
    QCOMPARE(built->formatOf(QStringLiteral("bold")).fontWeight(), int(QFont::Bold));
    QVERIFY(built->formatOf(QStringLiteral("leaning")).fontItalic());
    QVERIFY(built->formatOf(QStringLiteral("under")).fontUnderline());
    QVERIFY(built->formatOf(QStringLiteral("out")).fontStrikeOut());
    QCOMPARE(paperOf(built->formatOf(QStringLiteral("marked"))), int(PreviewStyle::MarkPaper));
    QCOMPARE(paperOf(built->formatOf(QStringLiteral("Ctrl"))), int(PreviewStyle::CodePaper));
    QCOMPARE(built->formatOf(QStringLiteral("2O")).verticalAlignment(), QTextCharFormat::AlignSubScript);
    QCOMPARE(built->formatOf(QStringLiteral("link")).anchorHref(), QStringLiteral("https://example.com"));
    QCOMPARE(built->formatOf(QStringLiteral("link")).toolTip(), QStringLiteral("https://example.com\nT"));
    QVERIFY(lines.at(1).contains(QStringLiteral("a break") + QChar::LineSeparator + QStringLiteral("here")));
    // What the policy does not support is shown as the text it is, and
    // nothing of it is acted on.
    QVERIFY2(lines.at(1).contains(QStringLiteral("<span class=\"x\">a span</span> <script>alert(1)</script>.")),
             qPrintable(lines.at(1)));
    QVERIFY(!built->find(QStringLiteral("<video src=\"x.mp4\"></video>")).isNull());

    // Blocks of raw HTML are the blocks they name.
    QCOMPARE(built->blockOf(QStringLiteral("An HTML heading")).blockFormat().headingLevel(), 2);
    QVERIFY2(lines.contains(QStringLiteral("A paragraph with folded space.")), qPrintable(lines.join(u'|')));
    QVERIFY(built->blockOf(QStringLiteral("Quoted.")).blockFormat().leftMargin() > 0);
    QCOMPARE(built->marks(Mark::QuoteBar).size(), 1);
    QVERIFY(lines.contains(QStringLiteral("4.") + gap + QStringLiteral("four")));
    QVERIFY(lines.contains(QStringLiteral("5.") + gap + QStringLiteral("five")));
    QVERIFY(lines.contains(QChar(0x2022) + QString(gap) + QStringLiteral("bullet")));
    QCOMPARE(built->formatOf(QStringLiteral("Term")).fontWeight(), int(QFont::DemiBold));
    QVERIFY(built->blockOf(QStringLiteral("Described")).blockFormat().leftMargin() > 0);
    QCOMPARE(built->marks(Mark::Rule).size(), 1);
    const Island *pre = built->island(Island::Code);
    QVERIFY(pre && pre->document);
    QCOMPARE(pre->document->toPlainText(), QStringLiteral("keep   this\n  as it is"));

    // A table with its caption, its header row and cells that span.
    QVERIFY(lines.contains(QStringLiteral("Sizes")));
    const Island *island = built->island(Island::Table);
    QVERIFY(island && island->document);
    QCOMPARE(island->headerRows, 1);
    QTextTable *table = nullptr;
    for (QTextFrame *frame : island->document->rootFrame()->childFrames())
        if (auto *found = qobject_cast<QTextTable *>(frame)) table = found;
    QVERIFY(table);
    QCOMPARE(table->rows(), 4);
    QCOMPARE(table->columns(), 2);
    QCOMPARE(table->cellAt(1, 0).columnSpan(), 2);
    QCOMPARE(table->cellAt(1, 0).firstCursorPosition().blockFormat().alignment(), Qt::AlignHCenter);
    QCOMPARE(table->cellAt(2, 0).rowSpan(), 2);
    QCOMPARE(table->cellAt(3, 1).firstCursorPosition().block().text(), QStringLiteral("y"));
    QCOMPARE(table->cellAt(2, 1).firstCursorPosition().block().text(), QStringLiteral("x"));
}

void previewPartsKeepTheirKeysAndSources()
{
    const QString before = QStringLiteral(
        "# Title\n\nFirst paragraph with a note.[^n]\n\nSecond paragraph.\n\n- a\n- b\n\n[^n]: The note.\n");
    const md::Document first = md::parse(before);
    const std::vector<Part> parts = preview::parts(first);
    // The blocks that read as something, then a rule and the note.
    QCOMPARE(parts.size(), size_t(6));
    QCOMPARE(parts[0].kind, Part::Block);
    QCOMPARE(parts[4].kind, Part::NotesRule);
    QCOMPARE(parts[5].kind, Part::Note);

    // An edit in one block changes that block's key and no other's, though
    // every block after it has moved in the source.
    QString after = before;
    after.replace(QStringLiteral("First paragraph"), QStringLiteral("The first paragraph"));
    const md::Document second = md::parse(after);
    const std::vector<Part> edited = preview::parts(second);
    QCOMPARE(edited.size(), parts.size());
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i == 1) QVERIFY(edited[i].key != parts[i].key);
        else QCOMPARE(edited[i].key, parts[i].key);
    }
    // What a block reads as can change without its own text changing: a
    // note that is numbered otherwise, a heading whose slug moves on.
    const md::Document renumbered = md::parse(QStringLiteral("Early.[^m]\n\n") + before + QStringLiteral("[^m]: Earlier.\n"));
    const std::vector<Part> shifted = preview::parts(renumbered);
    QCOMPARE(shifted.size(), size_t(8));
    QCOMPARE(shifted[1].key, parts[0].key);
    QVERIFY(shifted[2].key != parts[1].key);
    QCOMPARE(shifted[3].key, parts[2].key);
    // The same text written otherwise is another key, since its offsets differ.
    QVERIFY(preview::parts(md::parse(QStringLiteral("*a*\n")))[0].key != preview::parts(md::parse(QStringLiteral("_a_\n")))[0].key);

    // Each section says what part of the source it is, and each run of its
    // text where it came from.
    const auto built = build(before);
    QCOMPARE(built->sections.size(), size_t(6));
    QCOMPARE(built->sections[0].sourceStart, 0);
    QCOMPARE(built->sections[0].sourceEnd, 7);
    QCOMPARE(built->sections[1].sourceStart, int(before.indexOf(QStringLiteral("First"))));
    QCOMPARE(built->sections[3].sourceStart, int(before.indexOf(QStringLiteral("- a"))));
    QCOMPARE(built->sections[4].sourceStart, -1);
    QCOMPARE(built->sections[5].sourceStart, int(before.indexOf(QStringLiteral("[^n]:"))));
    int total = 1;
    for (const Section &section : built->sections) total += section.length;
    QCOMPARE(total, built->document.characterCount());
    // The second paragraph: its block starts where the paragraph does, and
    // its text runs beside its source.
    const Section &paragraph = built->sections[2];
    QCOMPARE(paragraph.anchors.size(), 2);
    QVERIFY(paragraph.anchors.at(0).block);
    QCOMPARE(paragraph.anchors.at(0).position, 0);
    QCOMPARE(paragraph.anchors.at(0).source, 0);
    QCOMPARE(paragraph.anchors.at(1).length, int(QStringLiteral("Second paragraph.").size()));
    QCOMPARE(built->document.findBlock(built->startOf(2)).text(), QStringLiteral("Second paragraph."));
    // In the heading, the text starts after its mark.
    QCOMPARE(built->sections[0].anchors.at(1).source, 2);
    // List items: each block anchor is the item's paragraph, after the marker.
    const Section &list = built->sections[3];
    QList<int> blockSources;
    for (const Anchor &anchor : list.anchors)
        if (anchor.block) blockSources << anchor.source;
    QCOMPARE(blockSources, (QList<int>{2, 6}));

    // An included file's text has no place in the document's own source:
    // the line that includes it stands for all of it.
    QTemporaryDir folder;
    writeFile(folder.filePath(QStringLiteral("chapter.md")), "# Chapter\n\nIts text.\n");
    md::FileIncludeResolver resolver;
    md::Source source;
    source.path = folder.filePath(QStringLiteral("book.md"));
    source.directory = folder.path();
    source.resolver = &resolver;
    const QString book = QStringLiteral("Opening.\n\nchapter.md\n\nmissing.md\n\nClosing.\n");
    const auto included = build(book, md::Options::writer(), source);
    QCOMPARE(included->lines(), (QStringList{QString(), QStringLiteral("Opening."), QStringLiteral("Chapter"),
                                             QStringLiteral("Its text."), QStringLiteral("missing.md"),
                                             QStringLiteral("Closing.")}));
    QCOMPARE(included->sections.size(), size_t(4));
    const Section &chapter = included->sections[1];
    QCOMPARE(chapter.sourceStart, int(book.indexOf(QStringLiteral("chapter.md"))));
    QCOMPARE(chapter.anchors.size(), 1);
    QCOMPARE(chapter.anchors.first().source, 0);
    QVERIFY(included->names().contains(QStringLiteral("chapter")));
    // A line that could not be included is shown in the danger ink, saying why.
    QCOMPARE(inkOf(included->formatOf(QStringLiteral("missing.md"))), int(PreviewStyle::Danger));
    QVERIFY(included->formatOf(QStringLiteral("missing.md")).toolTip().contains(QStringLiteral("does not exist")));
}

void previewBuildsInPlaceForPaper()
{
    // Exports and printing build the same document without islands, in a
    // style of their own.
    const QString markdown = QStringLiteral(
        "# Title\n\nText before.\n\n"
        "| A | B |\n|---|---|\n| 1 | 2 |\n\n"
        "Between.\n\n"
        "```c\nint main() { return 0; }\nint x;\n```\n\n"
        "$$\nx\n$$\n\n"
        "- [x] done\n\n"
        "Text after.[^n]\n\n[^n]: Note.\n");
    md::Document tree = md::parse(markdown);
    TestResources resources;
    preview::BuildOptions options;
    options.islands = false;
    options.interactive = false;
    QTextDocument document;
    const PreviewStyle paper = PreviewStyle::paper();
    const std::vector<Section> sections = preview::Builder(tree, paper, &resources, options).buildAll(&document);
    for (const Section &section : sections) QVERIFY(section.islands.empty());
    // The table is in the document itself, between its neighbours, with no
    // empty line left around it.
    QTextTable *table = nullptr;
    for (QTextFrame *frame : document.rootFrame()->childFrames())
        if (auto *found = qobject_cast<QTextTable *>(frame)) table = found;
    QVERIFY(table);
    QCOMPARE(table->rows(), 2);
    QCOMPARE(table->cellAt(1, 1).firstCursorPosition().block().text(), QStringLiteral("2"));
    QStringList lines;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) lines << block.text();
    const QChar gap(0x2002);
    const qsizetype between = lines.indexOf(QStringLiteral("Between."));
    QVERIFY2(between > 0, qPrintable(lines.join(u'|')));
    QCOMPARE(lines.at(between - 1), QStringLiteral("2"));
    // Code is one block on the code tint, its lines kept.
    const QString code = QStringLiteral("int main() { return 0; }") + QChar::LineSeparator + QStringLiteral("int x;");
    QCOMPARE(lines.at(between + 1), code);
    const QTextBlock codeBlock = document.find(QStringLiteral("int main")).block();
    QCOMPARE(codeBlock.blockFormat().background().color(), paper.paper(PreviewStyle::CodePaper));
    QTextCursor keyword = document.find(QStringLiteral("return"));
    QCOMPARE(inkOf(keyword.charFormat()), int(PreviewStyle::Keyword));
    QCOMPARE(keyword.charFormat().foreground().color(), paper.ink(PreviewStyle::Keyword));
    // The formula is a picture in a block of its own.
    QCOMPARE(lines.at(between + 2), QString(QChar::ObjectReplacementCharacter));
    QVERIFY(lines.contains(QChar::ObjectReplacementCharacter + QString(gap) + QStringLiteral("done")));
    // Nothing on paper can be followed.
    for (const QTextImageFormat &image : imagesIn(document)) QVERIFY(!image.isAnchor());
    QVERIFY(lines.contains(QStringLiteral("Text after.1")));
    QVERIFY(lines.contains(QStringLiteral("1.") + gap + QStringLiteral("Note.")));
    QCOMPARE(document.defaultFont().pixelSize(), 16);

    // The paper style never follows the desktop, and reads as print does.
    QCOMPARE(paper.background, QColor(Qt::white));
    QVERIFY(PreviewStyle::contrast(paper.text, paper.background) >= 12);
    for (int ink = PreviewStyle::Heading; ink < PreviewStyle::InkCount; ++ink) {
        if (ink == PreviewStyle::Faint) continue;
        const QColor behind = ink >= PreviewStyle::Keyword || ink == PreviewStyle::Code ? paper.paper(PreviewStyle::CodePaper)
                              : ink == PreviewStyle::Marked ? paper.paper(PreviewStyle::MarkPaper) : paper.background;
        QVERIFY2(PreviewStyle::contrast(paper.ink(PreviewStyle::Ink(ink)), behind) >= 4.5, qPrintable(QString::number(ink)));
    }
}

void previewStyleIsReadableInAnyTheme()
{
    // Light, dark, and themes whose own colours are too close to read.
    struct Colors {
        const char *mode, *background, *foreground, *accent;
    };
    const Colors themes[] = {
        {"light", "#F8F1E3", "#342B27", "#A7353A"}, {"dark", "#1a1b26", "#a9b1d6", "#7aa2f7"},
        {"light", "#ffffff", "#000000", "#ffff00"}, {"dark", "#000000", "#ffffff", "#000080"},
        {"light", "#eeeeee", "#bbbbbb", "#dddddd"}, {"dark", "#222222", "#555555", "#333333"},
        {"dark", "#2e3440", "#d8dee9", "#88c0d0"},  {"light", "#fdf6e3", "#657b83", "#268bd2"},
    };
    for (const Colors &colors : themes) {
        QTemporaryDir folder;
        writeTheme(folder.path(), colors.mode, colors.background, colors.foreground, colors.accent);
        const Theme theme(folder.path());
        const PreviewStyle style = PreviewStyle::screen(theme, QStringLiteral("serif"), 18, 1.6);
        const QByteArray name = QByteArray(colors.background) + " " + colors.foreground;
        QCOMPARE(style.background, theme.background());
        QCOMPARE(style.dark, theme.dark());
        // Text, headings, links, dimmed text and what failed read on the page.
        QVERIFY2(PreviewStyle::contrast(style.text, style.background) >= 4.5, name);
        for (const PreviewStyle::Ink ink : {PreviewStyle::Heading, PreviewStyle::Dim, PreviewStyle::Link, PreviewStyle::Danger})
            QVERIFY2(PreviewStyle::contrast(style.ink(ink), style.background) >= 4.5, name + " ink " + QByteArray::number(ink));
        // Code reads on its tint, whatever it is coloured as, and marked text on the marker.
        const QColor tint = style.paper(PreviewStyle::CodePaper);
        QVERIFY(tint != style.background);
        for (int ink = PreviewStyle::Keyword; ink < PreviewStyle::InkCount; ++ink)
            QVERIFY2(PreviewStyle::contrast(style.ink(PreviewStyle::Ink(ink)), tint) >= 4.5, name + " code " + QByteArray::number(ink));
        QVERIFY2(PreviewStyle::contrast(style.ink(PreviewStyle::Code), tint) >= 4.5, name);
        QVERIFY2(PreviewStyle::contrast(style.ink(PreviewStyle::Marked), style.paper(PreviewStyle::MarkPaper)) >= 4.5, name);
        // A colour that reads already is the theme's own.
        if (PreviewStyle::contrast(theme.foreground(), theme.background()) >= 4.5) QCOMPARE(style.text, theme.foreground());
        if (PreviewStyle::contrast(theme.accent(), theme.background()) >= 4.5) QCOMPARE(style.ink(PreviewStyle::Link), theme.accent());
    }
    // Typography and colours are told apart: a theme change is colours alone.
    QTemporaryDir folder;
    writeTheme(folder.path(), "light", "#F8F1E3", "#342B27", "#A7353A");
    const Theme light(folder.path());
    PreviewStyle a = PreviewStyle::screen(light, QStringLiteral("serif"), 18, 1.6);
    PreviewStyle b = PreviewStyle::screen(light, QStringLiteral("serif"), 20, 1.6);
    QVERIFY(a.sameColors(b) && !a.sameTypography(b));
    QTemporaryDir other;
    writeTheme(other.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    const Theme dark(other.path());
    const PreviewStyle c = PreviewStyle::screen(dark, QStringLiteral("serif"), 18, 1.6);
    QVERIFY(a.sameTypography(c) && !a.sameColors(c));
    QVERIFY(b.withColorsOf(c).sameColors(c) && b.withColorsOf(c).sameTypography(b));
    // Sizes step down with the level of a heading, from twice the text's.
    QCOMPARE(a.headingSize(1), 36.0);
    for (int level = 2; level <= 6; ++level) QVERIFY(a.headingSize(level) < a.headingSize(level - 1));
    QCOMPARE(a.lineSpacing(18), 29.0);
    // The reading face is the first installed of those made for reading,
    // and the desktop's serif when none is.
    QCOMPARE(PreviewStyle::defaultFamily({QStringLiteral("Noto Sans"), QStringLiteral("Liberation Serif"), QStringLiteral("Literata")}),
             QStringLiteral("Literata"));
    QCOMPARE(PreviewStyle::defaultFamily({QStringLiteral("Noto Sans")}), QStringLiteral("serif"));
    QVERIFY(!PreviewStyle::defaultFamily().isEmpty());
}

void codeIsColouredByLanguage()
{
    using CodeHighlight::Span;
    CodeHighlight::prepare();
    const QString python = QStringLiteral("def f(x):\n    return \"text\"  # note\n");
    const QList<Span> spans = CodeHighlight::spans(QStringLiteral("python"), python);
    const auto inkAt = [&](const QList<Span> &list, const QString &code, const QString &word) {
        const int at = int(code.indexOf(word));
        for (const Span &span : list)
            if (span.start <= at && at < span.start + span.length) return int(span.ink);
        return int(PreviewStyle::Code);
    };
    QCOMPARE(inkAt(spans, python, QStringLiteral("def")), int(PreviewStyle::Keyword));
    QCOMPARE(inkAt(spans, python, QStringLiteral("return")), int(PreviewStyle::Keyword));
    QCOMPARE(inkAt(spans, python, QStringLiteral("\"text\"")), int(PreviewStyle::String));
    QCOMPARE(inkAt(spans, python, QStringLiteral("# note")), int(PreviewStyle::Comment));
    QCOMPARE(inkAt(spans, python, QStringLiteral("x)")), int(PreviewStyle::Code));
    // Spans are in order, inside the text, and never overlap.
    int end = 0;
    for (const Span &span : spans) {
        QVERIFY(span.start >= end && span.length > 0);
        end = span.start + span.length;
    }
    QVERIFY(end <= python.size());
    // Languages are found by name, by the names people use, and by extension.
    for (const char *name : {"Python", "py", "c++", "cpp", "js", "javascript", "sh", "bash", "shell", "rust", "rs",
                             "json", "yaml", "yml", "html", "css", "go", "java", "ruby", "sql", "toml", "diff"})
        QVERIFY2(CodeHighlight::knows(QString::fromLatin1(name)), name);
    QVERIFY(!CodeHighlight::knows(QStringLiteral("no-such-language")));
    QVERIFY(!CodeHighlight::knows(QStringLiteral("text")));
    QVERIFY(CodeHighlight::spans(QStringLiteral("no-such-language"), python).isEmpty());
    QVERIFY(CodeHighlight::spans(QString(), python).isEmpty());
    // The language is the first word of a fence's info string.
    QCOMPARE(CodeHighlight::languageOf(QStringLiteral(" Python title=\"x\" ")), QStringLiteral("python"));
    QCOMPARE(CodeHighlight::languageOf(QStringLiteral("{.rust}")), QStringLiteral("rust"));
    QCOMPARE(CodeHighlight::languageOf(QStringLiteral("language-js")), QStringLiteral("js"));
    QCOMPARE(CodeHighlight::languageOf(QStringLiteral("c,linenos")), QStringLiteral("c"));
    QCOMPARE(CodeHighlight::languageOf(QString()), QString());
    // The same code is read once, and a long text in reasonable time.
    QString large;
    for (int i = 0; i < 2000; ++i) large += QStringLiteral("int value%1 = compute(%1, \"s\"); // c\n").arg(i);
    QElapsedTimer timer;
    timer.start();
    const QList<Span> first = CodeHighlight::spans(QStringLiteral("c"), large);
    const qint64 cold = timer.restart();
    const QList<Span> again = CodeHighlight::spans(QStringLiteral("c"), large);
    const qint64 warm = timer.elapsed();
    QVERIFY(first.size() > 2000);
    QCOMPARE(again.size(), first.size());
    QVERIFY2(cold < timeLimit(3000), qPrintable(QString::number(cold)));
    QVERIFY2(warm <= timeLimit(20), qPrintable(QString::number(warm)));
    // Code past the limit is left plain rather than read.
    QVERIFY(CodeHighlight::spans(QStringLiteral("c"), QString(int(CodeHighlight::MaxLength) + 1, u'x')).isEmpty());
}

namespace {
const QString essay = QStringLiteral(
    "# A room for thought\n\n"
    "There is a particular quiet before the first sentence. The page asks for nothing.\n\n"
    "## Paying attention\n\n"
    "A clear page. A steady rhythm. A little space to notice **what matters**.\n\n"
    "> Begin with what is already here.\n\n"
    "- [x] Find the thought\n- [ ] Give it room to grow\n\n"
    "| Step | What | When |\n|:-----|:-----|-----:|\n| One | Sit down | 9:00 |\n| Two | Write a line | 9:05 |\n\n"
    "```python\ndef begin(page):\n    return page.first_line()  # the rest can wait\n```\n\n"
    "The rest can wait.[^1]\n\n"
    "***\n\n"
    "[^1]: Or so it is said.\n");
} // namespace

void viewsShowTheTextThePreviewOrBoth()
{
    PreviewWindow w(essay);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    QQuickItem *divider = w.shell.item(QStringLiteral("viewDivider"));
    QQuickItem *names = w.shell.item(QStringLiteral("paneNames"));
    QVERIFY(divider && names);

    // A document opens as its source alone, and nothing is parsed for a
    // preview nobody sees.
    QCOMPARE(w.tabs()->activeMode(), TabModel::Source);
    QVERIFY(w.editorPane->isVisible());
    QVERIFY(!w.previewPane->isVisible());
    QVERIFY(!divider->isVisible());
    QVERIFY(!w.preview->active());
    QTest::qWait(Preview::PauseMs + 150);
    QCOMPARE(w.preview->updates(), 0);
    QCOMPARE(w.preview->textDocument()->blockCount(), 1);

    // Split: the text and its preview side by side, a divider between them.
    w.run("viewSplit");
    QCOMPARE(w.tabs()->activeMode(), TabModel::Split);
    QTRY_VERIFY(w.previewPane->isVisible());
    QVERIFY(w.editorPane->isVisible());
    QVERIFY(divider->isVisible());
    QVERIFY(names->isVisible());
    QVERIFY(w.settled());
    QVERIFY(w.lines().contains(QStringLiteral("A room for thought")));
    QVERIFY(w.lines().contains(QStringLiteral("The rest can wait.1")));
    const qreal room = w.editorPane->parentItem()->width();
    QVERIFY(qAbs(w.editorPane->width() - room / 2) <= divider->width());
    QCOMPARE(w.previewPane->x() + w.previewPane->width(), room);
    QVERIFY(w.previewPane->x() >= w.editorPane->width());
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(w.window, &problems), qPrintable(problems.join("; ")));
    w.shot("split");

    // Reading: the preview alone, its column in the middle and no wider
    // than the line length allows.
    w.run("viewReading");
    QCOMPARE(w.tabs()->activeMode(), TabModel::Reading);
    QTRY_VERIFY(!w.editorPane->isVisible());
    QVERIFY(w.previewPane->isVisible());
    QVERIFY(!divider->isVisible());
    QVERIFY(!names->isVisible());
    QCOMPARE(w.previewPane->width(), room);
    QVERIFY(w.surface->width() < room * 0.8);
    QVERIFY(qAbs(w.surface->x() - (room - w.surface->width()) / 2) <= 1);
    QVERIFY2(everyVisibleItemFitsWithin(w.window, &problems), qPrintable(problems.join("; ")));
    // What formats the text needs the text in view: the toolbar is hidden,
    // and the room it takes is kept, so that the preview stays where it is.
    QQuickItem *toolbar = w.shell.item(QStringLiteral("toolbar"));
    QQuickItem *bold = w.shell.item(QStringLiteral("toolbar-bold"));
    QVERIFY(toolbar && bold);
    QVERIFY(!toolbar->isVisible());
    QVERIFY(!bold->isVisible());
    QVERIFY(toolbar->height() > 0);
    QVERIFY(w.shell.settings()->showToolbar());
    const qreal readingTop = w.previewPane->mapToScene(QPointF(0, 0)).y();
    w.shot("reading");

    // Back to the source, where nothing is built while the preview is out of sight.
    w.run("viewSource");
    QTRY_VERIFY(!w.previewPane->isVisible());
    QVERIFY(w.editorPane->isVisible());
    QCOMPARE(w.editorPane->width(), room);
    QVERIFY(toolbar->isVisible());
    QVERIFY(bold->isVisible() && bold->isEnabled());
    QCOMPARE(w.editorPane->mapToScene(QPointF(0, 0)).y(), readingTop);
    const int updates = w.preview->updates();
    w.document->buffer()->replace({2, 0}, QStringLiteral("x"));
    QTest::qWait(Preview::PauseMs + 150);
    QCOMPARE(w.preview->updates(), updates);
    // Shown again, it shows the text as it is now.
    w.run("togglePreview");
    QCOMPARE(w.tabs()->activeMode(), TabModel::Split);
    QVERIFY(w.settled());
    QVERIFY(w.lines().contains(QStringLiteral("xA room for thought")));
    w.run("togglePreview");
    QCOMPARE(w.tabs()->activeMode(), TabModel::Source);

    // Each view has its keys, and the header its button.
    QTest::keyClick(w.window, Qt::Key_3, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(w.tabs()->activeMode(), TabModel::Reading);
    QTest::keyClick(w.window, Qt::Key_2, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(w.tabs()->activeMode(), TabModel::Split);
    QTest::keyClick(w.window, Qt::Key_1, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(w.tabs()->activeMode(), TabModel::Source);
    QTest::keyClick(w.window, Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
    QTRY_COMPARE(w.tabs()->activeMode(), TabModel::Split);
    QQuickItem *button = w.shell.item(QStringLiteral("previewButton"));
    QVERIFY(button);
    QCOMPARE(button->property("text").toString(), QStringLiteral("Hide preview"));
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, centreOf(button));
    QTRY_COMPARE(w.tabs()->activeMode(), TabModel::Source);
    QCOMPARE(button->property("text").toString(), QStringLiteral("Show preview"));
    // The keys stay with the document through every change of view.
    QCOMPARE(w.window->activeFocusItem(), w.editor);
    w.run("viewReading");
    QTRY_COMPARE(w.window->activeFocusItem(), w.surface);
    w.run("viewSplit");
    QTRY_COMPARE(w.window->activeFocusItem(), w.editor);
}

namespace {
// Numbered paragraphs under a few headings: long enough to scroll.
QString longText(int paragraphs)
{
    QString text;
    for (int i = 0; i < paragraphs; ++i) {
        if (i % 10 == 0) text += QStringLiteral("## Section %1\n\n").arg(i / 10);
        text += QStringLiteral("Paragraph %1 has a few words of its own, enough to fill a line or two of the page when it is set.\n\n").arg(i);
    }
    return text;
}

qreal contentY(QQuickItem *flickable) { return flickable->property("contentY").toReal(); }

// The text of the block at the top of the preview's view.
QString topLine(const PreviewWindow &w)
{
    QTextDocument *document = w.preview->textDocument();
    const int position = document->documentLayout()->hitTest(QPointF(0, w.preview->viewTop() + 2), Qt::FuzzyHit);
    return document->findBlock(position).text();
}
} // namespace

void previewFollowsEditsWithoutRebuilding()
{
    PreviewWindow w(longText(120));
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewSplit");
    QVERIFY(w.settled());
    QTextDocument *document = w.preview->textDocument();
    const int sections = int(w.preview->sections().size());
    QCOMPARE(sections, 120 + 12);
    QCOMPARE(w.preview->sectionsBuilt(), sections);
    const QString source = w.document->buffer()->text();

    // A selection in the preview, and a view scrolled into the middle.
    QTextCursor found = document->find(QStringLiteral("Paragraph 60 has"));
    QVERIFY(!found.isNull());
    QMetaObject::invokeMethod(w.surface, "select", Q_ARG(int, found.selectionStart()), Q_ARG(int, found.selectionEnd()));
    QCOMPARE(w.surface->property("selectedText").toString(), QStringLiteral("Paragraph 60 has"));
    // The editor does not lead while the preview is scrolled by itself.
    w.shell.settings()->setScrollSync(false);
    const qreal middle = document->documentLayout()->blockBoundingRect(found.block()).top();
    w.flickable->setProperty("contentY", w.surface->y() + middle);
    const QString top = topLine(w);
    QCOMPARE(top, found.block().text());
    const qreal before = contentY(w.flickable);

    // An edit above: one part is built again, the text that was at the top
    // of the view is still there, and the selection still holds its words.
    int built = w.preview->sectionsBuilt();
    QSignalSpy updated(w.preview, &Preview::updated);
    const int early = int(source.indexOf(QStringLiteral("Paragraph 3 has")));
    w.document->buffer()->replace({early, 0}, QStringLiteral("An added sentence that makes this paragraph longer than the others around it, by a line or more. "));
    QVERIFY(!w.preview->current());
    QVERIFY(w.settled());
    QCOMPARE(updated.count(), 1);
    QCOMPARE(w.preview->sectionsBuilt(), built + 1);
    QCOMPARE(int(w.preview->sections().size()), sections);
    QCOMPARE(topLine(w), top);
    QVERIFY(contentY(w.flickable) > before);
    QCOMPARE(w.surface->property("selectedText").toString(), QStringLiteral("Paragraph 60 has"));
    QVERIFY(!document->find(QStringLiteral("An added sentence")).isNull());

    // An edit below the view moves nothing in it.
    const qreal held = contentY(w.flickable);
    built = w.preview->sectionsBuilt();
    const QString now = w.document->buffer()->text();
    w.document->buffer()->replace({int(now.indexOf(QStringLiteral("Paragraph 110 has"))), 9}, QStringLiteral("Passage"));
    QVERIFY(w.settled());
    QCOMPARE(w.preview->sectionsBuilt(), built + 1);
    QCOMPARE(contentY(w.flickable), held);
    QCOMPARE(topLine(w), top);
    QVERIFY(!document->find(QStringLiteral("Passage 110 has")).isNull());

    // A paragraph taken out and one put in: only what changed is built.
    built = w.preview->sectionsBuilt();
    QString text = w.document->buffer()->text();
    const int gone = int(text.indexOf(QStringLiteral("Paragraph 115 has")));
    const int goneEnd = int(text.indexOf(QStringLiteral("Paragraph 116 has")));
    w.document->buffer()->replace({gone, goneEnd - gone}, QString());
    QVERIFY(w.settled());
    QCOMPARE(w.preview->sectionsBuilt(), built);
    QCOMPARE(int(w.preview->sections().size()), sections - 1);
    QVERIFY(document->find(QStringLiteral("Paragraph 115 has")).isNull());
    w.document->buffer()->replace({gone, 0}, QStringLiteral("A new paragraph.\n\nAnd another.\n\n"));
    QVERIFY(w.settled());
    QCOMPARE(w.preview->sectionsBuilt(), built + 2);
    QCOMPARE(int(w.preview->sections().size()), sections + 1);
    // What the document holds is what a fresh build of the same text holds.
    const auto fresh = build(w.document->buffer()->text());
    QCOMPARE(w.lines(), fresh->lines());
    // Undo is an edit like any other.
    w.document->buffer()->undo();
    QVERIFY(w.settled());
    QCOMPARE(int(w.preview->sections().size()), sections - 1);

    // Steady typing is followed while it goes on, not only once it stops,
    // though not key by key. The keys come as fast as the machine is given
    // time to read between them.
    const int updatesBefore = w.preview->updates();
    QElapsedTimer typing;
    typing.start();
    int typed = 0;
    while (typing.elapsed() < timeLimit(3 * Preview::MaxWaitMs)) {
        w.document->buffer()->replace({0, 0}, QStringLiteral("x"), EditKind::Typing);
        ++typed;
        QTest::qWait(int(timeLimit(20)));
    }
    QVERIFY(w.preview->updates() - updatesBefore >= 2);
    QVERIFY2(w.preview->updates() - updatesBefore <= typed / 8,
             qPrintable(QStringLiteral("%1 updates for %2 keys").arg(w.preview->updates() - updatesBefore).arg(typed)));
    QVERIFY(w.settled());
    QVERIFY(w.lines().at(1).startsWith(QString(typed, u'x')));
}

void staleResultsNeverReplaceNewerText()
{
    PreviewWindow w(longText(40));
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewSplit");
    QVERIFY(w.settled());
    // Reading the text takes long enough to be overtaken.
    Preview::setWorkerDelay(200);
    const auto prompt = qScopeGuard([] { Preview::setWorkerDelay(0); });
    QStringList seen;
    QObject::connect(w.preview, &Preview::updated, w.preview, [&] { seen << w.lines().at(1); });
    const int stale = w.preview->staleResults();
    const int updates = w.preview->updates();

    // An edit, and then, while the worker is reading the text of that
    // edit, another: the first result is for text that is no longer.
    w.document->buffer()->replace({3, 0}, QStringLiteral("First "));
    QVERIFY(QTest::qWaitFor([&] { return w.preview->parsing(); }, 5000));
    w.document->buffer()->replace({3, 0}, QStringLiteral("Second "));
    QVERIFY(!w.preview->current());
    QVERIFY(w.settled());
    QCOMPARE(w.preview->staleResults(), stale + 1);
    QCOMPARE(w.preview->updates(), updates + 1);
    // What was shown went from the text before both edits to the text after both.
    QCOMPARE(seen, QStringList{QStringLiteral("Second First Section 0")});
    QCOMPARE(w.preview->shownRevision(), w.document->buffer()->revision());

    // Another document in front: what was read for the one before is not shown.
    Document *other = w.shell.backend->store()->newDraft();
    other->buffer()->replace({0, 0}, QStringLiteral("# Another\n\nWith its own text.\n"));
    w.document->buffer()->replace({3, 0}, QStringLiteral("Third "));
    QVERIFY(QTest::qWaitFor([&] { return w.preview->parsing(); }, 5000));
    w.tabs()->openDocument(other);
    QCOMPARE(w.preview->document(), other);
    QVERIFY(w.settled());
    QCOMPARE(w.lines(), (QStringList{QString(), QStringLiteral("Another"), QStringLiteral("With its own text.")}));
}

// Keys that come quicker than the text is read overtake every read; the
// preview still shows how far each read took it once typing has gone on
// for MaxWaitMs, newer each time, and the text as it is once typing stops.
void typingQuickerThanReadsIsFollowed()
{
    PreviewWindow w(longText(40));
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewSplit");
    QVERIFY(w.settled());
    Preview::setWorkerDelay(60);
    const auto prompt = qScopeGuard([] { Preview::setWorkerDelay(0); });
    QList<Revision> shown;
    QObject::connect(w.preview, &Preview::updated, w.preview, [&] { shown << w.preview->shownRevision(); });

    QElapsedTimer typing;
    typing.start();
    int typed = 0;
    while (typing.elapsed() < 3 * Preview::MaxWaitMs + 100) {
        w.document->buffer()->replace({0, 0}, QStringLiteral("x"), EditKind::Typing);
        ++typed;
        QTest::qWait(20);
    }
    QVERIFY2(shown.size() >= 2, qPrintable(QStringLiteral("%1 updates for %2 keys").arg(shown.size()).arg(typed)));
    QVERIFY(std::is_sorted(shown.cbegin(), shown.cend()));
    QCOMPARE(std::adjacent_find(shown.cbegin(), shown.cend()), shown.cend());
    QVERIFY(shown.constLast() < w.document->buffer()->revision());
    QVERIFY(!w.preview->current());

    QVERIFY(w.settled());
    QCOMPARE(w.preview->shownRevision(), w.document->buffer()->revision());
    QVERIFY(w.lines().at(1).startsWith(QString(typed, u'x')));
}

void previewSaysWhenItIsBehind()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    PreviewWindow w(longText(40));
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewSplit");
    QVERIFY(w.settled());
    QQuickItem *mark = w.shell.item(QStringLiteral("previewUpdating"));
    QVERIFY(mark);
    QVERIFY(!mark->isVisible());
    // Reading the text takes long: what the preview showed stays, and after
    // a moment it says, quietly, that it is not the text as it is now.
    Preview::setWorkerDelay(1500);
    const auto prompt = qScopeGuard([] { Preview::setWorkerDelay(0); });
    w.document->buffer()->replace({3, 0}, QStringLiteral("First "));
    QVERIFY(!w.preview->current());
    QVERIFY(!mark->isVisible());
    QTRY_VERIFY(mark->isVisible());
    QVERIFY(!w.preview->current());
    QCOMPARE(mark->property("text").toString(), QStringLiteral("Updating…"));
    QVERIFY2(unnamedAccessibleItems(w.window).isEmpty(), qPrintable(unnamedAccessibleItems(w.window).join(", ")));
    QVERIFY(everyVisibleItemFitsWithin(w.window));
    // Once it has caught up, the mark goes.
    QVERIFY(w.settled());
    QVERIFY(!mark->isVisible());
    QCOMPARE(w.lines().at(1), QStringLiteral("First Section 0"));
}

namespace {
// The middle of a text of the preview's surface, in the window.
QPoint placeOf(const PreviewWindow &w, const QString &text)
{
    const QTextCursor found = w.preview->textDocument()->find(text);
    if (found.isNull()) return {};
    QRectF first, last;
    QMetaObject::invokeMethod(w.surface, "positionToRectangle", Q_RETURN_ARG(QRectF, first), Q_ARG(int, found.selectionStart()));
    QMetaObject::invokeMethod(w.surface, "positionToRectangle", Q_RETURN_ARG(QRectF, last), Q_ARG(int, found.selectionEnd()));
    const QPointF middle((first.x() + last.x()) / 2, first.y() + first.height() / 2);
    return w.surface->mapToScene(middle).toPoint();
}

int editorTopSource(const PreviewWindow &w) { return w.shell.binding()->topSourcePosition(); }
} // namespace

void previewAndSourceFindEachOther()
{
    const QString markdown = QStringLiteral(
        "# Title here\n\n"
        "A first paragraph with *leaning* words and `code` in it.\n\n"
        "- item one\n- item two with a [link](https://example.com)\n\n"
        "> quoted words\n\n"
        "| A | B |\n|---|---|\n| left cell | right cell |\n\n"
        "```\nfirst line\nsecond line\n```\n\n"
        "Last paragraph.[^n]\n\n"
        "[^n]: The note text.\n");
    PreviewWindow w(markdown);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewSplit");
    QVERIFY(w.settled());
    QTextDocument *document = w.preview->textDocument();
    // Every word of the preview knows where it was written.
    const auto sourceOf = [&](const QString &text) {
        const QTextCursor found = document->find(text);
        return found.isNull() ? -2 : w.preview->sourceAt(found.selectionStart());
    };
    for (const char *word : {"Title", "first paragraph", "leaning", "words and", "code", "item one", "item two", "link",
                             "quoted", "Last paragraph", "The note"})
        QCOMPARE(sourceOf(QString::fromLatin1(word)), int(markdown.indexOf(QLatin1String(word))));
    // Inside a run, positions go side by side with the source.
    const QTextCursor paragraph = document->find(QStringLiteral("paragraph with"));
    QCOMPARE(w.preview->sourceAt(paragraph.selectionStart() + 4), int(markdown.indexOf(QStringLiteral("paragraph with"))) + 4);
    // An island's text knows too: a table's cells and each line of code.
    quint64 tableIsland = 0, codeIsland = 0;
    QAbstractItemModel *marks = w.preview->marks();
    for (int row = 0; row < marks->rowCount(); ++row) {
        const QModelIndex index = marks->index(row, 0);
        if (marks->data(index, PreviewMarks::KindRole).toString() != QLatin1String("island")) continue;
        const QString kind = marks->data(index, PreviewMarks::IslandKindRole).toString();
        (kind == QLatin1String("table") ? tableIsland : codeIsland) = marks->data(index, PreviewMarks::IslandRole).toULongLong();
    }
    QVERIFY(tableIsland && codeIsland);
    QTextDocument *table = w.preview->islandDocument(tableIsland);
    QTextDocument *code = w.preview->islandDocument(codeIsland);
    QVERIFY(table && code);
    QCOMPARE(w.preview->sourceAt(table->find(QStringLiteral("right cell")).selectionStart(), tableIsland),
             int(markdown.indexOf(QStringLiteral("right cell"))));
    QCOMPARE(w.preview->sourceAt(code->find(QStringLiteral("second line")).selectionStart() + 3, codeIsland),
             int(markdown.indexOf(QStringLiteral("second line"))) + 3);

    // A double click on the preview puts the editor's caret at the source
    // of what was clicked, and gives the editor the keys.
    QTest::mouseDClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("quoted")));
    const int quoted = int(markdown.indexOf(QStringLiteral("quoted")));
    QTRY_VERIFY(qAbs(w.shell.binding()->cursorSource() - quoted) <= 6);
    QCOMPARE(w.shell.binding()->cursorSource(), w.shell.binding()->anchorSource());
    QTRY_COMPARE(w.window->activeFocusItem(), w.editor);
    // In the reading view the caret still goes there, for when the text is back.
    w.run("viewReading");
    QTRY_VERIFY(!w.editorPane->isVisible());
    QTest::mouseDClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("Last")));
    const int last = int(markdown.indexOf(QStringLiteral("Last paragraph")));
    QTRY_VERIFY(qAbs(w.shell.binding()->cursorSource() - last) <= 4);
    QCOMPARE(w.window->activeFocusItem(), w.surface);

    // Heights and source positions are two sides of one map: a place found
    // by height leads back to the same height, and later text is lower.
    qreal previous = -1;
    for (const char *word : {"Title", "first paragraph", "item one", "item two", "quoted", "left cell", "first line",
                             "second line", "Last paragraph"}) {
        const int source = int(markdown.indexOf(QLatin1String(word)));
        const qreal height = w.preview->heightOfSource(source);
        QVERIFY2(height > previous, word);
        const int back = w.preview->sourceAtHeight(height + 0.5);
        QVERIFY2(qAbs(back - source) <= 16, qPrintable(QStringLiteral("%1: %2 for %3").arg(QLatin1String(word)).arg(back).arg(source)));
        previous = height;
    }
    QCOMPARE(w.preview->heightOfSource(0), 0.0);
    QCOMPARE(w.preview->sourceAtHeight(0), 0);
    // The start of a paragraph is at the top of its block.
    const QTextBlock lastBlock = document->find(QStringLiteral("Last paragraph")).block();
    QCOMPARE(w.preview->heightOfSource(last), document->documentLayout()->blockBoundingRect(lastBlock).top());
    // Below the text, among the notes, the source is at its end.
    const qreal bottom = document->documentLayout()->documentSize().height();
    QVERIFY(w.preview->sourceAtHeight(bottom - 2) >= last);
}

void includedTextLeadsToItsFile()
{
    // A book of two chapters, each a file of its own that the book includes.
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString root = QFileInfo(folder.path()).canonicalFilePath();
    const QString book = root + QStringLiteral("/book.md"), one = root + QStringLiteral("/one.md"),
                  two = root + QStringLiteral("/two.md");
    const QString twoText = QStringLiteral("# Two\n\nSome words.\n\nThe second chapter ends here.\n");
    writeFile(book, "# Book\n\nOpening words.\n\none.md\n\ntwo.md\n\nClosing words.\n");
    writeFile(one, "# One\n\nThe first chapter.\n");
    writeFile(two, twoText.toUtf8());
    PreviewWindow w({});
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.shell.backend->openFiles({book});
    QTRY_COMPARE(w.tabs()->activePath(), book);
    Document *bookDocument = w.shell.document();
    w.run("viewSplit");
    QVERIFY(w.settled());
    QVERIFY(w.lines().contains(QStringLiteral("The second chapter ends here.")));
    // In the book's own text a double click stays in the book.
    QTest::mouseDClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("Opening words")));
    QTRY_VERIFY(w.shell.binding()->cursorSource() > 0);
    QCOMPARE(w.tabs()->activePath(), book);
    // In a chapter it opens the chapter's file, with the caret at the place
    // that was clicked and the keys in its text.
    const QVariantMap place = w.preview->includedAt(
        w.preview->textDocument()->find(QStringLiteral("second chapter ends")).selectionStart());
    QCOMPARE(place.value(QStringLiteral("path")).toString(), two);
    QCOMPARE(place.value(QStringLiteral("line")).toInt(), 4);
    QCOMPARE(place.value(QStringLiteral("column")).toInt(), 4);
    QTest::mouseDClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("second chapter ends")));
    QTRY_COMPARE(w.tabs()->activePath(), two);
    const int ends = int(twoText.indexOf(QStringLiteral("The second chapter ends here.")));
    QTRY_VERIFY(w.shell.binding()->cursorSource() >= ends);
    QVERIFY(w.shell.binding()->cursorSource() <= ends + int(QStringLiteral("The second chapter ends here.").size()));
    QTRY_COMPARE(w.window->activeFocusItem(), w.editor);

    // The outline lists the chapters' headings too, at the line that
    // includes them; one chosen opens its chapter at the heading.
    w.tabs()->setActiveIndex(w.tabs()->indexOf(bookDocument));
    QTRY_COMPARE(w.tabs()->activePath(), book);
    OutlineModel outline;
    outline.setDocument(w.shell.document());
    outline.setActive(true);
    QTRY_VERIFY(outline.ready());
    QCOMPARE(outline.rowCount(), 3);
    const auto at = [&](int row, OutlineModel::Role role) { return outline.data(outline.index(row), role); };
    QCOMPARE(at(0, OutlineModel::TextRole).toString(), QStringLiteral("Book"));
    QCOMPARE(at(0, OutlineModel::FileRole).toString(), QString());
    QCOMPARE(at(2, OutlineModel::TextRole).toString(), QStringLiteral("Two"));
    QCOMPARE(at(2, OutlineModel::FileRole).toString(), two);
    QCOMPARE(at(2, OutlineModel::FileLineRole).toInt(), 0);
    QCOMPARE(at(2, OutlineModel::PositionRole).toInt(), int(w.shell.document()->buffer()->text().indexOf(u"two.md")));
    if (!w.shell.settings()->outlineShown())
        QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, centreOf(w.shell.item(QStringLiteral("outlineToggle"))));
    QQuickItem *row = nullptr;
    QTRY_VERIFY((row = [&]() -> QQuickItem * {
        for (QQuickItem *item : visibleItems(w.window))
            if (item->objectName() == QStringLiteral("outlineRow") && item->property("text").toString() == QStringLiteral("One"))
                return item;
        return nullptr;
    }()));
    // The window may still be settling from the chapter opened above, when a
    // press can miss the row; the row is clicked again until it takes it,
    // as a person clicking would.
    QPointer<QQuickItem> rowGuard(row);
    QSignalSpy pressed(row, SIGNAL(pressed()));
    QTRY_VERIFY([&] {
        if (!rowGuard || pressed.count() > 0) return true;
        QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, centreOf(rowGuard));
        return pressed.count() > 0;
    }());
    QTRY_COMPARE(w.tabs()->activePath(), one);
    QTRY_COMPARE(w.shell.binding()->cursorSource(), 0);
}

void textAndPreviewScrollTogether()
{
    PreviewWindow w(longText(200));
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    auto *sync = qobject_cast<ScrollSync *>(w.shell.object(QStringLiteral("scrollSync")));
    QVERIFY(sync);
    QVERIFY(!sync->enabled());
    w.run("viewSplit");
    QVERIFY(w.settled());
    QVERIFY(sync->enabled());
    const QString source = w.document->buffer()->text();
    const auto settle = [] { QTest::qWait(ScrollSync::HoldMs + 60); };
    const qreal editorMost = w.editorFlickable->property("contentHeight").toReal() - w.editorFlickable->height();
    const qreal previewMost = w.flickable->property("contentHeight").toReal() - w.flickable->height();
    QVERIFY(editorMost > 1000 && previewMost > 1000);

    // The editor scrolls: the preview shows the same paragraph at its top.
    settle();
    int editorFollowed = sync->editorFollowed();
    const int target = int(source.indexOf(QStringLiteral("Paragraph 77 has")));
    QRectF line;
    QMetaObject::invokeMethod(w.editor, "positionToRectangle", Q_RETURN_ARG(QRectF, line),
                              Q_ARG(int, w.shell.binding()->sourceToEditor(target)));
    w.editorFlickable->setProperty("contentY", w.editor->y() + line.y());
    QTRY_COMPARE(topLine(w), QStringLiteral("Paragraph 77 has a few words of its own, enough to fill a line or two of the page when it is set."));
    // The preview following is not the preview leading: the editor stays where it was put.
    settle();
    QCOMPARE(contentY(w.editorFlickable), w.editor->y() + line.y());
    QCOMPARE(sync->editorFollowed(), editorFollowed);

    // Part of the way into a paragraph in one is part of the way into it in the other.
    QMetaObject::invokeMethod(w.editor, "positionToRectangle", Q_RETURN_ARG(QRectF, line),
                              Q_ARG(int, w.shell.binding()->sourceToEditor(int(source.indexOf(QStringLiteral("Paragraph 78 has"))))));
    const qreal next = w.editor->y() + line.y();
    const qreal here = contentY(w.editorFlickable);
    w.editorFlickable->setProperty("contentY", here + (next - here) / 2);
    settle();
    const QTextDocument *document = w.preview->textDocument();
    const qreal top77 = document->documentLayout()->blockBoundingRect(document->find(QStringLiteral("Paragraph 77 has")).block()).top();
    const qreal top78 = document->documentLayout()->blockBoundingRect(document->find(QStringLiteral("Paragraph 78 has")).block()).top();
    const qreal shown = w.preview->viewTop();
    QVERIFY2(shown > top77 + (top78 - top77) * 0.3 && shown < top77 + (top78 - top77) * 0.7,
             qPrintable(QStringLiteral("%1 between %2 and %3").arg(shown).arg(top77).arg(top78)));

    // The preview scrolls: the editor follows, and the preview is not moved back.
    settle();
    const int previewFollowed = sync->previewFollowed();
    const qreal wanted = document->documentLayout()->blockBoundingRect(document->find(QStringLiteral("Paragraph 140 has")).block()).top();
    w.flickable->setProperty("contentY", w.surface->y() + wanted);
    const int paragraph140 = int(source.indexOf(QStringLiteral("Paragraph 140 has")));
    QTRY_VERIFY2(qAbs(editorTopSource(w) - paragraph140) <= 60,
                 qPrintable(QStringLiteral("%1 for %2").arg(editorTopSource(w)).arg(paragraph140)));
    settle();
    QCOMPARE(contentY(w.flickable), w.surface->y() + wanted);
    QCOMPARE(sync->previewFollowed(), previewFollowed);
    QVERIFY(sync->editorFollowed() > editorFollowed);
    // Neither view moves again by itself.
    const qreal restingEditor = contentY(w.editorFlickable), restingPreview = contentY(w.flickable);
    QTest::qWait(400);
    QCOMPARE(contentY(w.editorFlickable), restingEditor);
    QCOMPARE(contentY(w.flickable), restingPreview);

    // The ends go together, whatever the heights of the two are.
    w.editorFlickable->setProperty("contentY", editorMost);
    QTRY_COMPARE(contentY(w.flickable), w.flickable->property("contentHeight").toReal() - w.flickable->height());
    settle();
    w.flickable->setProperty("contentY", 0);
    QTRY_COMPARE(contentY(w.editorFlickable), 0.0);
    settle();

    // Turned off, each scrolls by itself; turned on again, the preview
    // goes to where the text is.
    w.run("scrollSync");
    QVERIFY(!w.shell.settings()->scrollSync());
    QVERIFY(!sync->enabled());
    w.editorFlickable->setProperty("contentY", editorMost / 2);
    settle();
    QCOMPARE(contentY(w.flickable), 0.0);
    w.run("scrollSync");
    QVERIFY(sync->enabled());
    QTRY_VERIFY(contentY(w.flickable) > previewMost * 0.3);
    // The status line says so, and its button turns it off and on.
    QQuickItem *button = w.shell.item(QStringLiteral("scrollSyncButton"));
    QVERIFY(button && button->isVisible());
    QCOMPARE(button->property("text").toString(), QStringLiteral("Scroll sync"));
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, centreOf(button));
    QTRY_VERIFY(!w.shell.settings()->scrollSync());
    QCOMPARE(button->property("text").toString(), QStringLiteral("Scroll sync off"));
    w.shell.settings()->setScrollSync(true);
    // It is there only while both are side by side.
    w.run("viewSource");
    QTRY_VERIFY(!button->isVisible());
    QVERIFY(!sync->enabled());

    // An edit in the text keeps the preview where the text is.
    w.run("viewSplit");
    QVERIFY(w.settled());
    settle();
    w.editorFlickable->setProperty("contentY", editorMost / 3);
    settle();
    const int topBefore = editorTopSource(w);
    w.document->buffer()->replace({0, 0}, QStringLiteral("An opening paragraph, put in above everything else.\n\n"));
    QVERIFY(w.settled());
    settle();
    const int shownSource = w.preview->topSource();
    QVERIFY2(qAbs(shownSource - editorTopSource(w)) <= 120,
             qPrintable(QStringLiteral("%1 for %2 (was %3)").arg(shownSource).arg(editorTopSource(w)).arg(topBefore)));
}

void linksGoWhereTheyLead()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString other = folder.filePath(QStringLiteral("other notes.md"));
    QString otherText = QStringLiteral("# Other\n\n");
    for (int i = 0; i < 80; ++i) otherText += QStringLiteral("A line of the other document, number %1.\n\n").arg(i);
    otherText += QStringLiteral("## Second part\n\nIts end.\n");
    writeFile(other, otherText.toUtf8());
    writeFile(folder.filePath(QStringLiteral("paper.pdf")), "%PDF");
    const QString path = folder.filePath(QStringLiteral("links.md"));
    QString markdown = QStringLiteral(
        "# Top\n\n"
        "[web](https://example.com/a?b=c) and <mailto:someone@example.com> and [notes](other%20notes.md)\n"
        "and [paper](paper.pdf) and [script](javascript:alert(1)) and [down](#the-end)\n"
        "and [there](other%20notes.md#second-part).\n\n");
    for (int i = 0; i < 60; ++i) markdown += QStringLiteral("Filler paragraph %1.\n\n").arg(i);
    markdown += QStringLiteral("## The end\n\nDone.\n");
    writeFile(path, markdown.toUtf8());
    ShellOptions options;
    options.files = {path};
    PreviewWindow w(QString(), 1280, options);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    // What would go to the desktop goes to the test instead.
    QList<QUrl> opened;
    w.shell.backend->setLinkOpener([&](const QUrl &url) {
        opened << url;
        return true;
    });
    QSignalSpy documents(w.preview, &Preview::openRequested);
    w.run("viewReading");
    QVERIFY(w.settled());

    // What the pointer rests on says where it leads; plain text says nothing.
    const QPointF onLink = w.surface->mapFromScene(QPointF(placeOf(w, QStringLiteral("web"))));
    QCOMPARE(w.preview->hintAt(onLink.x(), onLink.y()), QStringLiteral("https://example.com/a?b=c"));
    const QPointF onText = w.surface->mapFromScene(QPointF(placeOf(w, QStringLiteral("Filler paragraph 1."))));
    QVERIFY(w.preview->hintAt(onText.x(), onText.y()).isEmpty());
    QVERIFY(w.preview->hintAt(-50, -50).isEmpty());

    // A web address and a mail address go to the desktop's handler.
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("web")));
    QTRY_COMPARE(opened.size(), 1);
    QCOMPARE(opened.last(), QUrl(QStringLiteral("https://example.com/a?b=c")));
    QVERIFY(w.preview->activate(QStringLiteral("mailto:someone@example.com"), 1));
    QCOMPARE(opened.last(), QUrl(QStringLiteral("mailto:someone@example.com")));
    // A Markdown file beside the document opens in parch, in a tab.
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("notes")));
    QTRY_COMPARE(documents.count(), 1);
    QCOMPARE(documents.first().first().toString(), other);
    QTRY_COMPARE(w.tabs()->rowCount(), 2);
    QCOMPARE(w.tabs()->activePath(), other);
    QCOMPARE(opened.size(), 2);
    QCOMPARE(w.shell.binding()->cursorSource(), 0);
    w.tabs()->setActiveIndex(0);
    QVERIFY(w.settled());
    // A link to a heading of another document opens it at that heading:
    // its line at the top of the text, with the caret on it.
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("there")));
    QTRY_COMPARE(documents.count(), 2);
    QCOMPARE(documents.last().at(1).toString(), QStringLiteral("second-part"));
    QTRY_COMPARE(w.tabs()->activePath(), other);
    QCOMPARE(w.tabs()->rowCount(), 2);
    QTRY_COMPARE(w.shell.binding()->cursorSource(), int(otherText.indexOf(QStringLiteral("## Second part"))));
    QCOMPARE(w.shell.binding()->topSourcePosition(), int(otherText.indexOf(QStringLiteral("## Second part"))));
    // A heading the other document does not have leaves it where it was.
    QCOMPARE(headingFound(w.preview, QStringLiteral("no-such-part")), -1);
    w.tabs()->setActiveIndex(0);
    QVERIFY(w.settled());
    // A document that cannot be opened leaves this one where it is, though
    // this one has a heading by the name the link gives.
    QVERIFY(headingFound(w.preview, QStringLiteral("the-end")) > 0);
    if (::geteuid() != 0) {
        const QString locked = folder.filePath(QStringLiteral("locked.md"));
        writeFile(locked, "# Locked\n\n## The end\n");
        QVERIFY(QFile::setPermissions(locked, {}));
        const int caret = w.shell.binding()->cursorSource();
        QVERIFY(w.preview->activate(QUrl::fromLocalFile(locked).toString() + QStringLiteral("#the-end"), 1));
        QTest::qWait(50);
        QCOMPARE(w.tabs()->rowCount(), 2);
        QCOMPARE(w.tabs()->activePath(), path);
        QCOMPARE(w.shell.binding()->cursorSource(), caret);
        QCOMPARE(contentY(w.flickable), 0.0);
        QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    }
    // Any other file is never handed to the desktop's handler: the window
    // asks first whether to show it in its folder.
    QSignalSpy asked(w.preview, &Preview::linkAsked);
    const int beforeFile = int(opened.size());
    QVERIFY(w.preview->activate(QUrl::fromLocalFile(folder.filePath(QStringLiteral("paper.pdf"))).toString(), 1));
    QCOMPARE(asked.count(), 1);
    QCOMPARE(asked.last().at(0).toString(), folder.filePath(QStringLiteral("paper.pdf")));
    QVERIFY(asked.last().at(3).toBool());
    QCOMPARE(opened.size(), beforeFile);
    QObject *linkDialog = w.shell.object(QStringLiteral("linkDialog"));
    QVERIFY(linkDialog);
    QTRY_VERIFY(linkDialog->property("opened").toBool());
    QTest::keyClick(w.window, Qt::Key_Escape);
    QTRY_VERIFY(!linkDialog->property("opened").toBool());
    QCOMPARE(opened.size(), beforeFile);
    QCOMPARE(w.preview->textDocument()->find(QStringLiteral("paper")).charFormat().anchorHref(),
             QUrl::fromLocalFile(folder.filePath(QStringLiteral("paper.pdf"))).toString());
    // Nothing that would run is a link at all, and nothing made up is followed.
    QVERIFY(!w.preview->textDocument()->find(QStringLiteral("script")).charFormat().isAnchor());
    const int before = int(opened.size());
    QVERIFY(!w.preview->activate(QStringLiteral("javascript:alert(1)"), 1));
    QVERIFY(!w.preview->activate(QStringLiteral("data:text/html,x"), 1));
    QVERIFY(!w.preview->activate(QStringLiteral("relative/without/a/scheme.md"), 1));
    QVERIFY(!w.preview->activate(QString(), 1));
    QCOMPARE(opened.size(), before);
    // Without an opener, as in every other test, nothing is opened.
    w.shell.backend->setLinkOpener({});
    QVERIFY(!w.preview->activate(QStringLiteral("https://example.com"), 1));

    // A link to a heading scrolls to it, and going back returns the view.
    QCOMPARE(contentY(w.flickable), 0.0);
    QVERIFY(!w.preview->canGoBack());
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("down")));
    QTRY_VERIFY(contentY(w.flickable) > 500);
    const QTextBlock end = w.preview->textDocument()->find(QStringLiteral("The end")).block();
    const qreal top = w.preview->textDocument()->documentLayout()->blockBoundingRect(end).top();
    QVERIFY(top >= w.preview->viewTop() && top < w.preview->viewTop() + w.flickable->height());
    QVERIFY(w.preview->canGoBack());
    w.run("previewBack");
    QTRY_COMPARE(contentY(w.flickable), 0.0);
    QVERIFY(!w.preview->canGoBack());
    // A heading nobody has leads nowhere.
    QVERIFY(!w.preview->goTo(QStringLiteral("no-such-heading")));
    QVERIFY(w.preview->goTo(QStringLiteral("the-end")));
}

void linksOfOtherKindsAskFirst()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    writeFile(folder.filePath(QStringLiteral("paper.pdf")), "%PDF");
    writeFile(folder.filePath(QStringLiteral("run.sh")), "#!/bin/sh\necho hi\n");
    writeFile(folder.filePath(QStringLiteral("app.desktop")), "[Desktop Entry]\nExec=true\n");
    QVERIFY(QDir(folder.path()).mkdir(QStringLiteral("sub")));
    const QString path = folder.filePath(QStringLiteral("links.md"));
    writeFile(path, "# Links\n\n"
                    "[zoom](zoommtg://zoom.us/join?confno=1 \"https://example.com/docs\") and\n"
                    "[web](https://example.com/page \"The page\") and [script](run.sh) and\n"
                    "[entry](app.desktop) and [inside](sub/) and <a href=\"steam://run/1\" title=\"A game\">game</a>.\n");
    ShellOptions options;
    options.files = {path};
    PreviewWindow w(QString(), 1280, options);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    QList<QUrl> opened;
    QStringList shown;
    w.shell.backend->setLinkOpener([&](const QUrl &url) {
        opened << url;
        return true;
    });
    w.shell.backend->setFolderShower([&](const QString &file) {
        shown << file;
        return true;
    });
    QSignalSpy asked(w.preview, &Preview::linkAsked);
    w.run("viewReading");
    QVERIFY(w.settled());

    // The hint always says where a link leads, with its title after it, so
    // that a title cannot pass for the destination.
    const QPointF onZoom = w.surface->mapFromScene(QPointF(placeOf(w, QStringLiteral("zoom"))));
    QCOMPARE(w.preview->hintAt(onZoom.x(), onZoom.y()),
             QStringLiteral("zoommtg://zoom.us/join?confno=1\nhttps://example.com/docs"));
    const QPointF onWeb = w.surface->mapFromScene(QPointF(placeOf(w, QStringLiteral("web"))));
    QCOMPARE(w.preview->hintAt(onWeb.x(), onWeb.y()), QStringLiteral("https://example.com/page\nThe page"));
    const QPointF onGame = w.surface->mapFromScene(QPointF(placeOf(w, QStringLiteral("game"))));
    QCOMPARE(w.preview->hintAt(onGame.x(), onGame.y()), QStringLiteral("steam://run/1\nA game"));

    // A web address opens at once.
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("web")));
    QTRY_COMPARE(opened.size(), 1);
    QCOMPARE(opened.last(), QUrl(QStringLiteral("https://example.com/page")));

    // Any other kind of address asks first, naming it and its scheme, with
    // the keys on Cancel; Escape opens nothing.
    QObject *dialog = w.shell.object(QStringLiteral("linkDialog"));
    QVERIFY(dialog);
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("zoom")));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(asked.count(), 1);
    QCOMPARE(asked.last().at(2).toString(), QStringLiteral("zoommtg"));
    QVERIFY(!asked.last().at(3).toBool());
    QCOMPARE(opened.size(), 1);
    QObject *question = dialog->findChild<QObject *>(QStringLiteral("linkQuestion"));
    QVERIFY(question);
    QCOMPARE(question->property("text").toString(),
             QStringLiteral("Open zoommtg://zoom.us/join?confno=1 with the program registered for it?"));
    QVERIFY(dialog->property("title").toString().contains(QStringLiteral("zoommtg")));
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(w.window, &problems), qPrintable(problems.join("; ")));
    QVERIFY2(unnamedAccessibleItems(w.window).isEmpty(), qPrintable(unnamedAccessibleItems(w.window).join(", ")));
    QCOMPARE(w.window->activeFocusItem()->objectName(), QStringLiteral("linkCancelChoice"));
    QTest::keyClick(w.window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QCOMPARE(opened.size(), 1);
    // Agreed to, it opens.
    QVERIFY(w.preview->activate(QStringLiteral("zoommtg://zoom.us/join?confno=1"), 1));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QObject *openChoice = dialog->findChild<QObject *>(QStringLiteral("linkOpenChoice"));
    QVERIFY(openChoice && openChoice->property("visible").toBool());
    QVERIFY(!dialog->findChild<QObject *>(QStringLiteral("linkShowChoice"))->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(openChoice, "clicked"));
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QCOMPARE(opened.size(), 2);
    QCOMPARE(opened.last(), QUrl(QStringLiteral("zoommtg://zoom.us/join?confno=1")));
    // A raw HTML link asks the same.
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("game")));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(asked.last().at(2).toString(), QStringLiteral("steam"));
    QTest::keyClick(w.window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());

    // A script, a desktop entry and a folder are never opened: each is
    // offered in its folder, and only that.
    for (const QString &name : {QStringLiteral("script"), QStringLiteral("entry"), QStringLiteral("inside")}) {
        const int before = int(asked.count());
        QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, name));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(asked.count(), before + 1);
        QVERIFY(asked.last().at(3).toBool());
        QVERIFY(dialog->findChild<QObject *>(QStringLiteral("linkShowChoice"))->property("visible").toBool());
        QVERIFY(!dialog->findChild<QObject *>(QStringLiteral("linkOpenChoice"))->property("visible").toBool());
        QVERIFY2(unnamedAccessibleItems(w.window).isEmpty(), qPrintable(unnamedAccessibleItems(w.window).join(", ")));
        QVERIFY(QMetaObject::invokeMethod(dialog->findChild<QObject *>(QStringLiteral("linkShowChoice")), "clicked"));
        QTRY_VERIFY(!dialog->property("opened").toBool());
    }
    QCOMPARE(shown, (QStringList{folder.filePath(QStringLiteral("run.sh")), folder.filePath(QStringLiteral("app.desktop")),
                                 folder.filePath(QStringLiteral("sub"))}));
    QCOMPARE(opened.size(), 2);

    // Whatever asks, the backend itself hands the desktop only web and mail
    // addresses, an address agreed to, and never a file or a script.
    QVERIFY(!w.shell.backend->openLink(QUrl::fromLocalFile(folder.filePath(QStringLiteral("run.sh")))));
    QVERIFY(!w.shell.backend->openLink(QUrl(QStringLiteral("zoommtg://x"))));
    QVERIFY(!w.shell.backend->openAsked(QUrl::fromLocalFile(folder.filePath(QStringLiteral("run.sh"))).toString()));
    QVERIFY(!w.shell.backend->openAsked(QStringLiteral("javascript:alert(1)")));
    QVERIFY(!w.shell.backend->openAsked(QStringLiteral("JavaScript:alert(1)")));
    QVERIFY(!w.shell.backend->openAsked(QStringLiteral("not a url")));
    QCOMPARE(opened.size(), 2);
    // What shows a file in its folder takes only a file that is there.
    QVERIFY(!w.shell.backend->showInFolder(folder.filePath(QStringLiteral("gone.txt"))));
    QVERIFY(!w.shell.backend->showInFolder(QStringLiteral("relative.txt")));
    QCOMPARE(shown.size(), 3);
    // The denylist still refuses outright, without asking.
    const int before = int(asked.count());
    for (const char *refused : {"javascript:alert(1)", "vbscript:x", "data:text/html,x", "about:blank", "qrc:/Main.qml",
                                "parch:x", "blob:x"})
        QVERIFY(!w.preview->activate(QString::fromLatin1(refused), 1));
    QCOMPARE(asked.count(), before);
    // An address too long to read is shortened in the middle, and what
    // cannot be seen in it is shown by its code, so that the question
    // shows what would open.
    const QString shownLong = Preview::shownLink(QUrl(QStringLiteral("zoommtg://x/") + QString(2000, u'a') + QStringLiteral("end")));
    QVERIFY(shownLong.size() < 600);
    QVERIFY(shownLong.startsWith(QStringLiteral("zoommtg://x/aaa")));
    QVERIFY(shownLong.endsWith(QStringLiteral("aend")));
    QVERIFY(shownLong.contains(u'…'));
    QVERIFY(!Preview::shownLink(QUrl(QStringLiteral("zoommtg://x/%E2%80%AEfdp.exe"))).contains(QChar(0x202E)));
}

void tasksAreTickedInTheSource()
{
    const QString markdown = QStringLiteral(
        "Things:\n\n- [ ] first\n- [x] second\n  - [ ] nested\n\nAfter.\n");
    PreviewWindow w(markdown);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewSplit");
    QVERIFY(w.settled());
    SourceBuffer *buffer = w.document->buffer();
    QSignalSpy toggled(w.preview, &Preview::taskToggled);
    // The boxes are the pictures before the items' words.
    const auto boxOf = [&](const QString &item) {
        const QTextCursor found = w.preview->textDocument()->find(item);
        QRectF box;
        // Two before the word: the box, then the space after it.
        QMetaObject::invokeMethod(w.surface, "positionToRectangle", Q_RETURN_ARG(QRectF, box), Q_ARG(int, found.selectionStart() - 2));
        return w.surface->mapToScene(QPointF(box.x() + 5, box.y() + box.height() - 6)).toPoint();
    };
    // A click ticks the task in the source, as one step that undoes.
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, boxOf(QStringLiteral("first")));
    QTRY_COMPARE(buffer->text(), QStringLiteral("Things:\n\n- [x] first\n- [x] second\n  - [ ] nested\n\nAfter.\n"));
    QCOMPARE(toggled.count(), 1);
    QCOMPARE(toggled.first().first().toInt(), int(markdown.indexOf(QStringLiteral("[ ] first"))));
    QVERIFY(w.settled());
    QCOMPARE(imagesIn(*w.preview->textDocument()).at(0).name(), preview::Builder::taskImageName(true));
    buffer->undo();
    QCOMPARE(buffer->text(), markdown);
    QVERIFY(!buffer->canUndo() || buffer->text() == markdown);
    QVERIFY(w.settled());
    // Another click clears one that is ticked, and a nested one is found too.
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, boxOf(QStringLiteral("second")));
    QTRY_COMPARE(buffer->text(), QStringLiteral("Things:\n\n- [ ] first\n- [ ] second\n  - [ ] nested\n\nAfter.\n"));
    QVERIFY(w.settled());
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, boxOf(QStringLiteral("nested")));
    QTRY_COMPARE(buffer->text(), QStringLiteral("Things:\n\n- [ ] first\n- [ ] second\n  - [x] nested\n\nAfter.\n"));
    QVERIFY(w.settled());
    // The undo and redo keys act on the document from the preview too.
    w.run("undo");
    QCOMPARE(buffer->text(), QStringLiteral("Things:\n\n- [ ] first\n- [ ] second\n  - [ ] nested\n\nAfter.\n"));
    w.run("redo");
    QCOMPARE(buffer->text(), QStringLiteral("Things:\n\n- [ ] first\n- [ ] second\n  - [x] nested\n\nAfter.\n"));
    QVERIFY(w.settled());

    // A box that shows older text than the source holds ticks nothing: the
    // text may have moved under it.
    const QPoint first = boxOf(QStringLiteral("first"));
    buffer->replace({0, 0}, QStringLiteral("- [ ] put before\n\n"));
    QVERIFY(!w.preview->current());
    const QString held = buffer->text();
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, first);
    QCOMPARE(buffer->text(), held);
    QVERIFY(w.settled());
    // Once it shows the text as it is, it ticks again, at the right place.
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, boxOf(QStringLiteral("first")));
    QTRY_VERIFY(buffer->text().contains(QStringLiteral("- [x] first")));
    QVERIFY(buffer->text().startsWith(QStringLiteral("- [ ] put before")));
    // A link that names no task changes nothing.
    QVERIFY(w.settled());
    QVERIFY(!w.preview->activate(QStringLiteral("parch:task/0"), 1));
    QVERIFY(!w.preview->activate(QStringLiteral("parch:task/x"), 1));
}

void notesGoThereAndBack()
{
    QString markdown = QStringLiteral("# Notes\n\nA claim.[^a] Then more.\n\n");
    for (int i = 0; i < 50; ++i) markdown += QStringLiteral("Filler paragraph %1.\n\n").arg(i);
    markdown += QStringLiteral("The claim again.[^a] And another.[^b]\n\n[^a]: The first note.\n[^b]: The second note.\n");
    PreviewWindow w(markdown);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewReading");
    QVERIFY(w.settled());
    QTextDocument *document = w.preview->textDocument();
    const auto visible = [&](const QString &text) {
        const QTextCursor found = document->find(text);
        const qreal top = document->documentLayout()->blockBoundingRect(found.block()).top();
        return top >= w.preview->viewTop() - 1 && top < w.preview->viewTop() + w.flickable->height();
    };
    QVERIFY(!visible(QStringLiteral("The first note.")));
    // The mark in the text leads to the note.
    const QTextCursor claim = document->find(QStringLiteral("A claim."));
    QRectF mark;
    QMetaObject::invokeMethod(w.surface, "positionToRectangle", Q_RETURN_ARG(QRectF, mark), Q_ARG(int, claim.selectionEnd()));
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier,
                      w.surface->mapToScene(QPointF(mark.x() + 3, mark.y() + mark.height() / 2)).toPoint());
    QTRY_VERIFY(visible(QStringLiteral("The first note.")));
    QVERIFY(w.preview->canGoBack());
    // The way back from the note returns the view to exactly where it was.
    QVERIFY(w.preview->activate(QStringLiteral("#fnref-1"), 1));
    QTRY_COMPARE(contentY(w.flickable), 0.0);
    QVERIFY(!w.preview->canGoBack());
    // Each reference to a note has a way back of its own.
    QVERIFY(w.preview->activate(QStringLiteral("#fnref-1-2"), 1));
    QTRY_VERIFY(visible(QStringLiteral("The claim again.")));
    QVERIFY(!visible(QStringLiteral("A claim.")));
    w.preview->goBack();
    QTRY_COMPARE(contentY(w.flickable), 0.0);
    QVERIFY(w.preview->activate(QStringLiteral("#fn-2"), 1));
    QTRY_VERIFY(visible(QStringLiteral("The second note.")));
    // The ways back are links in the notes themselves.
    QTextCursor back = document->find(QString(QChar(0x21a9)));
    QCOMPARE(back.charFormat().anchorHref(), QStringLiteral("#fnref-1"));
    back = document->find(QString(QChar(0x21a9)) + QStringLiteral("2"));
    QCOMPARE(back.charFormat().anchorHref(), QStringLiteral("#fnref-1-2"));
}

namespace {
// A book: chapters of long paragraphs, with lists, links, notes, a few code
// blocks and tables, of about this many words.
QString bookText(int words)
{
    static const char *const vocabulary[] = {
        "the",    "of",     "and",   "a",      "to",    "in",    "is",    "that",   "it",     "was",   "for",
        "on",     "are",    "as",    "with",   "they",  "at",    "be",    "this",   "from",   "or",    "one",
        "had",    "by",     "word",  "but",    "not",   "what",  "all",   "were",   "when",   "your",  "can",
        "said",   "there",  "use",   "each",   "which", "she",   "how",   "their",  "will",   "other", "about",
        "window", "quiet",  "paper", "morning", "river", "letter", "garden", "silence", "thought", "sentence",
    };
    const int count = int(sizeof(vocabulary) / sizeof(*vocabulary));
    QString text;
    quint32 state = 12345;
    const auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return int((state >> 8) % quint32(count));
    };
    int written = 0, paragraph = 0, notes = 0;
    QString definitions;
    while (written < words) {
        if (paragraph % 40 == 0) text += QStringLiteral("# Chapter %1\n\n").arg(paragraph / 40 + 1);
        else if (paragraph % 8 == 0) text += QStringLiteral("## Section %1\n\n").arg(paragraph / 8 + 1);
        if (paragraph % 37 == 5) {
            text += QStringLiteral("- first point of the list\n- a second point with **weight**\n- and a [link](https://example.com/%1)\n\n").arg(paragraph);
            written += 16;
        } else if (paragraph % 111 == 30) {
            text += QStringLiteral("```python\ndef step(n):\n    return n + %1  # onward\n```\n\n").arg(paragraph);
            written += 8;
        } else if (paragraph % 173 == 60) {
            text += QStringLiteral("| Name | Count |\n|------|------:|\n| river | %1 |\n| garden | 7 |\n\n").arg(paragraph);
            written += 8;
        }
        const int length = 60 + next() % 61;
        for (int i = 0; i < length; ++i) {
            const QString word = QString::fromLatin1(vocabulary[next()]);
            if (i == 0) text += word.at(0).toUpper() + word.mid(1);
            else if (i % 23 == 11) text += QStringLiteral("*") + word + QStringLiteral("*");
            else text += word;
            text += i + 1 == length ? QStringLiteral(".") : i % 17 == 16 ? QStringLiteral(", ") : QStringLiteral(" ");
        }
        written += length;
        if (paragraph % 29 == 7) {
            ++notes;
            text += QStringLiteral("[^n%1]").arg(notes);
            definitions += QStringLiteral("[^n%1]: A note on paragraph %2.\n").arg(notes).arg(paragraph);
        }
        text += QStringLiteral("\n\n");
        ++paragraph;
    }
    return text + definitions;
}

// Counts the longest time the event loop went without turning.
class Stalls : public QObject {
public:
    Stalls()
    {
        m_timer.setInterval(1);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            m_longest = std::max(m_longest, m_clock.restart());
        });
        m_clock.start();
        m_timer.start();
    }
    void reset()
    {
        m_longest = 0;
        m_clock.restart();
    }
    qint64 longest() const { return m_longest; }

private:
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_longest = 0;
};
} // namespace

void aBookIsFollowedWithinAFrame()
{
    const QString book = bookText(100000);
    const int words = int(book.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size());
    QVERIFY2(words >= 100000, qPrintable(QString::number(words)));
    PreviewWindow w{QString()};
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.document->buffer()->replace({0, 0}, book);
    QTest::qWait(300);

    // Building the whole book is done a slice at a time: the window answers
    // all the while, and no slice holds the thread for long. It is built
    // for the reading view, where the text beside it is not being wrapped
    // to a new width at the same time.
    Stalls stalls;
    QElapsedTimer whole;
    whole.start();
    w.preview->resetLongestSlice();
    w.run("viewReading");
    QVERIFY(QTest::qWaitFor([&] { return w.preview->working() || w.preview->current(); }, 10000));
    stalls.reset();
    QVERIFY(w.settled(120000));
    const qint64 buildMs = whole.elapsed();
    const qreal buildSlice = w.preview->longestSlice();
    const qint64 buildStall = stalls.longest();
    const int sections = int(w.preview->sections().size());
    QVERIFY(sections > 1000);
    QCOMPARE(w.preview->sectionsBuilt(), sections);
    w.run("viewSplit");
    QTRY_VERIFY(w.editorPane->isVisible());
    QTest::qWait(600);

    // One character typed in the middle of the book: one section is built
    // again, and nothing holds the thread for a frame.
    const int middle = int(book.indexOf(QStringLiteral(" the "), book.size() / 2)) + 1;
    QList<qreal> slices;
    QList<qint64> gaps;
    for (int round = 0; round < 12; ++round) {
        QVERIFY(w.settled());
        QTest::qWait(60);
        const int built = w.preview->sectionsBuilt();
        w.preview->resetLongestSlice();
        stalls.reset();
        w.document->buffer()->replace({middle, 0}, QStringLiteral("x"), EditKind::Typing);
        QVERIFY(w.settled());
        QTest::qWait(40);
        QCOMPARE(w.preview->sectionsBuilt(), built + 1);
        slices << w.preview->longestSlice();
        gaps << stalls.longest();
    }
    std::sort(slices.begin(), slices.end());
    std::sort(gaps.begin(), gaps.end());
    const qreal medianSlice = slices.at(slices.size() / 2), worstSlice = slices.last();
    const qint64 medianGap = gaps.at(gaps.size() / 2), worstGap = gaps.last();
    qInfo("a book of %d words in %d sections: built in %lld ms, longest slice %.1f ms, longest stall %lld ms; "
          "one character: slice median %.2f ms, worst %.2f ms; event loop gap median %lld ms, worst %lld ms",
          words, sections, buildMs, buildSlice, buildStall, medianSlice, worstSlice, medianGap, worstGap);
    // The preview's own work for one character is well within a frame.
    QVERIFY2(worstSlice <= timeLimit(16), qPrintable(QString::number(worstSlice)));
    // Laying out and drawing the change, off screen and unaccelerated, is
    // within two frames; on a display it is drawn by the GPU.
    QVERIFY2(medianGap <= timeLimit(33), qPrintable(QString::number(medianGap)));
    // Building the book never held the window for long either.
    QVERIFY2(buildSlice <= timeLimit(100), qPrintable(QString::number(buildSlice)));
    // What it shows is the text as it is.
    QVERIFY(!w.preview->textDocument()->find(QStringLiteral("xxxxxxxxxxxxthe")).isNull());
}

namespace {
// The items the preview places over its text, of one kind.
QList<QQuickItem *> marksOf(const PreviewWindow &w, const QString &kind)
{
    QList<QQuickItem *> found;
    for (QQuickItem *item : visibleItems(w.window))
        if (item->objectName() == QLatin1String("previewMark") && item->property("kind").toString() == kind) found << item;
    return found;
}

QQuickItem *childNamed(QQuickItem *item, const QString &name)
{
    return item ? findItem(item, name) : nullptr;
}

// Whether an item is another, or somewhere inside it.
bool inside(QQuickItem *item, const QQuickItem *ancestor)
{
    for (; item; item = item->parentItem())
        if (item == ancestor) return true;
    return false;
}
} // namespace

void wideBlocksScrollByThemselves()
{
    QString wideRow = QStringLiteral("| ");
    QString rule = QStringLiteral("|");
    for (int column = 0; column < 12; ++column) {
        wideRow += QStringLiteral("Column number %1 | ").arg(column);
        rule += QStringLiteral("---|");
    }
    const QString markdown = QStringLiteral("# Wide things\n\nA paragraph of ordinary prose that wraps to the column as prose does, however narrow the window is made.\n\n")
                             + wideRow + u'\n' + rule + u'\n' + wideRow + QStringLiteral("\n\n")
                             + QStringLiteral("| A | B |\n|---|---|\n| small | table |\n\n")
                             + QStringLiteral("```\nconst a_very_long_line_of_code = someFunction(with, a, great, many, arguments, that, go, on, and, on, past, the, edge, of, any, column);\nshort();\n```\n\n")
                             + QStringLiteral("    short code\n\nThe end.\n");
    PreviewWindow w(markdown, 900);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    // Tall enough for all of it to be in view, however narrow.
    w.window->setHeight(1400);
    w.run("viewReading");
    QVERIFY(w.settled());
    QTRY_COMPARE(marksOf(w, QStringLiteral("island")).size(), 4);
    QList<QQuickItem *> islands = marksOf(w, QStringLiteral("island"));
    const auto flickableOf = [](QQuickItem *island) { return childNamed(island, QStringLiteral("islandFlickable")); };
    const auto textOf = [](QQuickItem *island) { return childNamed(island, QStringLiteral("islandText")); };
    const auto overflow = [&](QQuickItem *island) {
        return flickableOf(island)->property("contentWidth").toReal() - flickableOf(island)->width();
    };

    for (const int width : {900, 360}) {
        resizeWindow(w.window, width);
        QTRY_COMPARE(w.surface->width(), w.previewPane->property("columnWidth").toReal());
        QTest::qWait(50);
        // An island's item lives while the island is near the view.
        QTRY_COMPARE(marksOf(w, QStringLiteral("island")).size(), 4);
        islands = marksOf(w, QStringLiteral("island"));
        for (QQuickItem *island : std::as_const(islands)) QVERIFY(flickableOf(island) && textOf(island));
        // Prose never runs past the column, and nothing of the surface does.
        QVERIFY(w.surface->property("contentWidth").toReal() <= w.surface->width() + 1);
        QVERIFY(w.surface->x() >= 0 && w.surface->x() + w.surface->width() <= w.previewPane->width());
        // Each island is as wide as the column and sits where its place is kept.
        for (QQuickItem *island : islands) {
            QCOMPARE(island->width(), w.surface->width());
            QVERIFY(island->height() > 10);
        }
        // The wide table and the long line of code are wider than the
        // column: they keep their whole width and scroll sideways, by themselves.
        QVERIFY2(overflow(islands.at(0)) > 100, qPrintable(QString::number(overflow(islands.at(0)))));
        QVERIFY(overflow(islands.at(2)) > 100);
        QCOMPARE(textOf(islands.at(2))->property("lineCount").toInt(), 2);
        // The small table and the short code fit, and scroll nowhere.
        QVERIFY(overflow(islands.at(1)) <= 0.5);
        QVERIFY(overflow(islands.at(3)) <= 0.5);
        // Scrolling an island sideways moves nothing else.
        QQuickItem *sideways = flickableOf(islands.at(0));
        sideways->setProperty("contentX", 80);
        QCOMPARE(sideways->property("contentX").toReal(), 80.0);
        QCOMPARE(flickableOf(islands.at(2))->property("contentX").toReal(), 0.0);
        QCOMPARE(w.flickable->property("contentX").toReal(), 0.0);
        sideways->setProperty("contentX", 0);
        // Islands lie one below the other, in the order of the text, and
        // the text after the last one starts below it.
        for (int i = 1; i < islands.size(); ++i) QVERIFY(islands.at(i)->y() >= islands.at(i - 1)->y() + islands.at(i - 1)->height());
        const QTextBlock end = w.preview->textDocument()->find(QStringLiteral("The end.")).block();
        const qreal endTop = w.preview->textDocument()->documentLayout()->blockBoundingRect(end).top();
        QVERIFY(endTop >= islands.last()->y() + islands.last()->height());
        // What an island shows is as tall as the place kept for it.
        for (QQuickItem *island : islands) {
            QQuickItem *text = textOf(island);
            const qreal padding = island == islands.at(2) || island == islands.at(3) ? w.preview->padding() : 0;
            QVERIFY2(qAbs(text->height() + 2 * padding - island->height()) <= 1.5,
                     qPrintable(QStringLiteral("%1 in %2").arg(text->height()).arg(island->height())));
        }
        w.shot(width == 360 ? "wide-360" : "wide-900");
    }
    // The keys scroll an island that has them: Right and Left, by themselves.
    QQuickItem *text = textOf(islands.at(2));
    text->forceActiveFocus(Qt::TabFocusReason);
    QCOMPARE(w.window->activeFocusItem(), text);
    QTest::keyClick(w.window, Qt::Key_Right);
    QVERIFY(flickableOf(islands.at(2))->property("contentX").toReal() > 0);
    QTest::keyClick(w.window, Qt::Key_Left);
    QCOMPARE(flickableOf(islands.at(2))->property("contentX").toReal(), 0.0);
    // Escape gives the keys back to the preview's text.
    QTest::keyClick(w.window, Qt::Key_Escape);
    QCOMPARE(w.window->activeFocusItem(), w.surface);

    // A table's lines are drawn between its rows, the header's plainer.
    QAbstractItemModel *marks = w.preview->marks();
    bool found = false;
    for (int row = 0; row < marks->rowCount(); ++row) {
        const QModelIndex index = marks->index(row, 0);
        if (marks->data(index, PreviewMarks::IslandKindRole).toString() != QLatin1String("table")) continue;
        const QVariantList rules = marks->data(index, PreviewMarks::RulesRole).toList();
        QCOMPARE(rules.size(), 1);
        QCOMPARE(marks->data(index, PreviewMarks::HeaderRuleRole).toInt(), 0);
        QVERIFY(rules.first().toReal() > 10);
        found = true;
    }
    QVERIFY(found);

    // Islands far from the view have no item; they get one as they come near.
    QString many = QStringLiteral("Start.\n\n");
    for (int i = 0; i < 60; ++i) many += QStringLiteral("Paragraph %1 before its code.\n\n```\ncode %1\n```\n\n").arg(i);
    w.document->buffer()->replace({0, w.document->buffer()->length()}, many);
    QVERIFY(w.settled());
    resizeWindow(w.window, 900);
    QTest::qWait(50);
    const qsizetype near = marksOf(w, QStringLiteral("island")).size();
    QVERIFY2(near >= 2 && near < 30, qPrintable(QString::number(near)));
    w.flickable->setProperty("contentY", w.flickable->property("contentHeight").toReal() - w.flickable->height());
    QTRY_VERIFY(!marksOf(w, QStringLiteral("island")).isEmpty());
    const QList<QQuickItem *> last = marksOf(w, QStringLiteral("island"));
    QVERIFY(last.size() < 30);
    QTRY_COMPARE(textOf(last.last())->property("text").toString().contains(QStringLiteral("code 59")), true);
    // Every island's text is built, shown or not.
    int islandCount = 0;
    for (const Section &section : w.preview->sections()) islandCount += int(section.islands.size());
    QCOMPARE(islandCount, 60);
}

void previewIsReachedAndReadWithoutAMouse()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir folder;
    QImage picture(40, 30, QImage::Format_RGB32);
    picture.fill(Qt::darkGreen);
    QVERIFY(picture.save(folder.filePath(QStringLiteral("leaf.png"))));
    const QString path = folder.filePath(QStringLiteral("read.md"));
    writeFile(path, QStringLiteral("# Reading aloud\n\nSome text with a picture: ![A green leaf](leaf.png)\n\n## Second part\n\n"
                                   "| A | B |\n|---|---|\n| 1 | 2 |\n\n```\ncode\n```\n\nThe end.\n")
                        .toUtf8());
    ShellOptions options;
    options.files = {path};
    PreviewWindow w(QString(), 1280, options);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewSplit");
    QVERIFY(w.settled());
    QTRY_COMPARE(marksOf(w, QStringLiteral("image")).size(), 1);

    // The preview says what it is, and that it is of this document.
    QAccessibleInterface *surface = QAccessible::queryAccessibleInterface(w.surface);
    QVERIFY(surface);
    QCOMPARE(surface->text(QAccessible::Name), QStringLiteral("Preview of read.md"));
    QCOMPARE(surface->role(), QAccessible::EditableText);
    QVERIFY(surface->state().readOnly);
    // Its text is the reading text.
    QAccessibleTextInterface *text = surface->textInterface();
    QVERIFY(text);
    QVERIFY(text->text(0, text->characterCount()).contains(QStringLiteral("Reading aloud")));
    // Headings are told as headings, with their level, where they are.
    const QList<QQuickItem *> headings = marksOf(w, QStringLiteral("heading"));
    QCOMPARE(headings.size(), 2);
    QAccessibleInterface *first = QAccessible::queryAccessibleInterface(headings.at(0));
    QVERIFY(first);
    QCOMPARE(first->role(), QAccessible::Heading);
    QCOMPARE(first->text(QAccessible::Name), QStringLiteral("Reading aloud"));
    QCOMPARE(first->text(QAccessible::Description), QStringLiteral("Heading level 1"));
    QCOMPARE(QAccessible::queryAccessibleInterface(headings.at(1))->text(QAccessible::Description), QStringLiteral("Heading level 2"));
    const QTextBlock headingBlock = w.preview->textDocument()->find(QStringLiteral("Second part")).block();
    QCOMPARE(headings.at(1)->y(), w.preview->textDocument()->documentLayout()->blockBoundingRect(headingBlock).top());
    // A picture is told with its alternative text.
    QAccessibleInterface *image = QAccessible::queryAccessibleInterface(marksOf(w, QStringLiteral("image")).first());
    QVERIFY(image);
    QCOMPARE(image->role(), QAccessible::Graphic);
    QCOMPARE(image->text(QAccessible::Name), QStringLiteral("A green leaf"));
    // A table and code say what they are.
    QStringList islandNames;
    for (QQuickItem *island : marksOf(w, QStringLiteral("island")))
        islandNames << QAccessible::queryAccessibleInterface(childNamed(island, QStringLiteral("islandText")))->text(QAccessible::Name);
    QCOMPARE(islandNames, (QStringList{QStringLiteral("Table, 2 columns, 2 rows"), QStringLiteral("Code")}));
    // Nothing that is told lacks a name, and Tab reaches every control:
    // the names of the panes, the preview, its table and code, and the divider.
    QVERIFY2(unnamedAccessibleItems(w.window).isEmpty(), qPrintable(unnamedAccessibleItems(w.window).join(", ")));
    QStringList problems;
    QVERIFY2(tabKeyReachesEveryControl(w.window, &problems), qPrintable(problems.join(", ")));
    for (const char *name : {"textPaneName", "previewPaneName", "previewButton", "scrollSyncButton"}) {
        QQuickItem *control = w.shell.item(QString::fromLatin1(name));
        QVERIFY2(control && control->isVisible(), name);
        QAccessibleInterface *access = QAccessible::queryAccessibleInterface(control);
        QVERIFY2(access && !access->text(QAccessible::Name).isEmpty(), name);
    }
    QCOMPARE(QAccessible::queryAccessibleInterface(w.shell.item(QStringLiteral("scrollSyncButton")))->state().checked, true);

    // F6 goes round the parts of the window: from the text to the preview,
    // which then shows that it has the keys.
    w.editor->forceActiveFocus();
    QQuickItem *mark = w.shell.item(QStringLiteral("previewFocusMark"));
    QVERIFY(mark && !mark->isVisible());
    QTest::keyClick(w.window, Qt::Key_F6);
    QTRY_COMPARE(w.window->activeFocusItem(), w.surface);
    QVERIFY(mark->isVisible());
    QVERIFY(contrastRatio(mark->property("color").value<QColor>(), w.shell.theme->background()) >= 3);
    QTest::keyClick(w.window, Qt::Key_F6, Qt::ShiftModifier);
    QTRY_COMPARE(w.window->activeFocusItem(), w.editor);
    QVERIFY(!mark->isVisible());
    // A click gives the preview the keys without the mark.
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("Some text")));
    QTRY_COMPARE(w.window->activeFocusItem(), w.surface);
    QVERIFY(!mark->isVisible());
    // The keys scroll the preview, and never type in the document.
    const QString before = w.document->buffer()->text();
    for (const QChar key : QStringLiteral("qwerty")) sendKey(w.window, key);
    QTest::keyClick(w.window, Qt::Key_Return);
    QTest::keyClick(w.window, Qt::Key_Backspace);
    QCOMPARE(w.document->buffer()->text(), before);
    // Tab leaves the preview rather than typing in it.
    QTest::keyClick(w.window, Qt::Key_Tab);
    QVERIFY(w.window->activeFocusItem() != w.surface);
    QCOMPARE(w.document->buffer()->text(), before);

    // In the reading view the keys go to the preview from the first, and scroll it.
    QString longer = before;
    for (int i = 0; i < 80; ++i) longer += QStringLiteral("\nLine %1 of more text.\n").arg(i);
    w.document->buffer()->replace({0, w.document->buffer()->length()}, longer);
    w.run("viewReading");
    QVERIFY(w.settled());
    QTRY_COMPARE(w.window->activeFocusItem(), w.surface);
    QCOMPARE(contentY(w.flickable), 0.0);
    QTest::keyClick(w.window, Qt::Key_Down);
    QVERIFY(contentY(w.flickable) > 0);
    const qreal line = contentY(w.flickable);
    QTest::keyClick(w.window, Qt::Key_PageDown);
    QVERIFY(contentY(w.flickable) > line + w.flickable->height() / 2);
    QTest::keyClick(w.window, Qt::Key_Space);
    QTest::keyClick(w.window, Qt::Key_PageUp);
    QTest::keyClick(w.window, Qt::Key_Up);
    QTest::keyClick(w.window, Qt::Key_End);
    QCOMPARE(contentY(w.flickable), w.flickable->property("contentHeight").toReal() - w.flickable->height());
    QTest::keyClick(w.window, Qt::Key_Home);
    QCOMPARE(contentY(w.flickable), 0.0);
    // With Shift the keys select, and what is selected stays in view.
    QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, placeOf(w, QStringLiteral("Some text")));
    for (int i = 0; i < 4; ++i) QTest::keyClick(w.window, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(w.surface->property("selectedText").toString().size(), 4);
    QTest::keyClick(w.window, Qt::Key_A, Qt::ControlModifier);
    QVERIFY(w.surface->property("selectedText").toString().size() > 500);
    // F6 from the preview alone goes to the library and back to it.
    QTest::keyClick(w.window, Qt::Key_F6);
    QVERIFY(w.window->activeFocusItem() != w.surface);
    QTest::keyClick(w.window, Qt::Key_F6);
    QTRY_COMPARE(w.window->activeFocusItem(), w.surface);
    QVERIFY2(unnamedAccessibleItems(w.window).isEmpty(), qPrintable(unnamedAccessibleItems(w.window).join(", ")));
    QVERIFY2(tabKeyReachesEveryControl(w.window, &problems), qPrintable(problems.join(", ")));
}

void copyingFromThePreviewGivesTextAndHtml()
{
    const QString markdown = QStringLiteral(
        "# Copy me\n\nA paragraph with **weight**, *lean*, `code` and a [link](https://example.com).\n\n"
        "- [x] done\n- [ ] to do\n\n1. first\n2. second\n\n"
        "| Name | Count |\n|------|------:|\n| river | 3 |\n\n"
        "```\nline one\nline two\n```\n\nThe end.\n");
    PreviewWindow w(markdown);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewReading");
    QVERIFY(w.settled());
    QTextDocument *document = w.preview->textDocument();
    // Nothing selected: nothing to copy.
    QVERIFY(w.preview->copied(w.surface).isEmpty());
    // Part of a paragraph: its words, and their formatting as HTML.
    QTextCursor found = document->find(QStringLiteral("with weight, lean, code and a link"));
    QVERIFY(!found.isNull());
    QMetaObject::invokeMethod(w.surface, "select", Q_ARG(int, found.selectionStart()), Q_ARG(int, found.selectionEnd()));
    QStringList copied = w.preview->copied(w.surface);
    QCOMPARE(copied.size(), 2);
    QCOMPARE(copied.at(0), QStringLiteral("with weight, lean, code and a link"));
    QVERIFY2(copied.at(1).contains(QStringLiteral("font-weight:700")) || copied.at(1).contains(QStringLiteral("font-weight:600"))
                 || copied.at(1).contains(QStringLiteral("<b>")),
             qPrintable(copied.at(1)));
    QVERIFY(copied.at(1).contains(QStringLiteral("font-style:italic")));
    QVERIFY(copied.at(1).contains(QStringLiteral("href=\"https://example.com\"")));
    QVERIFY(copied.at(1).contains(QStringLiteral("monospace")));
    // The colours and faces of this desktop do not go with it.
    QVERIFY(!copied.at(1).contains(QStringLiteral("background-color")));
    QVERIFY(!copied.at(1).contains(w.preview->style().ink(PreviewStyle::Link).name()));
    QVERIFY(!copied.at(1).contains(w.preview->bodyFamily()));

    // Everything: each block on its line, paragraphs apart, list items
    // together with their markers, tasks as they are written, a table's
    // rows with tabs between their cells, and code as it is.
    QMetaObject::invokeMethod(w.surface, "selectAll");
    copied = w.preview->copied(w.surface);
    QCOMPARE(copied.at(0),
             QStringLiteral("Copy me\n\nA paragraph with weight, lean, code and a link.\n\n"
                            "[x] done\n[ ] to do\n\n1. first\n2. second\n\n"
                            "Name\tCount\nriver\t3\n\nline one\nline two\n\nThe end."));
    QVERIFY(copied.at(1).contains(QStringLiteral("<h1")));
    QVERIFY(copied.at(1).contains(QStringLiteral("Copy me")));
    QVERIFY(copied.at(1).contains(QStringLiteral("<table")));
    QVERIFY(copied.at(1).contains(QStringLiteral("river")));
    QVERIFY(copied.at(1).contains(QStringLiteral("line two")));
    QVERIFY(!copied.at(1).contains(QStringLiteral("parch:")));
    // A table and code inside the selection show that they are part of it.
    QTRY_VERIFY(!marksOf(w, QStringLiteral("island")).isEmpty());
    for (QQuickItem *island : marksOf(w, QStringLiteral("island"))) QVERIFY(childNamed(island, QStringLiteral("islandCovered"))->isVisible());
    QMetaObject::invokeMethod(w.surface, "select", Q_ARG(int, 1), Q_ARG(int, 4));
    for (QQuickItem *island : marksOf(w, QStringLiteral("island"))) QVERIFY(!childNamed(island, QStringLiteral("islandCovered"))->isVisible());
    QMetaObject::invokeMethod(w.surface, "selectAll");

    // The copy key puts both on the clipboard.
    w.surface->forceActiveFocus();
    QGuiApplication::clipboard()->clear();
    QTest::keyClick(w.window, Qt::Key_C, Qt::ControlModifier);
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    QVERIFY(data);
    QCOMPARE(data->text(), copied.at(0));
    QVERIFY(data->hasHtml() && data->html().contains(QStringLiteral("<h1")));
    // An island's own selection is copied from the island.
    QTRY_VERIFY(!marksOf(w, QStringLiteral("island")).isEmpty());
    QQuickItem *code = nullptr;
    for (QQuickItem *island : marksOf(w, QStringLiteral("island")))
        if (island->property("islandKind").toString() == QLatin1String("code")) code = childNamed(island, QStringLiteral("islandText"));
    QVERIFY(code);
    QMetaObject::invokeMethod(code, "select", Q_ARG(int, 5), Q_ARG(int, 17));
    copied = w.preview->copied(code);
    QCOMPARE(copied.at(0), QStringLiteral("one\nline two"));
    code->forceActiveFocus();
    QTest::keyClick(w.window, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("one\nline two"));
}

namespace {
// Stands in for the network: answers each address when the test says so.
class TestFetcher : public RemoteFetcher {
public:
    struct Request {
        QUrl url;
        QPointer<QObject> context;
        std::function<void(const QByteArray &, const QString &)> done;
    };
    void fetch(const QUrl &url, QObject *context, std::function<void(const QByteArray &, const QString &)> done) override
    {
        requests.push_back({url, context, std::move(done)});
    }
    void answer(const QUrl &url, const QByteArray &data, const QString &error = {})
    {
        for (Request &request : requests) {
            if (request.url != url || !request.done) continue;
            auto done = std::exchange(request.done, nullptr);
            if (request.context) done(data, error);
        }
    }
    QList<QUrl> asked() const
    {
        QList<QUrl> urls;
        for (const Request &request : requests) urls << request.url;
        return urls;
    }
    std::vector<Request> requests;
};

QByteArray pngOf(int width, int height, const QColor &color)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}
} // namespace

void pathologicalBlocksStayCheapToLayOut()
{
    // One word of 300,000 characters: Qt breaks such a word into lines in
    // time that grows with its square, so the preview gives it a place to
    // break every few hundred characters, which nothing else sees.
    const QString word(300000, u'w');
    const auto built = build(QStringLiteral("Before.\n\n") + word + QStringLiteral("\n\nAfter.\n"));
    const QTextBlock block = built->blockOf(QStringLiteral("wwwwwwwwww"));
    QVERIFY(block.isValid());
    QVERIFY(block.text().count(QChar(0x200B)) >= 300000 / 1024);
    QCOMPARE(QString(block.text()).remove(QChar(0x200B)), word);
    built->document.setTextWidth(700);
    QElapsedTimer clock;
    clock.start();
    const qreal height = built->document.documentLayout()->documentSize().height();
    QVERIFY2(clock.elapsed() < timeLimit(1000), qPrintable(QString::number(clock.elapsed())));
    QVERIFY(height > 1000);
    // Ordinary words are left as they are.
    const auto prose = build(QStringLiteral("Some ordinary words in a line.\n"));
    QVERIFY(!prose->lines().join(u' ').contains(QChar(0x200B)));

    // A table too wide or too large to lay out is shown as it is written,
    // in a code island that scrolls sideways, and says why.
    QString wide = QStringLiteral("|");
    QString rule = QStringLiteral("|");
    QString row = QStringLiteral("|");
    for (int column = 0; column < 300; ++column) {
        wide += QStringLiteral(" h%1 |").arg(column);
        rule += QStringLiteral("---|");
        row += QStringLiteral(" c%1 |").arg(column);
    }
    const auto table = build(wide + u'\n' + rule + u'\n' + row + u'\n' + row + QStringLiteral("\n\nAfter.\n"));
    QVERIFY(!table->island(Island::Table));
    const Island *shown = table->island(Island::Code);
    QVERIFY(shown && shown->document);
    QVERIFY(shown->document->toPlainText().startsWith(QStringLiteral("| h0 | h1 |")));
    QCOMPARE(shown->document->defaultTextOption().wrapMode(), QTextOption::NoWrap);
    QVERIFY(shown->document->toPlainText().contains(QStringLiteral("too large to lay out as a table (300 columns, 3 rows)")));
    // An ordinary table is still a table.
    const auto ordinary = build(QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |\n"));
    QVERIFY(ordinary->island(Island::Table));

    // What is copied from the preview is the text as written, without the
    // places to break.
    PreviewWindow w(QStringLiteral("Start ") + QString(3000, u'z') + QStringLiteral(" end.\n"));
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewReading");
    QVERIFY(w.settled());
    QVERIFY(w.preview->textDocument()->toPlainText().contains(QChar(0x200B)));
    QMetaObject::invokeMethod(w.surface, "selectAll");
    const QStringList copied = w.preview->copied(w.surface);
    QCOMPARE(copied.value(0).trimmed(), QStringLiteral("Start ") + QString(3000, u'z') + QStringLiteral(" end."));
}

void webPicturesWaitUntilAsked()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString path = folder.filePath(QStringLiteral("web.md"));
    writeFile(path, "# Web\n\n![Far](https://example.com/far.png)\n\n![Odd](https://example.com/odd.png)\n\nText.\n");
    const QString plain = folder.filePath(QStringLiteral("plain.md"));
    writeFile(plain, "# Plain\n\nNo pictures from anywhere.\n");
    ShellOptions options;
    options.files = {plain, path};
    PreviewWindow w(QString(), 1000, options);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    TestFetcher fetcher;
    w.shell.backend->setRemoteFetcher(&fetcher);
    const auto unplugged = qScopeGuard([&] { w.shell.backend->setRemoteFetcher(nullptr); });
    // Pictures from the web are not fetched until the writer asks.
    QVERIFY(!w.shell.settings()->remoteImages());
    w.run("viewReading");
    QVERIFY(w.settled());
    QTRY_VERIFY(w.preview->webPictures());
    QTest::qWait(50);
    QVERIFY(fetcher.requests.empty());
    QVERIFY(!w.preview->textDocument()->find(QStringLiteral("Image not shown: Far")).isNull());
    // A quiet row says so, with a way to show them for this document.
    QQuickItem *row = w.shell.item(QStringLiteral("webPicturesNotice"));
    QVERIFY(row);
    QTRY_VERIFY(row->isVisible());
    QQuickItem *said = w.shell.item(QStringLiteral("webPicturesText"));
    QVERIFY(said);
    QCOMPARE(said->property("text").toString(), QStringLiteral("This document shows pictures from the web."));
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(w.window, &problems), qPrintable(problems.join("; ")));
    QVERIFY2(unnamedAccessibleItems(w.window).isEmpty(), qPrintable(unnamedAccessibleItems(w.window).join(", ")));
    // The row does not hide the start of the text.
    QVERIFY(row->mapToScene(QPointF(0, row->height())).y() <= w.surface->mapToScene(QPointF(0, 0)).y() + 1);
    QObject *show = w.shell.object(QStringLiteral("showWebPictures"));
    QVERIFY(show);
    QVERIFY(QMetaObject::invokeMethod(show, "clicked"));
    QTRY_COMPARE(fetcher.requests.size(), size_t(2));
    QTRY_VERIFY(!row->isVisible());
    fetcher.answer(QUrl(QStringLiteral("https://example.com/far.png")), pngOf(40, 20, Qt::darkBlue));
    QTRY_VERIFY(imagesIn(*w.preview->textDocument()).size() == 1);
    // A picture from the web is read only as PNG, JPEG, GIF, WebP or SVG,
    // whatever other readers are installed.
    QImage bitmap(8, 8, QImage::Format_RGB32);
    bitmap.fill(Qt::red);
    QByteArray bmp;
    QBuffer bmpBuffer(&bmp);
    bmpBuffer.open(QIODevice::WriteOnly);
    QVERIFY(bitmap.save(&bmpBuffer, "BMP"));
    fetcher.answer(QUrl(QStringLiteral("https://example.com/odd.png")), bmp);
    QTRY_VERIFY2(!w.preview->textDocument()->find(QStringLiteral("Image not shown: Odd (it is not a PNG, JPEG, GIF, WebP or SVG picture)")).isNull(),
                 qPrintable(w.lines().join(u'|')));
    // The choice is the document's, kept in its settings, and not the
    // application's.
    QVERIFY(!w.shell.settings()->remoteImages());
    QVERIFY(w.shell.backend->documentSettings()->remotePictures(w.document));
    DocumentSettings::Store kept(w.shell.paths().documentSettingsFile());
    QVERIFY(kept.load());
    QVERIFY(kept.remotePictures(w.document->id()));
    // It stays when the document's other settings change.
    QVERIFY(w.shell.backend->documentSettings()->setProfile(w.document, QStringLiteral("document"), QStringLiteral("standard")));
    QVERIFY(w.shell.backend->documentSettings()->remotePictures(w.document));
    // A document without pictures from the web shows no row.
    w.tabs()->setActiveIndex(0);
    w.run("viewReading");
    QVERIFY(w.settled());
    QTRY_VERIFY(!w.preview->webPictures());
    QVERIFY(!row->isVisible());
    // Taken back, the pictures go again.
    w.tabs()->setActiveIndex(1);
    QVERIFY(w.settled());
    QVERIFY(w.shell.backend->documentSettings()->setRemotePictures(w.document, false));
    QTRY_VERIFY(row->isVisible());
    DocumentSettings::Store again(w.shell.paths().documentSettingsFile());
    QVERIFY(again.load());
    QVERIFY(!again.remotePictures(w.document->id()));
    // The Preferences switch shows them in every document.
    w.shell.settings()->setRemoteImages(true);
    QTRY_VERIFY(!row->isVisible());
}

void picturesArriveAndFailInTheirPlace()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    writeFile(folder.filePath(QStringLiteral("assets/wide.png")), pngOf(2000, 500, Qt::darkCyan));
    writeFile(folder.filePath(QStringLiteral("assets/small picture.png")), pngOf(80, 40, Qt::darkRed));
    writeFile(folder.filePath(QStringLiteral("assets/not-a-picture.png")), "these are words");
    const QString path = folder.filePath(QStringLiteral("pictures.md"));
    writeFile(path, QStringLiteral(
                        "# Pictures\n\n"
                        "![Wide one](assets/wide.png)\n\n"
                        "Small ![small one](assets/small%20picture.png) in a line.\n\n"
                        "![Gone](assets/gone.png) and ![Words](assets/not-a-picture.png).\n\n"
                        "![From the web](https://example.com/far.png \"Far away\")\n\n"
                        "![Never comes](https://example.com/lost.png)\n\n"
                        "Inline $x+y$ and $refuse this$ here.\n\n"
                        "$$\nE = mc^2\n$$\n\n"
                        "```mermaid\nflowchart LR\n  a --> b\n```\n\n"
                        "```mermaid\nrefuse this chart\n```\n\nThe end.\n")
                        .toUtf8());
    ShellOptions options;
    options.files = {path};
    PreviewWindow w(QString(), 1000, options);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    // Stand-ins for the helpers that draw math and diagrams, and for the network.
    const QString helpers = folder.filePath(QStringLiteral("helpers"));
    QDir().mkpath(helpers);
    writeFakeHelpers(helpers, folder.filePath(QStringLiteral("helpers.log")));
    writeFile(helpers + QStringLiteral("/picture.png"), pngOf(80, 40, Qt::darkYellow));
    auto made = std::make_unique<RenderService>(renderOptions(HelperSet::in(helpers), folder.filePath(QStringLiteral("render"))));
    RenderService *service = made.get();
    w.shell.backend->setRenderService(std::move(made));
    TestFetcher fetcher;
    w.shell.backend->setRemoteFetcher(&fetcher);
    const auto unplugged = qScopeGuard([&] { w.shell.backend->setRemoteFetcher(nullptr); });
    w.shell.settings()->setRemoteImages(true);
    w.run("viewReading");
    QVERIFY(w.settled());
    QTextDocument *document = w.preview->textDocument();

    // While pictures are read and drawn, words and sources stand in their places.
    QVERIFY(!document->find(QStringLiteral("Loading image: From the web")).isNull());
    QVERIFY(!document->find(QStringLiteral("Loading image: Never comes")).isNull());
    QCOMPARE(fetcher.asked(), (QList<QUrl>{QUrl(QStringLiteral("https://example.com/far.png")), QUrl(QStringLiteral("https://example.com/lost.png"))}));

    // Local pictures, formulas and diagrams arrive by themselves.
    QVERIFY(QTest::qWaitFor([&] { return w.preview->resources()->pending() == 2 && !w.preview->working(); }, 30000));
    QTRY_VERIFY(document->find(QStringLiteral("Loading image: Wide one")).isNull());
    QVERIFY(QTest::qWaitFor([&] { return imagesIn(*document).size() == 4; }, 10000));
    QList<QTextImageFormat> images = imagesIn(*document);
    // The wide picture is never drawn wider than the column, and keeps its shape.
    QCOMPARE(images.at(0).width(), 2000.0);
    QCOMPARE(images.at(0).maximumWidth().type(), QTextLength::PercentageLength);
    const QTextBlock wideBlock = document->find(QString(QChar::ObjectReplacementCharacter)).block();
    QTest::qWait(50);
    const qreal column = w.surface->width();
    QVERIFY(w.surface->property("contentWidth").toReal() <= column + 1);
    const QRectF wideRect = document->documentLayout()->blockBoundingRect(wideBlock);
    QVERIFY2(qAbs(wideRect.height() - column / 4) < 40, qPrintable(QString::number(wideRect.height())));
    // The small one keeps its own size, in its line.
    QCOMPARE(images.at(1).width(), 80.0);
    QVERIFY(!document->find(QStringLiteral("Small ") + QChar::ObjectReplacementCharacter + QStringLiteral(" in a line.")).isNull());
    // A file that is not there, and one that is no picture, say so in place.
    QVERIFY2(!document->find(QStringLiteral("Image not shown: Gone (there is no such file)")).isNull(), qPrintable(w.lines().join(u'|')));
    QVERIFY(!document->find(QStringLiteral("Image not shown: Words (it is not a picture parch can read)")).isNull());
    // The inline formula is a picture on the line; the refused one keeps
    // its source, marked, with what the helper said.
    QCOMPARE(images.at(2).verticalAlignment(), QTextCharFormat::AlignBottom);
    QVERIFY(document->find(QStringLiteral("x+y")).isNull());
    QTextCursor refused = document->find(QStringLiteral("refuse this"));
    QVERIFY(!refused.isNull());
    QCOMPARE(inkOf(refused.charFormat()), int(PreviewStyle::Danger));
    QVERIFY2(refused.charFormat().toolTip().contains(QStringLiteral("fake refusal")), qPrintable(refused.charFormat().toolTip()));
    // The display formula is an island of its own; the diagram a picture in
    // the text, told with what it says; the refused diagram its source and why.
    QStringList kinds, texts;
    for (const Section &section : w.preview->sections())
        for (const Island &island : section.islands) {
            kinds << (island.kind == Island::Math ? QStringLiteral("math") : island.kind == Island::Code ? QStringLiteral("code") : QStringLiteral("table"));
            texts << island.document->toPlainText();
        }
    QCOMPARE(kinds, (QStringList{QStringLiteral("math"), QStringLiteral("code")}));
    QVERIFY2(texts.at(1).startsWith(QStringLiteral("refuse this chart\n")), qPrintable(texts.at(1)));
    QVERIFY(texts.at(1).size() > QStringLiteral("refuse this chart\n").size());
    QVERIFY(images.at(3).width() > 0);
    QCOMPARE(images.at(3).maximumWidth().type(), QTextLength::PercentageLength);
    QTRY_VERIFY(marksOf(w, QStringLiteral("image")).size() >= 2);
    w.shot("pictures");

    // A picture from the network arrives when the network answers, and one
    // that never does says why.
    const int built = w.preview->sectionsBuilt();
    fetcher.answer(QUrl(QStringLiteral("https://example.com/far.png")), pngOf(120, 60, Qt::darkBlue));
    QVERIFY(QTest::qWaitFor([&] { return imagesIn(*document).size() == 5; }, 10000));
    QCOMPARE(w.preview->sectionsBuilt(), built + 1);
    QVERIFY(document->find(QStringLiteral("Loading image: From the web")).isNull());
    fetcher.answer(QUrl(QStringLiteral("https://example.com/lost.png")), {}, QStringLiteral("Host not found"));
    QTRY_VERIFY(!document->find(QStringLiteral("Image not shown: Never comes (Host not found)")).isNull());
    QCOMPARE(w.preview->resources()->pending(), 0);
    // Each picture was asked for once, however often its section was built.
    QCOMPARE(fetcher.requests.size(), size_t(2));
    w.document->buffer()->replace({0, 0}, QStringLiteral("Before.\n\n"));
    QVERIFY(w.settled());
    QCOMPARE(fetcher.requests.size(), size_t(2));
    QCOMPARE(imagesIn(*document).size(), 5);

    // Turned off, pictures from the network are not fetched, and say so.
    w.shell.settings()->setRemoteImages(false);
    QTRY_VERIFY(!document->find(QStringLiteral("pictures from the network are turned off")).isNull());
    QCOMPARE(imagesIn(*document).size(), 4);
    w.document->buffer()->replace({w.document->buffer()->length(), 0}, QStringLiteral("\n![New](https://example.com/new.png)\n"));
    QVERIFY(w.settled());
    QCOMPARE(fetcher.requests.size(), size_t(2));
    w.shell.settings()->setRemoteImages(true);
    QTRY_COMPARE(fetcher.requests.size(), size_t(3));
    QTRY_COMPARE(imagesIn(*document).size(), 5);

    // A change of theme draws formulas and diagrams again, for its colours;
    // each stays as it is until its new picture has come, and then the
    // sections that show them are built again.
    const int runs = service->statistics().helperRuns;
    const int rebuilt = w.preview->sectionsBuilt();
    replaceTheme(w.shell.themeFolder.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    QTRY_COMPARE(w.preview->style().background, QColor("#1a1b26"));
    QCOMPARE(imagesIn(*document).size(), 5);
    // Two formulas and a diagram are drawn again; the diagram takes two runs.
    QVERIFY(QTest::qWaitFor([&] { return service->statistics().helperRuns >= runs + 4 && w.preview->resources()->pending() == 1 && !w.preview->working(); },
                            30000));
    QTRY_VERIFY(w.preview->sectionsBuilt() >= rebuilt + 3);
    QCOMPARE(imagesIn(*document).size(), 5);
    // What was asked for was asked in the new theme's colours.
    const QByteArray log = readFile(folder.filePath(QStringLiteral("helpers.log")));
    QVERIFY(log.count("start") >= 8);
}

void theRealHelpersDrawInThePreview()
{
    const HelperSet helpers = realHelpers();
    if (helpers.ratex.isEmpty() || helpers.merman.isEmpty())
        QSKIP("The real math and diagram helpers are not in build/helpers: run bin/fetch-helpers to test them");
    QTemporaryDir folder;
    PreviewWindow w(QStringLiteral(
        "# Drawn by the helpers\n\n"
        "Einstein wrote $E = mc^2$, and the roots are $x = \\frac{-b \\pm \\sqrt{b^2 - 4ac}}{2a}$ in a line of text.\n\n"
        "$$\n\\int_0^\\infty e^{-x^2}\\,dx = \\frac{\\sqrt{\\pi}}{2}\n$$\n\n"
        "A formula that is not one: $\\frac{1}{$ here.\n\n"
        "```mermaid\nflowchart LR\n  Draft --> Review --> Publish\n```\n\nThe end.\n"));
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.shell.backend->setRenderService(std::make_unique<RenderService>(renderOptions(helpers, folder.path())));
    w.run("viewReading");
    QVERIFY(w.settled());
    QTextDocument *document = w.preview->textDocument();
    // The source stands in until each picture has come.
    QVERIFY(QTest::qWaitFor([&] { return w.preview->resources()->pending() == 0 && !w.preview->working(); }, 60000));
    QTRY_COMPARE(imagesIn(*document).size(), 3);
    const QList<QTextImageFormat> images = imagesIn(*document);
    // The formulas in the line are drawn at the size of the text around them.
    const qreal size = w.preview->bodySize();
    QVERIFY2(images.at(0).height() > size * 0.5 && images.at(0).height() < size * 2, qPrintable(QString::number(images.at(0).height())));
    QVERIFY(images.at(1).height() > images.at(0).height());
    QVERIFY(images.at(1).width() > images.at(0).width());
    QVERIFY(document->find(QStringLiteral("E = mc^2")).isNull());
    // What cannot be drawn keeps its source, with the helper's own words.
    QTextCursor broken = document->find(QStringLiteral("\\frac{1}{"));
    QVERIFY(!broken.isNull());
    QCOMPARE(inkOf(broken.charFormat()), int(PreviewStyle::Danger));
    QVERIFY(!broken.charFormat().toolTip().isEmpty());
    // The display formula is an island, the diagram a picture told by its words.
    int formulas = 0;
    for (const Section &section : w.preview->sections())
        for (const Island &island : section.islands) formulas += island.kind == Island::Math;
    QCOMPARE(formulas, 1);
    bool described = false;
    for (const Section &section : w.preview->sections())
        for (const Mark &mark : section.marks) described = described || (mark.kind == Mark::Image && mark.text.contains(QStringLiteral("Draft")));
    QVERIFY(described);
    QVERIFY(images.at(2).width() > 100);
    w.shot("helpers");
}

void outlineAndPreferencesReachThePreview()
{
    PreviewWindow w(QStringLiteral("# One\n\ntext\n\n## Two\n\nmore\n\n") + longText(40) + QStringLiteral("# Last\n\nend\n"));
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    // The outline command shows the document's headings in the library,
    // with the keys on them, and hides them again.
    QVERIFY(!w.shell.settings()->outlineShown());
    w.run("outline");
    QVERIFY(w.shell.settings()->outlineShown());
    QQuickItem *list = w.shell.item(QStringLiteral("libraryOutlineList"));
    QVERIFY(list);
    QTRY_VERIFY(list->isVisible());
    QTRY_COMPARE(list->property("count").toInt(), 7);
    QTRY_VERIFY(inside(w.window->activeFocusItem(), list));
    QQuickItem *entry = qobject_cast<QQuickItem *>(w.shell.object(QStringLiteral("outlineEntry")));
    QVERIFY(entry);
    QCOMPARE(entry->property("text").toString(), QStringLiteral("Hide outline"));
    // A heading chosen there is shown in the reading view too.
    w.run("viewReading");
    QVERIFY(w.settled());
    QCOMPARE(contentY(w.flickable), 0.0);
    list->forceActiveFocus();
    list->setProperty("currentIndex", 6);
    QTest::keyClick(w.window, Qt::Key_Return);
    QTRY_VERIFY(contentY(w.flickable) > 500);
    const QTextBlock last = w.preview->textDocument()->find(QStringLiteral("Last")).block();
    const qreal top = w.preview->textDocument()->documentLayout()->blockBoundingRect(last).top();
    QVERIFY(top >= w.preview->viewTop() - 1 && top < w.preview->viewTop() + w.flickable->height());
    QTRY_COMPARE(w.window->activeFocusItem(), w.surface);
    w.run("outline");
    QVERIFY(!w.shell.settings()->outlineShown());
    QCOMPARE(entry->property("text").toString(), QStringLiteral("Show outline"));
    // In a window too narrow for the library beside the text, it opens over it.
    resizeWindow(w.window, 500);
    w.run("outline");
    QVERIFY(w.shell.settings()->outlineShown());
    QTRY_VERIFY(inside(w.window->activeFocusItem(), list));
    QTest::keyClick(w.window, Qt::Key_Escape);
    resizeWindow(w.window, 1280);

    // The preferences have a section for the preview; each choice applies at once and is kept.
    w.run("viewSplit");
    QVERIFY(w.settled());
    w.run("preferences");
    QObject *dialog = w.shell.object(QStringLiteral("preferencesDialog"));
    QTRY_VERIFY(dialog && dialog->property("opened").toBool());
    Settings *settings = w.shell.settings();
    const auto control = [&](const char *name) { return qobject_cast<QQuickItem *>(w.shell.object(QString::fromLatin1(name))); };
    for (const char *name : {"previewFontChoice", "previewTextSizeChoice", "previewFollowsTextSize", "previewLineHeightChoice",
                             "previewLineLengthChoice", "scrollSyncChoice", "remoteImagesChoice"})
        QVERIFY2(control(name), name);
    // The size is the text's and two pixels until one is chosen.
    QCOMPARE(settings->previewFontPixelSize(), settings->fontPixelSize() + 2);
    QVERIFY(!settings->previewSizeChosen());
    QCOMPARE(control("previewTextSizeChoice")->property("value").toInt(), settings->previewFontPixelSize());
    QVERIFY(!control("previewFollowsTextSize")->isEnabled());
    settings->zoom(2);
    QCOMPARE(settings->previewFontPixelSize(), settings->fontPixelSize() + 2);
    QTRY_COMPARE(w.preview->bodySize(), settings->fontPixelSize() + 2);
    settings->setPreviewFontPixelSize(24);
    QVERIFY(settings->previewSizeChosen());
    QTRY_COMPARE(w.preview->bodySize(), 24);
    QTRY_COMPARE(control("previewTextSizeChoice")->property("value").toInt(), 24);
    settings->zoom(1);
    QCOMPARE(settings->previewFontPixelSize(), 24);
    QVERIFY(control("previewFollowsTextSize")->isEnabled());
    QMetaObject::invokeMethod(control("previewFollowsTextSize"), "clicked");
    QVERIFY(!settings->previewSizeChosen());
    QTRY_COMPARE(w.preview->bodySize(), settings->fontPixelSize() + 2);
    // The line length sets the widest the column gets.
    QTest::keyClick(w.window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    w.run("viewReading");
    QTRY_VERIFY(!w.editorPane->isVisible());
    const qreal wide = w.surface->width();
    settings->setPreviewLineLength(40);
    QTRY_VERIFY(w.surface->width() < wide * 0.75);
    QVERIFY(qAbs(w.surface->x() - (w.previewPane->width() - w.surface->width()) / 2) <= 1);
    // The line height is the distance between the lines of a paragraph.
    QVERIFY(QTest::qWaitFor([&] { return !w.preview->working(); }));
    settings->setPreviewLineHeight(2.0);
    QTRY_COMPARE(w.preview->lineHeight(), 2.0);
    QVERIFY(QTest::qWaitFor([&] { return !w.preview->working(); }));
    const QTextBlock paragraph = w.preview->textDocument()->find(QStringLiteral("Paragraph 3 has")).block();
    QCOMPARE(paragraph.blockFormat().lineHeight(), qreal(qRound(w.preview->bodySize() * 2.0)));
    // Set again in another size, the preview comes back to the text it showed.
    const QTextBlock shown = w.preview->textDocument()->find(QStringLiteral("Paragraph 20 has")).block();
    w.flickable->setProperty("contentY", w.surface->y() + w.preview->textDocument()->documentLayout()->blockBoundingRect(shown).top());
    QCOMPARE(topLine(w).left(16), QStringLiteral("Paragraph 20 has"));
    settings->setPreviewFontPixelSize(30);
    QTRY_COMPARE(w.preview->bodySize(), 30);
    QVERIFY(QTest::qWaitFor([&] { return !w.preview->working(); }));
    QTRY_COMPARE(topLine(w).left(16), QStringLiteral("Paragraph 20 has"));
    settings->resetPreviewFontPixelSize();
    QVERIFY(QTest::qWaitFor([&] { return !w.preview->working(); }));
    // The face is the one chosen, or the default reading face.
    QCOMPARE(w.preview->bodyFamily(), PreviewStyle::defaultFamily());
    settings->setPreviewFontFamily(QStringLiteral("Liberation Sans"));
    QTRY_COMPARE(w.preview->bodyFamily(), QStringLiteral("Liberation Sans"));
    QCOMPARE(w.surface->property("font").value<QFont>().family(), QStringLiteral("Liberation Sans"));
    settings->setPreviewFontFamily(QString());
    QTRY_COMPARE(w.preview->bodyFamily(), PreviewStyle::defaultFamily());

    // All of it is kept, and a value out of range is not.
    settings->setPreviewFontPixelSize(22);
    settings->setScrollSync(false);
    // Pictures from the web are off until the writer turns them on.
    QVERIFY(!settings->remoteImages());
    settings->setRemoteImages(true);
    settings->setPreviewFontFamily(QStringLiteral("Liberation Serif"));
    settings->setPreviewLineHeight(9);
    settings->setPreviewLineLength(5);
    Settings again(w.shell.paths().settingsFile());
    QCOMPARE(again.previewFontPixelSize(), 22);
    QCOMPARE(again.previewLineHeight(), 2.0);
    QCOMPARE(again.previewLineLength(), 40);
    QCOMPARE(again.previewFontFamily(), QStringLiteral("Liberation Serif"));
    QVERIFY(!again.scrollSync());
    QVERIFY(again.remoteImages());
    // Damaged values are each their default.
    QTemporaryDir other;
    const QString file = other.filePath(QStringLiteral("settings.json"));
    writeFile(file, "{\"previewFontPixelSize\":900,\"previewLineHeight\":\"tall\",\"previewLineLength\":2,"
                    "\"previewFontFamily\":7,\"scrollSync\":\"yes\",\"remoteImages\":0}");
    Settings damaged(file);
    damaged.setThemeFontSize(12);
    QCOMPARE(damaged.previewFontPixelSize(), 18);
    QCOMPARE(damaged.previewLineHeight(), 1.6);
    QCOMPARE(damaged.previewLineLength(), 68);
    QVERIFY(damaged.previewFontFamily().isEmpty());
    QVERIFY(damaged.scrollSync());
    QVERIFY(!damaged.remoteImages());
}

void themeChangeRecoloursWithoutRebuilding()
{
    PreviewWindow w(essay + QStringLiteral("\nA [link](https://example.com) and ==marked== and `code`.\n"));
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewSplit");
    QVERIFY(w.settled());
    QTextDocument *document = w.preview->textDocument();
    const auto colourOf = [&](QTextDocument *in, const QString &text) {
        QTextCursor found = in->find(text);
        found.setPosition(found.selectionStart() + 1);
        return found.charFormat().foreground().color();
    };
    const auto islandOf = [&](const QString &kind) -> QTextDocument * {
        QAbstractItemModel *marks = w.preview->marks();
        for (int row = 0; row < marks->rowCount(); ++row)
            if (marks->data(marks->index(row, 0), PreviewMarks::IslandKindRole).toString() == kind)
                return w.preview->islandDocument(marks->data(marks->index(row, 0), PreviewMarks::IslandRole).toULongLong());
        return nullptr;
    };
    // Every colour is the theme's: the page, the text, a link, a heading,
    // and code in the colours made from the theme's own.
    const auto readable = [&](const char *theme) {
        const PreviewStyle &style = w.preview->style();
        QCOMPARE(w.previewPane->property("color").value<QColor>(), w.shell.theme->background());
        QCOMPARE(style.background, w.shell.theme->background());
        QCOMPARE(w.surface->property("color").value<QColor>(), style.text);
        QCOMPARE(style.text, w.shell.theme->foreground());
        QCOMPARE(colourOf(document, QStringLiteral("link")), style.ink(PreviewStyle::Link));
        QCOMPARE(colourOf(document, QStringLiteral("A room")), style.ink(PreviewStyle::Heading));
        QTextCursor marked = document->find(QStringLiteral("marked"));
        QCOMPARE(marked.charFormat().background().color(), style.paper(PreviewStyle::MarkPaper));
        QTextDocument *code = islandOf(QStringLiteral("code"));
        QVERIFY(code);
        QCOMPARE(colourOf(code, QStringLiteral("def")), style.ink(PreviewStyle::Keyword));
        QCOMPARE(colourOf(code, QStringLiteral("# the rest")), style.ink(PreviewStyle::Comment));
        // What is read stands out from what it is read on, by 4.5 to 1 or more.
        const QColor page = w.previewPane->property("color").value<QColor>();
        const QByteArray name(theme);
        QVERIFY2(contrastRatio(w.surface->property("color").value<QColor>(), page) >= 4.5, name);
        QVERIFY2(contrastRatio(colourOf(document, QStringLiteral("link")), page) >= 4.5, name);
        QVERIFY2(contrastRatio(colourOf(document, QStringLiteral("A room")), page) >= 4.5, name);
        const QColor tint = w.preview->codeTint();
        QVERIFY2(contrastRatio(colourOf(code, QStringLiteral("def")), tint) >= 4.5, name);
        QVERIFY2(contrastRatio(colourOf(code, QStringLiteral("begin")), tint) >= 4.5, name);
        QVERIFY2(contrastRatio(colourOf(code, QStringLiteral("# the rest")), tint) >= 4.5, name);
        QVERIFY2(contrastRatio(marked.charFormat().foreground().color(), marked.charFormat().background().color()) >= 4.5, name);
        // And so it is drawn: the page is the theme's background, and the
        // darkest or lightest thing on it is the text.
        const QImage shot = w.window->grabWindow();
        const QPointF corner = w.previewPane->mapToScene(QPointF(4, w.previewPane->height() - 4));
        QCOMPARE(shot.pixelColor(corner.toPoint()), page);
    };
    readable("light");
    if (QTest::currentTestFailed()) return;
    w.shot("light");

    // A selection, a scrolled view and what was built are all kept through
    // a change of theme: only the colours change.
    QTextCursor found = document->find(QStringLiteral("steady rhythm"));
    QMetaObject::invokeMethod(w.surface, "select", Q_ARG(int, found.selectionStart()), Q_ARG(int, found.selectionEnd()));
    const int built = w.preview->sectionsBuilt();
    const int updates = w.preview->updates();
    const QStringList lines = w.lines();
    const QColor lightLink = colourOf(document, QStringLiteral("link"));
    const QImage lightBox = document->resource(QTextDocument::ImageResource, QUrl(preview::Builder::taskImageName(true))).value<QImage>();
    replaceTheme(w.shell.themeFolder.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    QTRY_COMPARE(w.preview->style().background, QColor("#1a1b26"));
    QCOMPARE(w.preview->sectionsBuilt(), built);
    QCOMPARE(w.preview->updates(), updates);
    QCOMPARE(w.lines(), lines);
    QCOMPARE(w.surface->property("selectedText").toString(), QStringLiteral("steady rhythm"));
    QVERIFY(colourOf(document, QStringLiteral("link")) != lightLink);
    QVERIFY(w.preview->style().dark);
    // The boxes of tasks are drawn again in the new colours.
    const QImage darkBox = document->resource(QTextDocument::ImageResource, QUrl(preview::Builder::taskImageName(true))).value<QImage>();
    QVERIFY(darkBox != lightBox);
    readable("dark");
    if (QTest::currentTestFailed()) return;
    w.shot("dark");
    // Text typed after the change is built in the new colours.
    w.document->buffer()->replace({w.document->buffer()->length(), 0}, QStringLiteral("\nMore: [another](https://example.org).\n"));
    QVERIFY(w.settled());
    QCOMPARE(colourOf(document, QStringLiteral("another")), w.preview->style().ink(PreviewStyle::Link));

    // A change of the reading face or its size builds the text again, in
    // the same place.
    const int before = w.preview->sectionsBuilt();
    const int sections = int(w.preview->sections().size());
    w.shell.settings()->setPreviewFontPixelSize(26);
    QTRY_COMPARE(w.preview->bodySize(), 26);
    QVERIFY(QTest::qWaitFor([&] { return !w.preview->working(); }));
    QCOMPARE(w.preview->sectionsBuilt(), before + sections);
    QCOMPARE(w.surface->property("font").value<QFont>().pixelSize(), 26);
    QCOMPARE(w.lines().size(), document->blockCount());
    QVERIFY(w.lines().contains(QStringLiteral("A room for thought")));
    QCOMPARE(document->find(QStringLiteral("A room for thought")).charFormat().intProperty(QTextFormat::FontPixelSize), 52);
}

void narrowWindowsShowOnePaneOfASplit()
{
    PreviewWindow w(essay, 1280);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    QQuickItem *divider = w.shell.item(QStringLiteral("viewDivider"));
    QQuickItem *textName = w.shell.item(QStringLiteral("textPaneName"));
    QQuickItem *previewName = w.shell.item(QStringLiteral("previewPaneName"));
    QVERIFY(divider && textName && previewName);
    w.run("viewSplit");
    QVERIFY(w.settled());
    QVERIFY(w.editorPane->isVisible() && w.previewPane->isVisible() && divider->isVisible());
    // Over each pane stands its name.
    QVERIFY(textName->isVisible() && previewName->isVisible());
    QVERIFY(previewName->x() > w.previewPane->x());
    QVERIFY(textName->x() < w.editorPane->width());

    // Each pane keeps the room it takes to read in: at 640 pixels, where the
    // library gives way, there is just room for both.
    resizeWindow(w.window, 640);
    QTRY_VERIFY(w.editorPane->width() < 330);
    QVERIFY(divider->isVisible());
    QVERIFY(w.editorPane->isVisible() && w.previewPane->isVisible());
    QVERIFY(w.editorPane->width() >= w.window->property("minimumPane").toReal() - divider->width());
    QVERIFY(w.previewPane->width() >= w.window->property("minimumPane").toReal() - divider->width());
    QStringList fit;
    QVERIFY2(everyVisibleItemFitsWithin(w.window, &fit), qPrintable(fit.join("; ")));
    w.shot("split-640");

    // Too narrow for both: the split shows the text alone, with the names
    // of the two panes side by side to choose between.
    for (const int width : {560, 360}) {
        resizeWindow(w.window, width);
        QTRY_VERIFY(!divider->isVisible());
        QCOMPARE(w.tabs()->activeMode(), TabModel::Split);
        QVERIFY(w.editorPane->isVisible());
        QVERIFY(!w.previewPane->isVisible());
        QVERIFY(textName->isVisible() && previewName->isVisible());
        QVERIFY(previewName->x() >= textName->x() + textName->width());
        QStringList problems;
        QVERIFY2(everyVisibleItemFitsWithin(w.window, &problems), qPrintable(problems.join("; ")));
        // The preview's name shows the preview, across the whole width.
        QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, centreOf(previewName));
        QTRY_VERIFY(w.previewPane->isVisible());
        QVERIFY(!w.editorPane->isVisible());
        QCOMPARE(w.previewPane->width(), w.previewPane->parentItem()->width());
        QVERIFY(w.settled());
        QVERIFY(w.lines().contains(QStringLiteral("A room for thought")));
        QTRY_COMPARE(w.window->activeFocusItem(), w.surface);
        QVERIFY2(everyVisibleItemFitsWithin(w.window, &problems), qPrintable(problems.join("; ")));
        // Nothing of the reading text reaches past the column, however narrow.
        QVERIFY(w.surface->width() <= w.previewPane->width());
        QVERIFY(w.surface->property("contentWidth").toReal() <= w.surface->width() + 1);
        w.shot(width == 360 ? "narrow-360" : "narrow-560");
        // The keys switch back, and so does the text's name.
        w.run("otherPane");
        QTRY_VERIFY(w.editorPane->isVisible());
        QVERIFY(!w.previewPane->isVisible());
        QTRY_COMPARE(w.window->activeFocusItem(), w.editor);
        w.run("otherPane");
        QTRY_VERIFY(w.previewPane->isVisible());
        QTest::mouseClick(w.window, Qt::LeftButton, Qt::NoModifier, centreOf(textName));
        QTRY_VERIFY(w.editorPane->isVisible());
        QVERIFY(!w.previewPane->isVisible());
    }
    // What edits the text shows the text: a find asked for from the preview.
    w.run("otherPane");
    QTRY_VERIFY(w.previewPane->isVisible());
    w.run("findText");
    QTRY_VERIFY(w.editorPane->isVisible());
    QVERIFY(!w.previewPane->isVisible());
    QTest::keyClick(w.window, Qt::Key_Escape);

    // Wide again, both are back, since the view is still a split.
    w.run("otherPane");
    QTRY_VERIFY(w.previewPane->isVisible());
    resizeWindow(w.window, 1280);
    QTRY_VERIFY(divider->isVisible());
    QVERIFY(w.editorPane->isVisible() && w.previewPane->isVisible());
    // The reading view fits a narrow window as it is.
    w.run("viewReading");
    resizeWindow(w.window, 360);
    QTRY_VERIFY(w.previewPane->isVisible());
    QVERIFY(!w.editorPane->isVisible());
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(w.window, &problems), qPrintable(problems.join("; ")));
    // From the preview alone, a find brings the text back in a split's place.
    w.run("findText");
    QTRY_VERIFY(w.editorPane->isVisible());
    QCOMPARE(w.tabs()->activeMode(), TabModel::Source);
}

void dividerMovesAndIsRemembered()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString first = root.filePath(QStringLiteral("docs/first.md")), second = root.filePath(QStringLiteral("docs/second.md"));
    writeFile(first, essay.toUtf8());
    writeFile(second, "# Second\n\nIts own text.\n");
    ShellOptions options;
    options.root = root.filePath(QStringLiteral("app"));
    options.files = {first, second};
    qreal remembered = 0;
    {
        PreviewWindow w(QString(), 1280, options);
        QVERIFY2(w.ok(), qPrintable(w.shell.error));
        QQuickItem *divider = w.shell.item(QStringLiteral("viewDivider"));
        QVERIFY(divider);
        QCOMPARE(w.tabs()->rowCount(), 2);
        // The second document reads alone; the first is split, its divider moved.
        QCOMPARE(w.tabs()->activePath(), second);
        w.run("viewReading");
        w.tabs()->setActiveIndex(0);
        w.run("viewSplit");
        QVERIFY(w.settled());
        QCOMPARE(w.tabs()->activeSplit(), 0.5);
        const qreal room = w.editorPane->parentItem()->width();
        // Dragged, it gives the text more room and the preview less.
        const QPoint from = centreOf(divider);
        QTest::mousePress(w.window, Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 10; ++step) QTest::mouseMove(w.window, from + QPoint(step * 12, 0));
        QTest::mouseRelease(w.window, Qt::LeftButton, Qt::NoModifier, from + QPoint(120, 0));
        QTRY_VERIFY(w.tabs()->activeSplit() > 0.55);
        QVERIFY(qAbs(w.tabs()->activeSplit() - (0.5 + 120 / room)) < 0.03);
        QTRY_VERIFY(qAbs(w.editorPane->width() - room * w.tabs()->activeSplit()) <= divider->width());
        // With the keys: Tab reaches it, Left and Right move it, Home centres it.
        divider->forceActiveFocus(Qt::TabFocusReason);
        QCOMPARE(w.window->activeFocusItem(), divider);
        const qreal dragged = w.tabs()->activeSplit();
        QTest::keyClick(w.window, Qt::Key_Left);
        QCOMPARE(w.tabs()->activeSplit(), dragged - 0.02);
        QTest::keyClick(w.window, Qt::Key_Right);
        QTest::keyClick(w.window, Qt::Key_Right);
        QVERIFY(qAbs(w.tabs()->activeSplit() - (dragged + 0.02)) < 1e-9);
        QTest::keyClick(w.window, Qt::Key_Home);
        QCOMPARE(w.tabs()->activeSplit(), 0.5);
        // It stops where a pane would be too narrow to read in.
        for (int i = 0; i < 40; ++i) QTest::keyClick(w.window, Qt::Key_Left);
        QVERIFY(w.tabs()->activeSplit() >= 0.2);
        QVERIFY(w.editorPane->width() >= w.window->property("minimumPane").toReal() - divider->width());
        const qreal least = w.tabs()->activeSplit();
        QTest::keyClick(w.window, Qt::Key_Left);
        QCOMPARE(w.tabs()->activeSplit(), least);
        for (int i = 0; i < 6; ++i) QTest::keyClick(w.window, Qt::Key_Right);
        const qreal kept = w.tabs()->activeSplit();
        // It is named for assistive technology, and says where it is.
        QAccessibleInterface *access = QAccessible::queryAccessibleInterface(divider);
        QVERIFY(access);
        QCOMPARE(access->text(QAccessible::Name), QStringLiteral("Divider between the text and the preview"));
        // Each tab keeps its own view.
        w.tabs()->setActiveIndex(1);
        QCOMPARE(w.tabs()->activeMode(), TabModel::Reading);
        QTRY_VERIFY(!w.editorPane->isVisible());
        w.tabs()->setActiveIndex(0);
        QCOMPARE(w.tabs()->activeMode(), TabModel::Split);
        QCOMPARE(w.tabs()->activeSplit(), kept);
        QTRY_VERIFY(w.editorPane->isVisible());
        // Quitting keeps the session.
        QSignalSpy quits(w.shell.backend.get(), &Backend::quitRequested);
        w.shell.backend->quit();
        QCOMPARE(quits.count(), 1);
        options.settings = readFile(w.shell.paths().settingsFile());
        remembered = kept;
    }
    // The next launch names no files: the session brings them back, each
    // view as it was.
    options.files.clear();
    PreviewWindow again(QString(), 1280, options);
    QVERIFY2(again.ok(), qPrintable(again.shell.error));
    QCOMPARE(again.tabs()->rowCount(), 2);
    QCOMPARE(again.tabs()->activeIndex(), 0);
    QCOMPARE(again.tabs()->activeMode(), TabModel::Split);
    QCOMPARE(again.tabs()->activeSplit(), remembered);
    QTRY_VERIFY(again.previewPane->isVisible());
    QVERIFY(again.editorPane->isVisible());
    QVERIFY(again.settled());
    QVERIFY(again.lines().contains(QStringLiteral("A room for thought")));
    QQuickItem *restored = again.shell.item(QStringLiteral("viewDivider"));
    QTRY_VERIFY(qAbs(restored->x() + restored->width() / 2 - again.editorPane->parentItem()->width() * remembered) <= 1);
    again.tabs()->setActiveIndex(1);
    QCOMPARE(again.tabs()->activeMode(), TabModel::Reading);
    QTRY_VERIFY(!again.editorPane->isVisible());
    QVERIFY(again.settled());
    QVERIFY(again.lines().contains(QStringLiteral("Its own text.")));
}

namespace {
// The text an item of the window shows, or "(hidden)" while it is not shown.
QString shownIn(const PreviewWindow &w, const char *name)
{
    QQuickItem *item = w.shell.item(QString::fromLatin1(name));
    return item && item->isVisible() ? item->property("text").toString() : QStringLiteral("(hidden)");
}

bool isOpen(const QObject *popup) { return popup && popup->property("opened").toBool(); }
} // namespace

void vimKeysAreTheTextsAlone()
{
    // Vim keys are keys of the text. With the keys in the preview beside
    // it, they move no caret, change no text and no mode; where the preview
    // is shown alone there is no mode to show. The palette and the menu
    // open all the while.
    const QString text = QStringLiteral("# Title\n\nFirst words here.\n\nSecond words here.\n");
    PreviewWindow w(text);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.shell.settings()->setVimKeys(true);
    w.run("viewSplit");
    QVERIFY(w.settled());
    EditorBinding *binding = w.shell.binding();
    VimMode *vim = binding->vim();
    QObject *palette = w.shell.object(QStringLiteral("commandPalette"));
    QObject *menu = w.shell.object(QStringLiteral("windowMenu"));
    QVERIFY(vim && palette && menu);

    // In the text they act: j goes down a line, x takes a character.
    w.editor->forceActiveFocus();
    binding->restore(0, 0, 0);
    QTRY_COMPARE(shownIn(w, "vimMode"), QStringLiteral("NORMAL"));
    sendKey(w.window, u'j');
    sendKey(w.window, u'j');
    QCOMPARE(binding->cursorSource(), int(text.indexOf(QStringLiteral("First"))));
    sendKey(w.window, u'x');
    const QString edited = QString(text).remove(text.indexOf(QStringLiteral("First")), 1);
    QCOMPARE(w.document->buffer()->text(), edited);
    const int caret = binding->cursorSource();

    // In the preview they do nothing to the text, its caret or the mode.
    w.run("otherPane");
    QTRY_VERIFY(w.surface->hasActiveFocus());
    const auto untouched = [&] {
        return w.document->buffer()->text() == edited && binding->cursorSource() == caret
               && binding->anchorSource() == caret && vim->mode() == VimMode::Normal && vim->pending().isEmpty()
               && vim->commandLine().isEmpty();
    };
    for (const QChar key : QStringLiteral("jkhlxiovd2G:/u"))
        sendKey(w.window, key);
    sendKey(w.window, Qt::Key_Return);
    QVERIFY(untouched());
    // The mode beside the text still shows, since the text does.
    QCOMPARE(shownIn(w, "vimMode"), QStringLiteral("NORMAL"));
    // The preview keeps its own keys: Ctrl+A selects what it shows.
    QTest::keyClick(w.window, Qt::Key_A, Qt::ControlModifier);
    QVERIFY(w.surface->property("selectedText").toString().contains(QStringLiteral("Second words here.")));
    QVERIFY(untouched());
    // The palette and the menu open from it, and give the keys back to it.
    QTest::keyClick(w.window, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(isOpen(palette));
    QTest::keyClick(w.window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(palette));
    QTRY_VERIFY(w.surface->hasActiveFocus());
    QTest::keyClick(w.window, Qt::Key_F10);
    QTRY_VERIFY(isOpen(menu));
    QTest::keyClick(w.window, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());
    QTRY_VERIFY(w.surface->hasActiveFocus());
    QVERIFY(untouched());

    // Chosen from the menu, the other pane takes the keys and has them once
    // the menu has closed: back in the text, Vim's keys act again.
    QTest::keyClick(w.window, Qt::Key_F10);
    QTRY_VERIFY(isOpen(menu));
    QVERIFY(QMetaObject::invokeMethod(menu, "commandChosen", Q_ARG(QString, QStringLiteral("otherPane"))));
    QVERIFY(QMetaObject::invokeMethod(menu, "close"));
    QTRY_VERIFY(!menu->property("visible").toBool());
    QTRY_VERIFY(w.editor->hasActiveFocus());
    sendKey(w.window, u'x');
    QCOMPARE(w.document->buffer()->text(), QString(edited).remove(caret, 1));
    sendKey(w.window, u'u');
    QCOMPARE(w.document->buffer()->text(), edited);
    binding->restore(caret, caret, 0);

    // The preview alone: no mode is shown, the keys do nothing, and the
    // palette and the menu still open.
    w.run("viewReading");
    QTRY_VERIFY(!w.editorPane->isVisible());
    QTRY_VERIFY(w.surface->hasActiveFocus());
    QCOMPARE(shownIn(w, "vimArea"), QStringLiteral("(hidden)"));
    QCOMPARE(shownIn(w, "vimMode"), QStringLiteral("(hidden)"));
    for (const QChar key : QStringLiteral("jxiZddp:q"))
        sendKey(w.window, key);
    sendKey(w.window, Qt::Key_Return);
    QVERIFY(untouched());
    QCOMPARE(w.tabs()->rowCount(), 1);
    QTest::keyClick(w.window, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(isOpen(palette));
    QTest::keyClick(w.window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(palette));
    QTRY_VERIFY(w.surface->hasActiveFocus());
    QTest::keyClick(w.window, Qt::Key_F10);
    QTRY_VERIFY(isOpen(menu));
    QTest::keyClick(w.window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(menu));
    QTRY_VERIFY(w.surface->hasActiveFocus());
    QVERIFY(untouched());

    // The text shown again, its mode is too, as it was left.
    w.run("viewSource");
    QTRY_VERIFY(w.editor->hasActiveFocus());
    QTRY_COMPARE(shownIn(w, "vimMode"), QStringLiteral("NORMAL"));
    sendKey(w.window, u'i');
    QCOMPARE(shownIn(w, "vimMode"), QStringLiteral("INSERT"));
}

void focusModeDimsTheTextAlone()
{
    // Focus mode dims the text being written, not what it reads as: the
    // preview beside it, and the preview alone, stay as they were.
    PreviewWindow w(QStringLiteral("# Title\n\nOne. Two sentences here.\n\nAnother *paragraph* with `code`.\n"));
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewSplit");
    QVERIFY(w.settled());
    const Theme &theme = *w.shell.theme;
    auto *decorations = w.editorPane->findChild<EditorDecorations *>(QStringLiteral("decorations"));
    QVERIFY(decorations);
    QVERIFY(!decorations->focusing());
    QCOMPARE(w.editor->property("color").value<QColor>(), theme.foreground());
    const QColor reading = w.surface->property("color").value<QColor>();
    QCOMPARE(reading, w.preview->style().text);
    const QString html = w.preview->textDocument()->toHtml();
    const int built = w.preview->sectionsBuilt();
    const int updates = w.preview->updates();

    w.editor->forceActiveFocus();
    QTest::keyClick(w.window, Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(w.window->property("focusMode").toBool());
    QVERIFY(decorations->focusing());
    QCOMPARE(w.editor->property("color").value<QColor>(),
             FocusMode::dimmed(theme.foreground(), theme.background(), FocusMode::Strength::Strong));
    QTest::qWait(2 * Preview::PauseMs);
    QCOMPARE(w.surface->property("color").value<QColor>(), reading);
    QCOMPARE(w.preview->textDocument()->toHtml(), html);
    QCOMPARE(w.preview->sectionsBuilt(), built);
    QCOMPARE(w.preview->updates(), updates);
    QCOMPARE(w.surface->property("opacity").toReal(), 1.0);
    QCOMPARE(w.previewPane->property("opacity").toReal(), 1.0);

    // With the keys in the preview, and with the preview alone, the same.
    w.run("otherPane");
    QTRY_VERIFY(w.surface->hasActiveFocus());
    QVERIFY(decorations->focusing());
    QCOMPARE(w.surface->property("color").value<QColor>(), reading);
    w.run("viewReading");
    QTRY_VERIFY(!w.editorPane->isVisible());
    QVERIFY(w.settled());
    QVERIFY(w.window->property("focusMode").toBool());
    QCOMPARE(w.surface->property("color").value<QColor>(), reading);
    QCOMPARE(w.preview->textDocument()->toHtml(), html);
    // Its key turns it off and on from there too, for the text alone.
    QTest::keyClick(w.window, Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(!w.window->property("focusMode").toBool());
    QTest::keyClick(w.window, Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(w.window->property("focusMode").toBool());
    QCOMPARE(w.surface->property("color").value<QColor>(), reading);
    QCOMPARE(w.preview->textDocument()->toHtml(), html);

    // The text shown again is dimmed as it was.
    w.run("viewSource");
    QTRY_VERIFY(w.editorPane->isVisible());
    QVERIFY(decorations->focusing());
    QCOMPARE(w.editor->property("color").value<QColor>(),
             FocusMode::dimmed(theme.foreground(), theme.background(), FocusMode::Strength::Strong));
}

void distractionFreeWritingKeepsEachView()
{
    // F11 hides everything but the document in whichever view shows it: the
    // text and the preview side by side stay side by side, the preview
    // alone stays alone, each where it was, and the keys where they were.
    PreviewWindow w(longText(120), 1440);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    const QString text = w.document->buffer()->text();
    EditorBinding *binding = w.shell.binding();
    QQuickItem *panes = w.shell.item(QStringLiteral("panes"));
    QQuickItem *divider = w.shell.item(QStringLiteral("viewDivider"));
    QVERIFY(panes && divider);
    const QStringList chrome{"header", "toolbar", "statusLine", "library", "paneNames"};
    const auto chromeShown = [&](bool shown) {
        for (const QString &name : chrome)
            if (w.shell.item(name)->isVisible() != shown) return false;
        return true;
    };
    const auto fillsTheWindow = [&](QQuickItem *item) {
        return item->mapToScene(QPointF(0, 0)) == QPointF(0, 0) && item->width() == qreal(w.window->width())
               && item->height() == qreal(w.window->height());
    };

    // Side by side.
    w.run("viewSplit");
    QVERIFY(w.settled());
    QTRY_VERIFY(w.shell.item(QStringLiteral("library"))->isVisible());
    w.editor->forceActiveFocus();
    const int start = int(text.indexOf(QStringLiteral("Paragraph 42 ")));
    binding->restore(start + 9, start, start);
    QTRY_COMPARE(binding->topSourcePosition(), start);
    QTRY_COMPARE(topLine(w).left(13), QStringLiteral("Paragraph 42 "));
    QVERIFY(chromeShown(true));
    const qreal split = w.tabs()->activeSplit();

    QTest::keyClick(w.window, Qt::Key_F11);
    QVERIFY(w.window->property("distractionFree").toBool());
    QTRY_VERIFY(chromeShown(false));
    QCOMPARE(w.tabs()->activeMode(), TabModel::Split);
    QVERIFY(w.editorPane->isVisible() && w.previewPane->isVisible() && divider->isVisible());
    QVERIFY(fillsTheWindow(panes));
    QCOMPARE(w.editorPane->height(), panes->height());
    QCOMPARE(w.previewPane->height(), panes->height());
    QCOMPARE(w.editorPane->width() + divider->width() + w.previewPane->width(), panes->width());
    QCOMPARE(w.tabs()->activeSplit(), split);
    QTRY_COMPARE(binding->topSourcePosition(), start);
    QCOMPARE(binding->cursorSource(), start + 9);
    QCOMPARE(binding->anchorSource(), start);
    QTRY_COMPARE(topLine(w).left(13), QStringLiteral("Paragraph 42 "));
    QTRY_VERIFY(w.editor->hasActiveFocus());
    QVERIFY(w.settled());
    w.shot("distraction-free-split");
    // The keys go to the other pane and back as they do with everything shown.
    w.run("otherPane");
    QTRY_VERIFY(w.surface->hasActiveFocus());
    // F11 from the preview leaves, and the preview keeps the keys.
    QTest::keyClick(w.window, Qt::Key_F11);
    QVERIFY(!w.window->property("distractionFree").toBool());
    QTRY_VERIFY(chromeShown(true));
    QTRY_VERIFY(w.surface->hasActiveFocus());
    QTRY_COMPARE(binding->topSourcePosition(), start);
    QTRY_COMPARE(topLine(w).left(13), QStringLiteral("Paragraph 42 "));
    // And enters again from it; Escape in the preview leaves.
    QTest::keyClick(w.window, Qt::Key_F11);
    QTRY_VERIFY(chromeShown(false));
    QTRY_VERIFY(w.surface->hasActiveFocus());
    QTest::keyClick(w.window, Qt::Key_Escape);
    QVERIFY(!w.window->property("distractionFree").toBool());
    QTRY_VERIFY(chromeShown(true));
    QTRY_VERIFY(w.surface->hasActiveFocus());

    // The preview alone.
    w.run("viewReading");
    QTRY_VERIFY(!w.editorPane->isVisible());
    QVERIFY(w.settled());
    w.preview->showSource(int(text.indexOf(QStringLiteral("Paragraph 77 "))));
    QTRY_COMPARE(topLine(w).left(13), QStringLiteral("Paragraph 77 "));
    QTRY_VERIFY(w.surface->hasActiveFocus());
    QTest::keyClick(w.window, Qt::Key_F11);
    QVERIFY(w.window->property("distractionFree").toBool());
    for (const char *name : {"header", "toolbar", "statusLine", "library"})
        QTRY_VERIFY2(!w.shell.item(QString::fromLatin1(name))->isVisible(), name);
    QCOMPARE(w.tabs()->activeMode(), TabModel::Reading);
    QVERIFY(w.previewPane->isVisible());
    QVERIFY(!w.editorPane->isVisible());
    QVERIFY(!divider->isVisible());
    QVERIFY(fillsTheWindow(w.previewPane));
    // Its column is centred in the whole window.
    QVERIFY(qAbs(w.surface->mapToScene(QPointF(w.surface->width() / 2, 0)).x() - w.window->width() / 2.0) <= 1);
    QTRY_COMPARE(topLine(w).left(13), QStringLiteral("Paragraph 77 "));
    QTRY_VERIFY(w.surface->hasActiveFocus());
    QVERIFY(w.settled());
    w.shot("distraction-free-reading");
    // The menu still opens, and says how to leave.
    QObject *menu = w.shell.object(QStringLiteral("windowMenu"));
    QTest::keyClick(w.window, Qt::Key_F10);
    QTRY_VERIFY(isOpen(menu));
    QCOMPARE(w.shell.object(QStringLiteral("distractionFreeEntry"))->property("text").toString(),
             QStringLiteral("Leave distraction-free writing"));
    QTest::keyClick(w.window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(menu));
    QTRY_VERIFY(w.surface->hasActiveFocus());
    // The views change within it: the text comes beside the preview.
    w.run("viewSplit");
    QTRY_VERIFY(w.editorPane->isVisible());
    QVERIFY(w.window->property("distractionFree").toBool());
    QVERIFY(fillsTheWindow(panes));
    w.run("viewReading");
    QTRY_VERIFY(!w.editorPane->isVisible());
    w.preview->showSource(int(text.indexOf(QStringLiteral("Paragraph 77 "))));
    QTRY_COMPARE(topLine(w).left(13), QStringLiteral("Paragraph 77 "));
    // F11 brings everything back around the preview, where it was, but the
    // toolbar, which the preview alone never shows.
    QTest::keyClick(w.window, Qt::Key_F11);
    QVERIFY(!w.window->property("distractionFree").toBool());
    for (const char *name : {"header", "statusLine", "library"})
        QTRY_VERIFY2(w.shell.item(QString::fromLatin1(name))->isVisible(), name);
    QVERIFY(!w.shell.item(QStringLiteral("toolbar"))->isVisible());
    QCOMPARE(w.tabs()->activeMode(), TabModel::Reading);
    QTRY_COMPARE(topLine(w).left(13), QStringLiteral("Paragraph 77 "));
    QTRY_VERIFY(w.surface->hasActiveFocus());
    // Nothing was typed into the text by any of it.
    QCOMPARE(w.document->buffer()->text(), text);
}

namespace {
// Two works, as a bibliography file names them.
const char *references = R"(@book{doe2006,
  author    = {Doe, John},
  title     = {Writing with examples},
  publisher = {Example Press},
  year      = {2006}
}

@book{roe2010,
  author    = {Roe, Jane},
  title     = {Reading without examples},
  publisher = {Sample House},
  year      = {2010}
}
)";

// Front matter that names the bibliography beside the document and a style.
QString cited(const char *style, const char *more = "")
{
    return QStringLiteral("---\nbibliography: refs.bib\ncsl: %1\n%2---\n\n").arg(QLatin1String(style), QLatin1String(more));
}

// The line of a built document that holds a text.
QString lineWith(const Built &built, const QString &text)
{
    for (const QString &line : built.lines())
        if (line.contains(text)) return line;
    return QString();
}

// The Entry sections of a built document, in order.
QList<const Section *> entriesOf(const Built &built)
{
    QList<const Section *> entries;
    for (const Section &section : built.sections)
        if (section.kind == Part::Entry) entries << &section;
    return entries;
}
} // namespace

void citationsReadAsTheirStyleWordsThem()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    writeFile(folder.filePath(QStringLiteral("refs.bib")), references);
    const QChar gap(0x2002), back(0x21a9);
    const QString text = QStringLiteral("As shown [@doe2006, p. 23], and later [@roe2010; @doe2006].\n\nA second paragraph.\n");

    // Author and date: each citation in the style's words, and after the
    // text a rule and the style's bibliography, in its order, a title in
    // italics, every line of an entry after its first set in.
    const auto apa = buildRead(cited("apa") + text, folder.path());
    QVERIFY(apa->reading.formatted.active);
    QVERIFY2(apa->lines().contains(QStringLiteral("As shown (Doe, 2006, p. 23), and later (Doe, 2006; Roe, 2010).")),
             qPrintable(apa->lines().join(u'|')));
    QCOMPARE(apa->marks(Mark::NotesRule).size(), 1);
    const qsizetype doe = apa->lines().indexOf(QStringLiteral("Doe, J. (2006). Writing with examples. Example Press."));
    QVERIFY2(doe > 0, qPrintable(apa->lines().join(u'|')));
    QCOMPARE(apa->lines().at(doe + 1), QStringLiteral("Roe, J. (2010). Reading without examples. Sample House."));
    QVERIFY(apa->formatOf(QStringLiteral("Writing with examples")).fontItalic());
    QVERIFY(!apa->formatOf(QStringLiteral("Doe, J. (2006)")).fontItalic());
    const QTextBlockFormat hanging = apa->blockOf(QStringLiteral("Doe, J. (2006)")).blockFormat();
    QVERIFY(hanging.leftMargin() > 0);
    QCOMPARE(hanging.textIndent(), -hanging.leftMargin());
    // An entry is set smaller than the text, as a note is, and a link can go to it.
    QVERIFY(apa->formatOf(QStringLiteral("Doe, J. (2006)")).intProperty(QTextFormat::FontPixelSize) < 20);
    QCOMPARE(apa->names().value(QStringLiteral("ref-doe2006")), apa->blockOf(QStringLiteral("Doe, J. (2006)")).position());
    QCOMPARE(apa->names().value(QStringLiteral("ref-roe2010")), apa->blockOf(QStringLiteral("Roe, J. (2010)")).position());
    // The citations are plain text of the paragraph: no number, no link.
    QVERIFY(!apa->formatOf(QStringLiteral("(Doe, 2006, p. 23)")).isAnchor());
    QCOMPARE(inkOf(apa->formatOf(QStringLiteral("(Doe, 2006, p. 23)"))), int(PreviewStyle::NoInk));
    QVERIFY(!apa->names().contains(QStringLiteral("cite-1")));
    // An entry from a bibliography file has no place in the text.
    QCOMPARE(entriesOf(*apa).size(), 2);
    QCOMPARE(entriesOf(*apa).first()->sourceStart, -1);

    // The document can ask for its citations to link to their entries: one
    // that names a single work then leads to it.
    const auto linked = buildRead(cited("apa", "link-citations: true\n") + text, folder.path());
    QCOMPARE(linked->formatOf(QStringLiteral("(Doe, 2006, p. 23)")).anchorHref(), QStringLiteral("#ref-doe2006"));
    QCOMPARE(inkOf(linked->formatOf(QStringLiteral("(Doe, 2006, p. 23)"))), int(PreviewStyle::Link));
    QVERIFY(!linked->formatOf(QStringLiteral("(Doe, 2006; Roe, 2010)")).isAnchor());

    // Numbers: by first citation, the bibliography in that order, each
    // number hung before its entry and every entry's text starting at one place.
    const auto ieee = buildRead(cited("ieee") + QStringLiteral("First [@roe2010], then [@doe2006, p. 23] and [@roe2010].\n"),
                                folder.path());
    QVERIFY2(ieee->lines().contains(QStringLiteral("First [1], then [2, p. 23] and [1].")), qPrintable(ieee->lines().join(u'|')));
    const QString one = lineWith(*ieee, QStringLiteral("J. Roe"));
    const QString two = lineWith(*ieee, QStringLiteral("J. Doe"));
    QVERIFY2(one.startsWith(QStringLiteral("[1]") + gap + QStringLiteral("J. Roe, Reading without examples")),
             qPrintable(ieee->lines().join(u'|')));
    QVERIFY2(two.startsWith(QStringLiteral("[2]") + gap + QStringLiteral("J. Doe, Writing with examples")),
             qPrintable(ieee->lines().join(u'|')));
    QVERIFY(ieee->lines().indexOf(one) < ieee->lines().indexOf(two));
    QVERIFY(ieee->formatOf(QStringLiteral("Reading without examples")).fontItalic());
    const QTextBlockFormat numbered = ieee->blockOf(QStringLiteral("J. Roe")).blockFormat();
    QVERIFY(numbered.leftMargin() > 0);
    QCOMPARE(numbered.textIndent(), -numbered.leftMargin());
    QCOMPARE(ieee->blockOf(QStringLiteral("J. Doe")).blockFormat().leftMargin(), numbered.leftMargin());
    // The number is dimmed, as a list's marker is.
    QCOMPARE(inkOf(ieee->formatOf(QStringLiteral("[1]") + gap)), int(PreviewStyle::Dim));
    QCOMPARE(ieee->marks(Mark::NotesRule).size(), 1);

    // Notes: each citation in the text is a note of its own, numbered with
    // the notes the text has, its words the note's; a citation inside a
    // note stays in it; a work cited again is shortened; the bibliography
    // follows the notes.
    const auto notes = buildRead(cited("chicago-notes-bibliography")
                                     + QStringLiteral("First a note.[^a] Then a work [@doe2006, p. 23]. Again [@doe2006, p. 24]. "
                                                      "And a note that cites.[^b]\n\n"
                                                      "[^a]: Plain.\n[^b]: See [@roe2010]\n"),
                                 folder.path());
    QVERIFY(notes->reading.formatted.noteStyle);
    QVERIFY2(notes->lines().contains(QStringLiteral("First a note.1 Then a work 2. Again 3. And a note that cites.4")),
             qPrintable(notes->lines().join(u'|')));
    const QTextCursor mark = notes->find(QStringLiteral("work 2"));
    QTextCursor number(const_cast<QTextDocument *>(&notes->document));
    number.setPosition(mark.selectionEnd());
    QCOMPARE(number.charFormat().verticalAlignment(), QTextCharFormat::AlignSuperScript);
    QCOMPARE(number.charFormat().anchorHref(), QStringLiteral("#fn-2"));
    QCOMPARE(notes->names().value(QStringLiteral("fnref-2")), mark.selectionEnd() - 1);
    const QStringList noteLines = notes->lines();
    QVERIFY2(noteLines.contains(QStringLiteral("1.") + gap + QStringLiteral("Plain. ") + back), qPrintable(noteLines.join(u'|')));
    QVERIFY2(noteLines.contains(QStringLiteral("2.") + gap + QStringLiteral("John Doe, Writing with Examples (Example Press, 2006), 23. ")
                                + back),
             qPrintable(noteLines.join(u'|')));
    QVERIFY2(noteLines.contains(QStringLiteral("3.") + gap + QStringLiteral("Doe, Writing with Examples, 24. ") + back),
             qPrintable(noteLines.join(u'|')));
    QVERIFY2(noteLines.contains(QStringLiteral("4.") + gap
                                + QStringLiteral("See Jane Roe, Reading without Examples (Sample House, 2010). ") + back),
             qPrintable(noteLines.join(u'|')));
    QVERIFY(notes->formatOf(QStringLiteral("Reading without Examples (")).fontItalic());
    QVERIFY(notes->formatOf(QStringLiteral("Writing with Examples (")).fontItalic());
    QCOMPARE(notes->names().value(QStringLiteral("fn-2")), notes->blockOf(QStringLiteral("John Doe, Writing")).position());
    QCOMPARE(notes->find(QString(back), notes->blockOf(QStringLiteral("John Doe, Writing")).position()).charFormat().anchorHref(),
             QStringLiteral("#fnref-2"));
    // A rule before the notes and one before the bibliography, which lists each work once.
    QCOMPARE(notes->marks(Mark::NotesRule).size(), 2);
    QCOMPARE(entriesOf(*notes).size(), 2);
    QVERIFY2(!lineWith(*notes, QStringLiteral("Doe, John.")).isEmpty(), qPrintable(noteLines.join(u'|')));
    QVERIFY(noteLines.indexOf(lineWith(*notes, QStringLiteral("Doe, John."))) > noteLines.indexOf(lineWith(*notes, QStringLiteral("Doe, Writing with Examples, 24."))));
    // The mark of a citation's note comes from the citation's place in the text.
    const Section &paragraph = notes->sections.at(0);
    QVERIFY(paragraph.sourceStart >= 0);

    // A key no bibliography has stays in view, in the style's words for it,
    // in the colour of what is wrong, and says what it lacks; the works
    // that are there read as ever, and only they are listed.
    const auto missing = buildRead(cited("apa") + QStringLiteral("See [@nokey] and [@doe2006] and [@roe2010; @nothere].\n"), folder.path());
    const QString line = lineWith(*missing, QStringLiteral("See "));
    QVERIFY2(line.contains(QStringLiteral("nokey")) && line.contains(QStringLiteral("(Doe, 2006)")) && line.contains(QStringLiteral("nothere")),
             qPrintable(missing->lines().join(u'|')));
    QCOMPARE(inkOf(missing->formatOf(QStringLiteral("nokey"))), int(PreviewStyle::Danger));
    QVERIFY(missing->formatOf(QStringLiteral("nokey")).toolTip().contains(QStringLiteral("nokey")));
    QCOMPARE(inkOf(missing->formatOf(QStringLiteral("nothere"))), int(PreviewStyle::Danger));
    QVERIFY(missing->formatOf(QStringLiteral("nothere")).toolTip().contains(QStringLiteral("nothere")));
    QCOMPARE(inkOf(missing->formatOf(QStringLiteral("(Doe, 2006)"))), int(PreviewStyle::NoInk));
    QVERIFY(missing->formatOf(QStringLiteral("(Doe, 2006)")).toolTip().isEmpty());
    QCOMPARE(entriesOf(*missing).size(), 2);

    // A work the text itself defines is listed with those of the file, and
    // keeps its place in the text.
    const QString own = cited("apa") + QStringLiteral("One [p. 3][#Own:2001] and [@doe2006].\n\n[#Own:2001]: Own, A. *My Work*. 2001.\n");
    const auto both = buildRead(own, folder.path());
    QCOMPARE(entriesOf(*both).size(), 2);
    QVERIFY2(!lineWith(*both, QStringLiteral("Own, A. My Work. 2001.")).isEmpty(), qPrintable(both->lines().join(u'|')));
    QVERIFY(both->formatOf(QStringLiteral("My Work")).fontItalic());
    int placed = 0;
    for (const Section *entry : entriesOf(*both)) {
        if (entry->sourceStart < 0) continue;
        ++placed;
        QCOMPARE(entry->sourceStart, int(own.indexOf(QStringLiteral("[#Own:2001]:"))));
    }
    QCOMPARE(placed, 1);

    // Without a style or a bibliography the document's own numbers stand.
    const auto plain = buildRead(QStringLiteral("As shown [p. 23][#doe].\n\n[#doe]: Doe, J. *A Book*. 2020.\n"), folder.path());
    QVERIFY(!plain->reading.formatted.active);
    QVERIFY(plain->lines().contains(QStringLiteral("As shown [1, p. 23].")));
    QVERIFY(plain->lines().contains(QStringLiteral("[1]") + gap + QStringLiteral("Doe, J. A Book. 2020.")));

    // A part reads as the style words its citations: its key follows them,
    // though its own source is as it was, and no other part's key does.
    const QString before = cited("ieee") + QStringLiteral("Opening words.\n\nThen [@doe2006].\n\nClosing words.\n");
    const QString after = cited("ieee") + QStringLiteral("Opening [@roe2010].\n\nThen [@doe2006].\n\nClosing words.\n");
    const auto first = buildRead(before, folder.path()), second = buildRead(after, folder.path());
    QVERIFY(first->lines().contains(QStringLiteral("Then [1].")));
    QVERIFY(second->lines().contains(QStringLiteral("Then [2].")));
    const std::vector<Part> was = preview::parts(first->reading.document, &first->reading.formatted);
    const std::vector<Part> is = preview::parts(second->reading.document, &second->reading.formatted);
    QCOMPARE(was.size(), size_t(3 + 1 + 1));
    QCOMPARE(is.size(), size_t(3 + 1 + 2));
    QVERIFY(was[0].key != is[0].key);
    QVERIFY(was[1].key != is[1].key);
    QCOMPARE(was[2].key, is[2].key);
    QCOMPARE(was[3].key, is[3].key);
    // The same text read again has the same keys.
    const auto again = buildRead(after, folder.path());
    const std::vector<Part> same = preview::parts(again->reading.document, &again->reading.formatted);
    QCOMPARE(same.size(), is.size());
    for (size_t i = 0; i < is.size(); ++i) QCOMPARE(same[i].key, is[i].key);
}

void documentSettingsReachThePreview()
{
    // The preview reads a document in its settings: the citation style and
    // bibliography chosen for it, and its Markdown profile. A change of any
    // of them is shown at once, with the text as it was.
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString refs = folder.filePath(QStringLiteral("refs.bib"));
    writeFile(refs, references);
    const QString path = folder.filePath(QStringLiteral("cited.md"));
    const QString text = QStringLiteral("# Works\n\nSome ==marked== words, as shown [@doe2006, p. 23] and [@nokey].\n\nMore words.\n");
    writeFile(path, text.toUtf8());
    ShellOptions options;
    options.files = {path};
    PreviewWindow w(QString(), 1280, options);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewSplit");
    QVERIFY(w.settled());
    DocumentSettingsStore *settings = w.shell.backend->documentSettings();
    QVERIFY(settings);
    const QChar gap(0x2002);
    const auto shows = [&](const QString &needle) {
        const QStringList lines = w.lines();
        return std::any_of(lines.cbegin(), lines.cend(), [&](const QString &line) { return line.contains(needle); });
    };
    const Revision revision = w.document->buffer()->revision();

    // With no bibliography, a work nothing defines is shown as it is written.
    QVERIFY2(shows(QStringLiteral("Some marked words, as shown [@doe2006, p. 23] and [@nokey].")), qPrintable(w.lines().join(u'|')));
    QVERIFY(!shows(QStringLiteral("Doe, J.")));

    // A bibliography and an author-date style for the document: its
    // citations read as the style words them, the missing key stays in
    // view, and the bibliography appears after the text.
    QVERIFY(settings->setBibliography(w.document, QStringLiteral("document"), {refs}));
    QVERIFY(settings->setCitationStyle(w.document, QStringLiteral("document"), QStringLiteral("apa")));
    QTRY_VERIFY2(shows(QStringLiteral("as shown (Doe, 2006, p. 23) and ")), qPrintable(w.lines().join(u'|')));
    QVERIFY(w.settled());
    QVERIFY(shows(QStringLiteral("nokey")));
    QVERIFY2(w.lines().contains(QStringLiteral("Doe, J. (2006). Writing with examples. Example Press.")), qPrintable(w.lines().join(u'|')));
    const QTextCursor lacking = w.preview->textDocument()->find(QStringLiteral("nokey"));
    QVERIFY(!lacking.isNull());
    QCOMPARE(lacking.charFormat().intProperty(PreviewStyle::InkProperty), int(PreviewStyle::Danger));
    QCOMPARE(lacking.charFormat().foreground().color(), w.preview->style().ink(PreviewStyle::Danger));
    // Only what reads otherwise was built again: the heading and the last paragraph stayed.
    QCOMPARE(w.document->buffer()->revision(), revision);
    QCOMPARE(w.document->buffer()->text(), text);

    // A numeric style: numbers, and a numbered bibliography.
    int built = w.preview->sectionsBuilt();
    QVERIFY(settings->setCitationStyle(w.document, QStringLiteral("document"), QStringLiteral("ieee")));
    QTRY_VERIFY2(shows(QStringLiteral("as shown [1, p. 23] and ")), qPrintable(w.lines().join(u'|')));
    QVERIFY(w.settled());
    QVERIFY2(shows(QStringLiteral("[1]") + gap + QStringLiteral("J. Doe, Writing with examples")), qPrintable(w.lines().join(u'|')));
    QVERIFY(!shows(QStringLiteral("(Doe, 2006")));
    // The paragraph with the citations and the entry: the heading and the
    // paragraph without any were left as they were.
    QCOMPARE(w.preview->sectionsBuilt() - built, 2);
    w.shot("citations-numeric");

    // A note style: the citation is a note, and the bibliography follows the notes.
    QVERIFY(settings->setCitationStyle(w.document, QStringLiteral("document"), QStringLiteral("chicago-notes-bibliography")));
    QTRY_VERIFY2(shows(QStringLiteral("1.") + gap + QStringLiteral("John Doe, Writing with Examples (Example Press, 2006), 23.")),
                 qPrintable(w.lines().join(u'|')));
    QVERIFY(w.settled());
    QVERIFY2(shows(QStringLiteral("Some marked words, as shown 1 and 2.")), qPrintable(w.lines().join(u'|')));
    QVERIFY2(shows(QStringLiteral("Doe, John. Writing with Examples.")), qPrintable(w.lines().join(u'|')));
    // The note's mark leads to the note, and the note back.
    const QTextCursor mark = w.preview->textDocument()->find(QStringLiteral("as shown 1"));
    QVERIFY(!mark.isNull());
    QVERIFY(w.preview->goTo(QStringLiteral("fn-1")));
    QVERIFY(w.preview->goTo(QStringLiteral("fnref-1")));
    w.shot("citations-notes");

    // The Standard profile reads neither highlights nor citations: both are text.
    QVERIFY(settings->setProfile(w.document, QStringLiteral("document"), QStringLiteral("standard")));
    QTRY_VERIFY2(shows(QStringLiteral("Some ==marked== words, as shown [@doe2006, p. 23] and [@nokey].")), qPrintable(w.lines().join(u'|')));
    QVERIFY(w.settled());
    QVERIFY(!shows(QStringLiteral("John Doe")));
    QVERIFY(!shows(QStringLiteral("Doe, John.")));
    QCOMPARE(w.preview->textDocument()->find(QStringLiteral("marked")).charFormat().intProperty(PreviewStyle::PaperProperty),
             int(PreviewStyle::NoPaper));
    // And the Writer profile again reads them.
    QVERIFY(settings->setProfile(w.document, QStringLiteral("document"), QStringLiteral("writer")));
    QTRY_VERIFY2(shows(QStringLiteral("Some marked words, as shown 1 and 2.")), qPrintable(w.lines().join(u'|')));
    QVERIFY(w.settled());
    QCOMPARE(w.preview->textDocument()->find(QStringLiteral("marked")).charFormat().intProperty(PreviewStyle::PaperProperty),
             int(PreviewStyle::MarkPaper));

    // A change while the preview is not shown is there when it is shown again.
    w.run("viewSource");
    QTRY_VERIFY(!w.previewPane->isVisible());
    QVERIFY(settings->setCitationStyle(w.document, QStringLiteral("document"), QStringLiteral("apa")));
    QTest::qWait(2 * Preview::PauseMs);
    w.run("viewSplit");
    QTRY_VERIFY2(shows(QStringLiteral("as shown (Doe, 2006, p. 23) and ")), qPrintable(w.lines().join(u'|')));
    QVERIFY(w.settled());
    QVERIFY(!shows(QStringLiteral("John Doe, Writing")));
    QCOMPARE(w.document->buffer()->text(), text);
    w.shot("citations");
}

void previewFollowsFilesOnDisk()
{
    // What the text depends on is watched: a file it includes, its
    // bibliography, its style file and its pictures, changed on disk, are
    // shown changed with no edit, and only the parts that use them are
    // built again.
    QTemporaryDir made;
    QVERIFY(made.isValid());
    const QString folder = QFileInfo(made.path()).canonicalFilePath();
    const QString chapter = folder + QStringLiteral("/chapter.md");
    const QString refs = folder + QStringLiteral("/refs.bib");
    const QString style = folder + QStringLiteral("/style.csl");
    const QString picture = folder + QStringLiteral("/picture.png");
    const auto bundled = [](const char *name) {
        QFile file(QStringLiteral(":/csl/") + QLatin1String(name));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    writeFile(chapter, "Chapter words, first.\n");
    writeFile(refs, references);
    writeFile(style, bundled("apa.csl"));
    writeFile(picture, pngOf(80, 40, Qt::darkRed));
    const QString path = folder + QStringLiteral("/follows.md");
    const QString text = QStringLiteral("---\nbibliography: refs.bib\ncsl: style.csl\n---\n\n# Follows\n\n"
                                        "As shown [@doe2006].\n\nchapter.md\n\n![A picture](picture.png)\n\nThe end.\n");
    writeFile(path, text.toUtf8());
    ShellOptions options;
    options.files = {path};
    PreviewWindow w(QString(), 1280, options);
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    w.run("viewSplit");
    QVERIFY(w.settled());
    const auto shows = [&](const QString &needle) {
        const QStringList lines = w.lines();
        return std::any_of(lines.cbegin(), lines.cend(), [&](const QString &line) { return line.contains(needle); });
    };
    // The width of the picture as it is shown.
    const auto pictureWidth = [&] {
        for (QTextBlock block = w.preview->textDocument()->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextCharFormat format = it.fragment().charFormat();
                if (format.isImageFormat() && format.toImageFormat().name().startsWith(QLatin1String("parch:image/")))
                    return format.toImageFormat().width();
            }
        return -1.0;
    };
    const Revision revision = w.document->buffer()->revision();
    QTRY_COMPARE(pictureWidth(), 80.0);
    QVERIFY(w.settled());
    QVERIFY2(shows(QStringLiteral("As shown (Doe, 2006).")), qPrintable(w.lines().join(u'|')));
    QVERIFY(shows(QStringLiteral("Chapter words, first.")));
    QVERIFY(w.lines().contains(QStringLiteral("Doe, J. (2006). Writing with examples. Example Press.")));
    QTRY_COMPARE(QSet<QString>(w.preview->watchedFiles().cbegin(), w.preview->watchedFiles().cend()),
                 (QSet<QString>{chapter, refs, style, picture}));

    // The included file: its part alone.
    int built = w.preview->sectionsBuilt();
    writeFile(chapter, "Chapter words, second.\n");
    QTRY_VERIFY2(shows(QStringLiteral("Chapter words, second.")), qPrintable(w.lines().join(u'|')));
    QVERIFY(w.settled());
    QCOMPARE(w.preview->sectionsBuilt() - built, 1);

    // The bibliography: the entry alone, since the citation reads as it did.
    built = w.preview->sectionsBuilt();
    writeFile(refs, QByteArray(references).replace("Writing with examples", "Writing with more examples"));
    QTRY_VERIFY2(w.lines().contains(QStringLiteral("Doe, J. (2006). Writing with more examples. Example Press.")),
                 qPrintable(w.lines().join(u'|')));
    QVERIFY(w.settled());
    QCOMPARE(w.preview->sectionsBuilt() - built, 1);

    // The style: the paragraph that cites and the entry.
    built = w.preview->sectionsBuilt();
    writeFile(style, bundled("ieee.csl"));
    QTRY_VERIFY2(shows(QStringLiteral("As shown [1].")), qPrintable(w.lines().join(u'|')));
    QVERIFY(w.settled());
    QCOMPARE(w.preview->sectionsBuilt() - built, 2);

    // The picture: the section that shows it, once the new one is read.
    built = w.preview->sectionsBuilt();
    writeFile(picture, pngOf(120, 60, Qt::darkBlue));
    QTRY_COMPARE(pictureWidth(), 120.0);
    QVERIFY(w.settled());
    QCOMPARE(w.preview->sectionsBuilt() - built, 1);

    // A file that goes is seen to go.
    QVERIFY(QFile::remove(chapter));
    QTRY_VERIFY2(!shows(QStringLiteral("Chapter words")), qPrintable(w.lines().join(u'|')));
    QCOMPARE(w.document->buffer()->revision(), revision);
    QCOMPARE(w.document->buffer()->text(), text);

    // However many pictures a text names, the watch is bounded.
    QString many;
    for (int i = 0; i < PreviewWatch::MaxFiles + 40; ++i) many += QStringLiteral("![](many-%1.png)\n\n").arg(i);
    w.document->buffer()->replace({int(text.size()), 0}, many);
    QTRY_COMPARE(w.preview->watchedFiles().size(), PreviewWatch::MaxFiles);
}

void drawnPicturesAreDecodedOffTheUiThreadAndKeptToScreenSize()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString helpers = folder.path() + QStringLiteral("/helpers");
    QDir().mkpath(helpers);
    writeFakeHelpers(helpers, folder.path() + QStringLiteral("/helpers.log"));
    // A diagram drawn far wider than any screen.
    QImage wide(9000, 60, QImage::Format_ARGB32);
    wide.fill(Qt::darkCyan);
    QVERIFY(wide.save(helpers + QStringLiteral("/picture.png")));
    RenderService service(renderOptions(HelperSet::in(helpers), folder.path()));
    PreviewResources resources;
    resources.setRenderService(&service);
    resources.setScale(4);
    // Heard after the resources hear it: the picture is still being made
    // then, off this thread.
    QList<int> pendingWhenDrawn;
    QObject::connect(&service, &RenderService::finished, &resources,
                     [&] { pendingWhenDrawn.append(resources.pending()); });
    QSignalSpy changed(&resources, &PreviewResources::changed);

    const QString source = QStringLiteral("graph TD\n  A --> B\n");
    QCOMPARE(resources.diagram(source).state, preview::Picture::Pending);
    QVERIFY(changed.wait(30000));
    QCOMPARE(pendingWhenDrawn, QList<int>{1});
    const preview::Picture diagram = resources.diagram(source);
    QCOMPARE(diagram.state, preview::Picture::Ready);
    QCOMPARE(diagram.image.width(), PreviewResources::MaxSide);
    QCOMPARE(diagram.image.height(), qRound(60.0 * PreviewResources::MaxSide / 9000));
    QCOMPARE(resources.pending(), 0);

    // A formula drawn as SVG, which at this scale would be wider than Qt can
    // paint, is kept to the same size.
    const QString formula(5000, u'x');
    QCOMPARE(resources.math(formula, false, 16).state, preview::Picture::Pending);
    QVERIFY(changed.wait(30000));
    const preview::Picture math = resources.math(formula, false, 16);
    QCOMPARE(math.state, preview::Picture::Ready);
    QCOMPARE(math.image.width(), PreviewResources::MaxSide);
    QVERIFY(math.image.height() >= 1);
    QVERIFY(math.size.width() * 4 > PreviewResources::MaxSide);
}

void headingLinksAreFoundInTheSharedParse()
{
    // The heading a link names is looked for in the document's own parse,
    // which a worker makes, never by reading the text on the UI thread.
    QString text = QStringLiteral("# Start\n\n");
    for (int i = 0; i < 200; ++i) text += QStringLiteral("Paragraph %1 with some words in it.\n\n").arg(i);
    text += QStringLiteral("## The end\n\nLast words.\n");
    PreviewWindow w(text);
    QVERIFY(w.ok());
    DocumentParse::setWorkerDelay(300);
    const auto undelay = qScopeGuard([] { DocumentParse::setWorkerDelay(0); });
    w.document->buffer()->replace({int(w.document->buffer()->length()), 0}, QStringLiteral("\nMore.\n"));
    const int parses = w.document->parse()->parses();
    QSignalSpy found(w.preview, &Preview::headingFound);
    QElapsedTimer asking;
    asking.start();
    const int request = w.preview->findHeading(QStringLiteral("the-end"));
    QVERIFY(asking.elapsed() < 200);
    QVERIFY(found.isEmpty());
    QVERIFY(found.wait(10000));
    QCOMPARE(found.last().at(0).toInt(), request);
    QCOMPARE(found.last().at(1).toString(), QStringLiteral("the-end"));
    QCOMPARE(found.last().at(2).toInt(), int(w.document->buffer()->text().indexOf(QStringLiteral("## The end"))));
    QVERIFY(w.document->parse()->parses() <= parses + 1);
    QCOMPARE(headingFound(w.preview, QStringLiteral("start")), 0);
    QCOMPARE(headingFound(w.preview, QStringLiteral("nowhere")), -1);
}

} // namespace PreviewTests
