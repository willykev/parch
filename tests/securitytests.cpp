#include "securitytests.h"

#include "attachments.h"
#include "backend.h"
#include "dialectchoice.h"
#include "document.h"
#include "documentcheck.h"
#include "documentcitations.h"
#include "documentstore.h"
#include "editorbinding.h"
#include "sourcebuffer.h"
#include "history.h"
#include "exporter.h"
#include "exports.h"
#include "filepicker.h"
#include "previewresources.h"
#include "privatefiles.h"
#include "remotefetcher.h"
#include "renderhelpers.h"
#include "session.h"
#include "rendertesting.h"
#include "tabmodel.h"
#include "testfiles.h"
#include "windowcontext.h"
#include "windowtesting.h"

#include <QAccessible>
#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QHostAddress>
#include <QImage>
#include <QNetworkProxy>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QClipboard>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMimeData>
#include <QAbstractTextDocumentLayout>
#include <QQuickTextDocument>
#include <QTextDocument>
#include <QTextOption>
#include <memory>
#include <sys/stat.h>

namespace SecurityTests {
namespace {

// A web server on 127.0.0.1 for the fetcher to reach, directly or as its
// proxy: it answers each path as the test says, and keeps what it was asked.
class PictureServer : public QObject {
public:
    struct Answer {
        int status = 200;
        QByteArray type;
        QByteArray body;
        QByteArray location;
        // Sends the body a byte at a time, this far apart.
        int dripMs = 0;
    };

    PictureServer()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                ++connections;
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { serve(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    bool listening() const { return m_server.isListening(); }
    quint16 port() const { return m_server.serverPort(); }
    void answer(const QByteArray &path, const Answer &answer) { m_answers.insert(path, answer); }

    // What was asked for, as the request named it: a path, or a whole
    // address when the server is the fetcher's proxy.
    QList<QByteArray> asked;
    QList<QByteArray> hosts;
    int connections = 0;

private:
    void serve(QTcpSocket *socket)
    {
        QByteArray &pending = m_pending[socket];
        pending += socket->readAll();
        if (!pending.contains("\r\n\r\n")) return;
        const QList<QByteArray> words = pending.left(pending.indexOf("\r\n")).split(' ');
        for (const QByteArray &line : pending.split('\n'))
            if (line.toLower().startsWith("host:")) hosts << line.mid(5).trimmed();
        m_pending.remove(socket);
        const QByteArray target = words.value(1);
        asked << target;
        const QByteArray path = target.startsWith("http") ? QUrl(QString::fromLatin1(target)).path().toLatin1() : target;
        const Answer answer = m_answers.value(path, Answer{404, "text/plain", "not here", {}, 0});
        QByteArray head = "HTTP/1.1 " + QByteArray::number(answer.status) + " Answer\r\n";
        if (!answer.type.isEmpty()) head += "Content-Type: " + answer.type + "\r\n";
        if (!answer.location.isEmpty()) head += "Location: " + answer.location + "\r\n";
        head += "Content-Length: " + QByteArray::number(answer.body.size()) + "\r\nConnection: close\r\n\r\n";
        socket->write(head);
        if (answer.dripMs <= 0) {
            socket->write(answer.body);
            socket->disconnectFromHost();
            return;
        }
        auto *timer = new QTimer(socket);
        auto sent = std::make_shared<qsizetype>(0);
        connect(timer, &QTimer::timeout, socket, [socket, timer, sent, body = answer.body] {
            if (*sent >= body.size()) {
                timer->stop();
                socket->disconnectFromHost();
                return;
            }
            socket->write(body.mid((*sent)++, 1));
        });
        timer->start(answer.dripMs);
    }

    QTcpServer m_server;
    QHash<QByteArray, Answer> m_answers;
    QHash<QTcpSocket *, QByteArray> m_pending;
};

struct Fetched {
    QByteArray data;
    QString error;
    bool done = false;
};

Fetched fetchNow(NetworkFetcher &fetcher, const QString &address, int timeoutMs = 10000)
{
    QObject context;
    auto result = std::make_shared<Fetched>();
    fetcher.fetch(QUrl(address), &context, [result](const QByteArray &data, const QString &error) {
        *result = {data, error, true};
    });
    QTest::qWaitFor([&] { return result->done; }, timeoutMs);
    return *result;
}

QByteArray pngBytes()
{
    QImage image(6, 4, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

// A file dialog that counts what it was asked, and answers nothing.
class CountingPicker : public FilePicker {
public:
    int saves = 0;
    void openDocuments() override {}
    void openFolder() override {}
    void saveDocument(const QUrl &) override {}
    void saveFile(const QString &, const QUrl &, const QString &, const QStringList &) override { ++saves; }
};

} // namespace

void exportsAskBeforeReplacingALink()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = QFileInfo(files.path()).canonicalFilePath();
    const QString path = root + QStringLiteral("/repo/notes.md");
    writeFile(path, "# Notes\n\nWords.\n");
    writeFile(root + QStringLiteral("/victim_rc"), "kept\n");
    const QString link = root + QStringLiteral("/repo/notes.html");
    QVERIFY(QFile::link(QStringLiteral("../victim_rc"), link));
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    CountingPicker picker;
    shell.backend->setFilePicker(&picker);
    Exports *exports = shell.backend->exports();
    exports->setRenderOptions(renderOptions(HelperSet::in(root + QStringLiteral("/no-helpers")), root + QStringLiteral("/cache")));
    Document *document = shell.document();
    QObject *dialog = shell.object(QStringLiteral("exportLinkDialog"));
    QVERIFY(dialog);

    // An export to a link asks first, saying where the link leads, and
    // writes nothing meanwhile.
    exports->exportTo(document, QStringLiteral("html"), {}, link);
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(!exports->busy());
    const QString said = dialog->findChild<QObject *>(QStringLiteral("exportLinkQuestion"))->property("text").toString();
    QVERIFY2(said.contains(QStringLiteral("notes.html")) && said.contains(root + QStringLiteral("/victim_rc")), qPrintable(said));
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    QCOMPARE(window->activeFocusItem()->objectName(), QStringLiteral("exportLinkCancelChoice"));
    // Escape leaves everything as it was.
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QTest::qWait(100);
    QVERIFY(!exports->busy());
    QVERIFY(QFileInfo(link).isSymLink());
    QCOMPARE(readFile(root + QStringLiteral("/victim_rc")), QByteArray("kept\n"));

    // Export as… asks for another file.
    exports->exportTo(document, QStringLiteral("html"), {}, link);
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog->findChild<QObject *>(QStringLiteral("exportLinkElsewhereChoice")), "clicked"));
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QTRY_COMPARE(picker.saves, 1);
    QVERIFY(QFileInfo(link).isSymLink());

    // Replacing the link, once the writer says so, replaces the link
    // itself with the export; what it led to is left as it was.
    exports->exportTo(document, QStringLiteral("html"), {}, link);
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog->findChild<QObject *>(QStringLiteral("exportLinkReplaceChoice")), "clicked"));
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo(link).isSymLink() && !exports->busy(), 30000);
    QVERIFY(readFile(link).startsWith("<!DOCTYPE html>"));
    QCOMPARE(readFile(root + QStringLiteral("/victim_rc")), QByteArray("kept\n"));

    // Export again checks again, since a link may have taken the file's place.
    QVERIFY(QFile::remove(link));
    QVERIFY(QFile::link(QStringLiteral("../victim_rc"), link));
    QVERIFY(exports->exportAgain(document));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QVERIFY(QFileInfo(link).isSymLink());
    QCOMPARE(readFile(root + QStringLiteral("/victim_rc")), QByteArray("kept\n"));
}

namespace {

// What under the folders is open to the group or to others: each folder or
// file, with its permission bits.
QStringList openToOthers(const QStringList &folders)
{
    QStringList open;
    for (const QString &folder : folders) {
        QStringList paths{folder};
        QDirIterator it(folder, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) paths << it.next();
        for (const QString &path : paths) {
            struct stat status;
            if (::lstat(QFile::encodeName(path).constData(), &status) != 0 || S_ISLNK(status.st_mode)) continue;
            if ((status.st_mode & 077) != 0) open << QStringLiteral("%1 %2").arg(path).arg(status.st_mode & 0777, 0, 8);
        }
    }
    return open;
}

} // namespace

void parchKeepsItsFilesToItself()
{
    const mode_t umask = ::umask(022);
    const auto restore = qScopeGuard([umask] { ::umask(umask); });
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString root = folder.filePath(QStringLiteral("app"));
    const AppPaths paths = AppPaths::under(root);
    // What an earlier version left open to everyone.
    const QString earlier = paths.history() + QStringLiteral("/00000000-0000-4000-8000-000000000001");
    writeFile(earlier + QStringLiteral("/stray"), "an old copy\n");
    writeFile(paths.settingsFile(), "{}");
    writeFile(paths.personalDictionaryFile(), "parch\n");
    for (const QString &each : {paths.data, paths.history(), earlier, paths.config})
        QVERIFY(QFile::setPermissions(each, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                                | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                                | QFileDevice::ExeOther));
    QVERIFY(!openToOthers(PrivateFiles::folders(paths)).isEmpty());
    // A link among them is never followed.
    writeFile(folder.filePath(QStringLiteral("elsewhere.txt")), "not parch's\n");
    QVERIFY(QFile::setPermissions(folder.filePath(QStringLiteral("elsewhere.txt")),
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther));
    QVERIFY(QFile::link(folder.filePath(QStringLiteral("elsewhere.txt")), paths.data + QStringLiteral("/link.txt")));

    ShellFixture shell({.root = root});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    // Text typed in a draft goes to its file, to recovery, to history and
    // to the session, and a setting to the settings.
    Document *draft = shell.document();
    QVERIFY(draft);
    draft->buffer()->replace({0, 0}, QStringLiteral("A private journal.\n"));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(DocumentStore::draftFile(paths, draft->id())), 10000);
    QVERIFY(!shell.backend->history()->capture(draft->id(), "A private journal.\n").isEmpty());
    shell.settings()->setScrollSync(false);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(paths.sessionFile()), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!QDir(paths.recovery()).entryList(QDir::Files).isEmpty(), 10000);
    // Whatever the umask, every folder of parch's own and every file in them
    // is its owner's alone, the earlier ones too, quietly.
    QTRY_VERIFY2_WITH_TIMEOUT(openToOthers(PrivateFiles::folders(paths)).isEmpty(),
                              qPrintable(openToOthers(PrivateFiles::folders(paths)).join(u'\n')), 10000);
    // What is written from now on is private as it is written.
    QVERIFY(!shell.backend->history()->capture(draft->id(), "A second entry.\n").isEmpty());
    shell.settings()->setScrollSync(true);
    QVERIFY(Session::write(paths, "{}"));
    QVERIFY2(openToOthers(PrivateFiles::folders(paths)).isEmpty(), qPrintable(openToOthers(PrivateFiles::folders(paths)).join(u'\n')));
    QCOMPARE(readFile(earlier + QStringLiteral("/stray")), QByteArray("an old copy\n"));
    QVERIFY(QFileInfo(folder.filePath(QStringLiteral("elsewhere.txt"))).permissions() & QFileDevice::ReadOther);
    QVERIFY(shell.backend->problem().isEmpty() || shell.backend->notice());
    // The writer's own documents keep the usual permissions.
    const QString named = folder.filePath(QStringLiteral("named.md"));
    writeFile(named, "# Named\n");
    QVERIFY(QFileInfo(named).permissions() & QFileDevice::ReadOther);
}

void helpersRunWithinLimits()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    // A stand-in helper that says what it may use.
    const QString program = folder.filePath(QStringLiteral("limits"));
    writeFile(program, "#!/bin/sh\nexec cat /proc/self/limits\n");
    QVERIFY(QFile::setPermissions(program, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    const RenderLimits limits;
    HelperProcess process({program, {}, {}, {}, {}}, limits, folder.filePath(QStringLiteral("tmp")));
    HelperOutcome outcome;
    bool done = false;
    QObject::connect(&process, &HelperProcess::finished, &process, [&](const HelperOutcome &result) {
        outcome = result;
        done = true;
    });
    process.start();
    QVERIFY(QTest::qWaitFor([&] { return done; }, 30000));
    QVERIFY2(outcome.succeeded(), outcome.errors.constData());
    // Each limit, soft and hard alike, from /proc/self/limits.
    const auto limit = [&](const char *name) {
        for (const QByteArray &line : outcome.output.split('\n'))
            if (line.startsWith(name)) {
                const QList<QByteArray> words = line.mid(qsizetype(qstrlen(name))).simplified().split(' ');
                return words.value(0) == words.value(1) ? words.value(0) : words.value(0) + "/" + words.value(1);
            }
        return QByteArray("missing");
    };
    QCOMPARE(limit("Max address space"), QByteArray::number(limits.maxMemoryBytes));
    QCOMPARE(limit("Max cpu time"), QByteArray::number(limits.maxCpuSeconds));
    QCOMPARE(limit("Max core file size"), QByteArray("0"));
    QCOMPARE(limit("Max file size"), QByteArray::number(limits.maxOutputBytes));
    QCOMPARE(limits.maxMemoryBytes, qint64(2) << 30);
    QCOMPARE(limits.maxCpuSeconds, 30);
    // Cut off from the network wherever the kernel allows it, which parch
    // can tell beforehand.
    QCOMPARE(outcome.networkIsolated, HelperProcess::canIsolate());
}

void aLinkedAssetsFolderIsNotFollowed()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString document = folder.filePath(QStringLiteral("repo"));
    const QString elsewhere = folder.filePath(QStringLiteral("autostart"));
    QVERIFY(QDir().mkpath(document));
    QVERIFY(QDir().mkpath(elsewhere));
    writeFile(folder.filePath(QStringLiteral("dropped.desktop")), "[Desktop Entry]\nExec=true\n");
    const QDate today(2026, 11, 16);
    // An assets folder that is a link, to a folder outside the document's
    // or to one that is not there, is never followed: what would be pasted
    // or dropped is refused, saying why, and nothing lands where it leads.
    for (const QString &target : {QStringLiteral("../autostart"), elsewhere, QStringLiteral("../missing")}) {
        const QString assets = document + QStringLiteral("/assets");
        QFile::remove(assets);
        QVERIFY(QFile::link(target, assets));
        const Attachments::Placed pasted = Attachments::savePasted(document, today, QStringLiteral("png"), "picture");
        QVERIFY2(pasted.relative.isEmpty(), qPrintable(pasted.relative));
        QVERIFY2(pasted.error.contains(QStringLiteral("is a link")), qPrintable(pasted.error));
        const Attachments::Placed dropped = Attachments::copyIn(document, folder.filePath(QStringLiteral("dropped.desktop")));
        QVERIFY(dropped.relative.isEmpty());
        QVERIFY2(dropped.error.contains(QStringLiteral("is a link")), qPrintable(dropped.error));
        QVERIFY(QDir(elsewhere).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot));
        QVERIFY(!QFileInfo::exists(folder.filePath(QStringLiteral("missing"))));
        QVERIFY(QFileInfo(assets).isSymLink());
    }
    // A real folder takes them, made when it is not there.
    QVERIFY(QFile::remove(document + QStringLiteral("/assets")));
    const Attachments::Placed pasted = Attachments::savePasted(document, today, QStringLiteral("png"), "picture");
    QCOMPARE(pasted.relative, QStringLiteral("assets/pasted-2026-11-16-1.png"));
    QCOMPARE(readFile(document + QStringLiteral("/assets/pasted-2026-11-16-1.png")), QByteArray("picture"));
    QCOMPARE(Attachments::savePasted(document, today, QStringLiteral("png"), "picture").relative, pasted.relative);
    const Attachments::Placed dropped = Attachments::copyIn(document, folder.filePath(QStringLiteral("dropped.desktop")));
    QCOMPARE(dropped.relative, QStringLiteral("assets/dropped.desktop"));
    // A link in the assets folder in place of a name is passed over, never written through.
    QVERIFY(QFile::link(elsewhere + QStringLiteral("/target.png"), document + QStringLiteral("/assets/pasted-2026-11-16-2.png")));
    QCOMPARE(Attachments::savePasted(document, today, QStringLiteral("png"), "other").relative,
             QStringLiteral("assets/pasted-2026-11-16-3.png"));
    QVERIFY(!QFileInfo::exists(elsewhere + QStringLiteral("/target.png")));
}

void bigPastesConvertOffTheWindowThread()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString path = QFileInfo(folder.path()).canonicalFilePath() + QStringLiteral("/paste.md");
    writeFile(path, "Start.\n");
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    activate(shell.window.get());
    EditorBinding *binding = shell.binding();
    Document *document = shell.document();
    QVERIFY(binding && document);
    SourceBuffer *buffer = document->buffer();
    QTRY_COMPARE(buffer->text(), QStringLiteral("Start.\n"));
    binding->select(buffer->length(), buffer->length());
    const auto copy = [](const QString &html, const QString &text) {
        auto *mime = new QMimeData;
        mime->setHtml(html);
        mime->setText(text);
        QGuiApplication::clipboard()->setMimeData(mime);
    };
    // A large page converts on a worker: the paste returns at once, and the
    // text comes in, as one undo step, when it is ready.
    QString html = QStringLiteral("<p>");
    for (int i = 0; i < 40000; ++i) html += QStringLiteral("<em>word</em> ");
    html += QStringLiteral("</p>");
    copy(html, QStringLiteral("plain words"));
    QElapsedTimer clock;
    clock.start();
    QVERIFY(QMetaObject::invokeMethod(binding, "paste"));
    const qint64 took = clock.elapsed();
    QVERIFY2(took < timeLimit(100), qPrintable(QString::number(took)));
    QCOMPARE(buffer->text(), QStringLiteral("Start.\n"));
    QTRY_VERIFY2_WITH_TIMEOUT(buffer->text().contains(QStringLiteral("*word word word")), qPrintable(buffer->text().left(300).replace(u'\n', u'|')), 10000);
    QVERIFY(buffer->text().startsWith(QStringLiteral("Start.")));
    buffer->undo();
    QCOMPARE(buffer->text(), QStringLiteral("Start.\n"));
    // HTML whose Markdown would grow far past it, as deeply nested lists
    // do, is pasted as its plain text instead.
    QString nested;
    for (int i = 0; i < 20000; ++i) nested += QStringLiteral("<ul><li>x");
    copy(nested, QStringLiteral("just x"));
    binding->select(buffer->length(), buffer->length());
    clock.restart();
    QVERIFY(QMetaObject::invokeMethod(binding, "paste"));
    QVERIFY2(clock.elapsed() < timeLimit(100), qPrintable(QString::number(clock.elapsed())));
    QCOMPARE(buffer->text(), QStringLiteral("Start.\n"));
    QTRY_VERIFY_WITH_TIMEOUT(buffer->text().contains(QStringLiteral("just x")), 10000);
    QVERIFY2(buffer->text().size() < 100, qPrintable(buffer->text().left(200)));
}

void aWordTooLongToBreakKeepsTypingQuick()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString root = QFileInfo(folder.path()).canonicalFilePath();
    const QString longOne = root + QStringLiteral("/long.md");
    const QString word(300000, u'w');
    writeFile(longOne, (QStringLiteral("Before.\n\n") + word + QStringLiteral("\n\nAfter.\n")).toUtf8());
    const QString plain = root + QStringLiteral("/plain.md");
    writeFile(plain, "Ordinary words in ordinary lines.\n");
    ShellFixture shell({.files = {plain, longOne}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    activate(shell.window.get());
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    EditorBinding *binding = shell.binding();
    QVERIFY(editor && binding);
    QTRY_COMPARE(shell.document()->buffer()->length(), int(word.size()) + 18);
    // A document with a word Qt could only break in time that grows with
    // its square wraps at any character instead, which costs time in
    // proportion to the line; a keystroke inside the word stays quick.
    QTRY_COMPARE(editor->property("wrapMode").toInt(), int(QTextOption::WrapAnywhere));
    auto *quick = editor->property("textDocument").value<QQuickTextDocument *>();
    QVERIFY(quick && quick->textDocument());
    QTextDocument *text = quick->textDocument();
    (void)text->documentLayout()->documentSize();
    QElapsedTimer clock;
    clock.start();
    QVERIFY(shell.document()->buffer()->replace({150000, 0}, QStringLiteral("x")) == SourceBuffer::EditOutcome::Applied);
    (void)text->documentLayout()->documentSize();
    QVERIFY2(clock.elapsed() < timeLimit(300), qPrintable(QString::number(clock.elapsed())));
    // Ordinary text wraps between words, as always.
    shell.context()->tabs()->setActiveIndex(0);
    QTRY_COMPARE(editor->property("wrapMode").toInt(), int(QTextOption::WrapAtWordBoundaryOrAnywhere));
    // A long word typed or pasted into ordinary text turns it on too.
    QVERIFY(shell.document()->buffer()->replace({0, 0}, QString(20000, u'q')) == SourceBuffer::EditOutcome::Applied);
    QTRY_COMPARE(editor->property("wrapMode").toInt(), int(QTextOption::WrapAnywhere));
}

void aLongPdfStaysCancellableWhileItIsSet()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString path = folder.filePath(QStringLiteral("long.md"));
    QString text;
    for (int i = 0; i < 3000; ++i) text += QStringLiteral("Paragraph %1 with a few words in it.\n\n").arg(i);
    writeFile(path, text.toUtf8());
    DocumentCheck::Loaded loaded;
    QString error;
    QVERIFY2(DocumentCheck::load(path, Dialect::choose({}), &loaded, &error), qPrintable(error));
    ExportRecipe recipe;
    recipe.source = loaded.path;
    recipe.format = QStringLiteral("pdf");
    recipe.output = folder.filePath(QStringLiteral("long.pdf"));
    recipe.dialect = loaded.dialect;
    recipe.citations = std::make_shared<const DocumentCitations::Formatted>(loaded.formatted);
    // Laying out the pages reports its progress as it goes, and stops when
    // asked, before the whole document is laid out and any page is set.
    bool cancelled = false;
    QList<qreal> laying;
    QStringList steps;
    ExportRun run;
    run.pictures = false;
    run.cancelled = [&] { return cancelled; };
    run.progress = [&](qreal done, const QString &step) {
        if (steps.isEmpty() || steps.last() != step) steps << step;
        if (step != QStringLiteral("Laying out the pages")) return;
        laying << done;
        if (laying.size() == 3) cancelled = true;
    };
    const ExportResult result = Exporters::find(QStringLiteral("pdf"))->write(loaded.document, recipe, run);
    QVERIFY2(result.cancelled, qPrintable(result.error));
    QVERIFY(!result.ok);
    QVERIFY(!QFileInfo::exists(recipe.output));
    QCOMPARE(laying.size(), 3);
    QVERIFY(laying.first() >= 0.6 && laying.last() < 0.75);
    QVERIFY2(!steps.contains(QStringLiteral("Setting the pages")), qPrintable(steps.join(u',')));
    // Not cancelled, it is written, its progress rising to the end.
    cancelled = false;
    QList<qreal> setting;
    run.progress = [&](qreal done, const QString &) { setting << done; };
    const ExportResult written = Exporters::find(QStringLiteral("pdf"))->write(loaded.document, recipe, run);
    QVERIFY2(written.ok, qPrintable(written.error));
    QVERIFY(std::is_sorted(setting.cbegin(), setting.cend()));
    QCOMPARE(setting.last(), 1.0);
}

void webPictureRequestsKeepTheirHost()
{
    const QUrl origin(QStringLiteral("https://pictures.example:8443/figures/one.png?size=2"));
    for (const QHostAddress &address : {QHostAddress("203.0.113.7"), QHostAddress("2001:db8::7")}) {
        const QNetworkRequest request = NetworkFetcher::pictureRequest(origin, address);
        QCOMPARE(QHostAddress(request.url().host()), address);
        QCOMPARE(request.url().port(), origin.port());
        QCOMPARE(request.url().path(), origin.path());
        QCOMPARE(request.url().query(), origin.query());
        QCOMPARE(request.rawHeader("Host"), QByteArray("pictures.example:8443"));
        QCOMPARE(request.peerVerifyName(), origin.host());
    }
}

void webPicturesComeOnlyFromPublicAddresses()
{
    PictureServer server;
    QVERIFY(server.listening());
    const QByteArray png = pngBytes();
    server.answer("/pic.png", {200, "image/png", png, {}, 0});
    server.answer("/page.html", {200, "text/html; charset=utf-8", "<html>not a picture</html>", {}, 0});
    server.answer("/pic.bmp", {200, "image/bmp", "BM....", {}, 0});
    server.answer("/to-inside", {302, {}, {}, "http://inside.test/pic.png", 0});
    server.answer("/to-loopback", {301, {}, {}, "http://127.0.0.1:1/pic.png", 0});
    server.answer("/to-ftp", {302, {}, {}, "ftp://public.test/pic.png", 0});
    server.answer("/to-file", {302, {}, {}, "file:///etc/passwd", 0});
    server.answer("/r1", {302, {}, {}, "/r2", 0});
    server.answer("/r2", {307, {}, {}, "http://public.test/r3", 0});
    server.answer("/r3", {308, {}, {}, "pic.png", 0});
    server.answer("/s1", {302, {}, {}, "/s2", 0});
    server.answer("/s2", {302, {}, {}, "/s3", 0});
    server.answer("/s3", {302, {}, {}, "/s4", 0});
    server.answer("/s4", {302, {}, {}, "/pic.png", 0});
    server.answer("/drip.png", {200, "image/png", png, {}, 100});

    // A server on this computer is never asked, by its address or by a name.
    NetworkFetcher direct;
    for (const QString &address : {QStringLiteral("http://127.0.0.1:%1/pic.png"), QStringLiteral("http://[::1]:%1/pic.png"),
                                   QStringLiteral("http://localhost:%1/pic.png")}) {
        const Fetched fetched = fetchNow(direct, address.arg(server.port()));
        QVERIFY2(fetched.done && !fetched.error.isEmpty() && fetched.data.isEmpty(), qPrintable(address));
    }
    QCOMPARE(server.connections, 0);

    // The rest goes through the server as the fetcher's proxy, with names
    // the test resolves, so that nothing reaches the internet: what the
    // fetcher refuses never reaches the proxy at all.
    NetworkFetcher fetcher;
    fetcher.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), server.port()));
    const QHash<QString, QStringList> names{
        {QStringLiteral("public.test"), {QStringLiteral("203.0.113.7"), QStringLiteral("2001:db8::7")}},
        {QStringLiteral("inside.test"), {QStringLiteral("10.1.2.3")}},
        {QStringLiteral("router.test"), {QStringLiteral("192.168.1.1")}},
        {QStringLiteral("office.test"), {QStringLiteral("172.20.0.4")}},
        {QStringLiteral("mixed.test"), {QStringLiteral("203.0.113.8"), QStringLiteral("192.168.0.9")}},
        {QStringLiteral("self.test"), {QStringLiteral("127.0.0.2")}},
        {QStringLiteral("cgnat.test"), {QStringLiteral("100.64.0.1")}},
        {QStringLiteral("metadata.test"), {QStringLiteral("169.254.169.254")}},
        {QStringLiteral("ula.test"), {QStringLiteral("fd12:3456::1")}},
        {QStringLiteral("linklocal6.test"), {QStringLiteral("fe80::1")}},
        {QStringLiteral("loopback6.test"), {QStringLiteral("::1")}},
        {QStringLiteral("mapped.test"), {QStringLiteral("::ffff:127.0.0.1")}},
        {QStringLiteral("multicast.test"), {QStringLiteral("239.1.2.3")}},
        {QStringLiteral("multicast6.test"), {QStringLiteral("ff02::1")}},
        {QStringLiteral("zero.test"), {QStringLiteral("0.0.0.0")}},
        {QStringLiteral("zero6.test"), {QStringLiteral("::")}},
        {QStringLiteral("nowhere.test"), {}},
    };
    QStringList resolved;
    fetcher.setResolver([&](const QString &host, std::function<void(const QList<QHostAddress> &, const QString &)> answer) {
        resolved << host;
        QList<QHostAddress> addresses;
        for (const QString &address : names.value(host)) addresses << QHostAddress(address);
        answer(addresses, addresses.isEmpty() ? QStringLiteral("Host not found") : QString());
    });
    for (auto it = names.cbegin(); it != names.cend(); ++it) {
        if (it.key() == u"public.test") continue;
        const Fetched fetched = fetchNow(fetcher, QStringLiteral("http://%1/pic.png").arg(it.key()));
        QVERIFY2(fetched.done && !fetched.error.isEmpty() && fetched.data.isEmpty(), qPrintable(it.key()));
    }
    QCOMPARE(server.connections, 0);
    QVERIFY(server.asked.isEmpty());

    // A picture at a public address comes, and its name was resolved first.
    resolved.clear();
    Fetched fetched = fetchNow(fetcher, QStringLiteral("http://public.test/pic.png"));
    QVERIFY2(fetched.done && fetched.error.isEmpty(), qPrintable(fetched.error));
    QCOMPARE(fetched.data, png);
    QCOMPARE(resolved, QStringList{QStringLiteral("public.test")});
    QCOMPARE(server.asked, QList<QByteArray>{"http://203.0.113.7/pic.png"});
    QCOMPARE(server.hosts, QList<QByteArray>{"public.test"});

    // Only pictures are taken.
    for (const char *path : {"/page.html", "/pic.bmp", "/missing.png"}) {
        fetched = fetchNow(fetcher, QStringLiteral("http://public.test") + QLatin1StringView(path));
        QVERIFY2(fetched.done && !fetched.error.isEmpty() && fetched.data.isEmpty(), path);
    }

    // A redirect is checked as the first address was: its name resolved
    // again, its addresses public, and http or https only.
    for (const char *path : {"/to-inside", "/to-loopback", "/to-ftp", "/to-file"}) {
        server.asked.clear();
        fetched = fetchNow(fetcher, QStringLiteral("http://public.test") + QLatin1StringView(path));
        QVERIFY2(fetched.done && !fetched.error.isEmpty() && fetched.data.isEmpty(), path);
        QCOMPARE(server.asked.size(), 1);
    }
    // Three redirects are followed, relative ones too; a fourth is not.
    resolved.clear();
    server.asked.clear();
    fetched = fetchNow(fetcher, QStringLiteral("http://public.test/r1"));
    QVERIFY2(fetched.done && fetched.error.isEmpty(), qPrintable(fetched.error));
    QCOMPARE(fetched.data, png);
    QCOMPARE(server.asked.size(), 4);
    QCOMPARE(resolved.size(), 4);
    server.asked.clear();
    fetched = fetchNow(fetcher, QStringLiteral("http://public.test/s1"));
    QVERIFY(fetched.done && !fetched.error.isEmpty() && fetched.data.isEmpty());
    QCOMPARE(server.asked.size(), 4);
    // Never from https to http, and nowhere but http and https.
    QVERIFY(!NetworkFetcher::redirectRefusal(QUrl(QStringLiteral("https://a.test/x")), QUrl(QStringLiteral("http://a.test/y"))).isEmpty());
    QVERIFY(NetworkFetcher::redirectRefusal(QUrl(QStringLiteral("http://a.test/x")), QUrl(QStringLiteral("https://b.test/y"))).isEmpty());
    QVERIFY(NetworkFetcher::redirectRefusal(QUrl(QStringLiteral("https://a.test/x")), QUrl(QStringLiteral("https://b.test/y"))).isEmpty());
    QVERIFY(!NetworkFetcher::redirectRefusal(QUrl(QStringLiteral("https://a.test/x")), QUrl(QStringLiteral("data:image/png,x"))).isEmpty());
    // Only https and http addresses are fetched at all.
    for (const char *address : {"ftp://public.test/pic.png", "file:///etc/hostname", "gopher://public.test/"}) {
        fetched = fetchNow(fetcher, QLatin1StringView(address));
        QVERIFY2(fetched.done && !fetched.error.isEmpty(), address);
    }

    // A server that sends a picture too slowly is given up within the time
    // allowed for the whole fetch, however steadily it sends.
    fetcher.setTimeout(400);
    fetched = fetchNow(fetcher, QStringLiteral("http://public.test/drip.png"), 5000);
    QVERIFY(fetched.done);
    QVERIFY2(!fetched.error.isEmpty() && fetched.data.isEmpty(), qPrintable(fetched.error));

    // What counts as an address parch fetches from.
    for (const char *open : {"203.0.113.7", "8.8.8.8", "2001:db8::1", "2606:4700::1111", "100.63.255.255", "100.128.0.0",
                             "172.15.0.1", "172.32.0.1"})
        QVERIFY2(NetworkFetcher::isPublic(QHostAddress(QLatin1StringView(open))), open);
    for (const char *closed : {"127.0.0.1", "127.255.255.254", "10.0.0.1", "172.16.0.1", "172.31.255.255", "192.168.0.1",
                               "100.64.0.0", "100.127.255.255", "169.254.0.1", "224.0.0.1", "255.255.255.255", "0.0.0.0",
                               "0.1.2.3", "::", "::1", "fc00::1", "fdff::1", "fe80::1", "febf::1", "ff00::1",
                               "::ffff:10.0.0.1", "::ffff:127.0.0.1", "::127.0.0.1", "64:ff9b::7f00:1"})
        QVERIFY2(!NetworkFetcher::isPublic(QHostAddress(QLatin1StringView(closed))), closed);

    // Bytes from the web are read as one of five kinds of picture, by what
    // they are, and as nothing else.
    QCOMPARE(PreviewResources::webPictureFormat(png), QByteArray("png"));
    QCOMPARE(PreviewResources::webPictureFormat(QByteArray("\xff\xd8\xff\xe0", 4) + QByteArray(20, '\0')), QByteArray("jpeg"));
    QCOMPARE(PreviewResources::webPictureFormat("GIF89a" + QByteArray(20, '\0')), QByteArray("gif"));
    QCOMPARE(PreviewResources::webPictureFormat(QByteArray("RIFF\x10\0\0\0WEBPVP8 ", 16) + QByteArray(20, '\0')), QByteArray("webp"));
    QCOMPARE(PreviewResources::webPictureFormat("<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>"),
             QByteArray("svg"));
    QVERIFY(PreviewResources::webPictureFormat("BM" + QByteArray(40, '\0')).isEmpty());
    QVERIFY(PreviewResources::webPictureFormat("8BPS" + QByteArray(40, '\0')).isEmpty());
    QVERIFY(PreviewResources::webPictureFormat("<html><body>hi</body></html>").isEmpty());
    QVERIFY(PreviewResources::webPictureFormat({}).isEmpty());
}

} // namespace SecurityTests
