#include "screenshottests.h"

#include "apppaths.h"
#include "backend.h"
#include "cli.h"
#include "document.h"
#include "editorbinding.h"
#include "exports.h"
#include "preview.h"
#include "previewresources.h"
#include "printpreview.h"
#include "renderservice.h"
#include "rendertesting.h"
#include "screenshot.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "spellchecker.h"
#include "tabmodel.h"
#include "testfiles.h"
#include "windowcontext.h"
#include "windowtesting.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QProcess>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <unistd.h>

namespace ScreenshotTests {
namespace {

const QString parchBinary = QStringLiteral(PARCH_SOURCE_DIR "/build/parch");

// A document long enough to scroll, with a little of everything the
// preview draws: headings, prose, code, a table, a formula.
QByteArray longDocument()
{
    QByteArray text = "# Station notes\n\nThe first line of prose.\n\n";
    for (int i = 1; i <= 40; ++i) {
        text += "## Day " + QByteArray::number(i) + "\n\nA note about the weather on day " + QByteArray::number(i)
                + ", and the wind.\n\n";
        if (i == 20)
            text += "```python\nprint(\"twenty\")\n```\n\n| Hour | Reading |\n| --- | --- |\n| 9 | 12.5 |\n\n"
                    "$$T_d = b\\gamma / (a - \\gamma)$$\n\n";
    }
    return text;
}

// Formulas and diagrams drawn by the build's helpers, as for the writer, but
// kept in a folder of the tests' own: parch's own cache stays empty.
void drawApart(ShellFixture &shell)
{
    static QTemporaryDir drawings;
    shell.backend->setRenderService(
        std::make_unique<RenderService>(renderOptions(realHelpers(), drawings.filePath(QStringLiteral("render")))));
    shell.backend->exports()->setRenderOptions(renderOptions(realHelpers(), drawings.filePath(QStringLiteral("export"))));
}

// Whether all of an item is in sight: inside the frame, and inside every
// item that clips it.
bool inSight(QQuickItem *item, QQuickItem *frame)
{
    if (!item || !frame || !item->isVisible()) return false;
    const QRectF drawn = item->mapRectToScene(QRectF(0, 0, item->width(), item->height())).adjusted(0.5, 0.5, -0.5, -0.5);
    if (!frame->mapRectToScene(QRectF(0, 0, frame->width(), frame->height())).contains(drawn)) return false;
    for (QQuickItem *at = item->parentItem(); at; at = at->parentItem())
        if (at->clip() && !at->mapRectToScene(at->clipRect()).contains(drawn)) return false;
    return true;
}

// Where line `line`, counted from 1, starts in the text.
int lineStart(const QString &text, int line)
{
    int at = 0;
    for (int i = 1; i < line && at >= 0; ++i) {
        at = int(text.indexOf(u'\n', at));
        if (at >= 0) ++at;
    }
    return std::max(0, at);
}

// What one in-process picture gave.
struct Shot {
    int code = -1;
    QString message;
    QImage image;
};

// Takes a picture of the fixture's window as `parch --screenshot` takes one,
// with the caret steady, as main() keeps it for a picture.
Shot take(ShellFixture &shell, Picture picture, int waitMs = Screenshot::WaitMs)
{
    QTemporaryDir out;
    if (picture.file.isEmpty()) picture.file = out.filePath(QStringLiteral("shot.png"));
    Shot shot;
    Screenshot screenshot(picture, shell.backend.get(), waitMs);
    QObject::connect(&screenshot, &Screenshot::finished, &screenshot, [&shot](int code, const QString &message) {
        shot.code = code;
        shot.message = message;
    });
    screenshot.start(shell.window.get());
    if (!QTest::qWaitFor([&shot] { return shot.code >= 0; }, waitMs + 10000)) shot.message = QStringLiteral("never finished");
    shot.image = QImage(picture.file);
    return shot;
}

// The whole of a folder's files, by path beneath it, with their bytes.
QMap<QString, QByteArray> filesIn(const QString &folder)
{
    QMap<QString, QByteArray> files;
    QDirIterator it(folder, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        files.insert(QDir(folder).relativeFilePath(path), readFile(path));
    }
    return files;
}

// parch run as a process in folders of its own beneath `root`, with nothing
// of a desktop to reach: not even a platform it could open a window on.
struct Ran {
    int code = -1;
    QString said;
};

Ran runParch(const QString &root, const QStringList &arguments, const QString &scale = QStringLiteral("1"))
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const char *name : {"HYPRLAND_INSTANCE_SIGNATURE", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY",
                             "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID", "QT_LINUX_ACCESSIBILITY_ALWAYS_ON",
                             "QT_QUICK_BACKEND", "PARCH_HELPERS"})
        environment.remove(QString::fromLatin1(name));
    const QList<std::pair<const char *, const char *>> folders{
        {"HOME", "home"},           {"XDG_CONFIG_HOME", "config"}, {"XDG_DATA_HOME", "data"},
        {"XDG_STATE_HOME", "state"}, {"XDG_CACHE_HOME", "cache"},   {"XDG_RUNTIME_DIR", "runtime"},
        {"TMPDIR", "tmp"}};
    for (const auto &[name, folder] : folders) {
        const QString path = root + u'/' + QString::fromLatin1(folder);
        QDir().mkpath(path);
        environment.insert(QString::fromLatin1(name), path);
    }
    QFile::setPermissions(root + QStringLiteral("/runtime"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("unix:path=") + root + QStringLiteral("/no-bus"));
    environment.insert(QStringLiteral("NO_AT_BRIDGE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("no-such-platform"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORMTHEME"), QStringLiteral("generic"));
    environment.insert(QStringLiteral("QML_DISABLE_DISK_CACHE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("QT_SCALE_FACTOR"), scale);
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(root);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(parchBinary, arguments);
    Ran ran;
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished();
        ran.said = QStringLiteral("did not finish");
        return ran;
    }
    ran.code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
    ran.said = QString::fromLocal8Bit(process.readAll());
    return ran;
}

// The colour most of a picture is.
QRgb commonest(const QImage &image)
{
    QHash<QRgb, int> counts;
    for (int y = 0; y < image.height(); y += 4)
        for (int x = 0; x < image.width(); x += 4) ++counts[image.pixel(x, y) | 0xff000000u];
    QRgb best = 0;
    int most = 0;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        if (it.value() > most) {
            most = it.value();
            best = it.key();
        }
    return best;
}

// A small English dictionary for Hunspell, so that no test depends on the system's.
void writeDictionary(const QString &folder)
{
    writeFile(folder + QStringLiteral("/en_US.aff"), "SET UTF-8\nTRY esianrtolcdugmphbyfvkwz\n");
    const QStringList words = QString::fromUtf8(
                                  "a about and day first line note notes of on prose station the weather wind print "
                                  "hour reading twenty")
                                  .split(u' ');
    writeFile(folder + QStringLiteral("/en_US.dic"),
              (QString::number(words.size()) + u'\n' + words.join(u'\n') + u'\n').toUtf8());
}

} // namespace

void optionsAreParsed()
{
    const Launch plain = parseLaunch({"parch", "a.md"});
    QVERIFY(plain.picture.file.isEmpty());
    QVERIFY(!plain.defaultTheme);

    const Launch all = parseLaunch({"parch", "open", "--screenshot", "out.png", "--size", "1280x800", "--view", "split",
                                    "--show", "export", "--focus", "--line", "12", "--select", "40,7", "--library",
                                    "--default-theme", "a.md"});
    QCOMPARE(all.kind, Launch::Gui);
    QCOMPARE(all.files, QStringList{"a.md"});
    QCOMPARE(all.picture.file, QStringLiteral("out.png"));
    QCOMPARE(all.picture.size, QSize(1280, 800));
    QCOMPARE(all.picture.view, QStringLiteral("split"));
    QCOMPARE(all.picture.show, QStringLiteral("export"));
    QVERIFY(all.picture.focus);
    QCOMPARE(all.picture.line, 12);
    QCOMPARE(all.picture.scroll, -1.0);
    QCOMPARE(all.picture.selectFrom, 40);
    QCOMPARE(all.picture.selectTo, 7);
    QCOMPARE(all.picture.library, std::optional<bool>(true));
    QVERIFY(all.defaultTheme);

    // Values after '=' too, and each choice of the options that take one.
    const Launch equals = parseLaunch({"parch", "--screenshot=s.png", "--size=960x600", "--view=reading",
                                       "--scroll=0.25", "--no-library", "--show=document-settings"});
    QCOMPARE(equals.kind, Launch::Gui);
    QCOMPARE(equals.picture.file, QStringLiteral("s.png"));
    QCOMPARE(equals.picture.size, QSize(960, 600));
    QCOMPARE(equals.picture.view, QStringLiteral("reading"));
    QCOMPARE(equals.picture.scroll, 0.25);
    QCOMPARE(equals.picture.line, 0);
    QCOMPARE(equals.picture.library, std::optional<bool>(false));
    QCOMPARE(equals.picture.show, QStringLiteral("document-settings"));
    QVERIFY(!equals.picture.focus);
    QCOMPARE(equals.picture.selectFrom, -1);
    for (const QString &view : {QStringLiteral("source"), QStringLiteral("split"), QStringLiteral("reading")})
        QCOMPARE(parseLaunch({"parch", "--screenshot", "s.png", "--view", view}).picture.view, view);
    for (const QString &panel : Picture::panels())
        QCOMPARE(parseLaunch({"parch", "--screenshot", "s.png", "--show", panel}).picture.show, panel);
    for (const char *panel : {"export", "document-settings", "history", "palette", "outline"})
        QVERIFY2(Picture::panels().contains(QLatin1StringView(panel)), panel);
    QCOMPARE(parseLaunch({"parch", "--screenshot", "s.png", "--scroll", "0"}).picture.scroll, 0.0);
    QCOMPARE(parseLaunch({"parch", "--screenshot", "s.png", "--scroll", "1"}).picture.scroll, 1.0);
    // The default theme needs no picture: it is a way to open the window as well.
    const Launch theme = parseLaunch({"parch", "--default-theme", "a.md"});
    QCOMPARE(theme.kind, Launch::Gui);
    QVERIFY(theme.defaultTheme);
    QVERIFY(theme.picture.file.isEmpty());
}

void malformedOptionsAreRefused()
{
    struct Case {
        QStringList arguments;
        const char *said;
    };
    const QList<Case> cases{
        {{"--screenshot"}, "--screenshot needs a value"},
        {{"--screenshot="}, "--screenshot needs a value"},
        {{"--screenshot", "s.png", "--size", "1280"}, "--size takes"},
        {{"--screenshot", "s.png", "--size", "1280x"}, "--size takes"},
        {{"--screenshot", "s.png", "--size", "x800"}, "--size takes"},
        {{"--screenshot", "s.png", "--size", "0x800"}, "--size takes"},
        {{"--screenshot", "s.png", "--size", "99x800"}, "--size takes"},
        {{"--screenshot", "s.png", "--size", "20000x800"}, "--size takes"},
        {{"--screenshot", "s.png", "--size", "12.5x800"}, "--size takes"},
        {{"--screenshot", "s.png", "--size", "big"}, "--size takes"},
        {{"--screenshot", "s.png", "--size"}, "--size needs a value"},
        {{"--screenshot", "s.png", "--view", "side"}, "--view takes"},
        {{"--screenshot", "s.png", "--view", "Split"}, "--view takes"},
        {{"--screenshot", "s.png", "--show", "everything"}, "--show takes"},
        {{"--screenshot", "s.png", "--show"}, "--show needs a value"},
        {{"--screenshot", "s.png", "--scroll", "1.5"}, "--scroll takes"},
        {{"--screenshot", "s.png", "--scroll", "half"}, "--scroll takes"},
        {{"--screenshot", "s.png", "--scroll", "nan"}, "--scroll takes"},
        {{"--screenshot", "s.png", "--scroll", "-0.5"}, "--scroll needs a value"},
        {{"--screenshot", "s.png", "--line", "0"}, "--line takes"},
        {{"--screenshot", "s.png", "--line", "two"}, "--line takes"},
        {{"--screenshot", "s.png", "--line", "3.5"}, "--line takes"},
        {{"--screenshot", "s.png", "--select", "5"}, "--select takes"},
        {{"--screenshot", "s.png", "--select", "a,b"}, "--select takes"},
        {{"--screenshot", "s.png", "--select", "1,2,3"}, "--select takes"},
        {{"--screenshot", "s.png", "--select", "1,"}, "--select takes"},
        {{"--screenshot", "s.png", "--select", "1;2"}, "--select takes"},
        {{"--screenshot", "s.png", "--focus=yes"}, "--focus takes no value"},
        {{"--screenshot", "s.png", "--library=no"}, "--library takes no value"},
        {{"--default-theme=nord"}, "--default-theme takes no value"},
        // Options that cannot go together.
        {{"--screenshot", "s.png", "--line", "3", "--scroll", "0.5"}, "--line and --scroll"},
        {{"--screenshot", "s.png", "--library", "--no-library"}, "--library and --no-library"},
        {{"--theme", "nord", "--default-theme"}, "--theme and --default-theme"},
        {{"--screenshot", "s.png", "--benchmark", "out.json", "a.md"}, "--screenshot and --benchmark"},
        // What sets the window up for a picture needs one.
        {{"--size", "1280x800"}, "--size works only with --screenshot"},
        {{"--view", "split"}, "--view works only with --screenshot"},
        {{"--show", "export"}, "--show works only with --screenshot"},
        {{"--focus"}, "--focus works only with --screenshot"},
        {{"--line", "3"}, "--line works only with --screenshot"},
        {{"--scroll", "0.5"}, "--scroll works only with --screenshot"},
        {{"--select", "1,2"}, "--select works only with --screenshot"},
        {{"--library"}, "--library works only with --screenshot"},
        {{"--no-library"}, "--no-library works only with --screenshot"},
    };
    for (const Case &each : cases) {
        const Launch launch = parseLaunch(QStringList{QStringLiteral("parch")} + each.arguments);
        const QString context = each.arguments.join(u' ') + QStringLiteral(" -> ") + launch.error;
        QVERIFY2(launch.kind == Launch::Invalid, qPrintable(context));
        QVERIFY2(launch.error.startsWith(QStringLiteral("parch: ")), qPrintable(context));
        QVERIFY2(launch.error.contains(QLatin1StringView(each.said)), qPrintable(context));
    }
}

void helpNamesEveryOption()
{
    const QString help = openHelpText();
    QVERIFY(help.startsWith(QStringLiteral("Usage: parch [open] [--theme DIR] [--benchmark OUT] [FILE...]\n"
                                           "       parch --screenshot PNG [OPTION...] [FILE...]\n")));
    for (const char *row : {"  --screenshot ", "  --default-theme ", "  --size WxH ", "  --view VIEW ", "  --show PANEL ",
                            "  --library ", "  --no-library ", "  --focus ", "  --line N ", "  --scroll AMOUNT ",
                            "  --select A,B "})
        QVERIFY2(help.contains(QLatin1StringView(row)), row);
    for (const QString &panel : Picture::panels())
        QVERIFY2(help.contains(panel), qPrintable(panel));
}

void windowIsSetUpAsAsked()
{
    // The caret stays lit in a picture, as main() keeps it for one.
    const int flash = QGuiApplication::styleHints()->cursorFlashTime();
    QGuiApplication::styleHints()->setCursorFlashTime(0);
    const auto restore = qScopeGuard([flash] { QGuiApplication::styleHints()->setCursorFlashTime(flash); });
    QTemporaryDir files;
    const QString file = files.filePath(QStringLiteral("notes.md"));
    writeFile(file, longDocument());
    {
        ShellFixture shell({.files = {file}});
        QVERIFY2(shell.ok(), qPrintable(shell.error));
        drawApart(shell);
        Picture picture;
        picture.view = QStringLiteral("split");
        picture.focus = true;
        picture.line = 30;
        picture.library = false;
        const Shot shot = take(shell, picture);
        QVERIFY2(shot.code == 0, qPrintable(shot.message));
        QQuickWindow *window = shell.window.get();
        QVERIFY(!shot.image.isNull());
        QCOMPARE(shot.image.size(), window->size() * window->devicePixelRatio());

        QCOMPARE(window->property("viewMode").toInt(), int(TabModel::Split));
        QVERIFY(window->property("splitBoth").toBool());
        QVERIFY(window->property("focusMode").toBool());
        QVERIFY(!shell.context()->libraryOpen());
        QVERIFY(!shell.item(QStringLiteral("library"))->isVisible());
        // Line 30 is at the top of the text and of the preview beside it.
        const QString text = shell.document()->buffer()->text();
        const int top = lineStart(text, 30);
        QCOMPARE(shell.binding()->topSourcePosition(), top);
        auto *preview = shell.item(QStringLiteral("previewPane"))->findChild<Preview *>(QStringLiteral("preview"));
        QVERIFY(preview);
        const int previewTop = preview->topSource();
        QVERIFY2(previewTop >= top && previewTop < lineStart(text, 32), qPrintable(QString::number(previewTop)));
        // The text has the keys, its caret is shown and stays.
        QVERIFY(shell.item(QStringLiteral("editor"))->hasActiveFocus());
        QQuickItem *caret = findItem(shell.item(QStringLiteral("editor")), QStringLiteral("caret"));
        QVERIFY(caret && caret->isVisible());
        QObject *blink = shell.item(QStringLiteral("editorPane"))->findChild<QObject *>(QStringLiteral("caretBlink"));
        QVERIFY(blink && !blink->property("running").toBool());
        // The picture is what the window shows.
        QCOMPARE(shot.image.convertToFormat(QImage::Format_RGB32), window->grabWindow().convertToFormat(QImage::Format_RGB32));
    }
    {
        // A selection, the library shown and the text alone.
        ShellFixture shell({.files = {file}});
        QVERIFY2(shell.ok(), qPrintable(shell.error));
        drawApart(shell);
        shell.context()->setLibraryOpen(false);
        Picture picture;
        picture.view = QStringLiteral("source");
        picture.selectFrom = 2;
        picture.selectTo = 15;
        picture.library = true;
        const Shot shot = take(shell, picture);
        QVERIFY2(shot.code == 0, qPrintable(shot.message));
        QCOMPARE(shell.window->property("viewMode").toInt(), int(TabModel::Source));
        QVERIFY(!shell.window->property("focusMode").toBool());
        QVERIFY(shell.context()->libraryOpen());
        QVERIFY(shell.item(QStringLiteral("library"))->isVisible());
        QCOMPARE(shell.binding()->anchorSource(), 2);
        QCOMPARE(shell.binding()->cursorSource(), 15);
        QCOMPARE(shell.item(QStringLiteral("editorPane"))->property("selectedText").toString(), QStringLiteral("Station notes"));
    }
    {
        // The preview alone, scrolled to the end; then the outline in the library.
        ShellFixture shell({.files = {file}});
        QVERIFY2(shell.ok(), qPrintable(shell.error));
        drawApart(shell);
        Picture picture;
        picture.view = QStringLiteral("reading");
        picture.scroll = 1;
        const Shot shot = take(shell, picture);
        QVERIFY2(shot.code == 0, qPrintable(shot.message));
        QCOMPARE(shell.window->property("viewMode").toInt(), int(TabModel::Reading));
        QQuickItem *flickable = shell.item(QStringLiteral("previewFlickable"));
        const qreal most = flickable->property("contentHeight").toReal() - flickable->height();
        QVERIFY(most > 0);
        QVERIFY2(std::abs(flickable->property("contentY").toReal() - most) < 1, qPrintable(QString::number(most)));

        Picture outline;
        outline.show = QStringLiteral("outline");
        const Shot second = take(shell, outline);
        QVERIFY2(second.code == 0, qPrintable(second.message));
        QVERIFY(shell.settings()->outlineShown());
        QVERIFY(shell.item(QStringLiteral("libraryPanel"))->isVisible());
    }
    for (const QString &panel : Picture::panels()) {
        if (panel == u"outline" || panel == u"export") continue;
        ShellFixture shell({.files = {file}});
        QVERIFY2(shell.ok(), qPrintable(shell.error));
        drawApart(shell);
        Picture picture;
        picture.show = panel;
        const Shot shot = take(shell, picture);
        QVERIFY2(shot.code == 0, qPrintable(panel + u' ' + shot.message));
        QVERIFY2(shell.window->property("dialogOpen").toBool(), qPrintable(panel));
    }
}

void pictureWaitsForEverythingDrawn()
{
    QTemporaryDir files, dictionaries;
    writeDictionary(dictionaries.path());
    const QString file = files.filePath(QStringLiteral("notes.md"));
    writeFile(file, "# Station notes\n\nThe first line of prose, with a mistaek.\n\n"
                    "```python\nprint(\"twenty\")\n```\n\n$$T_d = b\\gamma / (a - \\gamma)$$\n\n"
                    "```mermaid\nflowchart LR\n  A[Sensor] --> B[Notes]\n```\n");
    ShellFixture shell({.files = {file}, .dictionaries = dictionaries.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    drawApart(shell);
    Picture picture;
    picture.view = QStringLiteral("split");
    // The state of everything as the picture is taken: finished() comes
    // straight after it is saved.
    bool styled = false, spelled = false, built = false, drawn = false;
    int underlines = -1;
    Screenshot screenshot([&] {
        Picture with = picture;
        with.file = files.filePath(QStringLiteral("shot.png"));
        return with;
    }(), shell.backend.get());
    int code = -1;
    QString message;
    QObject::connect(&screenshot, &Screenshot::finished, &screenshot, [&](int finishedCode, const QString &said) {
        code = finishedCode;
        message = said;
        QQuickItem *pane = shell.item(QStringLiteral("editorPane"));
        styled = !pane->findChild<QObject *>(QStringLiteral("styling"))->property("busy").toBool();
        spelled = shell.backend->spelling()->active()
                  && !pane->findChild<QObject *>(QStringLiteral("spelling"))->property("checking").toBool();
        underlines = int(findItem(shell.item(QStringLiteral("editor")), QStringLiteral("wave")) != nullptr);
        auto *preview = shell.item(QStringLiteral("previewPane"))->findChild<Preview *>(QStringLiteral("preview"));
        built = preview->current() && !preview->working() && !preview->parsing();
        drawn = preview->resources()->pending() == 0;
    });
    screenshot.start(shell.window.get());
    QTRY_VERIFY_WITH_TIMEOUT(code >= 0, Screenshot::WaitMs + 10000);
    QVERIFY2(code == 0, qPrintable(message));
    QVERIFY(styled);
    QVERIFY(spelled);
    QCOMPARE(underlines, 1);
    QVERIFY(built);
    QVERIFY(drawn);
    QVERIFY(!QImage(files.filePath(QStringLiteral("shot.png"))).isNull());
}

void exportDialogShowsItsPages()
{
    QTemporaryDir files;
    const QString file = files.filePath(QStringLiteral("notes.md"));
    writeFile(file, longDocument());
    // Wide enough for the pages beside the options, and too narrow for that.
    for (const int width : {960, 700}) {
        ShellFixture shell({.files = {file}});
        QVERIFY2(shell.ok(), qPrintable(shell.error));
        drawApart(shell);
        shell.window->resize(width, 600);
        QTRY_COMPARE(shell.window->contentItem()->width(), qreal(width));
        Picture picture;
        picture.show = QStringLiteral("export");
        const Shot shot = take(shell, picture);
        QVERIFY2(shot.code == 0, qPrintable(shot.message));
        QObject *dialog = shell.object(QStringLiteral("exportDialog"));
        QVERIFY(dialog->property("visible").toBool());
        QCOMPARE(dialog->property("format").toString(), QStringLiteral("pdf"));
        QCOMPARE(dialog->property("sideBySide").toBool(), width == 960);
        PrintPreview *pages = shell.backend->exports()->preview();
        QVERIFY(!pages->busy());
        QVERIFY(pages->pageCount() > 1);
        // The pages are drawn, and in sight with what the format leaves
        // out; beside them, the format and its options from the top.
        auto *frame = dialog->property("background").value<QQuickItem *>();
        QQuickItem *list = findItem(shell.window->contentItem(), QStringLiteral("previewPages"));
        QQuickItem *first = findItem(list, QStringLiteral("previewPage"));
        QVERIFY(first && first->property("ready").toBool());
        QVERIFY(inSight(findItem(shell.window->contentItem(), QStringLiteral("exportLimitations")), frame));
        QVERIFY(inSight(list, frame));
        QCOMPARE(inSight(findItem(shell.window->contentItem(), QStringLiteral("exportFormat")), frame), width == 960);
    }
}

void drawsOffScreenAtTheSizeAsked()
{
    QVERIFY2(QFileInfo(parchBinary).isExecutable(), "build parch with bin/build first");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = root.filePath(QStringLiteral("notes.md"));
    writeFile(file, longDocument());
    // No platform it could show a window on: the window is drawn off screen,
    // at twice its size for a screen scaled twice.
    const Ran dark = runParch(root.path(), {QStringLiteral("--screenshot"), QStringLiteral("dark.png"), QStringLiteral("--size"),
                                            QStringLiteral("640x400"), QStringLiteral("--default-theme"), file},
                              QStringLiteral("2"));
    QVERIFY2(dark.code == 0, qPrintable(dark.said));
    const QImage drawn(root.filePath(QStringLiteral("dark.png")));
    QCOMPARE(drawn.size(), QSize(1280, 800));
    // Omarchy's default, Tokyo Night, whatever the desktop shows.
    QCOMPARE(QColor(commonest(drawn)), QColor(QStringLiteral("#1a1b26")));

    // Another theme, by its folder, at the scale of the screen.
    writeTheme(root.filePath(QStringLiteral("paper")), "light", "#F8F1E3", "#342B27", "#A7353A");
    const Ran light = runParch(root.path(), {QStringLiteral("--screenshot"), QStringLiteral("light.png"), QStringLiteral("--size"),
                                             QStringLiteral("500x300"), QStringLiteral("--theme"),
                                             root.filePath(QStringLiteral("paper/theme")), QStringLiteral("--view"),
                                             QStringLiteral("reading"), file});
    QVERIFY2(light.code == 0, qPrintable(light.said));
    const QImage paper(root.filePath(QStringLiteral("light.png")));
    QCOMPARE(paper.size(), QSize(500, 300));
    QCOMPARE(QColor(commonest(paper)), QColor(QStringLiteral("#F8F1E3")));
}

void writesNothingOfThePersons()
{
    QVERIFY2(QFileInfo(parchBinary).isExecutable(), "build parch with bin/build first");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = root.filePath(QStringLiteral("notes/notes.md"));
    writeFile(file, longDocument());
    {
        QFile written(file);
        QVERIFY(written.open(QIODevice::ReadWrite));
        QVERIFY(written.setFileTime(QDateTime(QDate(2026, 1, 2), QTime(3, 4)), QFileDevice::FileModificationTime));
    }
    // The person's settings, which a picture is drawn with and never changes.
    writeFile(root.filePath(QStringLiteral("config/parch/settings.json")),
              "{\"libraryFolders\": [\"" + QFile::encodeName(root.filePath(QStringLiteral("notes"))) + "\"]}\n");
    const auto before = [&root] {
        QMap<QString, QByteArray> all;
        for (const char *folder : {"config", "data", "state", "runtime", "notes", "home"}) {
            const QMap<QString, QByteArray> files = filesIn(root.filePath(QString::fromLatin1(folder)));
            for (auto it = files.cbegin(); it != files.cend(); ++it)
                all.insert(QString::fromLatin1(folder) + u'/' + it.key(), it.value());
        }
        return all;
    };
    const QMap<QString, QByteArray> was = before();
    // Every option that changes something a window would otherwise keep.
    const Ran ran = runParch(root.path(), {QStringLiteral("--screenshot"), QStringLiteral("shot.png"), QStringLiteral("--show"),
                                           QStringLiteral("outline"), QStringLiteral("--view"), QStringLiteral("split"),
                                           QStringLiteral("--focus"), QStringLiteral("--line"), QStringLiteral("20"),
                                           QStringLiteral("--select"), QStringLiteral("3,9"), QStringLiteral("--library"),
                                           QStringLiteral("--default-theme"), file});
    QVERIFY2(ran.code == 0, qPrintable(ran.said));
    QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("shot.png"))));
    const QMap<QString, QByteArray> now = before();
    QCOMPARE(now.keys(), was.keys());
    QVERIFY(now == was);
    QCOMPARE(QFileInfo(file).lastModified(), QDateTime(QDate(2026, 1, 2), QTime(3, 4)));
}

void screenshotRefusesAnExistingFile()
{
    QTemporaryDir root;
    const QString input = root.filePath(QStringLiteral("note.md"));
    const QString output = root.filePath(QStringLiteral("window.png"));
    writeFile(input, "# Notes\n");
    writeFile(output, "keep");
    for (const QString &option : {QStringLiteral("--screenshot"), QStringLiteral("--benchmark")}) {
        const Ran refused = runParch(root.path(), {option, output, input});
        QVERIFY(refused.code != 0);
        QVERIFY2(refused.said.contains(QStringLiteral("--force")), qPrintable(refused.said));
        QCOMPARE(readFile(output), QByteArray("keep"));
        const QString link = root.filePath(QStringLiteral("link"));
        QVERIFY(QFile::link(output, link));
        const Ran linked = runParch(root.path(), {option, link, QStringLiteral("--force"), input});
        QVERIFY(linked.code != 0);
        QVERIFY2(linked.said.contains(QStringLiteral("link")), qPrintable(linked.said));
        QVERIFY(QFileInfo(link).isSymLink());
        QVERIFY(QFile::remove(link));
    }
    const Ran replaced = runParch(root.path(), {QStringLiteral("--screenshot"), output, QStringLiteral("--force"), input});
    QVERIFY2(replaced.code == 0, qPrintable(replaced.said));
    QVERIFY(!QImage(output).isNull());
}

void namesWhatCannotBeDone()
{
    QVERIFY2(QFileInfo(parchBinary).isExecutable(), "build parch with bin/build first");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = root.filePath(QStringLiteral("notes.md"));
    writeFile(file, "# Notes\n");
    const QString hard = root.filePath("second.md"), sym = root.filePath("link.md");
    QVERIFY(::link(QFile::encodeName(file).constData(), QFile::encodeName(hard).constData()) == 0);
    QVERIFY(QFile::link(file, sym));
    for (const QString &option : {QStringLiteral("--screenshot"), QStringLiteral("--benchmark")}) {
        for (const QString &out : {file, hard, sym}) {
            const Ran same = runParch(root.path(), {option, out, file});
            QCOMPARE(same.code, 1);
            QVERIFY2(same.said.contains("input document"), qPrintable(same.said));
            QCOMPARE(readFile(file), QByteArray("# Notes\n"));
        }
    }
    const QString chapter = root.filePath("part.md"), photo = root.filePath("photo.png");
    writeFile(file, "part.md\n");
    const QByteArray chapterText = "---\nbibliography: refs.bib\n---\n\n<img src=\"photo.png\" alt=\"Figure\">\n";
    writeFile(chapter, chapterText);
    writeFile(photo, "picture bytes");
    const QString references = root.filePath("refs.bib");
    writeFile(references, "@book{entry, title={Notes}}");
    const QString alias = root.filePath("another-name");
    QVERIFY(::link(QFile::encodeName(photo).constData(), QFile::encodeName(alias).constData()) == 0);
    for (const QString &option : {QStringLiteral("--screenshot"), QStringLiteral("--benchmark")}) {
        for (const auto &out : {chapter, photo, alias, references}) {
            const Ran same = runParch(root.path(), {option, out, file});
            QCOMPARE(same.code, 1);
            QVERIFY2(same.said.contains("input document"), qPrintable(same.said));
        }
    }
    QCOMPARE(readFile(chapter), chapterText);
    QCOMPARE(readFile(photo), QByteArray("picture bytes"));
    // A folder that is not there is said before any window.
    const Ran missing = runParch(root.path(), {QStringLiteral("--screenshot"), QStringLiteral("no-such/shot.png"), file});
    QCOMPARE(missing.code, 1);
    QVERIFY2(missing.said.contains(QStringLiteral("there is no folder for")), qPrintable(missing.said));
    // So is a theme that is not there.
    const Ran theme = runParch(root.path(), {QStringLiteral("--screenshot"), QStringLiteral("shot.png"), QStringLiteral("--theme"),
                                             QStringLiteral("no-such-theme"), file});
    QCOMPARE(theme.code, 1);
    QVERIFY2(theme.said.contains(QStringLiteral("there is no theme no-such-theme")), qPrintable(theme.said));
    // A picture that cannot be written.
    QDir().mkpath(root.filePath(QStringLiteral("locked")));
    QFile::setPermissions(root.filePath(QStringLiteral("locked")), QFile::ReadOwner | QFile::ExeOwner);
    const auto unlock = qScopeGuard([&root] {
        QFile::setPermissions(root.filePath(QStringLiteral("locked")), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    });
    const Ran locked = runParch(root.path(), {QStringLiteral("--screenshot"), QStringLiteral("locked/shot.png"), file});
    QCOMPARE(locked.code, 3);
    QVERIFY2(locked.said.contains(QStringLiteral("cannot write the picture")), qPrintable(locked.said));

    // A window that is not drawn in time says what it was still waiting for.
    QTemporaryDir files;
    const QString slow = files.filePath(QStringLiteral("slow.md"));
    writeFile(slow, longDocument());
    ShellFixture shell({.files = {slow}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    drawApart(shell);
    Preview::setWorkerDelay(4000);
    const auto quick = qScopeGuard([] { Preview::setWorkerDelay(0); });
    Picture picture;
    picture.view = QStringLiteral("split");
    const Shot shot = take(shell, picture, 1000);
    QCOMPARE(shot.code, 4);
    QVERIFY2(shot.message.contains(QStringLiteral("not drawn within 1 second")), qPrintable(shot.message));
    QVERIFY2(shot.message.contains(QStringLiteral("the preview")), qPrintable(shot.message));
    QVERIFY(shot.image.isNull());
}

} // namespace ScreenshotTests
