#include "librarytests.h"

#include "apppaths.h"
#include "appservice.h"
#include "documentcitations.h"
#include "librarywalk.h"
#include "saferead.h"
#include "autosave.h"
#include "backend.h"
#include "diskfile.h"
#include "documentlinks.h"
#include "documentreading.h"
#include "documentstore.h"
#include "encodings.h"
#include "filepicker.h"
#include "fileorganizer.h"
#include "foldersearch.h"
#include "history.h"
#include "librarymodel.h"
#include "recentfiles.h"
#include "rendertesting.h"
#include "session.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "tabmodel.h"
#include "testfiles.h"
#include "windowcontext.h"
#include "windowtesting.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimeZone>
#include <QUuid>
#include <QSemaphore>
#include <QtTest>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>

namespace {

QString newDocumentId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

bool isOpen(const QObject *popup) { return popup && popup->property("opened").toBool(); }

// Runs a command in a window, as its shortcuts, menu and palette do.
void runIn(QQuickWindow *window, const char *command)
{
    QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QString::fromLatin1(command)));
}

QStringList namesOf(const QList<History::Version> &versions)
{
    QStringList names;
    for (const History::Version &version : versions) names << version.name;
    return names;
}

QString canonical(const QString &path) { return QFileInfo(path).canonicalFilePath(); }

// The library's row by this name, as the tree shows it now.
QQuickItem *libraryRow(QQuickWindow *window, const QString &name)
{
    for (QQuickItem *item : visibleItems(window)) {
        if (item->objectName() != QStringLiteral("libraryRow") || item->property("name").toString() != name) continue;
        QObject *tree = item->property("treeView").value<QObject *>();
        if (!tree) continue;
        const int row = item->property("row").toInt();
        QQuickItem *held = nullptr;
        QMetaObject::invokeMethod(tree, "itemAtCell", Q_RETURN_ARG(QQuickItem *, held), Q_ARG(QPoint, QPoint(0, row)));
        QModelIndex index;
        QMetaObject::invokeMethod(tree, "index", Q_RETURN_ARG(QModelIndex, index), Q_ARG(int, row), Q_ARG(int, 0));
        if (held == item && index.data(LibraryModel::NameRole).toString() == name) return item;
    }
    return nullptr;
}

// Where the tree has a row by this name, as its index finds it at once; -1 for none.
int treeRow(QObject *tree, const QString &name)
{
    for (int row = 0;; ++row) {
        QModelIndex index;
        QMetaObject::invokeMethod(tree, "index", Q_RETURN_ARG(QModelIndex, index), Q_ARG(int, row), Q_ARG(int, 0));
        if (!index.isValid()) return -1;
        if (index.data(LibraryModel::NameRole).toString() == name) return row;
    }
}

// Gives the library's tree the keys, on the row by this name.
bool keysOnRow(const ShellFixture &shell, const QString &name)
{
    QObject *tree = shell.object(QStringLiteral("libraryTree"));
    QObject *panel = shell.object(QStringLiteral("libraryPanel"));
    static_cast<QQuickItem *>(tree)->forceActiveFocus(Qt::TabFocusReason);
    const int row = treeRow(tree, name);
    if (row < 0) return false;
    QMetaObject::invokeMethod(panel, "moveTo", Q_ARG(QVariant, row));
    return QTest::qWaitFor([&] {
        QQuickItem *item = libraryRow(shell.window.get(), name);
        return item && item->property("keyboard").toBool();
    }, 3000);
}

// Opens a section of the library, Drafts or Recent, by its name.
bool openSection(const ShellFixture &shell, const QString &name)
{
    QObject *tree = shell.object(QStringLiteral("libraryTree"));
    if (!QTest::qWaitFor([&] { return treeRow(tree, name) >= 0; }, 3000)) return false;
    bool expanded = false;
    const int row = treeRow(tree, name);
    QMetaObject::invokeMethod(tree, "isExpanded", Q_RETURN_ARG(bool, expanded), Q_ARG(int, row));
    if (!expanded) QMetaObject::invokeMethod(tree, "expand", Q_ARG(int, row));
    return true;
}

// Writes an unnamed draft as parch keeps one, with an asset beside it.
QString writeDraft(const AppPaths &paths, const QByteArray &text)
{
    const QString id = newDocumentId();
    writeFile(DocumentStore::draftFile(paths, id), text);
    writeFile(paths.drafts() + u'/' + id + QStringLiteral("/assets/picture.png"), "not really a picture");
    return id;
}

} // namespace

namespace LibraryTests {

void historyLimitsChangeWhileHistoryRuns()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDateTime clock = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    QMutex clockLock;
    const auto now = [&] {
        QMutexLocker lock(&clockLock);
        return clock;
    };
    const auto advance = [&](qint64 seconds) {
        QMutexLocker lock(&clockLock);
        clock = clock.addSecs(seconds);
    };
    History history(AppPaths::under(root.path()), HistoryLimits{10, 30, 10000}, now);
    // What history last said of its pause, heard on this thread.
    QObject listener;
    std::optional<History::Pause> said;
    QObject::connect(&history, &History::pausedChanged, &listener, [&](History::Pause pause) { said = pause; },
                     Qt::QueuedConnection);
    const QString id = newDocumentId();
    QVERIFY(!history.capture(id, "oldest").isEmpty());
    advance(5 * 24 * 3600);
    QVERIFY(!history.capture(id, QByteArray(100, 'n'), QStringLiteral("kept")).isEmpty());
    for (int i = 0; i < 4; ++i) {
        advance(600);
        QVERIFY(!history.capture(id, QByteArray(100, char('a' + i))).isEmpty());
    }
    QCOMPARE(history.versions(id).size(), 6);

    // Fewer versions: each document's oldest unnamed ones go at once, and
    // the named one stays.
    history.setLimits(HistoryLimits{3, 30, 10000});
    QCOMPARE(history.limits().versions, 3);
    QTRY_COMPARE(history.versions(id).size(), 4);
    QCOMPARE(namesOf(history.versions(id)), (QStringList{{}, {}, {}, QStringLiteral("kept")}));
    QCOMPARE(history.read(id, history.versions(id).first().id), QByteArray(100, 'd'));

    // Fewer days: every unnamed version older than that goes, never the named one.
    advance(2 * 24 * 3600);
    history.setLimits(HistoryLimits{3, 1, 10000});
    QTRY_COMPARE(history.versions(id).size(), 1);
    QCOMPARE(history.versions(id).first().name, QStringLiteral("kept"));

    // Less space: the oldest unnamed versions go until the rest fits.
    for (int i = 0; i < 2; ++i) {
        advance(600);
        QVERIFY(!history.capture(id, QByteArray(100, char('w' + i))).isEmpty());
    }
    QCOMPARE(history.usage().bytes, qint64(300));
    history.setLimits(HistoryLimits{3, 1, 250});
    QTRY_COMPARE(history.usage().bytes, qint64(200));
    QCOMPARE(history.read(id, history.versions(id).first().id), QByteArray(100, 'x'));

    // Less space than the named versions take: nothing more goes, since
    // that would not make room, and history pauses until the space grows.
    history.setLimits(HistoryLimits{3, 1, 50});
    QTRY_COMPARE(history.pause(), History::Pause::NamedVersionsFill);
    QTRY_COMPARE(said, std::optional(History::Pause::NamedVersionsFill));
    QCOMPARE(history.versions(id).size(), 2);
    QVERIFY(history.capture(id, "refused").isEmpty());
    history.setLimits(HistoryLimits{3, 1, 1000});
    QTRY_COMPARE(history.pause(), History::Pause::None);
    QTRY_COMPARE(said, std::optional(History::Pause::None));
    advance(60);
    QVERIFY(!history.capture(id, "taken again").isEmpty());

    // More versions and days keep what is there, and keep more from then on.
    history.setLimits(HistoryLimits{5, 30, 1000});
    for (int i = 0; i < 4; ++i) {
        advance(600);
        QVERIFY(!history.capture(id, QByteArray::number(i)).isEmpty());
    }
    QCOMPARE(history.versions(id).size(), 6);
}

void captureHookOutlivesItsReplacement()
{
    // A capture on another thread is inside the hook, waiting, when the hook
    // is replaced: what the hook holds stays alive until it returns.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    History history(AppPaths::under(root.path()));
    const QString id = newDocumentId();
    auto token = std::make_shared<int>(1);
    const std::weak_ptr<int> watched = token;
    QSemaphore entered, leave;
    // What the hook found it held as it returned.
    std::atomic<int> held{0};
    History::setBeforeCapture([token, &entered, &leave, &held] {
        entered.release();
        leave.acquire();
        held.store(*token);
    });
    token.reset();
    const auto unhook = qScopeGuard([] { History::setBeforeCapture({}); });
    History::CaptureStatus status = History::CaptureStatus::Failed;
    std::thread capture([&] { history.captureUnlessNewest(id, "text", History::Reason::Closed, &status); });
    // However the test ends, the capture is let go and waited for.
    const auto joined = qScopeGuard([&] {
        leave.release();
        if (capture.joinable()) capture.join();
    });
    QVERIFY(entered.tryAcquire(1, 5000));
    History::setBeforeCapture({});
    QVERIFY(!watched.expired());
    // A new hook takes effect for the next capture, and the old one is let go
    // once its call has returned.
    int later = 0;
    History::setBeforeCapture([&later] { ++later; });
    leave.release();
    capture.join();
    QCOMPARE(held.load(), 1);
    QCOMPARE(status, History::CaptureStatus::Recorded);
    QVERIFY(watched.expired());
    QVERIFY(!history.captureUnlessNewest(id, "more", History::Reason::Closed).isEmpty());
    QCOMPARE(later, 1);

    // Many captures while the hook is replaced again and again.
    std::atomic<bool> stop{false}, whole{true};
    std::thread replacing([&stop, &whole] {
        for (int i = 0; !stop.load(); ++i)
            History::setBeforeCapture(i % 2 ? std::function<void()>([kept = std::make_shared<QByteArray>(64, 'x'), &whole] {
                if (kept->size() != 64) whole.store(false);
            })
                                            : std::function<void()>());
    });
    for (int i = 0; i < 200; ++i) history.captureUnlessNewest(id, QByteArray::number(i), History::Reason::Closed);
    stop.store(true);
    replacing.join();
    QVERIFY(whole.load());
}

void historyRetentionIsAPreference()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    {
        // The defaults, the ranges, and the values kept for the next launch.
        Settings settings(paths.settingsFile());
        QCOMPARE(settings.historyVersions(), 100);
        QCOMPARE(settings.historyDays(), 30);
        QCOMPARE(settings.historyGigabytes(), 1.0);
        settings.setHistoryVersions(0);
        settings.setHistoryDays(Settings::MaxHistoryDays + 1);
        settings.setHistoryGigabytes(0.0);
        QCOMPARE(settings.historyVersions(), 100);
        QCOMPARE(settings.historyDays(), 30);
        QCOMPARE(settings.historyGigabytes(), 1.0);
        settings.setHistoryVersions(7);
        settings.setHistoryDays(400);
        settings.setHistoryGigabytes(2.5);
    }
    {
        Settings again(paths.settingsFile());
        QCOMPARE(again.historyVersions(), 7);
        QCOMPARE(again.historyDays(), 400);
        QCOMPARE(again.historyGigabytes(), 2.5);
    }
    // A value that is out of range, or not a number, is its default.
    writeFile(paths.settingsFile(), R"({"historyVersions": -3, "historyDays": "many", "historyGigabytes": 1e9})");
    {
        Settings damaged(paths.settingsFile());
        QCOMPARE(damaged.historyVersions(), 100);
        QCOMPARE(damaged.historyDays(), 30);
        QCOMPARE(damaged.historyGigabytes(), 1.0);
    }

    // A version older than the default 30 days, which a launch that keeps
    // 400 days must not remove as history starts.
    const QString id = newDocumentId();
    {
        const QDateTime then = QDateTime::currentDateTimeUtc().addDays(-100);
        History old(paths, HistoryLimits{100, 3650, qint64(1) << 30}, [then] { return then; });
        QVERIFY(!old.capture(id, "a hundred days ago").isEmpty());
        QVERIFY(!old.capture(id, "named a hundred days ago", QStringLiteral("named")).isEmpty());
    }
    ShellOptions options;
    options.root = root.path();
    options.settings = R"({"historyVersions": 3, "historyDays": 400, "historyGigabytes": 0.5})";
    ShellFixture shell(options);
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    History *history = shell.backend->history();
    QCOMPARE(history->limits().versions, 3);
    QCOMPARE(history->limits().days, 400);
    QCOMPARE(history->limits().bytes, qint64(1) << 29);
    QCOMPARE(history->versions(id).size(), 2);

    // Preferences change them, for history too, and keep them. A higher
    // limit applies at once; a lower one waits until the limits stop
    // changing, and removes versions only once the writer agrees.
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1000);
    QObject *dialog = shell.object(QStringLiteral("preferencesDialog"));
    runIn(window, "preferences");
    QTRY_VERIFY(isOpen(dialog));
    const auto control = [&](const char *name) {
        return qobject_cast<QQuickItem *>(findObject(window, QString::fromLatin1(name)));
    };
    QQuickItem *section = control("historyPreferences");
    QVERIFY(section);
    const int settleMs = section->property("settleMs").toInt();
    QVERIFY(settleMs >= 500);
    QQuickItem *confirmation = control("historyRemovals");
    QVERIFY(confirmation);
    QQuickItem *days = control("historyDaysChoice");
    QVERIFY(days);
    QCOMPARE(days->property("value").toInt(), 400);
    days->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Down);
    QCOMPARE(days->property("value").toInt(), 399);
    QCOMPARE(shell.settings()->historyDays(), 400);
    // Nothing is past 399 days, so nothing is asked.
    QTRY_COMPARE(shell.settings()->historyDays(), 399);
    QTRY_COMPARE(history->limits().days, 399);
    QVERIFY(!confirmation->isVisible());
    QQuickItem *space = control("historySpaceChoice");
    QVERIFY(space);
    space->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Up);
    QCOMPARE(shell.settings()->historyGigabytes(), 0.6);
    for (int i = 0; i < 3; ++i) QVERIFY(!history->capture(id, QByteArray::number(i)).isEmpty());
    QTRY_COMPARE(history->versions(id).size(), 4);

    // Stepped down past what was meant and back again, it removes nothing.
    QQuickItem *versions = control("historyVersionsChoice");
    QVERIFY(versions);
    versions->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Down);
    QCOMPARE(versions->property("value").toInt(), 1);
    QTest::keyClick(window, Qt::Key_Up);
    QTest::keyClick(window, Qt::Key_Up);
    QCOMPARE(versions->property("value").toInt(), 3);
    QTest::qWait(settleMs + 500);
    QCOMPARE(shell.settings()->historyVersions(), 3);
    QCOMPARE(history->limits().versions, 3);
    QCOMPARE(history->versions(id).size(), 4);
    QVERIFY(!confirmation->isVisible());

    // Lowered and left, it says how many versions go, and Keep them leaves them.
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_VERIFY(confirmation->isVisible());
    QCOMPARE(section->property("removals").toInt(), 1);
    QCOMPARE(control("historyRemovalsText")->property("text").toString(),
             QStringLiteral("The lower limit removes 1 older version for good."));
    QCOMPARE(shell.settings()->historyVersions(), 3);
    QCOMPARE(history->versions(id).size(), 4);
    QMetaObject::invokeMethod(control("historyRemovalsKeep"), "clicked");
    QTRY_VERIFY(!confirmation->isVisible());
    QCOMPARE(versions->property("value").toInt(), 3);
    QCOMPARE(shell.settings()->historyVersions(), 3);
    QTest::qWait(settleMs + 500);
    QCOMPARE(history->versions(id).size(), 4);

    // Remove them lowers it, which prunes, but only the unnamed versions.
    versions->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(section->property("removals").toInt(), 2);
    QTRY_VERIFY(confirmation->isVisible());
    QCOMPARE(control("historyRemovalsText")->property("text").toString(),
             QStringLiteral("The lower limit removes 2 older versions for good."));
    QMetaObject::invokeMethod(control("historyRemovalsApply"), "clicked");
    QCOMPARE(shell.settings()->historyVersions(), 1);
    QTRY_COMPARE(history->limits().versions, 1);
    QTRY_COMPARE(history->versions(id).size(), 2);
    QCOMPARE(namesOf(history->versions(id)), (QStringList{{}, QStringLiteral("named")}));
    QVERIFY(!confirmation->isVisible());

    // A lower limit not yet agreed to is dropped as Preferences close.
    days->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Down);
    QCOMPARE(days->property("value").toInt(), 398);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));
    QTest::qWait(settleMs + 500);
    QCOMPARE(shell.settings()->historyDays(), 399);
    QCOMPARE(history->limits().days, 399);
    runIn(window, "preferences");
    QTRY_VERIFY(isOpen(dialog));
    QCOMPARE(days->property("value").toInt(), 399);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));

    const QJsonObject kept = QJsonDocument::fromJson(readFile(paths.settingsFile())).object();
    QCOMPARE(kept.value(QStringLiteral("historyVersions")).toInt(), 1);
    QCOMPARE(kept.value(QStringLiteral("historyDays")).toInt(), 399);
    QCOMPARE(kept.value(QStringLiteral("historyGigabytes")).toDouble(), 0.6);

    // What a lower limit would remove is counted without removing it, the
    // space limit's oldest versions of every document too.
    History counted(AppPaths::under(root.path() + QStringLiteral("/counted")), HistoryLimits{10, 30, qint64(1) << 30});
    const QString a = newDocumentId(), b = newDocumentId();
    for (int i = 0; i < 4; ++i) QVERIFY(!counted.capture(a, QByteArray(100, char('a' + i))).isEmpty());
    for (int i = 0; i < 2; ++i) QVERIFY(!counted.capture(b, QByteArray(100, char('k' + i))).isEmpty());
    QVERIFY(!counted.capture(b, QByteArray(100, 'n'), QStringLiteral("named")).isEmpty());
    // Said from the pruner's thread, and taken here.
    QList<std::pair<int, int>> said;
    QObject::connect(
        &counted, &History::removalsCounted, &counted,
        [&said](int versions, int, qint64, int count) { said.append({versions, count}); }, Qt::QueuedConnection);
    counted.countRemovals(HistoryLimits{2, 30, qint64(1) << 30});
    QTRY_COMPARE(said.size(), 1);
    QCOMPARE(said.last(), (std::pair<int, int>{2, 2}));
    counted.countRemovals(HistoryLimits{10, 30, 450});
    QTRY_COMPARE(said.size(), 2);
    QCOMPARE(said.last(), (std::pair<int, int>{10, 3}));
    QCOMPARE(counted.versions(a).size(), 4);
    QCOMPARE(counted.versions(b).size(), 3);
}

void recentFilesCanBeClearedOrForgotten()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = canonical(root.path());
    const QString a = folder + QStringLiteral("/a.md"), b = folder + QStringLiteral("/b.md"),
                  c = folder + QStringLiteral("/c.md");
    for (const QString &path : {a, b, c}) writeFile(path, "text\n");
    ShellFixture shell({.root = folder + QStringLiteral("/app")});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1200);
    activate(window);
    RecentFiles *recent = shell.backend->recentFiles();
    for (const QString &path : {c, b, a}) recent->note(path);
    QCOMPARE(recent->paths(), (QStringList{a, b, c}));
    QVERIFY(openSection(shell, QStringLiteral("Recent")));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("b.md")));

    // Delete on a recent file takes it out of Recent, and leaves the file.
    QVERIFY(keysOnRow(shell, QStringLiteral("b.md")));
    QTest::keyClick(window, Qt::Key_Delete);
    QTRY_COMPARE(recent->paths(), (QStringList{a, c}));
    QVERIFY(QFile::exists(b));
    QTRY_VERIFY(!libraryRow(window, QStringLiteral("b.md")));
    // So does its menu, which can clear Recent too.
    QVERIFY(keysOnRow(shell, QStringLiteral("c.md")));
    QTest::keyClick(window, Qt::Key_F10, Qt::ShiftModifier);
    QObject *menu = shell.object(QStringLiteral("libraryListMenu"));
    QTRY_VERIFY(isOpen(menu));
    QVERIFY(shell.item(QStringLiteral("menuClearRecent"))->isVisible());
    QVERIFY(!shell.item(QStringLiteral("menuDeleteDraft"))->isVisible());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(shell.item(QStringLiteral("menuForgetRecent"))));
    QTRY_COMPARE(recent->paths(), QStringList{a});
    QTRY_VERIFY(!isOpen(menu));

    // Clear recent files, from the window's commands: the notice offers Undo.
    QVERIFY(shell.backend->commands()->canRun(QStringLiteral("clearRecent"), shell.context()));
    recent->note(c);
    runIn(window, "clearRecent");
    QVERIFY(recent->paths().isEmpty());
    QVERIFY(shell.backend->notice());
    QVERIFY(shell.backend->noticeUndoable());
    QQuickItem *undo = shell.item(QStringLiteral("noticeUndo"));
    QTRY_VERIFY(undo && undo->isVisible());
    {
        QAccessible::setActive(true);
        const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
        QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    }
    // Undo stands before Dismiss, once the row is laid out.
    QQuickItem *dismiss = shell.item(QStringLiteral("noticeDismiss"));
    QTRY_VERIFY(undo->mapToScene({undo->width(), 0}).x() <= dismiss->mapToScene({0, 0}).x());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(undo));
    QTRY_COMPARE(recent->paths(), (QStringList{c, a}));
    QVERIFY(!shell.backend->notice());
    // Undo goes once the notice does.
    runIn(window, "clearRecent");
    QVERIFY(shell.backend->noticeUndoable());
    shell.backend->dismissNotice();
    QVERIFY(!shell.backend->noticeUndoable());
    shell.backend->undoNotice();
    QVERIFY(recent->paths().isEmpty());
    // The window's menu has it.
    QVERIFY(findObject(window, QStringLiteral("clearRecentEntry")));

    // Kept for the next launch.
    QTRY_COMPARE(QJsonDocument::fromJson(readFile(shell.paths().recentFile())).object().value("paths").toArray().size(), 0);
}

void deletedDraftsGoToHistoryAndCanComeBack()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = canonical(root.path());
    const AppPaths paths = AppPaths::under(folder);
    const QString idea = writeDraft(paths, "# Old idea\n\nWords.\n");
    const QString other = writeDraft(paths, "# Second thought\n");
    const QString ideaFolder = paths.drafts() + u'/' + idea;
    ShellFixture shell({.root = folder});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1200);
    activate(window);
    History *history = shell.backend->history();
    QVERIFY(openSection(shell, QStringLiteral("Drafts")));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Old idea")));

    // Delete on a draft that is not open: its text goes to history, and its
    // folder, assets and all, is set aside; the notice can undo it.
    QVERIFY(keysOnRow(shell, QStringLiteral("Old idea")));
    QTest::keyClick(window, Qt::Key_Delete);
    QTRY_VERIFY(!QFileInfo::exists(ideaFolder));
    QTRY_VERIFY(shell.backend->noticeUndoable());
    QVERIFY(shell.backend->problem().contains(QStringLiteral("Old idea")));
    QCOMPARE(history->versions(idea).size(), 1);
    QCOMPARE(history->versions(idea).first().reason, History::Reason::Deleted);
    QCOMPARE(history->read(idea, history->versions(idea).first().id), QByteArray("# Old idea\n\nWords.\n"));
    QTRY_VERIFY(!libraryRow(window, QStringLiteral("Old idea")));
    QQuickItem *undo = shell.item(QStringLiteral("noticeUndo"));
    QQuickItem *dismiss = shell.item(QStringLiteral("noticeDismiss"));
    QTRY_VERIFY(undo && undo->isVisible() && undo->mapToScene({undo->width(), 0}).x() <= dismiss->mapToScene({0, 0}).x());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(undo));
    QTRY_VERIFY(QFile::exists(ideaFolder + QStringLiteral("/assets/picture.png")));
    QCOMPARE(readFile(DocumentStore::draftFile(paths, idea)), QByteArray("# Old idea\n\nWords.\n"));
    // Put back, it opens in a tab.
    QTRY_VERIFY(shell.document() && shell.document()->id() == idea);

    // Deleted while open with text just written, from its menu: the text as
    // it is now goes to history, and its tab closes.
    typeText(window, QStringLiteral("More "));
    const QByteArray written = shell.document()->bytes();
    QVERIFY(written.startsWith("More "));
    const QString shownTitle = shell.document()->title();
    QTRY_VERIFY(libraryRow(window, shownTitle));
    QVERIFY(keysOnRow(shell, shownTitle));
    QTest::keyClick(window, Qt::Key_F10, Qt::ShiftModifier);
    QObject *menu = shell.object(QStringLiteral("libraryListMenu"));
    QTRY_VERIFY(isOpen(menu));
    QVERIFY(!shell.item(QStringLiteral("menuClearRecent"))->isVisible());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(shell.item(QStringLiteral("menuDeleteDraft"))));
    QTRY_VERIFY(!QFileInfo::exists(ideaFolder));
    QTRY_VERIFY(shell.backend->noticeUndoable());
    QCOMPARE(history->read(idea, history->versions(idea).first().id), written);
    QVERIFY(shell.context()->tabs()->rowCount() >= 1);
    QVERIFY(!shell.document() || shell.document()->id() != idea);
    for (Document *document : shell.backend->store()->documents()) QVERIFY(document->id() != idea);
    // Once the notice goes, it can no longer be undone, and nothing of it is left aside.
    const QString aside = DocumentStore::asideFolder(paths, idea);
    QVERIFY(QFileInfo::exists(aside));
    shell.backend->dismissNotice();
    QTRY_VERIFY(!QFileInfo::exists(aside));
    // Nor is recovery left holding it.
    QTRY_VERIFY(!QFileInfo::exists(paths.recovery() + u'/' + idea + QStringLiteral(".recovery")));

    // Open with unsaved text in another window: refused, and nothing changes.
    shell.backend->newWindow();
    QQuickWindow *second = shell.windowFor(shell.backend->windows().at(1));
    QVERIFY(second);
    QTRY_VERIFY(second->isActive());
    shell.backend->openDraft(other);
    QTRY_VERIFY(shell.backend->document() && shell.backend->document()->id() == other);
    Document *otherDraft = shell.backend->document();
    typeText(second, QStringLiteral("Unsaved "));
    QCOMPARE(otherDraft->state(), Document::State::Modified);
    activate(window);
    shell.backend->activate(shell.context());
    shell.backend->deleteDraft(other);
    QVERIFY(shell.backend->notice());
    QVERIFY(!shell.backend->noticeUndoable());
    QVERIFY(shell.backend->problem().contains(QStringLiteral("not deleted")));
    QVERIFY(QFileInfo::exists(paths.drafts() + u'/' + other));
    QCOMPARE(shell.backend->windows().at(1)->tabs()->indexOf(otherDraft), 0);
    // Once its text is saved there, it can be.
    QTRY_COMPARE(otherDraft->state(), Document::State::Clean);
    shell.backend->deleteDraft(other);
    QTRY_VERIFY(!QFileInfo::exists(paths.drafts() + u'/' + other));
    QTRY_VERIFY(shell.backend->noticeUndoable());
    // The other window keeps a tab of its own.
    QCOMPARE(shell.backend->windows().size(), 2);
    QVERIFY(shell.backend->windows().at(1)->tabs()->rowCount() >= 1);
    QCOMPARE(shell.backend->windows().at(1)->tabs()->indexOf(otherDraft), -1);
    // Set aside until the notice goes.
    QVERIFY(QFileInfo::exists(DocumentStore::asideFolder(paths, other)));
}

void draftsSetAsideAreForgottenAtLaunch()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString id = newDocumentId();
    writeFile(DocumentStore::asideFolder(paths, id) + QStringLiteral("/draft.md"), "left by a crash\n");
    const QString kept = writeDraft(paths, "# Kept\n");
    ShellFixture shell({.root = root.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QTRY_VERIFY(!QFileInfo::exists(DocumentStore::asideFolder(paths, id)));
    QVERIFY(QFileInfo::exists(DocumentStore::draftFile(paths, kept)));
}

void userGuideOpensAsReadOnlyHelp()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    ShellFixture shell({.root = root.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1200);
    activate(window);
    TabModel *tabs = shell.context()->tabs();
    const int before = tabs->rowCount();
    // Not installed, the guide is the copy parch carries, the same as docs/guide.md.
    const QByteArray bundled = readFile(QStringLiteral(PARCH_SOURCE_DIR "/docs/guide.md"));
    QVERIFY(bundled.startsWith("# parch guide"));
    // F1 reaches the window from the text.
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    editor->forceActiveFocus();
    QCOMPARE(window->activeFocusItem(), editor);
    QTest::keyClick(window, Qt::Key_F1);
    QTRY_VERIFY(shell.document() && shell.document()->isScratch());
    Document *guide = shell.document();
    // It takes the place of the empty draft the window opened with.
    QCOMPARE(tabs->rowCount(), before);
    QCOMPARE(guide->buffer()->text(), QString::fromUtf8(bundled));
    QCOMPARE(guide->title(), QStringLiteral("User guide"));
    QCOMPARE(guide->state(), Document::State::ReadOnly);
    QCOMPARE(shell.object(QStringLiteral("header"))->property("place").toString(), QStringLiteral("Help"));
    // Read-only: typing changes nothing, and no notice says it is broken.
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("x"));
    QCOMPARE(guide->buffer()->text(), QString::fromUtf8(bundled));
    QVERIFY(shell.object(QStringLiteral("noticeBar"))->property("kinds").toStringList().isEmpty());
    // Saving it says it is read-only and asks for nothing.
    for (const char *command : {"save", "saveAs"}) {
        runIn(window, command);
        QVERIFY(shell.backend->notice());
        QVERIFY2(shell.backend->problemDetail().contains(QStringLiteral("read-only")), qPrintable(shell.backend->problemDetail()));
        shell.backend->dismissNotice();
    }
    // It is no draft: the library does not list it, and the session does not keep it.
    QVERIFY(openSection(shell, QStringLiteral("Drafts")));
    QTest::qWait(50);
    QCOMPARE(treeRow(shell.object(QStringLiteral("libraryTree")), QStringLiteral("User guide")), -1);
    for (const ViewState &view : shell.context()->state().tabs) QVERIFY(view.documentId != guide->id());
    // Asked again, the tab that shows it comes to the front.
    QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QStringLiteral("newDraft")));
    QTRY_VERIFY(shell.document() != guide);
    runIn(window, "userGuide");
    QTRY_COMPARE(shell.document(), guide);
    QCOMPARE(tabs->rowCount(), before + 1);
    // Closing it asks nothing, and nothing of it was ever written.
    const QString id = guide->id();
    shell.backend->requestCloseTab(tabs->activeIndex());
    QTRY_COMPARE(tabs->rowCount(), before);
    QVERIFY(!shell.backend->notice());
    QTRY_VERIFY(!shell.backend->store()->documents().contains(guide));
    QVERIFY(!QFileInfo::exists(paths.drafts() + u'/' + id));
    QVERIFY(!QFileInfo::exists(paths.recovery() + u'/' + id + QStringLiteral(".recovery")));
    QVERIFY(!QFileInfo::exists(paths.history() + u'/' + id));

    // Installed, the guide is the installed copy.
    QTemporaryDir installed;
    QVERIFY(installed.isValid());
    const QString copy = installed.filePath(QStringLiteral("share/doc/parch/guide.md"));
    writeFile(copy, "# The installed guide\n");
    shell.backend->setInstalledGuide(copy);
    runIn(window, "userGuide");
    QTRY_VERIFY(shell.document() && shell.document()->isScratch());
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("# The installed guide\n"));
    // The window's menu has it.
    QVERIFY(findObject(window, QStringLiteral("userGuideEntry")));
}

void eachWindowKeepsItsLibraryWidth()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = canonical(root.path());
    const QString one = folder + QStringLiteral("/one.md"), two = folder + QStringLiteral("/two.md");
    writeFile(one, "one\n");
    writeFile(two, "two\n");
    const QString app = folder + QStringLiteral("/app");
    int firstWidth = 0, secondWidth = 0;
    {
        ShellFixture shell({.settings = R"({"libraryWidth": 300})", .root = app});
        QVERIFY2(shell.ok(), qPrintable(shell.error));
        QQuickWindow *window = shell.window.get();
        resizeWindow(window, 1400);
        activate(window);
        shell.backend->openFiles({one});
        WindowContext *first = shell.context();
        QQuickItem *library = shell.item(QStringLiteral("library"));
        QQuickItem *splitter = shell.item(QStringLiteral("librarySplitter"));
        QVERIFY(splitter && splitter->isVisible());
        QCOMPARE(first->libraryWidth(), 300);
        QCOMPARE(library->width(), 300.0);
        // Named, and reached by Tab.
        QAccessibleInterface *access = QAccessible::queryAccessibleInterface(splitter);
        QVERIFY(access && !access->text(QAccessible::Name).isEmpty());
        QVERIFY(splitter->activeFocusOnTab());
        QStringList problems;
        QVERIFY2(tabKeyReachesEveryControl(window, &problems), qPrintable(problems.join("; ")));

        // The arrows make it wider and narrower, a step at a time, and End
        // and Home as wide and as narrow as it goes.
        splitter->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Right);
        const int step = first->libraryWidth() - 300;
        QVERIFY(step > 0);
        QCOMPARE(library->width(), 300.0 + step);
        QTest::keyClick(window, Qt::Key_Left);
        QTest::keyClick(window, Qt::Key_Left);
        QCOMPARE(first->libraryWidth(), 300 - step);
        QTest::keyClick(window, Qt::Key_Home);
        QCOMPARE(first->libraryWidth(), 160);
        QTest::keyClick(window, Qt::Key_End);
        QCOMPARE(first->libraryWidth(), 640);
        // The editor keeps its room, so the library stays beside it.
        QVERIFY(window->property("libraryDocked").toBool());
        QTest::keyClick(window, Qt::Key_Home);
        // A drag moves it with the pointer.
        const QPoint grab = splitter->mapToScene({splitter->width() / 2, splitter->height() / 2}).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, grab);
        QTest::mouseMove(window, grab + QPoint(40, 0));
        QTest::mouseMove(window, grab + QPoint(90, 0));
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, grab + QPoint(90, 0));
        QTRY_COMPARE(first->libraryWidth(), 250);
        QCOMPARE(library->width(), 250.0);
        // The width chosen last is where a new window starts.
        QCOMPARE(shell.settings()->libraryWidth(), 250);

        // A second window has a width of its own.
        shell.backend->newWindow();
        WindowContext *second = shell.backend->windows().at(1);
        QQuickWindow *other = shell.windowFor(second);
        QVERIFY(other);
        QTRY_VERIFY(other->isActive());
        resizeWindow(other, 1400);
        shell.backend->openFiles({two});
        QCOMPARE(second->libraryWidth(), 250);
        QQuickItem *otherSplitter = findItem(other->contentItem(), QStringLiteral("librarySplitter"));
        QVERIFY(otherSplitter);
        otherSplitter->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(other, Qt::Key_End);
        QCOMPARE(second->libraryWidth(), 640);
        QCOMPARE(first->libraryWidth(), 250);
        QCOMPARE(library->width(), 250.0);
        firstWidth = first->libraryWidth();
        secondWidth = second->libraryWidth();
        shell.backend->quit();
    }
    // The session keeps each window's width.
    const SessionState kept = Session::load(AppPaths::under(app));
    QCOMPARE(kept.windows.size(), 2);
    QCOMPARE(kept.windows.at(0).libraryWidth, firstWidth);
    QCOMPARE(kept.windows.at(1).libraryWidth, secondWidth);
    {
        ShellFixture again({.root = app});
        QVERIFY2(again.ok(), qPrintable(again.error));
        QCOMPARE(again.backend->windows().size(), 2);
        QCOMPARE(again.backend->windows().at(0)->libraryWidth(), firstWidth);
        QCOMPARE(again.backend->windows().at(1)->libraryWidth(), secondWidth);
        // A window opened now starts at the width chosen last.
        again.backend->newWindow();
        QCOMPARE(again.backend->windows().at(2)->libraryWidth(), 640);
    }
    // A width out of range in the session is the default's.
    QByteArray session = readFile(AppPaths::under(app).sessionFile());
    QVERIFY(session.contains("\"libraryWidth\": 250"));
    session.replace("\"libraryWidth\": 250", "\"libraryWidth\": 5");
    writeFile(AppPaths::under(app).sessionFile(), session);
    QCOMPARE(Session::load(AppPaths::under(app)).windows.at(0).libraryWidth, 0);
}

void renamesByOtherProgramsAreFollowed()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = canonical(root.path());
    const QString notes = folder + QStringLiteral("/Writing/notes.md");
    const QString ideas = folder + QStringLiteral("/Writing/ideas.md");
    const QString neighbour = folder + QStringLiteral("/Other/neighbour.md");
    const QString filed = folder + QStringLiteral("/Other/ideas.md");
    const QString away = folder + QStringLiteral("/Away/ideas.md");
    writeFile(notes, "First line\nSecond line\n");
    writeFile(neighbour, "neighbour\n");
    QVERIFY(QDir().mkpath(folder + QStringLiteral("/Away")));
    ShellFixture shell({.root = folder + QStringLiteral("/app")});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({neighbour, notes});
    Document *document = shell.document();
    QVERIFY(document);
    QCOMPARE(document->path(), notes);
    const QString id = document->id();
    QTRY_VERIFY(shell.backend->recentFiles()->paths().contains(notes));
    TabModel *tabs = shell.context()->tabs();

    // Renamed in its folder, with text not yet saved: the document follows,
    // keeping its identity and its text, and a notice says so.
    typeText(window, QStringLiteral("New "));
    QVERIFY(QFile::rename(notes, ideas));
    QTRY_COMPARE(document->path(), ideas);
    QCOMPARE(document->id(), id);
    QVERIFY(document->state() != Document::State::Conflict);
    QVERIFY(document->buffer()->text().startsWith(QStringLiteral("New First line")));
    QCOMPARE(tabs->activeTitle(), QStringLiteral("ideas.md"));
    QVERIFY(shell.backend->notice());
    QVERIFY(shell.backend->problem().contains(QStringLiteral("ideas.md")));
    QVERIFY(shell.backend->problemDetail().contains(QStringLiteral("notes.md")));
    QTRY_VERIFY(shell.backend->recentFiles()->paths().contains(ideas));
    QVERIFY(!shell.backend->recentFiles()->paths().contains(notes));
    // Its text is saved at the new name, never at the old one.
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(readFile(ideas), QByteArray("New First line\nSecond line\n"));
    QVERIFY(!QFileInfo::exists(notes));

    // Moved, and changed on the way, into a folder parch watches, as that of
    // another open document: followed, and the change taken.
    QVERIFY(QFile::rename(ideas, filed));
    {
        QFile moved(filed);
        QVERIFY(moved.open(QIODevice::Append));
        moved.write("Third line\n");
    }
    QTRY_COMPARE(document->path(), filed);
    QTRY_COMPARE(document->buffer()->text(), QStringLiteral("New First line\nSecond line\nThird line\n"));
    QCOMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->id(), id);

    // Set aside and written anew in its place, as some editors save: not a
    // rename, so the document stays and takes the new text.
    QVERIFY(QFile::rename(filed, filed + QStringLiteral("~")));
    writeFile(filed, "Rewritten\n");
    QTRY_COMPARE(document->buffer()->text(), QStringLiteral("Rewritten\n"));
    QCOMPARE(document->path(), filed);

    // Moved where parch watches nothing: the file is missing, as before.
    shell.backend->dismissNotice();
    QVERIFY(QFile::rename(filed, away));
    QTRY_COMPARE(document->state(), Document::State::Conflict);
    QVERIFY(document->fileMissing());
    QCOMPARE(document->path(), filed);
}

void encodingsDecodeWhatUtf8CannotRead()
{
    // Offered: the common ones first, by names a person knows them by, and
    // the converter's others after.
    const QList<Encodings::Encoding> offered = Encodings::offered();
    QStringList names;
    for (const Encodings::Encoding &encoding : offered) names << encoding.name;
    for (const char *name : {"windows-1252", "ISO-8859-1", "UTF-16LE", "UTF-16BE"})
        QVERIFY2(names.contains(QString::fromLatin1(name)), name);
    QCOMPARE(names.first(), QStringLiteral("windows-1252"));
    QCOMPARE(names.size(), QSet<QString>(names.cbegin(), names.cend()).size());
    for (const Encodings::Encoding &encoding : offered) QVERIFY(!encoding.label.isEmpty());

    // Windows-1252, with its curly quotes and euro sign, and CRLF kept.
    const QByteArray western("\x93quoted\x94 caf\xe9 \x80\r\nline\r\n");
    Encodings::Decoded decoded = Encodings::decode(western, QStringLiteral("windows-1252"));
    QVERIFY(decoded.known);
    QCOMPARE(decoded.text, QStringLiteral("“quoted” café €\r\nline\r\n"));
    QCOMPARE(decoded.convention.newline, QStringLiteral("\r\n"));
    QCOMPARE(decoded.failures, 0);
    // Latin-1 reads the same bytes as control characters and letters.
    decoded = Encodings::decode(western, QStringLiteral("ISO-8859-1"));
    QCOMPARE(decoded.text.at(0), QChar(0x93));
    QCOMPARE(decoded.text.mid(1, 6), QStringLiteral("quoted"));
    // UTF-16, with its byte-order mark or without it.
    const QString text = QStringLiteral("Grüße, 世界\n");
    QByteArray little("\xff\xfe"), big;
    for (const QChar c : text) {
        little.append(char(c.unicode() & 0xff)).append(char(c.unicode() >> 8));
        big.append(char(c.unicode() >> 8)).append(char(c.unicode() & 0xff));
    }
    QCOMPARE(Encodings::decode(little, QStringLiteral("UTF-16LE")).text, text);
    QCOMPARE(Encodings::decode(big, QStringLiteral("UTF-16BE")).text, text);
    QCOMPARE(Encodings::decode(little, QStringLiteral("UTF-16")).text, text);
    // What an encoding cannot read is counted.
    QVERIFY(Encodings::decode(QByteArray("\xff\xfe\x41\x00\x42", 5), QStringLiteral("UTF-16LE")).failures > 0);
    QVERIFY(!Encodings::decode("text", QStringLiteral("no such encoding")).known);
    // The likely encoding: as a byte-order mark says, UTF-16 by its zeros,
    // and otherwise Windows-1252.
    QCOMPARE(Encodings::likely(little), QStringLiteral("UTF-16LE"));
    QCOMPARE(Encodings::likely(big), QStringLiteral("UTF-16BE"));
    QCOMPARE(Encodings::likely(little.mid(2)), QStringLiteral("UTF-16LE"));
    QCOMPARE(Encodings::likely(western), QStringLiteral("windows-1252"));
}

void reopeningInAnotherEncodingSavesOnlyOnceConfirmed()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = canonical(root.path());
    const QString path = folder + QStringLiteral("/letter.md");
    const QByteArray original("Dear \x93" "friend\x94,\r\nthe caf\xe9 is open.\r\n");
    writeFile(path, original);
    ShellFixture shell({.root = folder + QStringLiteral("/app"), .files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1200);
    activate(window);
    Document *document = shell.document();
    QVERIFY(document);
    QCOMPARE(document->state(), Document::State::ReadOnly);
    const auto kinds = [&] { return shell.object(QStringLiteral("noticeBar"))->property("kinds").toStringList(); };
    QCOMPARE(kinds(), QStringList{QStringLiteral("readOnly")});

    // Reopen as… is the read-only notice's action: a dialog with the
    // encodings, the likely one chosen, and the text as it reads in it.
    QQuickItem *reopen = findItem(shell.item(QStringLiteral("readOnlyNotice")), QStringLiteral("noticeAction"));
    QVERIFY(reopen);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(reopen));
    QObject *dialog = shell.object(QStringLiteral("encodingDialog"));
    QTRY_VERIFY(isOpen(dialog));
    QObject *choice = shell.object(QStringLiteral("encodingChoice"));
    QTRY_VERIFY(choice->property("ready").toBool());
    QCOMPARE(choice->property("name").toString(), QStringLiteral("windows-1252"));
    QTRY_VERIFY(choice->property("preview").toString().startsWith(QStringLiteral("Dear “friend”,")));
    {
        QAccessible::setActive(true);
        const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
        QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    }
    // It fits a narrow window too.
    for (int width : {360, 1200}) {
        resizeWindow(window, width);
        QTest::qWait(50);
        QStringList problems;
        QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    }
    // Another encoding shows another reading, and nothing changes until Convert.
    QQuickItem *list = shell.item(QStringLiteral("encodingList"));
    QVERIFY(list);
    const QStringList names = choice->property("names").toStringList();
    QMetaObject::invokeMethod(list, "activated", Q_ARG(int, int(names.indexOf(QStringLiteral("ISO-8859-1")))));
    QTRY_COMPARE(choice->property("name").toString(), QStringLiteral("ISO-8859-1"));
    QTRY_VERIFY(choice->property("preview").toString().startsWith(QStringLiteral("Dear \u0093friend")));
    QCOMPARE(document->state(), Document::State::ReadOnly);
    QMetaObject::invokeMethod(list, "activated", Q_ARG(int, int(names.indexOf(QStringLiteral("windows-1252")))));
    QTRY_VERIFY(choice->property("preview").toString().startsWith(QStringLiteral("Dear “friend”,")));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(shell.item(QStringLiteral("convertEncoding"))));
    QTRY_VERIFY(!isOpen(dialog));

    // Converted: editable, the text as it reads in Windows-1252, its line
    // endings kept, with no way back by undo to the text read as UTF-8.
    QCOMPARE(document->buffer()->text(), QStringLiteral("Dear “friend”,\r\nthe café is open.\r\n"));
    QCOMPARE(document->convention().newline, QStringLiteral("\r\n"));
    QCOMPARE(document->convertedFrom(), QStringLiteral("windows-1252"));
    QCOMPARE(document->state(), Document::State::Modified);
    QVERIFY(!document->buffer()->canUndo());
    QCOMPARE(kinds(), QStringList{QStringLiteral("converted")});
    // Nothing is written by itself, however long it waits.
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QTRY_VERIFY(!editor->property("readOnly").toBool());
    editor->forceActiveFocus();
    selectRange(editor, 0, 0);
    typeText(window, QStringLiteral("My "));
    QTest::qWait(Autosave::SaveIdleMs + 500);
    QCOMPARE(readFile(path), original);

    // Saving asks first, and Cancel writes nothing.
    QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QStringLiteral("save")));
    QTRY_VERIFY(isOpen(dialog));
    QCOMPARE(dialog->property("confirming").toBool(), true);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));
    QCOMPARE(readFile(path), original);
    // Confirmed, the file is UTF-8, and its original bytes are in history.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      centreOf(findItem(shell.item(QStringLiteral("convertedNotice")), QStringLiteral("noticeAction"))));
    QTRY_VERIFY(isOpen(dialog));
    // Cancel has the keys; Tab goes on to Save as UTF-8.
    QTRY_COMPARE(window->activeFocusItem(), shell.item(QStringLiteral("cancelEncoding")));
    QTest::keyClick(window, Qt::Key_Tab);
    QCOMPARE(window->activeFocusItem(), shell.item(QStringLiteral("confirmEncoding")));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(readFile(path), QStringLiteral("My Dear “friend”,\r\nthe café is open.\r\n").toUtf8());
    QVERIFY(document->convertedFrom().isEmpty());
    bool kept = false;
    for (const History::Version &version : shell.backend->history()->versions(document->id()))
        kept = kept || shell.backend->history()->read(document->id(), version.id) == original;
    QVERIFY(kept);
    QVERIFY(kinds().isEmpty());
    // From now on it saves as any document does.
    selectRange(editor, 0, 0);
    typeText(window, QStringLiteral("Oh, "));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QVERIFY(readFile(path).startsWith("Oh, My Dear"));
}

void historyRestoresBytesThatAreNotUtf8()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = canonical(root.path());
    const QString path = folder + QStringLiteral("/letter.md");
    const QByteArray original("Dear \x93" "friend\x94,\ncaf\xe9.\n");
    writeFile(path, original);
    ShellFixture shell({.root = folder + QStringLiteral("/app"), .files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1440);
    activate(window);
    Document *document = shell.document();
    QVERIFY(document);
    QCOMPARE(document->state(), Document::State::ReadOnly);
    History *history = shell.backend->history();

    // Read as Windows-1252 and written as UTF-8, as the writer confirms, then edited.
    EncodingChoice choice;
    choice.setDocument(document);
    QTRY_VERIFY(choice.ready());
    QVERIFY(shell.backend->reopenAs(document, &choice));
    shell.backend->confirmEncoding(document);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(readFile(path), QStringLiteral("Dear “friend”,\ncafé.\n").toUtf8());
    document->buffer()->replace({0, 0}, QStringLiteral("Edited. "));
    shell.backend->store()->save(document);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    const QByteArray edited = QStringLiteral("Edited. Dear “friend”,\ncafé.\n").toUtf8();
    QCOMPARE(readFile(path), edited);

    // The original bytes are a version, with the encoding they were read in.
    QString originalId;
    for (const History::Version &version : history->versions(document->id()))
        if (history->read(document->id(), version.id) == original) {
            originalId = version.id;
            QCOMPARE(version.encoding, QStringLiteral("windows-1252"));
        }
    QVERIFY(!originalId.isEmpty());

    // The history dialog shows it as that encoding reads it, and says so.
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QObject *dialog = shell.object(QStringLiteral("historyDialog"));
    QTRY_VERIFY(isOpen(dialog));
    int row = -1;
    QTRY_VERIFY([&] {
        const QVariantList rows = dialog->property("rows").toList();
        for (int i = 0; i < rows.size(); ++i)
            if (rows.at(i).toMap().value(QStringLiteral("id")).toString() == originalId) row = i;
        return row > 0;
    }());
    dialog->setProperty("currentIndex", row);
    QObject *preview = shell.object(QStringLiteral("versionPreview"));
    QTRY_COMPARE(preview->property("text").toString(), QStringLiteral("Dear “friend”,\ncafé.\n"));
    QVERIFY2(preview->property("caption").toString().contains(QStringLiteral("Windows-1252")),
             qPrintable(preview->property("caption").toString()));
    QQuickItem *restore = shell.item(QStringLiteral("restoreVersion"));
    QVERIFY(restore);
    QTRY_VERIFY(restore->isEnabled());
    QMetaObject::invokeMethod(restore, "clicked");
    QTRY_VERIFY(!isOpen(dialog));

    // Restored: the file holds its original bytes again, the document is
    // read-only as such a file is, Reopen as… reads it again, and the text
    // it replaced is a version.
    QTRY_COMPARE(readFile(path), original);
    QTRY_COMPARE(document->state(), Document::State::ReadOnly);
    QTRY_COMPARE(shell.object(QStringLiteral("noticeBar"))->property("kinds").toStringList(),
                 QStringList{QStringLiteral("readOnly")});
    bool kept = false;
    for (const History::Version &version : history->versions(document->id()))
        kept = kept || history->read(document->id(), version.id) == edited;
    QVERIFY(kept);
    EncodingChoice again;
    again.setDocument(document);
    QTRY_VERIFY(again.ready());
    QCOMPARE(again.decoded().text, QStringLiteral("Dear “friend”,\ncafé.\n"));

    // A draft keeps UTF-8 text only.
    Document *draft = shell.backend->store()->newDraft();
    const QString bytesId = history->capture(draft->id(), original);
    QVERIFY(!bytesId.isEmpty());
    QString refused;
    shell.backend->store()->restoreBytes(draft, bytesId, [&refused](const QString &error) { refused = error; });
    QTRY_VERIFY(!refused.isEmpty());
    QVERIFY(draft->buffer()->text().isEmpty());
}

void restoringBytesEndsAsASaveDoes()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = canonical(root.path());
    const QByteArray original("Dear \x93" "friend\x94,\ncaf\xe9.\n");
    // The first capture after this is held until let go, as a slow disk would.
    QSemaphore entered, leave;
    std::atomic<bool> holding{false};
    const auto hold = [&] {
        holding.store(true);
        History::setBeforeCapture([&] {
            if (!holding.exchange(false)) return;
            entered.release();
            leave.acquire();
        });
    };
    const auto unhook = qScopeGuard([&] {
        leave.release(4);
        History::setBeforeCapture({});
    });
    {
        DocumentStore store(AppPaths::under(folder + QStringLiteral("/app")));
        const QString path = folder + QStringLiteral("/letter.md");
        writeFile(path, "Edited.\n");
        Document *document = store.open(path);
        QVERIFY(document);
        const QString versionId = store.history()->capture(document->id(), original);
        QVERIFY(!versionId.isEmpty());
        QVERIFY(store.waitForSaves(10000));
        QList<std::pair<Document *, SaveStatus>> finished;
        QObject::connect(&store, &DocumentStore::saveFinished, &store,
                         [&finished](Document *saved, const QString &, SaveStatus status) { finished.append({saved, status}); });
        hold();
        QString error = QStringLiteral("<not done>");
        store.restoreBytes(document, versionId, [&error](const QString &said) { error = said; });
        QVERIFY(entered.tryAcquire(1, 5000));
        QCOMPARE(document->state(), Document::State::Saving);
        // Typed while the version goes into the file: kept, never lost.
        document->buffer()->replace({0, 0}, QStringLiteral("Typed. "));
        leave.release();
        QTRY_VERIFY(error != u"<not done>");
        QCOMPARE(error, QString());
        QCOMPARE(readFile(path), original);
        QTRY_COMPARE(document->state(), Document::State::ReadOnly);
        // It ends as a save does, with a word to whatever waits for one.
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first() == (std::pair<Document *, SaveStatus>{document, SaveStatus::Saved}));
        QVERIFY(store.waitForSaves(10000));
        bool kept = false;
        for (const History::Version &version : store.history()->versions(document->id()))
            kept = kept || store.history()->read(document->id(), version.id) == QByteArray("Typed. Edited.\n");
        QVERIFY(kept);

        // Given up while it runs, the document goes once it has ended.
        const QString other = folder + QStringLiteral("/other.md");
        writeFile(other, "Other.\n");
        Document *given = store.open(other);
        QVERIFY(given);
        const QString givenId = given->id();
        const QString otherVersion = store.history()->capture(givenId, original);
        QVERIFY(store.waitForSaves(10000));
        QSignalSpy closed(&store, &DocumentStore::documentClosed);
        hold();
        bool done = false;
        store.restoreBytes(given, otherVersion, [&done](const QString &) { done = true; });
        QVERIFY(entered.tryAcquire(1, 5000));
        QVERIFY(store.drop(given));
        QVERIFY(store.documents().contains(given));
        leave.release();
        QTRY_VERIFY(done);
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(closed.first().at(0).toString(), givenId);
        QVERIFY(!store.documents().contains(given));
        QCOMPARE(readFile(other), original);
    }

    // In the window: a tab closed while it runs closes once it has ended.
    const QString first = folder + QStringLiteral("/first.md"), second = folder + QStringLiteral("/second.md");
    writeFile(first, "First.\n");
    writeFile(second, "Second.\n");
    ShellFixture shell({.root = folder + QStringLiteral("/shell"), .files = {first, second}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    TabModel *tabs = shell.context()->tabs();
    QTRY_COMPARE(tabs->rowCount(), 2);
    Document *document = shell.document();
    QCOMPARE(document->path(), second);
    const QString versionId = shell.backend->history()->capture(document->id(), original);
    QVERIFY(!versionId.isEmpty());
    QVERIFY(shell.backend->store()->waitForSaves(10000));
    hold();
    shell.backend->restoreVersion(versionId);
    QVERIFY(QTest::qWaitFor([&] { return entered.tryAcquire(); }, 5000));
    shell.backend->requestCloseTab(tabs->activeIndex());
    QCOMPARE(tabs->rowCount(), 2);
    leave.release();
    QTRY_COMPARE(tabs->rowCount(), 1);
    QCOMPARE(readFile(second), original);
}

void quitLeavesAtOnceOnlyForAStuckSave()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = canonical(root.path());
    const QString path = folder + QStringLiteral("/letter.md");
    writeFile(path, "Slow $slow a$ and $slow b$ and $slow c$ and $slow d$.\n");
    ShellFixture shell({.root = folder + QStringLiteral("/app"), .files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    DocumentStore *store = shell.backend->store();
    Document *document = shell.document();
    QVERIFY(document);
    History *history = shell.backend->history();
    const QString older = history->capture(document->id(), "Older.\n");
    const QString bytes = history->capture(document->id(), "caf\xe9\n");
    QVERIFY(!older.isEmpty() && !bytes.isEmpty());
    QVERIFY(store->waitForSaves(10000));
    QSemaphore entered, leave;
    std::atomic<bool> holding{false};
    const auto hold = [&] {
        holding.store(true);
        History::setBeforeCapture([&] {
            if (!holding.exchange(false)) return;
            entered.release();
            leave.acquire();
        });
    };
    const auto unhook = qScopeGuard([&] {
        leave.release(4);
        History::setBeforeCapture({});
    });

    // History that does not answer holds up a restore, which is refused,
    // but no save runs, so quitting waits for it as it ends.
    hold();
    QString refusal;
    QCOMPARE(store->restoreVersion(document, older, &refusal), DocumentStore::Restore::Refused);
    QVERIFY(entered.tryAcquire(1, 5000));
    QVERIFY(!store->savesRunning());
    leave.release();
    QVERIFY(store->waitForSaves(10000));

    // A restore of bytes that does not end is a save that does not end.
    // Quitting leaves it, but first the export that runs is cancelled,
    // leaving no file of its own, and every file's id is written.
    Exports *exports = shell.backend->exports();
    const QString work = folder + QStringLiteral("/work");
    QDir().mkpath(work + QStringLiteral("/helpers"));
    writeFakeHelpers(work + QStringLiteral("/helpers"), work + QStringLiteral("/helpers.log"));
    exports->setRenderOptions(renderOptions(HelperSet::in(work + QStringLiteral("/helpers")), work));
    const QString never = folder + QStringLiteral("/never.pdf");
    exports->exportTo(document, QStringLiteral("pdf"), {}, never);
    QVERIFY(exports->busy());
    hold();
    store->restoreBytes(document, bytes, [](const QString &) {});
    QVERIFY(entered.tryAcquire(1, 5000));
    QVERIFY(store->savesRunning());
    const QString other = folder + QStringLiteral("/other.md");
    writeFile(other, "Other.\n");
    shell.backend->openFiles({other});
    QTRY_COMPARE(shell.document()->path(), other);
    const QString otherId = shell.document()->id();
    QElapsedTimer waited;
    waited.start();
    shell.backend->endJobsBeforeLeaving(Backend::EndWaitMs);
    QVERIFY(waited.elapsed() < 2 * Backend::EndWaitMs);
    const QJsonObject registry = QJsonDocument::fromJson(readFile(shell.paths().registryFile())).object();
    QVERIFY2(QJsonDocument(registry).toJson().contains(otherId.toUtf8()), qPrintable(QJsonDocument(registry).toJson()));
    QTest::qWait(2000);
    const QStringList left = QDir(folder).entryList(QDir::Files | QDir::Hidden);
    QVERIFY2(!left.contains(QStringLiteral("never.pdf")), qPrintable(left.join(u',')));
    for (const QString &name : left) QVERIFY2(!name.startsWith(QStringLiteral("never.pdf")), qPrintable(name));
    leave.release();
    QVERIFY(store->waitForSaves(10000));
}

void restoringReadsHistoryOffTheUiThread()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = canonical(root.path());
    const QString path = folder + QStringLiteral("/notes.md");
    writeFile(path, "Newer.\n");
    ShellFixture shell({.root = folder + QStringLiteral("/app"), .files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    Document *document = shell.document();
    QVERIFY(document);
    History *history = shell.backend->history();
    const QString older = history->capture(document->id(), "Older.\n");
    QVERIFY(!older.isEmpty());
    QVERIFY(shell.backend->store()->waitForSaves(10000));

    // Restore reads the version on a worker: the text is as it was until then.
    QVERIFY(shell.backend->review()->restoreVersion(document, older));
    QCOMPARE(document->buffer()->text(), QStringLiteral("Newer.\n"));
    QTRY_COMPARE(document->buffer()->text(), QStringLiteral("Older.\n"));
    QVERIFY(shell.backend->problem().isEmpty());
    document->buffer()->undo();
    QCOMPARE(document->buffer()->text(), QStringLiteral("Newer.\n"));
    // A version that is gone is said to be, once it has been looked for.
    QVERIFY(shell.backend->review()->restoreVersion(document, QStringLiteral("20200101T000000000-000000000000")));
    QTRY_COMPARE(shell.backend->problem(), QStringLiteral("Not restored"));
    QCOMPARE(shell.backend->problemDetail(), QStringLiteral("That version is no longer in history"));
    QCOMPARE(document->buffer()->text(), QStringLiteral("Newer.\n"));

    // Keeping the text it replaces waits on history alone, not on every
    // other job: with the shared pool busy, as with saves and looks at
    // files on a slow disk, the restore still goes ahead at once.
    const int threads = QThreadPool::globalInstance()->maxThreadCount();
    const auto gate = std::make_shared<QSemaphore>();
    for (int i = 0; i < threads; ++i) QThreadPool::globalInstance()->start([gate] { gate->acquire(); });
    const auto released = qScopeGuard([&] { gate->release(threads); });
    QElapsedTimer clock;
    clock.start();
    QString refusal;
    QCOMPARE(shell.backend->store()->restoreVersion(document, older, &refusal), DocumentStore::Restore::Restored);
    QVERIFY2(clock.elapsed() < DocumentStore::HistoryWaitMs, qPrintable(QString::number(clock.elapsed())));
    QCOMPARE(document->buffer()->text(), QStringLiteral("Older.\n"));
}

namespace {

// A file dialog that answers Save as… only when told.
class NamePicker : public FilePicker {
public:
    int saves = 0;
    void openDocuments() override {}
    void openFolder() override {}
    void saveDocument(const QUrl &) override { ++saves; }
};

} // namespace

void savingAsOverAReencodedFileAsksFirst()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = canonical(root.path());
    const QString path = folder + QStringLiteral("/letter.md");
    const QByteArray original("Dear \x93" "friend\x94,\ncaf\xe9.\n");
    const QByteArray edited = QStringLiteral("Edited. Dear “friend”,\ncafé.\n").toUtf8();
    writeFile(path, original);
    {
        // The store says it did not save, rather than that it did.
        DocumentStore store(AppPaths::under(folder + QStringLiteral("/store")));
        Document *document = store.open(path);
        QVERIFY(document);
        EncodingChoice choice;
        choice.setDocument(document);
        QTRY_VERIFY(choice.ready());
        const Encodings::Decoded decoded = choice.decoded();
        QVERIFY(store.reopenAs(document, decoded.text, decoded.convention, choice.name(), choice.version()));
        document->buffer()->replace({0, 0}, QStringLiteral("Edited. "));
        QList<std::pair<QString, SaveStatus>> finished;
        QObject::connect(&store, &DocumentStore::saveFinished, &store,
                         [&finished](Document *, const QString &name, SaveStatus status) { finished.append({name, status}); });
        store.saveAs(document, path);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first() == (std::pair<QString, SaveStatus>{path, SaveStatus::Failed}));
        QVERIFY(store.waitForSaves(10000));
        QCOMPARE(readFile(path), original);
    }

    ShellFixture shell({.root = folder + QStringLiteral("/app"), .files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    Document *document = shell.document();
    QVERIFY(document);
    EncodingChoice choice;
    choice.setDocument(document);
    QTRY_VERIFY(choice.ready());
    QVERIFY(shell.backend->reopenAs(document, &choice));
    document->buffer()->replace({0, 0}, QStringLiteral("Edited. "));
    QCOMPARE(document->state(), Document::State::Modified);
    NamePicker picker;
    shell.backend->setFilePicker(&picker);
    QObject *dialog = shell.object(QStringLiteral("encodingDialog"));
    QVERIFY(dialog);

    // Save as… onto its own file asks what Save asks, and writes nothing yet.
    shell.backend->saveAs();
    QCOMPARE(picker.saves, 1);
    emit picker.saveSelected(QUrl::fromLocalFile(path));
    QTRY_VERIFY(isOpen(dialog));
    QVERIFY(dialog->property("confirming").toBool());
    QCOMPARE(readFile(path), original);
    QCOMPARE(document->state(), Document::State::Modified);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));
    // So does trying a save again.
    shell.backend->retrySave();
    QTRY_VERIFY(isOpen(dialog));
    QVERIFY(dialog->property("confirming").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));
    QCOMPARE(readFile(path), original);
    // Confirmed, it is written as UTF-8.
    shell.backend->confirmEncoding(document);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(readFile(path), edited);
}

void reopeningRefusesAFileThatChanged()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = canonical(root.path()) + QStringLiteral("/old.md");
    writeFile(path, "caf\xe9\n");
    DocumentStore store(AppPaths::under(root.path() + QStringLiteral("/app")));
    Document *document = store.open(path);
    QVERIFY(document);
    QCOMPARE(document->state(), Document::State::ReadOnly);
    EncodingChoice choice;
    choice.setDocument(document);
    QTRY_VERIFY(choice.ready());
    // Changed after it was read for the choice: nothing is converted.
    writeFile(path, "caf\xe9 au lait\n");
    QTRY_VERIFY(!(document->diskVersion() == choice.version()));
    QString refusal;
    QVERIFY(!store.reopenAs(document, choice.decoded().text, choice.decoded().convention, choice.name(),
                            choice.version(), &refusal));
    QVERIFY(!refusal.isEmpty());
    QCOMPARE(document->state(), Document::State::ReadOnly);
    // A document that is not read-only is never converted.
    const QString plain = canonical(root.path()) + QStringLiteral("/plain.md");
    writeFile(plain, "plain\n");
    Document *utf8 = store.open(plain);
    QVERIFY(!store.reopenAs(utf8, QStringLiteral("other"), {}, QStringLiteral("windows-1252"), utf8->diskVersion(), &refusal));
    QCOMPARE(utf8->buffer()->text(), QStringLiteral("plain\n"));
}

void repeatedLibrarySearchesReadLittleAgain()
{
    // 10,000 documents whose text takes more than the search's cache holds
    // (2,500 and 6,000 bytes each: 25 and 60 million characters), and fewer
    // (60 bytes). A first search reads every file; a search repeated reads
    // again only what the cache could not hold, rather than every file, as
    // a cache that lets the least recently used go would when a search runs
    // through more than it holds. Each is timed. The cache is not emptied
    // between libraries, so each library's searches make room in what the
    // one before kept.
    FolderSearch::clearCache();
    for (const int bytes : {60, 2500, 6000}) {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString notes = canonical(root.path()) + QStringLiteral("/Notes");
        QByteArray body;
        const QByteArray line("Some words of a note, written in a library of ten thousand of them.\n");
        while (body.size() < bytes) body += line;
        body.truncate(bytes);
        for (int folder = 0; folder < 100; ++folder) {
            const QString path = notes + QStringLiteral("/Folder %1").arg(folder, 3, 10, QLatin1Char('0'));
            QVERIFY(QDir().mkpath(path));
            for (int note = 0; note < 100; ++note) {
                QFile file(path + QStringLiteral("/Note %1.md").arg(note, 3, 10, QLatin1Char('0')));
                QVERIFY(file.open(QIODevice::WriteOnly));
                file.write(note == 7 ? body + "needle\n" : body);
            }
        }
        const AppPaths paths = AppPaths::under(root.path() + QStringLiteral("/app"));
        writeFile(paths.settingsFile(), QJsonDocument(QJsonObject{{QStringLiteral("libraryFolders"), QJsonArray{notes}}}).toJson());
        Settings settings(paths.settingsFile());
        DocumentStore store(paths);
        FolderSearch search(paths, &store, &settings);
        search.setPattern(QStringLiteral("needle"));
        const qint64 characters = qint64(bytes + 1) * 10000;
        // What the cache can hold, as a share of the files.
        const qint64 held = std::min<qint64>(10000, FolderSearch::CacheUnits / (bytes + 1));
        for (int run = 0; run < 3; ++run) {
            const quint64 readsBefore = FolderSearch::fileReads();
            QElapsedTimer took;
            took.start();
            search.start();
            while (!search.finished() && took.elapsed() < 120000)
                QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 2);
            const qint64 reads = qint64(FolderSearch::fileReads() - readsBefore);
            qInfo("10,000 documents of %d bytes (%lld characters), search %d: %lld ms, %lld files read", bytes,
                  characters, run + 1, took.elapsed(), reads);
            QVERIFY(search.finished());
            QCOMPARE(search.filesSearched(), 10000);
            QCOMPARE(search.filesMatched(), 100);
            QVERIFY2(took.elapsed() <= timeLimit(5000), qPrintable(QString::number(took.elapsed())));
            if (run == 0) QCOMPARE(reads, 10000);
            // Again: only what the cache could not hold, with slack for the
            // files around its edge: several workers read at once, so which
            // ones the first search kept varies with how they were scheduled.
            else QVERIFY2(reads <= 10000 - held + 500, qPrintable(QStringLiteral("%1 files read again").arg(reads)));
        }
    }
    FolderSearch::clearCache();
}

namespace {

using namespace Qt::StringLiterals;

struct LibraryMoves {
    QTemporaryDir root;
    QString library = root.filePath(u"library"_s);
    AppPaths paths = AppPaths::under(root.filePath(u"app"_s));
    DocumentStore store{paths};
    Settings settings{paths.settingsFile()};
    FileOrganizer organizer{&store, &settings};

    LibraryMoves()
    {
        QDir().mkpath(library);
        settings.setLibraryFolders({library});
    }
    QString file(const QString &relative) const { return library + u'/' + relative; }
    void save(Document *document, const QByteArray &bytes)
    {
        store.save(document);
        QTRY_VERIFY(!document->differsFromFile());
        QCOMPARE(readFile(document->path()), bytes);
    }
};

} // namespace

void renameUpdatesTextFilesAndHtmlAttributes_data()
{
    QTest::addColumn<bool>("approve");
    QTest::newRow("update") << true;
    QTest::newRow("leave-links") << false;
}

void renameUpdatesTextFilesAndHtmlAttributes()
{
    QFETCH(bool, approve);
    LibraryMoves f;
    const QString old = f.file(u"chapter.md"_s), renamed = f.file(u"first chapter (draft).md"_s);
    const QString html = f.file(u"index.md"_s), notes = f.file(u"notes.txt"_s);
    const QByteArray original =
        "[Chapter](chapter.md?mode=1#top)\n\n"
        "<a title=\"href='chapter.md'\" HrEf='chapter.md?view=plain&amp;mode=1#top'>One</a>\n\n"
        "<a\n  href=chapter.md>Two</a>\n\n"
        "> <a title=\"A\"\n>   href=chapter.md>Quoted</a>\n\n"
        "> <p>\r\n> <img alt='Quoted'\r\n>      src=chapter.md>\r\n> </p>\r\n\r\n"
        "`<a href='chapter.md'>Code</a>` <a href='https://e.com/chapter.md'>Web</a>\n\n"
        "<a href='javascript:chapter.md'>Dropped URL</a>\n\n"
        "<!-- <a href='chapter.md'>Comment</a> -->\n\n"
        "<img alt='Map' SRC='assets/map%20one.png?zoom=2#mark'>\n";
    const QByteArray withLinks =
        "[Chapter](first%20chapter%20%28draft%29.md?mode=1#top)\n\n"
        "<a title=\"href='chapter.md'\" HrEf='first%20chapter%20%28draft%29.md?view=plain&amp;mode=1#top'>One</a>\n\n"
        "<a\n  href=first%20chapter%20%28draft%29.md>Two</a>\n\n"
        "> <a title=\"A\"\n>   href=first%20chapter%20%28draft%29.md>Quoted</a>\n\n"
        "> <p>\r\n> <img alt='Quoted'\r\n>      src=first%20chapter%20%28draft%29.md>\r\n> </p>\r\n\r\n"
        "`<a href='chapter.md'>Code</a>` <a href='https://e.com/chapter.md'>Web</a>\n\n"
        "<a href='javascript:chapter.md'>Dropped URL</a>\n\n"
        "<!-- <a href='chapter.md'>Comment</a> -->\n\n"
        "<img alt='Map' SRC='assets/map%20one.png?zoom=2#mark'>\n";
    writeFile(old, "# Top\n");
    writeFile(notes, "Read [the chapter](chapter.md#top).\r\n");
    writeFile(html, original);
    writeFile(f.file(u"assets/map one.png"_s), "map");
    QVERIFY(QDir().mkpath(f.file(u"archive"_s)));
    Document *index = f.store.open(html);
    QVERIFY(index);
    QSignalSpy moved(&f.organizer, &FileOrganizer::moved);
    f.organizer.rename(old, u"first chapter (draft).md"_s);
    QTRY_VERIFY(!f.organizer.review().isEmpty());
    QCOMPARE(f.organizer.review().value(u"changes"_s).toInt(), 6);
    QCOMPARE(f.organizer.review().value(u"files"_s).toInt(), 2);
    f.organizer.confirmMove(approve);
    QTRY_COMPARE(moved.count(), 1);
    QCOMPARE(readFile(renamed), QByteArray("# Top\n"));
    QCOMPARE(index->buffer()->text().toUtf8(), approve ? withLinks : original);
    QCOMPARE(readFile(notes), approve ? QByteArray("Read [the chapter](first%20chapter%20%28draft%29.md#top).\r\n")
                                      : QByteArray("Read [the chapter](chapter.md#top).\r\n"));
    f.save(index, approve ? withLinks : original);
    if (!approve) return;
    const auto links = DocumentLinks::find(index->buffer()->text(), f.library, f.library);
    QCOMPARE(std::count_if(links.cbegin(), links.cend(), [&](const DocumentLinks::Link &link) {
        return link.destination.path == renamed;
    }), 5);
    f.organizer.move(f.file(u"assets"_s), f.file(u"archive"_s));
    QTRY_VERIFY(!f.organizer.review().isEmpty());
    QCOMPARE(f.organizer.review().value(u"changes"_s).toInt(), 1);
    f.organizer.confirmMove(true);
    QTRY_COMPARE(moved.count(), 2);
    QByteArray after = withLinks;
    after.replace("'assets/map", "'archive/assets/map");
    f.save(index, after);
    QCOMPARE(readFile(f.file(u"archive/assets/map one.png"_s)), QByteArray("map"));

    const QString splitPath = f.file(u"split\npath.md"_s), quoted = f.file(u"quoted.md"_s);
    const QByteArray originalQuote = "> <p>\n> <a href='split\n> path.md'>Split</a>\n> </p>\n";
    writeFile(splitPath, "Split");
    writeFile(quoted, originalQuote);
    f.organizer.rename(splitPath, u"whole.md"_s);
    QTRY_VERIFY(!f.organizer.review().isEmpty());
    const auto row = f.organizer.review().value(u"rows"_s).toList().first().toMap();
    QCOMPARE(row.value(u"file"_s).toString(), u"quoted.md"_s);
    QVERIFY(row.value(u"after"_s).toString().contains(u"no exact source range"));
    f.organizer.confirmMove(true);
    QTRY_COMPARE(moved.count(), 3);
    QVERIFY(moved.last().at(3).toBool());
    QCOMPARE(readFile(quoted), originalQuote);
}

void undoRenameRestoresExactReferenceSpelling_data()
{
    QTest::addColumn<QString>("variation");
    QTest::newRow("exact-bytes") << u"clean"_s;
    QTest::newRow("later-edits") << u"later"_s;
    QTest::newRow("edited-reference") << u"touched"_s;
    QTest::newRow("edited-reference-prefix") << u"prefix"_s;
    QTest::newRow("table-formatting") << u"table"_s;
    QTest::newRow("old-name-taken") << u"collision"_s;
    QTest::newRow("closed-file-changed-before-saving") << u"write"_s;
}

void undoRenameRestoresExactReferenceSpelling()
{
    QFETCH(QString, variation);
    LibraryMoves f;
    const QString chapter = f.file(u"chapter.md"_s), renamed = f.file(u"first chapter.md"_s);
    const QString openPath = f.file(u"index.md"_s), closed = f.file(u"notes.md"_s);
    const QByteArray original = "\xef\xbb\xbf" "chapter.md\r\n\r\nCustom: <% keep %>\n";
    QByteArray updated = original;
    updated.replace("chapter.md", "\"first chapter.md\"");
    writeFile(chapter, "# Chapter\n");
    QByteArray openOriginal = original;
    if (variation == u"table")
        openOriginal += "\n| Doc | Note |\n| --- | --- |\n| [C](chapter.md) | see |\n";
    writeFile(openPath, openOriginal);
    writeFile(closed, original);
    Document *index = f.store.open(openPath);
    QVERIFY(index);
    const QString closedId = f.store.documentIdFor(closed);
    QSignalSpy moved(&f.organizer, &FileOrganizer::moved);
    QSignalSpy problems(&f.organizer, &FileOrganizer::problem);
    f.organizer.rename(chapter, u"first chapter.md"_s);
    QTRY_VERIFY(!f.organizer.review().isEmpty());
    QCOMPARE(f.organizer.review().value(u"changes"_s).toInt(), variation == u"table" ? 3 : 2);
    f.organizer.confirmMove(true);
    QTRY_COMPARE(moved.count(), 1);
    QCOMPARE(readFile(closed), updated);
    const QByteArray openUpdated = index->bytes();
    if (variation == u"table") {
        QVERIFY(openUpdated.startsWith(updated));
        QVERIFY(openUpdated.contains("[C](first%20chapter.md)"));
        QVERIFY(openUpdated.contains("| Doc"));
        QVERIFY(!openUpdated.contains("| Doc | Note |"));
    } else {
        QCOMPARE(openUpdated, updated);
    }
    f.save(index, openUpdated);

    QByteArray openExpected = openOriginal, closedExpected = original;
    if (variation == u"later") {
        index->buffer()->replace({0, 0}, u"An unsaved preface.\r\n\r\n"_s);
        openExpected.insert(3, "An unsaved preface.\r\n\r\n");
        writeFile(closed, updated + "\nAn external postscript.\n");
        closedExpected += "\nAn external postscript.\n";
    } else if (variation == u"touched") {
        const QString path = u"first chapter.md"_s;
        const int at = int(index->buffer()->text().indexOf(path));
        index->buffer()->replace({at, int(path.size())}, u"different.md"_s);
    } else if (variation == u"prefix") {
        index->buffer()->replace({0, 0}, u"other/"_s);
    } else if (variation == u"collision") {
        writeFile(chapter, "Another file\n");
    }
    const QByteArray beforeUndo = index->bytes();
    const auto resetHook = qScopeGuard([] { History::setBeforeCapture({}); });
    if (variation == u"write") {
        History::setBeforeCapture([closed, once = std::make_shared<std::atomic<bool>>(false)] {
            if (once->exchange(true)) return;
            QFile file(closed);
            if (file.open(QIODevice::Append)) file.write("\nAn edit during the save.\n");
        });
    }
    f.organizer.undoMove(moved.first().at(2).toULongLong());
    QTRY_VERIFY(!f.organizer.busy());
    if (variation == u"touched" || variation == u"prefix" || variation == u"collision") {
        QTRY_COMPARE(problems.count(), 1);
        QVERIFY(QFileInfo::exists(renamed));
        QCOMPARE(readFile(chapter), variation == u"collision" ? QByteArray("Another file\n") : QByteArray());
        QCOMPARE(index->bytes(), beforeUndo);
        QCOMPARE(readFile(closed), updated);
        if (variation != u"collision")
            QVERIFY((problems.first()[0].toString() + problems.first()[1].toString()).contains(u"index.md"));
        return;
    }
    QCOMPARE(readFile(chapter), QByteArray("# Chapter\n"));
    QVERIFY(!QFileInfo::exists(renamed));
    QCOMPARE(index->bytes(), openExpected);
    f.save(index, openExpected);
    if (variation == u"write") {
        QTRY_COMPARE(moved.count(), 2);
        QVERIFY(moved.last().at(3).toBool());
        QVERIFY(moved.last().at(1).toString().contains(u"notes.md"));
        const QByteArray external = updated + "\nAn edit during the save.\n";
        QCOMPARE(readFile(closed), external);
        // Taking the partial undo back only reapplies the edits that succeeded.
        f.organizer.undoMove(moved.last().at(2).toULongLong());
        QTRY_COMPARE(moved.count(), 3);
        QVERIFY(!moved.last().at(3).toBool());
        QCOMPARE(index->bytes(), updated);
        QCOMPARE(readFile(closed), external);
        QCOMPARE(readFile(renamed), QByteArray("# Chapter\n"));
    } else {
        QTRY_COMPARE(moved.count(), 2);
        QCOMPARE(readFile(closed), closedExpected);
    }
    const auto versions = f.store.history()->versions(closedId);
    QVERIFY(std::any_of(versions.begin(), versions.end(), [&](const History::Version &version) {
        return f.store.history()->read(closedId, version.id) == original;
    }));
}

void undoMoveWaitsForAnOpenReview()
{
    LibraryMoves f;
    const QString first = f.file(u"first.md"_s), movedFirst = f.file(u"renamed.md"_s);
    const QString second = f.file(u"second.md"_s);
    writeFile(first, "First.\n");
    writeFile(second, "Second.\n");
    writeFile(f.file(u"index.md"_s), "[Second](second.md)\n");
    QSignalSpy moved(&f.organizer, &FileOrganizer::moved);
    QSignalSpy problems(&f.organizer, &FileOrganizer::problem);
    f.organizer.rename(first, u"renamed.md"_s);
    QTRY_COMPARE(moved.count(), 1);
    const quint64 undo = moved.first().at(2).toULongLong();
    QVERIFY(undo > 0);
    QVERIFY(QFileInfo::exists(movedFirst));

    f.organizer.rename(second, u"new-second.md"_s);
    QTRY_VERIFY(!f.organizer.review().isEmpty());
    f.organizer.undoMove(undo);
    QCOMPARE(problems.count(), 1);
    QCOMPARE(moved.count(), 1);
    QVERIFY(QFileInfo::exists(movedFirst));

    f.organizer.cancelMove();
    f.organizer.undoMove(undo);
    QTRY_COMPARE(moved.count(), 2);
    QCOMPARE(readFile(first), QByteArray("First.\n"));
    QVERIFY(!QFileInfo::exists(movedFirst));
    QVERIFY(QFileInfo::exists(second));
}

void undoNoticeWaitsForAMoveReview()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString library = root.filePath(u"library"_s);
    const QString first = library + u"/first.md"_s, movedFirst = library + u"/renamed.md"_s;
    const QString second = library + u"/second.md"_s;
    writeFile(first, "First.\n");
    writeFile(second, "Second.\n");
    writeFile(library + u"/index.md"_s, "[Second](second.md)\n");
    Backend backend(AppPaths::under(root.filePath(u"app"_s)));
    backend.start({});
    backend.settings()->setLibraryFolders({library});
    FileOrganizer *organizer = backend.organizer();
    organizer->rename(first, u"renamed.md"_s);
    QTRY_VERIFY(backend.noticeUndoable());
    QTRY_VERIFY(!organizer->busy());

    organizer->rename(second, u"new-second.md"_s);
    QVERIFY(organizer->busy());
    backend.undoNotice();
    QVERIFY(backend.noticeUndoable());
    QTRY_VERIFY(!organizer->review().isEmpty());
    backend.undoNotice();
    QVERIFY(backend.noticeUndoable());
    QVERIFY(QFileInfo::exists(movedFirst));

    organizer->cancelMove();
    backend.undoNotice();
    QTRY_VERIFY(QFileInfo::exists(first));
    QVERIFY(!QFileInfo::exists(movedFirst));
    QVERIFY(QFileInfo::exists(second));
}

void moveKeepsFrontMatterCitationFilesReachable_data()
{
    QTest::addColumn<QString>("variation");
    QTest::newRow("flow-list-in-an-open-document") << u"flow"_s;
    QTest::newRow("block-list-in-a-closed-document") << u"block"_s;
    QTest::newRow("bundled-style") << u"bundled"_s;
    QTest::newRow("folder-with-dependencies") << u"folder"_s;
    QTest::newRow("same-folder-rename") << u"rename"_s;
    QTest::newRow("dependency-outside-destination-library") << u"outside"_s;
}

void moveKeepsFrontMatterCitationFilesReachable()
{
    QFETCH(QString, variation);
    LibraryMoves f;
    const QString paper = f.file(u"docs/paper.md"_s), deeper = f.file(u"docs/book/deeper"_s);
    const QString outside = f.root.filePath(u"second-library"_s);
    const QString movedPaper = variation == u"rename" ? f.file(u"docs/revised.md"_s)
                             : variation == u"folder" ? f.file(u"archive/docs/paper.md"_s)
                             : variation == u"outside" ? outside + u"/paper.md"_s : deeper + u"/paper.md"_s;
    const QString style = variation == u"bundled" ? u"ieee"_s : u"'style.csl'"_s;
    QString bibliography = u"bibliography: refs.bib # Keep this comment"_s;
    if (variation == u"flow") bibliography = u"Bibliography: [ refs.bib,\r\n  'other refs.bib', \"third.bib\" ] # Keep this comment"_s;
    if (variation == u"block") bibliography = u"bibliography:\r\n  - refs.bib # Keep this comment\r\n  - 'other refs.bib'"_s;
    const QString original = u"---\r\n%1\r\ncsl: %2\r\nunknown: { untouched: yes }\r\nbibliography: ignored.bib\r\n---\r\n\r\nSee [@doe].\r\n"_s
                                 .arg(bibliography, style);
    writeFile(paper, original.toUtf8());
    writeFile(f.file(u"docs/refs.bib"_s), "@book{doe, author={Jane Doe}, title={The Sample Book}, year={2024}}");
    writeFile(f.file(u"docs/other refs.bib"_s), "");
    writeFile(f.file(u"docs/third.bib"_s), "");
    writeFile(f.file(u"docs/style.csl"_s),
              "<style xmlns=\"http://purl.org/net/xbiblio/csl\" version=\"1.0\" class=\"in-text\">"
              "<info><title>Numbers</title><id>http://example.org/local</id></info>"
              "<citation><layout prefix=\"[\" suffix=\"]\"><text variable=\"citation-number\"/></layout></citation>"
              "<bibliography><layout><text variable=\"title\"/></layout></bibliography></style>");
    QVERIFY(QDir().mkpath(deeper));
    QVERIFY(QDir().mkpath(f.file(u"archive"_s)));
    QVERIFY(QDir().mkpath(outside));
    f.settings.setLibraryFolders({f.library, outside});
    Document *open = variation == u"flow" ? f.store.open(paper) : nullptr;
    if (variation == u"flow") QVERIFY(open);
    const QString id = f.store.documentIdFor(paper);
    QSignalSpy moved(&f.organizer, &FileOrganizer::moved);
    const bool together = variation == u"folder" || variation == u"rename";
    if (variation == u"folder") f.organizer.move(f.file(u"docs"_s), f.file(u"archive"_s));
    else if (variation == u"rename") f.organizer.rename(paper, u"revised.md"_s);
    else f.organizer.move(paper, variation == u"outside" ? outside : deeper);
    QString expected = original;
    if (!together) {
        QTRY_VERIFY(!f.organizer.busy());
        QVERIFY(!f.organizer.review().isEmpty());
        const QVariantMap review = f.organizer.review();
        const int references = variation == u"flow" ? 4 : variation == u"block" ? 3 : variation == u"bundled" ? 1 : 2;
        QCOMPARE(review.value(u"changes"_s).toInt(), references);
        if (variation == u"outside")
            QVERIFY(review.value(u"rows"_s).toList().first().toMap().value(u"after"_s).toString().contains(u"outside"));
        f.organizer.confirmMove(true);
        if (variation != u"outside") {
            for (const QString &name : {u"refs.bib"_s, u"other refs.bib"_s, u"third.bib"_s, u"style.csl"_s})
                expected.replace(name, u"../../"_s + name);
            // The suffix of the quoted path is also a substring of a longer path.
            expected.replace(u"other ../../refs.bib"_s, u"../../other refs.bib"_s);
        }
    }
    if (variation == u"outside") {
        QTRY_COMPARE(moved.count(), 1);
        QVERIFY(moved.last().at(3).toBool());
        QVERIFY(moved.last().at(1).toString().contains(u"paper.md"));
        QCOMPARE(readFile(movedPaper), original.toUtf8());
        f.organizer.undoMove(moved.last().at(2).toULongLong());
        QTRY_COMPARE(moved.count(), 2);
        QCOMPARE(readFile(paper), original.toUtf8());
        return;
    }
    QTRY_COMPARE(moved.count(), 1);
    if (open) {
        QCOMPARE(open->path(), movedPaper);
        f.save(open, expected.toUtf8());
    }
    QCOMPARE(readFile(movedPaper), expected.toUtf8());
    QCOMPARE(f.store.documentIdFor(movedPaper), id);
    md::Source source;
    source.path = movedPaper;
    source.libraryRoot = f.library;
    const auto reading = DocumentReading::read(expected, {}, source);
    const QString rendered = md::toHtml(reading.document, reading.htmlOptions());
    QCOMPARE(reading.document.bibliography.size(), 1u);
    QVERIFY2(rendered.contains(u"The Sample Book", Qt::CaseInsensitive), qPrintable(rendered));
    const QString resources = f.file(variation == u"folder" ? u"archive/docs"_s : u"docs"_s);
    QCOMPARE(reading.dialect.bibliography.first(), resources + u"/refs.bib"_s);
    QCOMPARE(reading.dialect.citationStyle, variation == u"bundled" ? u"ieee"_s : resources + u"/style.csl"_s);
    QVERIFY(std::none_of(reading.citations.problems.cbegin(), reading.citations.problems.cend(), [](const auto &problem) {
        return problem.code == u"unreadable-bibliography" || problem.code == u"missing-citation";
    }));
    f.organizer.undoMove(moved.first().at(2).toULongLong());
    QTRY_COMPARE(moved.count(), 2);
    if (open) f.save(open, original.toUtf8());
    QCOMPARE(readFile(paper), original.toUtf8());
    QCOMPARE(f.store.documentIdFor(paper), id);
}

void movingADocumentLeavesUnchangedCitationPathsAlone()
{
    LibraryMoves f;
    const QString paper = f.file(u"notes/paper.md"_s);
    const QString destination = f.file(u"archive"_s);
    const QString bibliography = f.root.filePath(u"resources/refs.bib"_s);
    const QByteArray original = u"---\nbibliography: ['%1', '~/refs.bib']\ncsl: ~/style.csl\n---\n\nSee [@doe].\n"_s
                                    .arg(bibliography).toUtf8();
    writeFile(paper, original);
    QVERIFY(QDir().mkpath(destination));
    QSignalSpy moved(&f.organizer, &FileOrganizer::moved);
    f.organizer.move(paper, destination);
    QTRY_VERIFY(!f.organizer.busy());
    QVERIFY(f.organizer.review().isEmpty());
    QCOMPARE(moved.count(), 1);
    QCOMPARE(readFile(destination + u"/paper.md"_s), original);
}

void linkedFoldersOutsideTheLibraryAreNotWalked()
{
    QTemporaryDir home;
    const QString library = home.filePath("Library"), outside = home.filePath("Garden"), link = library + "/Garden";
    QVERIFY(QDir().mkpath(library));
    Backend backend(AppPaths::under(home.filePath("app")));
    backend.start({});
    backend.settings()->setLibraryFolders({library});
    writeFile(outside + "/seeds.md", "Seeds");
    QVERIFY(QFile::link(outside, link));
    QVERIFY(QFile::link(outside + "/seeds.md", library + "/seeds.md"));
    QStringList files;
    LibraryWalk::walk({library}, [&](const LibraryWalk::Found &file) { files << file.path; return true; });
    QVERIFY(files.isEmpty());
    auto *model = backend.activeWindow()->library();
    const QModelIndex root = model->index(2, 0);
    model->setExpanded(root, true);
    QTRY_COMPARE(model->rowCount(root), 2);
    for (int row = 0; row < 2; ++row) {
        const auto at = model->index(row, 0, root);
        QVERIFY(at.data(model->roleNames().key("outsideLink")).toBool());
        QVERIFY(!model->hasChildren(at));
        QVERIFY(!model->canFetchMore(at));
    }
    model->setFilter("seeds");
    QTRY_VERIFY(!model->searching());
    QCOMPARE(model->matchCount(), 0);
    model->setFilter({});
    backend.settings()->setLibraryFolders({library, outside});
    const int linkRole = model->roleNames().key("outsideLink");
    QTRY_VERIFY(!model->index(0, 0, root).data(linkRole).toBool());
    backend.settings()->setLibraryFolders({library});
    QTRY_VERIFY(model->index(0, 0, root).data(linkRole).toBool());
    backend.openFromLibrary(library + "/seeds.md", true);
    QCOMPARE(backend.document()->path(), outside + "/seeds.md");
}

void linkUpdatesNeverRewriteFilesOutsideTheLibrary()
{
    LibraryMoves f;
    const QString other = f.root.filePath("Garden"), outside = other + "/garden.md";
    const QByteArray original = "[Notes](notes.md)\n";
    writeFile(outside, original);
    QVERIFY(QFile::link(outside, f.file("index.md")));
    writeFile(f.file("notes.md"), "Notes");
    f.organizer.rename(f.file("notes.md"), "journal");
    QTRY_VERIFY(!f.organizer.busy());
    QVERIFY(f.organizer.review().isEmpty());
    QVERIFY(QFile::exists(f.file("journal.md")));
    QCOMPARE(readFile(outside), original);

    // Removing a library while the move is under review withdraws the
    // permission to update links in it.
    f.settings.setLibraryFolders({f.library, other});
    writeFile(f.file("notes.md"), "Notes");
    f.organizer.rename(f.file("notes.md"), "daily");
    QTRY_VERIFY(!f.organizer.review().isEmpty());
    f.settings.setLibraryFolders({f.library});
    f.organizer.confirmMove(true);
    QTRY_VERIFY(!f.organizer.busy());
    QCOMPARE(readFile(outside), original);
    QVERIFY(QFile::exists(f.file("daily.md")));
}

void aLinkedFolderOutsideIsNotAMoveDestination()
{
    LibraryMoves f;
    const QString outside = f.root.path() + "/Garden", link = f.file("Garden");
    QVERIFY(QDir().mkpath(outside));
    QVERIFY(QFile::link(outside, link));
    f.organizer.listFolders();
    QTRY_VERIFY(!f.organizer.busy());
    for (const QVariant &row : f.organizer.folders()) QVERIFY(row.toMap().value("path") != link);
    writeFile(f.file("seeds.txt"), "Seeds");
    f.organizer.move(f.file("seeds.txt"), link);
    QTRY_VERIFY(!f.organizer.busy());
    QVERIFY(QFile::exists(f.file("seeds.txt")));
    QVERIFY(!QFile::exists(outside + "/seeds.txt"));
}

void renameAndTrashRefuseAPathThroughALinkOutside()
{
    LibraryMoves f;
    const QString outside = f.root.path() + "/Garden", link = f.file("Garden");
    writeFile(outside + "/seeds.txt", "Seeds");
    QVERIFY(QFile::link(outside, link));
    QVERIFY(!f.organizer.organizes(link + "/seeds.txt"));
    f.organizer.rename(link + "/seeds.txt", "flowers");
    f.organizer.askToTrash(link + "/seeds.txt");
    QTRY_VERIFY(!f.organizer.busy());
    QVERIFY(f.organizer.trashing().isEmpty());
    f.settings.setLibraryFolders({f.library, outside});
    f.organizer.askToTrash(link + "/seeds.txt");
    QVERIFY(!f.organizer.trashing().isEmpty());
    f.settings.setLibraryFolders({f.library});
    f.organizer.confirmTrash();
    QCOMPARE(readFile(outside + "/seeds.txt"), QByteArray("Seeds"));
}

void linksInsideTheLibraryStillWork()
{
    LibraryMoves f;
    const QString garden = f.root.path() + "/Garden";
    writeFile(garden + "/seeds.txt", "Seeds");
    QVERIFY(QFile::link(garden, f.file("plants")));
    f.settings.setLibraryFolders({f.library, garden});
    QStringList files;
    LibraryWalk::walk({f.library, garden}, [&](const LibraryWalk::Found &file) { files << file.path; return true; });
    QCOMPARE(files.size(), 1);
    QCOMPARE(canonical(files.first()), garden + "/seeds.txt");
    f.organizer.rename(f.file("plants/seeds.txt"), "flowers");
    QTRY_VERIFY(QFile::exists(garden + "/flowers.txt"));
}

void aNewSaveAsNameUsesTheUmask()
{
    LibraryMoves f;
    const QString original = f.file("original.md"), copy = f.file("copy.md");
    writeFile(original, "Text");
    QVERIFY(::chmod(QFile::encodeName(original), 0664) == 0);
    auto *doc = f.store.open(original);
    const mode_t previous = ::umask(0077);
    const auto restore = qScopeGuard([&] { ::umask(previous); });
    f.store.saveAs(doc, copy);
    f.store.waitForSaves();
    struct stat disk {};
    QVERIFY(::stat(QFile::encodeName(copy), &disk) == 0);
    QCOMPARE(disk.st_mode & 0777, mode_t(0600));
}

void closingAMissingLinkReleasesItsName()
{
    LibraryMoves f;
    const QString name = f.file("chapter.md");
    QVERIFY(QFile::link(f.file("missing.md"), name));
    auto *placeholder = f.store.openMissingLink(name);
    f.store.retain(placeholder);
    f.store.release(placeholder);
    QVERIFY(!f.store.documents().contains(placeholder));
    auto *doc = f.store.newDraft();
    doc->buffer()->replace({0, 0}, "Chapter");
    f.store.saveAs(doc, name);
    f.store.answerSaveLink(doc, name, f.file("missing.md"), DocumentStore::SaveLinkChoice::Replace);
    f.store.waitForSaves();
    QCOMPARE(readFile(name), QByteArray("Chapter"));
}

void aLinkEntryCanBeRenamedAndTrashed()
{
    LibraryMoves f;
    for (const bool dangling : {false, true}) {
        const QString outside = f.root.filePath(dangling ? "missing.md" : "outside.md");
        if (!dangling) writeFile(outside, "Other");
        const QString link = f.file("link.md"), renamed = f.file("renamed.md");
        QVERIFY(QFile::link(outside, link));
        QVERIFY(f.organizer.organizes(link));
        f.organizer.rename(link, "renamed");
        QTRY_VERIFY(QFileInfo(renamed).isSymLink());
        f.organizer.askToTrash(renamed);
        QCOMPARE(f.organizer.trashing().value("path").toString(), renamed);
        f.organizer.confirmTrash();
        QVERIFY(!QFileInfo(renamed).isSymLink());
        if (!dangling) QCOMPARE(readFile(outside), QByteArray("Other"));
    }
}

void aLibraryUnderALinkScansItsDocuments()
{
    LibraryMoves f;
    const QString alias = f.root.filePath("shelf");
    QVERIFY(QFile::link(f.library, alias));
    f.settings.setLibraryFolders({alias});
    writeFile(f.file("chapter.md"), "Chapter");
    QStringList files;
    LibraryWalk::walk(f.settings.libraryFolders(), [&](const LibraryWalk::Found &entry) {
        files << canonical(entry.path);
        return true;
    });
    QCOMPARE(files, QStringList{f.file("chapter.md")});
    f.organizer.newFolder(alias, "Drafts");
    QVERIFY(QFileInfo(f.file("Drafts")).isDir());
    f.settings.setLibraryFolders({alias + "/later/shelf"});
    writeFile(f.file("later/shelf/page.md"), "Page");
    QCOMPARE(f.settings.libraryFolders(), QStringList{f.file("later/shelf")});

    const QString home = f.root.filePath("home"), homeAlias = f.root.filePath("home-link");
    QVERIFY(QDir().mkpath(home));
    QVERIFY(QFile::link(home, homeAlias));
    const AppPaths paths = AppPaths::under(homeAlias);
    DocumentStore store(paths);
    Settings settings(paths.settingsFile());
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    writeFile(DocumentStore::draftFile(paths, id), "Draft with a bookmark");
    FolderSearch search(paths, &store, &settings);
    search.setPattern("bookmark");
    search.start();
    QTRY_VERIFY(search.finished());
    QCOMPARE(search.matchCount(), 1);
}

void linkUpdatesKeepOtherHardLinksIntact()
{
    for (const bool live : {false, true}) {
        LibraryMoves f;
        const QString index = f.file("index.md"), outside = f.root.filePath("other.md");
        const QByteArray text("[Notes](notes.md)\n");
        writeFile(index, text);
        QVERIFY(::link(QFile::encodeName(index), QFile::encodeName(outside)) == 0);
        writeFile(f.file("notes.md"), "Notes");
        auto *doc = live ? f.store.open(index) : nullptr;
        f.organizer.rename(f.file("notes.md"), "journal");
        QTRY_VERIFY(!f.organizer.review().isEmpty());
        f.organizer.confirmMove(true);
        QTRY_VERIFY(!f.organizer.busy());
        if (live) {
            f.store.save(doc);
            f.store.waitForSaves();
        }
        QCOMPARE(readFile(index), QByteArray("[Notes](journal.md)\n"));
        QCOMPARE(readFile(outside), text);
    }
}

void openingAMovingDocumentSharesItsBuffer()
{
    LibraryMoves f;
    const auto folder = f.file("books"), name = f.file("page.txt"), target = folder + "/page.txt";
    QVERIFY(QDir().mkpath(folder));
    writeFile(name, "Notes");
    auto *doc = f.store.open(name);
    QSemaphore published, finish;
    DiskFile::setAfterNaming([&](const QString &path) {
        if (path == target) { published.release(); finish.acquire(); }
    });
    const auto release = qScopeGuard([&] {
        finish.release();
        f.organizer.waitForWork();
        DiskFile::setAfterNaming({});
    });
    f.organizer.setCrossDeviceForTests(true);
    f.organizer.move(name, folder);
    QTRY_VERIFY(published.available() > 0);
    auto *opened = f.store.open(target);
    QCOMPARE(opened, doc);
    doc->buffer()->replace({0, 5}, "Edited");
    f.store.save(doc);
    finish.release();
    QTRY_VERIFY(!f.organizer.busy());
    f.store.waitForSaves();
    QCOMPARE(doc->path(), target);
    QCOMPARE(readFile(target), QByteArray("Edited"));
    QVERIFY(!QFileInfo::exists(name));
}

void replacingALinkKeepsAConcurrentFile()
{
    LibraryMoves f;
    const QString name = f.file("notes.md"), other = f.file("other.md");
    QVERIFY(QFile::link(f.file("missing.md"), name));
    const auto approved = DiskFile::nameLink(name);
    QVERIFY(approved);
    writeFile(other, "Other revision");
    DiskFile::setBeforeClaim([&](const QString &path) {
        if (path == name) ::rename(QFile::encodeName(other), QFile::encodeName(name));
    });
    const auto clear = qScopeGuard([] { DiskFile::setBeforeClaim({}); });
    QCOMPARE(DiskFile::putAt(name, "Notes", 0, &*approved).status, SaveStatus::Conflict);
    QCOMPARE(readFile(name), QByteArray("Other revision"));
}

void aCopyCanUseAnExclusiveFileOnALinklessVolume()
{
    LibraryMoves f;
    const QString name = f.file("source.txt"), target = f.file("copy.txt");
    const QByteArray bytes(135000, 'n');
    writeFile(name, bytes);
    QFile source(name);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const int directory = ::open(QFile::encodeName(f.library), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(directory >= 0);
    bool direct = false;
    DiskFile::setRenameWithoutReplacing(false);
    DiskFile::setLinking(false);
    DiskFile::setDirectWriteFailure([&](const QString &path) { direct = (path == target); return 0; });
    const auto clear = qScopeGuard([&] {
        ::close(directory);
        DiskFile::setDirectWriteFailure({});
        DiskFile::setLinking(true);
        DiskFile::setRenameWithoutReplacing(true);
    });
    QCOMPARE(DiskFile::copyAt(directory, f.library, "copy.txt", source.handle(), bytes.size(), 0600).status, SaveStatus::Saved);
    QVERIFY(direct);
    QCOMPARE(readFile(target), bytes);
}

void aMoveRemovesOnlyTheRevisionsItClaimed()
{
    for (const bool changeDestination : {false, true}) {
        LibraryMoves f;
        const auto folder = f.file("books"), source = f.file("page.bin"), target = folder + "/page.bin";
        QVERIFY(QDir().mkpath(folder));
        writeFile(source, "Original");
        const QString newer = f.file("new.bin");
        writeFile(newer, "New source");
        const QString other = f.file("another.bin");
        writeFile(other, "New destination");
        DiskFile::setBeforeClaim([&](const QString &path) {
            if (path == source) ::rename(QFile::encodeName(newer), QFile::encodeName(source));
            if (changeDestination && path == target) ::rename(QFile::encodeName(other), QFile::encodeName(target));
        });
        const auto clear = qScopeGuard([] { DiskFile::setBeforeClaim({}); });
        f.organizer.setCrossDeviceForTests(true);
        f.organizer.move(source, folder);
        QTRY_VERIFY(!f.organizer.busy());
        QVERIFY(QFileInfo::exists(source));
        QCOMPARE(readFile(source), QByteArray("New source"));
        if (changeDestination) QCOMPARE(readFile(target), QByteArray("New destination"));
        else QVERIFY(!QFileInfo::exists(target));
    }
}

void aFolderMoveCanExchangeAnExclusiveName()
{
    LibraryMoves f;
    const QString folder = f.file("drafts"), result = f.file("archive");
    writeFile(folder + "/chapter.md", "Notes");
    struct stat before {};
    QVERIFY(::stat(QFile::encodeName(folder), &before) == 0);
    DiskFile::setRenameWithoutReplacing(false);
    const auto restore = qScopeGuard([] { DiskFile::setRenameWithoutReplacing(true); });
    f.organizer.rename(folder, "archive");
    QTRY_VERIFY(!f.organizer.busy());
    QCOMPARE(readFile(result + "/chapter.md"), QByteArray("Notes"));
    struct stat after {};
    QVERIFY(::stat(QFile::encodeName(result), &after) == 0);
    QCOMPARE(after.st_ino, before.st_ino);
    QVERIFY(!QFileInfo::exists(folder));
    QVERIFY(QDir().mkdir(folder));
    f.organizer.rename(result, "drafts");
    QTRY_VERIFY(!f.organizer.busy());
    QCOMPARE(readFile(result + "/chapter.md"), QByteArray("Notes"));
    QVERIFY(QDir(folder).isEmpty());
    QVERIFY(QDir().rmdir(folder));
    const auto replacement = f.file("letter.txt");
    writeFile(replacement, "Another writer");
    DiskFile::setBeforeClaim([&](const QString &path) {
        if (path == folder) ::rename(QFile::encodeName(replacement), QFile::encodeName(folder));
    });
    const auto clear = qScopeGuard([] { DiskFile::setBeforeClaim({}); });
    f.organizer.rename(result, "drafts");
    QTRY_VERIFY(!f.organizer.busy());
    QCOMPARE(readFile(folder), QByteArray("Another writer"));
    QCOMPARE(readFile(result + "/chapter.md"), QByteArray("Notes"));
}

void saveAsOntoADanglingLinkAsks()
{
    LibraryMoves f;
    const QString name = f.file("Notes.md"), target = f.file("Missing.md");
    QVERIFY(QFile::link(target, name));
    Document *draft = f.store.newDraft();
    draft->buffer()->replace({0, 0}, "Notes");
    auto *store = &f.store;
    using Choice = DocumentStore::SaveLinkChoice;
    QSignalSpy asked(store, &DocumentStore::saveLinkRequested);
    store->saveAs(draft, name);
    QCOMPARE(asked.size(), 1);
    QCOMPARE(asked.last().at(2).toString(), target);
    QVERIFY(!asked.last().at(3).toBool());
    store->answerSaveLink(draft, name, target, Choice::Cancel);
    store->waitForSaves();
    QVERIFY(!QFileInfo::exists(target));
    QVERIFY(QFileInfo(name).isSymLink());
    QVERIFY(draft->isDraft());
    store->saveAs(draft, name);
    store->answerSaveLink(draft, name, target, Choice::Replace);
    store->waitForSaves();
    QCOMPARE(draft->path(), name);
    QCOMPARE(readFile(name), QByteArray("Notes"));
    QVERIFY(!QFileInfo(name).isSymLink());
    QVERIFY(!QFileInfo::exists(target));

    const QString link = f.file("Other.md");
    writeFile(target, "Old text");
    QVERIFY(QFile::link(target, link));
    store->saveAs(draft, link);
    QCOMPARE(asked.size(), 3);
    QVERIFY(asked.last().at(3).toBool());
    QCOMPARE(readFile(target), QByteArray("Old text"));
    store->answerSaveLink(draft, link, target, Choice::Follow);
    store->waitForSaves();
    QCOMPARE(draft->path(), target);
    QCOMPARE(readFile(target), QByteArray("Notes"));
    QVERIFY(QFileInfo(link).isSymLink());
}

void savingAtALinkOffersTheAvailableChoices()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    NamePicker picker;
    shell.backend->setFilePicker(&picker);
    const QString file = shell.root.filePath("notes%2.md"), target = shell.root.filePath("journal%1.md");
    QVERIFY(QFile::link(target, file));
    auto *window = shell.window.get();
    activate(window);
    typeText(window, "Notes");
    const auto choose = [&] {
        shell.backend->saveAs();
        emit picker.saveSelected(QUrl::fromLocalFile(file));
    };
    choose();
    QObject *dialog = shell.object("saveLinkDialog");
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->property("opened").toBool());
    const QString question = dialog->findChild<QObject *>("saveLinkQuestion")->property("text").toString();
    QVERIFY(question.contains(target));
    QCOMPARE(question.count(file), 2);
    QCOMPARE(window->activeFocusItem()->objectName(), QString("saveLinkCancel"));
    QVERIFY(!shell.item("saveLinkFollow")->isVisible());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(!QFileInfo::exists(target));
    writeFile(target, "Original");
    choose();
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(shell.item("saveLinkFollow")->isVisible());
    QVERIFY(QMetaObject::invokeMethod(shell.item("saveLinkFollow"), "clicked"));
    QTRY_COMPARE(shell.document()->path(), target);
    QCOMPARE(readFile(target), QByteArray("Notes"));
    QVERIFY(QFileInfo(file).isSymLink());
}

void aDanglingLinkNamedAtLaunchCreatesNothing()
{
    QTemporaryDir root;
    const QString name = root.filePath("Notes.md"), target = root.filePath("Missing.md");
    QVERIFY(QFile::link(target, name));
    Backend backend(AppPaths::under(root.path()));
    backend.start({name});
    QVERIFY(backend.document());
    QCOMPARE(backend.document()->state(), Document::State::ReadOnly);
    QVERIFY(backend.document()->property("missingLink").toBool());
    QVERIFY((backend.problem() + backend.problemDetail()).contains(target));
    QVERIFY(!QFileInfo::exists(target));
    QVERIFY(QFileInfo(name).isSymLink());
}

void savingADocumentOpenedThroughALinkStillWritesItsTarget()
{
    LibraryMoves f;
    const QString target = f.file("Notes.md"), name = f.file("Current.md");
    writeFile(target, "Before");
    QVERIFY(QFile::link(target, name));
    Document *document = f.store.open(f.file("Current.md"));
    document->buffer()->replace({0, 6}, "After");
    f.store.save(document);
    f.store.waitForSaves();
    QVERIFY(QFileInfo(name).isSymLink());
    QCOMPARE(readFile(target), QByteArray("After"));
}

void bibliographyReadsStayWithinTheDocumentsFolders()
{
    LibraryMoves f;
    const QString path = f.file("sources.bib"), outside = f.root.path() + "/sources.bib";
    writeFile(outside, "@book{garden,title={Garden}}");
    QVERIFY(QFile::link(outside, path));
    const auto reach = DocumentCitations::Reach::of({}, f.library);
    const auto read = BibliographyDatabase::fromFile(path, BibliographyDatabase::Format::Auto, &reach.folders);
    QVERIFY(!read.isValid());
    QVERIFY(!read.find("garden"));
    QCOMPARE(read.diagnostics().first().kind, BibliographyDiagnostic::Kind::Unreadable);
    const QString style = f.file("style.csl");
    QVERIFY(QFile::link(outside, style));
    QString reason;
    QVERIFY(!CslCatalog::styleFromFile(style, &reason, &reach.folders));
    QVERIFY(reason.contains("outside"));
    Dialect::Choice choice;
    choice.options.citations = true;
    choice.bibliography = {path};
    choice.bibliographyFrom = "document";
    QVERIFY(DocumentCitations::prepare(choice, true, reach).keys().contains("garden"));
    choice.bibliographyFrom = "front-matter";
    QVERIFY(DocumentCitations::prepare(choice, true, reach).keys().isEmpty());
}

void aCrossDeviceMoveOfALargeFileIsRefused()
{
    LibraryMoves f;
    QTemporaryDir destination;
    QVERIFY(destination.isValid());
    f.settings.setLibraryFolders({f.library, destination.path()});
    f.organizer.setCrossDeviceForTests(true);
    QFile file(f.file("notes.txt"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(SafeRead::DocumentBytes + 1));
    file.close();
    QSignalSpy refused(&f.organizer, &FileOrganizer::problem);
    f.organizer.move(file.fileName(), destination.path());
    QTRY_VERIFY(!f.organizer.busy());
    QCOMPARE(refused.size(), 1);
    QVERIFY(file.exists());
    QVERIFY(!QFileInfo::exists(destination.filePath("notes.txt")));
}

void oneBusyDocumentDoesNotPruneOthersHistory()
{
    QTemporaryDir root;
    QDateTime now(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(AppPaths::under(root.path()), HistoryLimits{100, 30, 600}, [&] { return now; });
    const QString quiet = newDocumentId(), busy = newDocumentId();
    for (int i = 0; i < 12; ++i) {
        now = now.addSecs(600);
        QVERIFY(!history.capture(i < 3 ? quiet : busy, QByteArray(100, char('a' + i))).isEmpty());
    }
    QCOMPARE(history.versions(quiet).size(), 3);
    QCOMPARE(history.versions(busy).size(), 3);
    QCOMPARE(history.usage().bytes, 600);

    // Two documents exceed their share. The one using the most bytes
    // gives up its oldest version even though it has fewer versions.
    History mixed(AppPaths::under(root.filePath("mixed")), HistoryLimits{100, 30, 1050}, [&] { return now; });
    const QString large = newDocumentId();
    mixed.capture(large, QByteArray(250, 'x'));
    now = now.addSecs(600);
    mixed.capture(large, QByteArray(300, 'y'));
    mixed.capture(quiet, QByteArray(110, 'q'));
    for (int i = 0; i < 4; ++i) {
        now = now.addSecs(600);
        mixed.capture(busy, QByteArray(100, char('a' + i)));
    }
    QCOMPARE(mixed.versions(large).size(), 1);
    QCOMPARE(mixed.versions(large).first().bytes, 300);
    QCOMPARE(mixed.versions(busy).size(), 4);
    QCOMPARE(mixed.versions(quiet).size(), 1);
}

void outsideReloadsShareTheHistoryInterval()
{
    LibraryMoves f;
    writeFile(f.file("notes.md"), "Opened");
    Document *doc = f.store.open(f.file("notes.md"));
    auto *store = &f.store;
    doc->buffer()->replace({0, 0}, "First ");
    store->save(doc);
    QTRY_COMPARE(doc->state(), Document::State::Clean);
    const auto count = store->history()->versions(doc->id()).size();
    writeFile(doc->path(), "Reloaded");
    QTRY_COMPARE(doc->buffer()->text(), QString("Reloaded"));
    doc->buffer()->replace({0, 0}, "Second ");
    store->save(doc);
    QTRY_COMPARE(doc->state(), Document::State::Clean);
    QCOMPARE(store->history()->versions(doc->id()).size(), count);
}

void withoutARuntimeFolderParchUsesItsOwn()
{
    QTemporaryDir root;
    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR"), state = qgetenv("XDG_STATE_HOME");
    const auto restore = qScopeGuard([&] {
        if (runtime.isNull()) qunsetenv("XDG_RUNTIME_DIR");
        else qputenv("XDG_RUNTIME_DIR", runtime);
        if (state.isNull()) qunsetenv("XDG_STATE_HOME");
        else qputenv("XDG_STATE_HOME", state);
    });
    const bool testPaths = QStandardPaths::isTestModeEnabled();
    const auto restoreLocations = qScopeGuard([testPaths] { QStandardPaths::setTestModeEnabled(testPaths); });
    QStandardPaths::setTestModeEnabled(false);
    qunsetenv("XDG_RUNTIME_DIR");
    qputenv("XDG_STATE_HOME", QFile::encodeName(root.path()));
    auto paths = AppPaths::standard();
    QVERIFY(paths.singleInstance);
    auto longest = paths;
    longest.runtime = QString(107 - QStringLiteral("/parch.sock").size(), 'a');
    QVERIFY(longest.socketPathFits());
    longest.runtime += u'b';
    QVERIFY(!longest.socketPathFits());
    QVERIFY(paths.runtime.startsWith(paths.state + '/'));
    struct stat mode {};
    QVERIFY(::lstat(QFile::encodeName(paths.runtime).constData(), &mode) == 0);
    QCOMPARE(mode.st_mode & 0777U, 0700U);
    qputenv("XDG_STATE_HOME", QFile::encodeName(root.filePath(QString(110, 's'))));
    paths = AppPaths::standard();
    QVERIFY(!paths.singleInstance);
    QCOMPARE(paths.runtime, AppPaths::standard().runtime);
    QVERIFY(::lstat(QFile::encodeName(paths.runtime).constData(), &mode) == 0);
    QCOMPARE(mode.st_mode & 0777U, 0700U);
    AppService service(paths);
    QVERIFY2(service.listen(), qPrintable(service.error()));
    QVERIFY(!AppService::forward(paths, {}));
    QVERIFY(!QFileInfo::exists(AppService::socketPath(paths)));
}

} // namespace LibraryTests
