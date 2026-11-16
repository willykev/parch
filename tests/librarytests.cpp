#include "librarytests.h"

#include "apppaths.h"
#include "autosave.h"
#include "backend.h"
#include "documentstore.h"
#include "encodings.h"
#include "filepicker.h"
#include "foldersearch.h"
#include "history.h"
#include "librarymodel.h"
#include "recentfiles.h"
#include "rendertesting.h"
#include "session.h"
#include "settings.h"
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
#include <QSignalSpy>
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

} // namespace LibraryTests
