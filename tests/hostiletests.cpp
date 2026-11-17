#include "hostiletests.h"

#include "apppaths.h"
#include "backend.h"
#include "document.h"
#include "documentchecker.h"
#include "documentfind.h"
#include "documentparse.h"
#include "documentstore.h"
#include "exports.h"
#include "preview.h"
#include "printpreview.h"
#include "session.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "tabmodel.h"
#include "windowcontext.h"
#include "windowtesting.h"
#include "workerfailure.h"
#include "citations.h"
#include "cli.h"
#include "clicommands.h"
#include "cslreference.h"
#include "cslstyle.h"
#include "diskfile.h"
#include "documentcheck.h"
#include "documentreading.h"
#include "foldersearch.h"
#include "mdinclude.h"
#include "mdhtml.h"
#include "mdtree.h"
#include "previewbuilder.h"
#include "previewresources.h"
#include "saferead.h"
#include "testfiles.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QUuid>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QtConcurrent>

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace HostileTests {
namespace {

// A FIFO no one writes to blocks a plain open for good. While a test runs,
// this opens every FIFO it is given for writing now and then, after a
// while, and closes it at once, so that a reader that did block gets to the
// end and the test fails instead of hanging. A reader that never blocks
// never meets it.
class FifoRelease {
public:
    explicit FifoRelease(QStringList fifos) : m_fifos(std::move(fifos))
    {
        m_thread = std::thread([this] {
            for (int tick = 0; !m_stop.load(); ++tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (tick < 30 || tick % 4 != 0) continue;
                for (const QString &fifo : std::as_const(m_fifos)) {
                    const int written = ::open(QFile::encodeName(fifo).constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
                    if (written >= 0) {
                        m_released.fetch_add(1);
                        ::close(written);
                    }
                }
            }
        });
    }
    ~FifoRelease()
    {
        m_stop = true;
        m_thread.join();
    }
    // How many times a reader was found waiting.
    int released() const { return m_released.load(); }

private:
    QStringList m_fifos;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_released{0};
    std::thread m_thread;
};

QString makeFifo(const QString &path)
{
    QDir().mkpath(QFileInfo(path).path());
    return ::mkfifo(QFile::encodeName(path).constData(), 0600) == 0 ? path : QString();
}

// A link to a file of the system that says it is empty, as procfs files do.
QString linkToProc(const QString &path)
{
    QDir().mkpath(QFileInfo(path).path());
    return QFile::link(QStringLiteral("/proc/self/status"), path) ? path : QString();
}

// A sparse file one byte larger than the limit: it takes no room on disk.
QString sparseFile(const QString &path, qint64 bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.resize(bytes) ? path : QString();
}

struct Ran {
    int code = -1;
    QString out;
    QString err;
    QString said() const { return QStringLiteral("exit %1\n--- out\n%2\n--- err\n%3").arg(code).arg(out, err); }
};

Ran runCli(const QString &folder, const QStringList &arguments)
{
    QBuffer out, err;
    out.open(QIODevice::WriteOnly);
    err.open(QIODevice::WriteOnly);
    const Cli::Context context{folder, folder + QStringLiteral("/home"), AppPaths::under(folder + QStringLiteral("/app")),
                               &out, &err};
    Ran ran;
    ran.code = Cli::run(QStringList{QStringLiteral("parch")} + arguments, context);
    ran.out = QString::fromUtf8(out.data());
    ran.err = QString::fromUtf8(err.data());
    return ran;
}

bool unreadable(const BibliographyDatabase &db, const QString &why)
{
    for (const BibliographyDiagnostic &d : db.diagnostics())
        if (d.kind == BibliographyDiagnostic::Kind::Unreadable && d.severity == BibliographyDiagnostic::Severity::Error
            && d.message.contains(why))
            return true;
    return false;
}

QString messages(const BibliographyDatabase &db)
{
    QStringList all;
    for (const BibliographyDiagnostic &d : db.diagnostics()) all.append(d.message);
    return all.join(u'\n');
}

const QString notRegular = QStringLiteral("not a regular file");
const QString ofTheSystem = QStringLiteral("file of the system");

} // namespace

void readsStopAtTheirLimit()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString fifo = makeFifo(folder.filePath(QStringLiteral("fifo.md")));
    QVERIFY(!fifo.isEmpty());
    const QString proc = linkToProc(folder.filePath(QStringLiteral("status.md")));
    QVERIFY(!proc.isEmpty());
    FifoRelease release({fifo});

    // A FIFO is refused at once, without waiting for a writer.
    QElapsedTimer clock;
    clock.start();
    SafeRead::Result read = SafeRead::read(fifo, 1024);
    QVERIFY(clock.elapsed() < 1000);
    QVERIFY(!read.ok && !read.missing && read.bytes.isEmpty());
    QVERIFY2(read.error.contains(notRegular), qPrintable(read.error));

    // Devices and the files of /proc, /sys and /dev, however they are named.
    for (const QString &path : {QStringLiteral("/dev/null"), QStringLiteral("/dev/zero"), QStringLiteral("/dev/urandom"),
                                QStringLiteral("/proc/self/environ"), QStringLiteral("/sys/kernel/mm/transparent_hugepage/enabled"),
                                proc}) {
        read = SafeRead::read(path, 1024);
        QVERIFY2(!read.ok && read.bytes.isEmpty(), qPrintable(path));
        QVERIFY2(read.error.contains(ofTheSystem) || (!QFileInfo::exists(path) && read.missing),
                 qPrintable(path + u' ' + read.error));
    }
    QVERIFY(SafeRead::isSystemPath(QStringLiteral("/proc")));
    QVERIFY(SafeRead::isSystemPath(QStringLiteral("/dev/shm/x")));
    QVERIFY(!SafeRead::isSystemPath(QStringLiteral("/devices/notes.md")));
    QVERIFY(!SafeRead::isSystemPath(QStringLiteral("/home/proc/notes.md")));

    // parch's own resources are read from memory, within the limit too.
    read = SafeRead::read(QStringLiteral(":/guide/guide-include.md"), 1 << 20);
    QVERIFY(read.ok && !read.bytes.isEmpty());
    QVERIFY(SafeRead::read(QStringLiteral(":/guide/guide-include.md"), 4).tooLarge);
    QVERIFY(SafeRead::read(QStringLiteral(":/guide/none.md"), 1024).missing);

    // A folder, and nothing at all.
    read = SafeRead::read(folder.path(), 1024);
    QVERIFY(!read.ok && !read.missing && read.error.contains(QStringLiteral("folder")));
    read = SafeRead::read(folder.filePath(QStringLiteral("none.md")), 1024);
    QVERIFY(!read.ok && read.missing);

    // A file larger than the limit is not read; one at the limit is.
    const QString large = sparseFile(folder.filePath(QStringLiteral("large.md")), 4097);
    QVERIFY(!large.isEmpty());
    read = SafeRead::read(large, 4096);
    QVERIFY(!read.ok && read.tooLarge && read.bytes.isEmpty());
    QVERIFY2(read.error.contains(QStringLiteral("larger than the 4 KiB")), qPrintable(read.error));
    read = SafeRead::read(large, 4097);
    QVERIFY(read.ok && read.bytes.size() == 4097);
    QCOMPARE(read.canonicalPath, QFileInfo(large).canonicalFilePath());

    // The limit holds whatever size the file claims: a pipe says it is empty.
    int ends[2];
    QCOMPARE(::pipe(ends), 0);
    const QByteArray written(3000, 'x');
    QCOMPARE(::write(ends[1], written.constData(), size_t(written.size())), ssize_t(written.size()));
    ::close(ends[1]);
    QByteArray bytes;
    QString error;
    bool tooLarge = false;
    QVERIFY(!SafeRead::readDescriptor(ends[0], 2999, bytes, error, &tooLarge));
    QVERIFY(tooLarge && bytes.isEmpty());
    ::close(ends[0]);
    QCOMPARE(::pipe(ends), 0);
    QCOMPARE(::write(ends[1], written.constData(), size_t(written.size())), ssize_t(written.size()));
    ::close(ends[1]);
    QVERIFY(SafeRead::readDescriptor(ends[0], 3000, bytes, error, &tooLarge));
    QCOMPARE(bytes, written);
    ::close(ends[0]);

    // A cross-filesystem copy must account for every byte in the original.
    QFile sample(large);
    QVERIFY(sample.open(QIODevice::ReadOnly));
    QVERIFY(!SafeRead::readForCopy(sample.handle(), sample.size() + 1, bytes, error));
    QVERIFY(!error.isEmpty() && bytes.isEmpty());
    QVERIFY(!SafeRead::readForCopy(-1, sample.size(), bytes, error));
    QVERIFY(!error.isEmpty() && bytes.isEmpty());
    QCOMPARE(release.released(), 0);
}

void bibliographiesAndStylesAreReadSafely()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = QFileInfo(folder.path()).canonicalFilePath();
    const QString fifoBib = makeFifo(base + QStringLiteral("/fifo.bib"));
    const QString fifoCsl = makeFifo(base + QStringLiteral("/fifo.csl"));
    const QString fifoLocale = makeFifo(base + QStringLiteral("/locales-de-DE.xml"));
    QVERIFY(!fifoBib.isEmpty() && !fifoCsl.isEmpty() && !fifoLocale.isEmpty());
    const QString procBib = linkToProc(base + QStringLiteral("/status.bib"));
    const QString procCsl = linkToProc(base + QStringLiteral("/status.csl"));
    QVERIFY(!procBib.isEmpty() && !procCsl.isEmpty());
    FifoRelease release({fifoBib, fifoCsl, fifoLocale});

    QElapsedTimer clock;
    clock.start();
    // Bibliographies: a FIFO, a device and a file of /proc are each refused
    // as unreadable, and a file larger than 32 MiB is not read.
    BibliographyDatabase db = BibliographyDatabase::fromFile(fifoBib);
    QVERIFY2(unreadable(db, notRegular), qPrintable(messages(db)));
    for (const QString &path : {QStringLiteral("/dev/null"), QStringLiteral("/dev/zero"), procBib}) {
        db = BibliographyDatabase::fromFile(path, BibliographyDatabase::Format::BibLaTeX);
        QVERIFY2(unreadable(db, ofTheSystem), qPrintable(path + u'\n' + messages(db)));
    }
    const QString huge = sparseFile(base + QStringLiteral("/huge.json"), SafeRead::ReferenceBytes + 1);
    QVERIFY(!huge.isEmpty());
    db = BibliographyDatabase::fromFile(huge);
    QVERIFY2(unreadable(db, QStringLiteral("larger than the 32 MiB")), qPrintable(messages(db)));

    // Styles and locales.
    QString error;
    QVERIFY(!CslCatalog::styleFromFile(fifoCsl, &error));
    QVERIFY2(error.contains(notRegular), qPrintable(error));
    for (const QString &path : {QStringLiteral("/dev/null"), QStringLiteral("/dev/urandom"), procCsl}) {
        error.clear();
        QVERIFY(!CslCatalog::styleFromFile(path, &error));
        QVERIFY2(error.contains(ofTheSystem), qPrintable(path + u' ' + error));
    }
    error.clear();
    QVERIFY(!CslCatalog::localeFromFile(fifoLocale, &error));
    QVERIFY2(error.contains(notRegular), qPrintable(error));
    error.clear();
    QVERIFY(CslCatalog::locale(QStringLiteral("de-DE"), {base}, &error));
    QVERIFY2(error.contains(notRegular), qPrintable(error));

    // A whole document that names them in its front matter, as the preview
    // and every export read it.
    const QString text = QStringLiteral("---\nbibliography: [fifo.bib, /dev/zero, status.bib]\ncsl: /dev/urandom\n---\n\n"
                                        "A claim [@doe2006].\n");
    md::Source source;
    source.path = base + QStringLiteral("/doc.md");
    const DocumentReading::Reading reading = DocumentReading::read(text, {}, source);
    QStringList codes, said;
    for (const DocumentCitations::Problem &problem : reading.citations.problems) {
        codes.append(problem.code);
        said.append(problem.message);
    }
    QVERIFY2(codes.count(QStringLiteral("unreadable-bibliography")) == 3, qPrintable(said.join(u'\n')));
    QVERIFY2(codes.contains(QStringLiteral("citation-style")), qPrintable(said.join(u'\n')));
    QVERIFY2(clock.elapsed() < 1500, qPrintable(QString::number(clock.elapsed())));
    QCOMPARE(release.released(), 0);
}

void documentsAndIncludesAreReadSafely()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = QFileInfo(folder.path()).canonicalFilePath();
    const QString fifo = makeFifo(base + QStringLiteral("/fifo.md"));
    const QString proc = linkToProc(base + QStringLiteral("/status.md"));
    QVERIFY(!fifo.isEmpty() && !proc.isEmpty());
    FifoRelease release({fifo});
    QElapsedTimer clock;
    clock.start();

    // Opening a document: a file of /proc says it is empty, and is refused
    // as a file of the system; a FIFO as no regular file.
    LoadedFile loaded = DiskFile::load(proc);
    QVERIFY(!loaded.ok && !loaded.missing);
    QVERIFY2(loaded.unreadable.contains(ofTheSystem), qPrintable(loaded.unreadable));
    loaded = DiskFile::load(QStringLiteral("/dev/zero"));
    QVERIFY(!loaded.ok && !loaded.missing);
    loaded = DiskFile::load(fifo);
    QVERIFY(!loaded.ok && loaded.notRegular);

    // An include that is one of them is refused with the reason.
    md::FileIncludeResolver resolver;
    md::IncludeResolver::File file = resolver.read(proc, 1 << 20);
    QVERIFY(!file.ok && !file.missing && !file.tooLarge);
    QVERIFY2(file.error.contains(ofTheSystem), qPrintable(file.error));
    file = resolver.read(fifo, 1 << 20);
    QVERIFY(!file.ok && file.error.contains(notRegular));
    // An include file larger than the 64 MiB a document may be is not read,
    // whatever room the include limits leave.
    const QString huge = sparseFile(base + QStringLiteral("/huge.md"), SafeRead::DocumentBytes + 1);
    QVERIFY(!huge.isEmpty());
    file = resolver.read(huge, qint64(1) << 30);
    QVERIFY(!file.ok);
    QVERIFY2(file.error.contains(QStringLiteral("larger than the 64 MiB")), qPrintable(file.error));

    md::Source source;
    source.path = base + QStringLiteral("/main.md");
    source.libraryRoot = base;
    source.resolver = &resolver;
    const md::Document doc =
        md::parse(QStringLiteral("fifo.md\n\nstatus.md\n\nhuge.md\n"), md::Options::forProfile(md::Profile::Writer), source);
    // The file of /proc is outside the library as well.
    int refused = 0;
    for (const md::Diagnostic &d : doc.diagnostics)
        refused += d.code == md::DiagnosticCode::IncludeUnreadable || d.code == md::DiagnosticCode::IncludeOutside;
    QCOMPARE(refused, 3);
    QVERIFY(doc.incomplete);

    // The command line reads a document the same way.
    QString error;
    DocumentCheck::Loaded checked;
    QVERIFY(!DocumentCheck::load(proc, Dialect::Layers{}, &checked, &error));
    QVERIFY2(error.contains(ofTheSystem), qPrintable(error));
    const Ran ran = runCli(base, {QStringLiteral("stats"), proc});
    QVERIFY2(ran.code != Cli::Ok && ran.err.contains(ofTheSystem), qPrintable(ran.said()));
    QVERIFY2(clock.elapsed() < 1500, qPrintable(QString::number(clock.elapsed())));
    QCOMPARE(release.released(), 0);
}

void picturesAreReadSafely()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = QFileInfo(folder.path()).canonicalFilePath();
    const QString fifo = makeFifo(base + QStringLiteral("/fifo.png"));
    const QString proc = linkToProc(base + QStringLiteral("/status.png"));
    QVERIFY(!fifo.isEmpty() && !proc.isEmpty());
    FifoRelease release({fifo});
    writeFile(base + QStringLiteral("/doc.md"), "# Pictures\n\n![a](fifo.png)\n\n![b](status.png)\n\n![c](/dev/zero)\n");

    QElapsedTimer clock;
    clock.start();
    // Every export reads the pictures the same way, and says which it could not.
    for (const QString &format : {QStringLiteral("html"), QStringLiteral("epub"), QStringLiteral("docx"),
                                  QStringLiteral("pdf"), QStringLiteral("md")}) {
        QStringList arguments{QStringLiteral("export"), base + QStringLiteral("/doc.md"), QStringLiteral("-o"),
                              base + QStringLiteral("/out.") + format, QStringLiteral("--force")};
        if (format == u"md") arguments << QStringLiteral("--images") << QStringLiteral("copy");
        const Ran ran = runCli(base, arguments);
        QVERIFY2(ran.code == Cli::Ok, qPrintable(format + u'\n' + ran.said()));
        if (format != u"pdf") {
            QVERIFY2(ran.out.contains(notRegular) || ran.err.contains(notRegular), qPrintable(format + u'\n' + ran.said()));
            QVERIFY2(ran.out.contains(ofTheSystem) || ran.err.contains(ofTheSystem), qPrintable(format + u'\n' + ran.said()));
        }
    }
    QVERIFY2(clock.elapsed() < timeLimit(20000), qPrintable(QString::number(clock.elapsed())));

    // The preview reads them on a worker, and shows why it could not.
    PreviewResources resources;
    QSignalSpy changed(&resources, &PreviewResources::changed);
    const QHash<QString, QString> expected{{fifo, notRegular}, {proc, ofTheSystem}, {QStringLiteral("/dev/zero"), ofTheSystem}};
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        md::Destination destination;
        destination.kind = md::Destination::Kind::LocalFile;
        destination.path = it.key();
        destination.absolute = true;
        resources.image(destination, it.key(), {base});
    }
    QTRY_COMPARE_WITH_TIMEOUT(resources.pending(), 0, 1000);
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        md::Destination destination;
        destination.kind = md::Destination::Kind::LocalFile;
        destination.path = it.key();
        destination.absolute = true;
        const preview::Picture picture = resources.image(destination, it.key(), {base});
        QCOMPARE(picture.state, preview::Picture::Failed);
        QVERIFY2(picture.diagnostic.contains(it.value()), qPrintable(it.key() + u' ' + picture.diagnostic));
    }
    QCOMPARE(release.released(), 0);
}

void folderSearchReadsSafely()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString proc = linkToProc(folder.filePath(QStringLiteral("status.md")));
    const QString fifo = makeFifo(folder.filePath(QStringLiteral("fifo.md")));
    QVERIFY(!proc.isEmpty() && !fifo.isEmpty());
    FifoRelease release({fifo});
    writeFile(folder.filePath(QStringLiteral("plain.md")), "Some text\n");
    QString text;
    // What says it is empty is not read whatever it holds.
    QVERIFY(!FolderSearch::readText(proc, 0, 0, &text));
    QVERIFY(text.isEmpty());
    QVERIFY(!FolderSearch::readText(fifo, 0, 0, &text));
    QVERIFY(FolderSearch::readText(folder.filePath(QStringLiteral("plain.md")), 10, 0, &text));
    QCOMPARE(text, QStringLiteral("Some text\n"));
    QCOMPARE(release.released(), 0);
}

namespace {

int countOf(const md::Document &doc, md::DiagnosticCode code, QStringList *messages = nullptr)
{
    int n = 0;
    for (const md::Diagnostic &d : doc.diagnostics) {
        if (d.code != code) continue;
        ++n;
        if (messages) messages->append(d.message);
    }
    return n;
}

QString problemsOf(const DocumentCitations::Setup &setup)
{
    QStringList all;
    for (const DocumentCitations::Problem &problem : setup.problems) all.append(problem.code + u' ' + problem.message);
    return all.join(u'\n');
}

} // namespace

void includesStayInTheirLibrary()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = QFileInfo(folder.path()).canonicalFilePath();
    const QString library = base + QStringLiteral("/lib");
    writeFile(base + QStringLiteral("/outside.txt"), "SECRET-OUTSIDE-LIBRARY\n");
    writeFile(base + QStringLiteral("/other/notes.md"), "SECRET-IN-ANOTHER-FOLDER\n");
    writeFile(library + QStringLiteral("/ok.md"), "Inside the library.\n");
    writeFile(library + QStringLiteral("/book/chapter.md"), "A chapter.\n\n../ok.md\n");
    QVERIFY(QDir().mkpath(library + QStringLiteral("/sub")));
    QVERIFY(QFile::link(base + QStringLiteral("/outside.txt"), library + QStringLiteral("/sub/link.md")));
    QVERIFY(QFile::link(QStringLiteral("/proc/self/environ"), library + QStringLiteral("/sub/env.md")));
    QVERIFY(QFile::link(QStringLiteral("../ok.md"), library + QStringLiteral("/sub/inside.md")));
    const QString text = QStringLiteral("sub/link.md\n\n../outside.txt\n\n/../outside.txt\n\n../other/notes.md\n\n"
                                        "sub/env.md\n\nsub/inside.md\n\n/ok.md\n\nbook/chapter.md\n");
    writeFile(library + QStringLiteral("/main.md"), text.toUtf8());

    // In a library: what leads out of it, through "..", a rooted path or a
    // link, is not read, and says so; what stays in it is read, links and all.
    md::FileIncludeResolver resolver;
    md::Source source;
    source.path = library + QStringLiteral("/main.md");
    source.libraryRoot = library;
    source.resolver = &resolver;
    const md::Document doc = md::parse(text, md::Options::forProfile(md::Profile::Writer), source);
    const QString html = md::toHtml(doc);
    QVERIFY2(!html.contains(QStringLiteral("SECRET")), qPrintable(html));
    QVERIFY2(!html.contains(QStringLiteral("HOME=")) && !html.contains(QStringLiteral("PATH=")), qPrintable(html));
    QCOMPARE(html.count(QStringLiteral("Inside the library.")), 3);
    QStringList said;
    QCOMPARE(countOf(doc, md::DiagnosticCode::IncludeOutside, &said), 5);
    for (const QString &message : std::as_const(said))
        QVERIFY2(message.contains(QStringLiteral("outside the library")), qPrintable(message));
    QCOMPARE(QString::fromLatin1(md::codeName(md::DiagnosticCode::IncludeOutside)), QStringLiteral("include-outside"));
    QVERIFY(doc.incomplete);
    // The preview says why in the include's place.
    const QStringList shown = preview::Unresolved(doc).messages();
    QVERIFY2(!shown.filter(QStringLiteral("outside the library")).isEmpty(), qPrintable(shown.join(u'\n')));

    // In no library: the document's folder and the folders in it.
    md::Source alone;
    alone.path = library + QStringLiteral("/book/main.md");
    alone.resolver = &resolver;
    const md::Document lone =
        md::parse(QStringLiteral("chapter.md\n\n../ok.md\n"), md::Options::forProfile(md::Profile::Writer), alone);
    said.clear();
    // The chapter is read; its own "../ok.md", and the document's, are not.
    QCOMPARE(countOf(lone, md::DiagnosticCode::IncludeOutside, &said), 2);
    for (const QString &message : std::as_const(said))
        QVERIFY2(message.contains(QStringLiteral("outside the document's folder")), qPrintable(message));
    QVERIFY(md::toHtml(lone).contains(QStringLiteral("A chapter.")));

    // The command line reads includes the same way, and an export of a
    // document whose includes cannot all be read is not made.
    Ran ran = runCli(base, {QStringLiteral("render"), library + QStringLiteral("/main.md"), QStringLiteral("--library"),
                            library, QStringLiteral("--to"), QStringLiteral("text")});
    QVERIFY2(!ran.out.contains(QStringLiteral("SECRET")) && !ran.out.contains(QStringLiteral("HOME=")), qPrintable(ran.said()));
    ran = runCli(base, {QStringLiteral("check"), library + QStringLiteral("/main.md"), QStringLiteral("--library"), library,
                        QStringLiteral("--no-render")});
    QVERIFY2(ran.code == Cli::Problems && ran.out.contains(QStringLiteral("[include-outside]")), qPrintable(ran.said()));
    ran = runCli(base, {QStringLiteral("export"), library + QStringLiteral("/main.md"), QStringLiteral("--library"), library,
                        QStringLiteral("-o"), base + QStringLiteral("/out.md")});
    QVERIFY2(ran.code != Cli::Ok && !QFileInfo::exists(base + QStringLiteral("/out.md")), qPrintable(ran.said()));
}

void frontMatterFilesStayInTheirLibrary()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = QFileInfo(folder.path()).canonicalFilePath();
    const QString library = base + QStringLiteral("/lib");
    writeFile(base + QStringLiteral("/secret.bib"), "@book{secretkey, title = {Secret}, author = {Doe, Jane}, year = 2001}\n");
    writeFile(library + QStringLiteral("/ok.bib"), "@book{okkey, title = {Fine}, author = {Roe, Rick}, year = 2002}\n");
    QFile style(QStringLiteral(":/csl/apa.csl"));
    QVERIFY(style.open(QIODevice::ReadOnly));
    const QByteArray apa = style.readAll();
    writeFile(base + QStringLiteral("/style.csl"), apa);
    writeFile(library + QStringLiteral("/style.csl"), apa);
    QVERIFY(QDir().mkpath(library + QStringLiteral("/sub")));
    QVERIFY(QFile::link(base + QStringLiteral("/secret.bib"), library + QStringLiteral("/sub/linked.bib")));

    // A document in no library may name files in its folder and the folders in it.
    const QString text = QStringLiteral("---\nbibliography: [../secret.bib, ok.bib, sub/linked.bib, %1/secret.bib]\n"
                                        "csl: ../style.csl\n---\n\nA claim [@okkey] and [@secretkey].\n")
                             .arg(base);
    writeFile(library + QStringLiteral("/doc.md"), text.toUtf8());
    md::Source source;
    source.path = library + QStringLiteral("/doc.md");
    DocumentReading::Reading reading = DocumentReading::read(text, {}, source);
    QSet<QString> keys = reading.citations.keys();
    QVERIFY2(keys.contains(QStringLiteral("okkey")) && !keys.contains(QStringLiteral("secretkey")),
             qPrintable(problemsOf(reading.citations)));
    // The two spellings of secret.bib are one file, said once; the link, and the style.
    QCOMPARE(problemsOf(reading.citations).count(QStringLiteral("outside the document's folder")), 3);
    QVERIFY2(!reading.citations.style, qPrintable(problemsOf(reading.citations)));

    // In a library, the library is the bound.
    md::Source inLibrary;
    inLibrary.path = library + QStringLiteral("/sub/doc.md");
    inLibrary.libraryRoot = library;
    reading = DocumentReading::read(QStringLiteral("---\nbibliography: [../ok.bib, ../../secret.bib]\ncsl: ../style.csl\n"
                                                   "---\n\nA claim [@okkey].\n"),
                                    {}, inLibrary);
    keys = reading.citations.keys();
    QVERIFY2(keys.contains(QStringLiteral("okkey")) && !keys.contains(QStringLiteral("secretkey")),
             qPrintable(problemsOf(reading.citations)));
    QCOMPARE(problemsOf(reading.citations).count(QStringLiteral("outside the library")), 1);
    QVERIFY2(reading.citations.style, qPrintable(problemsOf(reading.citations)));

    // An included chapter's own front matter is held to the same bound.
    writeFile(library + QStringLiteral("/sub/chapter.md"), "---\nbibliography: ../../secret.bib\n---\n\nSee [@secretkey].\n");
    md::FileIncludeResolver resolver;
    inLibrary.resolver = &resolver;
    reading = DocumentReading::read(QStringLiteral("---\nbibliography: ../ok.bib\n---\n\nchapter.md\n"), {}, inLibrary);
    QVERIFY2(!reading.citations.keys().contains(QStringLiteral("secretkey")), qPrintable(problemsOf(reading.citations)));
    QVERIFY2(problemsOf(reading.citations).contains(QStringLiteral("outside the library")),
             qPrintable(problemsOf(reading.citations)));

    // What the writer chooses in Document settings or on the command line
    // is read wherever it is.
    Dialect::Layers layers;
    layers.document.bibliography = QStringList{base + QStringLiteral("/secret.bib")};
    layers.document.citationStyle = base + QStringLiteral("/style.csl");
    reading = DocumentReading::read(QStringLiteral("A claim [@secretkey].\n"), layers, source);
    QVERIFY2(reading.citations.keys().contains(QStringLiteral("secretkey")), qPrintable(problemsOf(reading.citations)));
    QVERIFY2(reading.citations.style, qPrintable(problemsOf(reading.citations)));
    const Ran ran = runCli(base, {QStringLiteral("check"), library + QStringLiteral("/doc.md"), QStringLiteral("--bibliography"),
                                  base + QStringLiteral("/secret.bib"), QStringLiteral("--csl"),
                                  base + QStringLiteral("/style.csl"), QStringLiteral("--no-render")});
    QVERIFY2(!ran.out.contains(QStringLiteral("outside")) && !ran.out.contains(QStringLiteral("secretkey")),
             qPrintable(ran.said()));
}

namespace {

// Runs the work on a worker of a thread pool, with the stack such a worker
// has, as the preview and the exports read a document.
template <typename Work>
auto onWorker(Work work)
{
    QThreadPool pool;
    QFuture<decltype(work())> future = QtConcurrent::run(&pool, work);
    future.waitForFinished();
    return future.result();
}

QByteArray style(int nesting, int macros)
{
    QByteArray xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                     "<style xmlns=\"http://purl.org/net/xbiblio/csl\" class=\"in-text\" version=\"1.0\">\n"
                     "<info><title>Deep</title><id>deep</id><updated>2024-01-01T00:00:00+00:00</updated></info>\n";
    for (int i = 0; i < macros; ++i) {
        xml += "<macro name=\"m" + QByteArray::number(i) + "\">";
        xml += i + 1 < macros ? "<group><text macro=\"m" + QByteArray::number(i + 1) + "\"/></group>"
                              : QByteArray("<text variable=\"title\"/>");
        xml += "</macro>\n";
    }
    xml += "<citation><layout>";
    for (int i = 0; i < nesting; ++i) xml += "<group>";
    xml += macros ? "<text macro=\"m0\"/>" : "<text variable=\"title\"/>";
    for (int i = 0; i < nesting; ++i) xml += "</group>";
    xml += "</layout></citation>\n</style>\n";
    return xml;
}

QString cited(const QString &folder, const QByteArray &csl)
{
    writeFile(folder + QStringLiteral("/deep.csl"), csl);
    writeFile(folder + QStringLiteral("/refs.json"), R"([{"id": "doe", "type": "book", "title": "A Title"}])");
    return onWorker([folder] {
        Dialect::Layers layers;
        layers.flags.citationStyle = folder + QStringLiteral("/deep.csl");
        layers.flags.bibliography = QStringList{folder + QStringLiteral("/refs.json")};
        md::Source source;
        source.path = folder + QStringLiteral("/doc.md");
        const DocumentReading::Reading reading = DocumentReading::read(QStringLiteral("A claim [@doe].\n"), layers, source);
        QStringList said;
        for (const DocumentCitations::Problem &problem : reading.citations.problems) said.append(problem.message);
        return reading.citations.style ? QStringLiteral("formatted: ") + md::toHtml(reading.document, reading.htmlOptions())
                                       : said.join(u'\n');
    });
}

QString bibliographyProblems(const BibliographyDatabase &db)
{
    QStringList all;
    for (const BibliographyDiagnostic &d : db.diagnostics())
        if (d.severity == BibliographyDiagnostic::Severity::Error) all.append(d.message);
    return all.join(u'\n');
}

} // namespace

void deepStylesAreRefused()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    // Real styles nest a few levels deep, and through their macros a few
    // dozen: such a style is read and formats on a worker.
    QString said = cited(folder.path(), style(100, 0));
    QVERIFY2(said.startsWith(QStringLiteral("formatted: ")) && said.contains(QStringLiteral("A Title")), qPrintable(said));
    said = cited(folder.path(), style(120, 80));
    QVERIFY2(said.startsWith(QStringLiteral("formatted: ")) && said.contains(QStringLiteral("A Title")), qPrintable(said));

    // Deeper is refused, with the reason, however deep.
    for (const int nesting : {129, 1000, 10000, 100000}) {
        said = cited(folder.path(), style(nesting, 0));
        QVERIFY2(said.contains(QStringLiteral("more than 128 deep")), qPrintable(QString::number(nesting) + u' ' + said.left(300)));
    }
    for (const int macros : {100, 5000, 50000}) {
        said = cited(folder.path(), style(1, macros));
        QVERIFY2(said.contains(QStringLiteral("more than 256 deep")), qPrintable(QString::number(macros) + u' ' + said.left(300)));
    }
    // A locale is read by the same reader.
    QByteArray locale = "<locale xmlns=\"http://purl.org/net/xbiblio/csl\" xml:lang=\"de-DE\">";
    for (int i = 0; i < 20000; ++i) locale += "<terms>";
    QString error;
    QVERIFY(!onWorker([&] { return bool(CslLocale::fromXml(locale, &error)); }));
    QVERIFY2(error.contains(QStringLiteral("more than 128 deep")), qPrintable(error));
}

void deepBibliographiesAreRefused()
{
    // Nesting a real bibliography has, read on a worker.
    const auto read = [](const QByteArray &data, BibliographyDatabase::Format format) {
        return onWorker([data, format] { return BibliographyDatabase::fromData(data, format, QStringLiteral("deep")); });
    };
    QByteArray fine = "@book{doe, title = {" + QByteArray(200, '{') + "Deep" + QByteArray(200, '}') + "}}\n";
    BibliographyDatabase db = read(fine, BibliographyDatabase::Format::BibLaTeX);
    QVERIFY2(db.find(QStringLiteral("doe")) && bibliographyProblems(db).isEmpty(), qPrintable(bibliographyProblems(db)));

    // Braces, commands and declarations past 256 levels refuse the file.
    for (const int depth : {300, 5000, 100000}) {
        const QList<QByteArray> values{
            QByteArray(depth, '{') + "x" + QByteArray(depth, '}'),
            "{" + QByteArray("\\textit{").repeated(depth) + "x" + QByteArray(depth, '}') + "}",
            "{" + QByteArray("\\em ").repeated(depth) + "x}",
            "{" + QByteArray("\\'{").repeated(depth) + "x" + QByteArray(depth, '}') + "}",
        };
        for (const QByteArray &value : values) {
            db = read("@book{doe, author = {Doe, Jane}, title = " + value + "}\n@book{roe, title = {Fine}}\n",
                      BibliographyDatabase::Format::BibLaTeX);
            QVERIFY2(bibliographyProblems(db).contains(QStringLiteral("more than 256 deep")),
                     qPrintable(QString::number(depth) + u' ' + QString::fromLatin1(value.left(40))));
            QVERIFY(!db.find(QStringLiteral("doe")) && !db.find(QStringLiteral("roe")));
            // Nor are names a way round it.
            db = read("@book{doe, author = " + value + "}\n", BibliographyDatabase::Format::BibLaTeX);
            QVERIFY2(bibliographyProblems(db).contains(QStringLiteral("more than 256 deep")), qPrintable(QString::number(depth)));
        }

        // CSL YAML and CSL JSON.
        db = read("- id: doe\n  title: " + QByteArray(depth, '[') + "x" + QByteArray(depth, ']') + "\n",
                  BibliographyDatabase::Format::CslYaml);
        QVERIFY2(bibliographyProblems(db).contains(QStringLiteral("more than 256 deep")),
                 qPrintable(QString::number(depth) + u' ' + bibliographyProblems(db)));
        db = read("[{\"id\": \"doe\", \"note\": " + QByteArray(depth, '[') + QByteArray(depth, ']') + "}]",
                  BibliographyDatabase::Format::CslJson);
        QVERIFY2(!bibliographyProblems(db).isEmpty() && !db.find(QStringLiteral("doe")),
                 qPrintable(QString::number(depth) + u' ' + bibliographyProblems(db)));
        if (depth < 1000)
            QVERIFY2(bibliographyProblems(db).contains(QStringLiteral("more than 256 deep")), qPrintable(bibliographyProblems(db)));
    }
    db = read("- id: doe\n  title: [[[[x]]]]\n", BibliographyDatabase::Format::CslYaml);
    QVERIFY2(bibliographyProblems(db).isEmpty() && db.find(QStringLiteral("doe")), qPrintable(bibliographyProblems(db)));
}

void deepMarkdownStaysWithinTheStack()
{
    // The engine nests blocks 1,000 deep and inlines 64 deep, and shows
    // deeper nesting as the text it is: 100,000 levels are read on a worker.
    const int n = 100000;
    const QStringList documents{
        QString(n, u'>') + QStringLiteral(" deep\n"),
        QStringLiteral("- ").repeated(n) + QStringLiteral("deep\n"),
        QStringLiteral("1. ").repeated(n) + QStringLiteral("deep\n"),
        QStringLiteral("> - ").repeated(n) + QStringLiteral("deep\n"),
        QStringLiteral("*a ").repeated(n) + QStringLiteral("deep") + QStringLiteral(" b*").repeated(n) + u'\n',
        QString(n, u'[') + QStringLiteral("deep") + QString(n, u']') + u'\n',
        QStringLiteral("[a").repeated(n) + QStringLiteral("deep") + QStringLiteral("](/u)").repeated(n) + u'\n',
        QStringLiteral("<b>").repeated(n) + QStringLiteral("deep") + QStringLiteral("</b>").repeated(n) + u'\n',
        QStringLiteral("<div>\n").repeated(n) + QStringLiteral("deep\n") + QStringLiteral("</div>\n").repeated(n),
        QStringLiteral("^[").repeated(n) + QStringLiteral("deep") + QString(n, u']') + u'\n',
    };
    for (const QString &text : documents) {
        const QString out = onWorker([text] {
            md::Source source;
            const DocumentReading::Reading reading = DocumentReading::read(text, {}, source);
            const auto parts = preview::parts(reading.document, &reading.formatted);
            return md::toHtml(reading.document, reading.htmlOptions()) + reading.document.plainText()
                   + QString::number(parts.size());
        });
        QVERIFY2(out.contains(QStringLiteral("deep")), qPrintable(text.left(20)));
    }
}

void bibtexStringsCannotExplode()
{
    const auto messages = [](const BibliographyDatabase &db, BibliographyDiagnostic::Severity severity) {
        QStringList all;
        for (const BibliographyDiagnostic &d : db.diagnostics())
            if (d.severity == severity) all.append(d.message);
        return all.join(u'\n');
    };
    using Severity = BibliographyDiagnostic::Severity;

    // Each @string twice the one before: LEVELS levels make 2^LEVELS copies
    // of "xx" out of a file of a few hundred bytes. The file is refused once
    // what it expands to passes four times its size, at once and in little
    // memory.
    for (const int levels : {20, 27, 39}) {
        QByteArray bib = "@string{a0 = \"xx\"}\n";
        for (int i = 1; i <= levels; ++i)
            bib += "@string{a" + QByteArray::number(i) + " = a" + QByteArray::number(i - 1) + " # a" +
                   QByteArray::number(i - 1) + "}\n";
        bib += "@book{k, title = a" + QByteArray::number(levels) + "}\n";
        QElapsedTimer clock;
        clock.start();
        const BibliographyDatabase db = BibliographyDatabase::fromData(bib, BibliographyDatabase::Format::BibLaTeX, {});
        QVERIFY2(clock.elapsed() < timeLimit(500), qPrintable(QString::number(clock.elapsed())));
        QVERIFY2(messages(db, Severity::Error).contains(QStringLiteral("expands to more than")),
                 qPrintable(messages(db, Severity::Error)));
        QVERIFY(!db.find(QStringLiteral("k")));
    }

    // A value is cut at 64 Ki characters, with a warning, however it is made:
    // written out, from a @string, or joined from many.
    const QByteArray big(70000, 'y');
    QByteArray bib = "@string{big = \"" + big + "\"}\n@book{one, title = {" + big + "}}\n@book{two, title = big}\n"
                     "@book{three, note = big" + QByteArray(" # big").repeated(5000) + ", title = {Short}}\n";
    QElapsedTimer clock;
    clock.start();
    const BibliographyDatabase db = BibliographyDatabase::fromData(bib, BibliographyDatabase::Format::BibLaTeX, {});
    QVERIFY2(clock.elapsed() < timeLimit(1000), qPrintable(QString::number(clock.elapsed())));
    QVERIFY2(messages(db, Severity::Error).isEmpty(), qPrintable(messages(db, Severity::Error)));
    const QString warnings = messages(db, Severity::Warning);
    QVERIFY2(warnings.count(QStringLiteral("cut short")) >= 3, qPrintable(warnings.left(1000)));
    for (const QString &key : {QStringLiteral("one"), QStringLiteral("two")}) {
        const CslReference *reference = db.find(key);
        QVERIFY(reference);
        const QString title = reference->field(QStringLiteral("title"));
        QVERIFY2(title.size() <= 65536 && title.size() > 60000, qPrintable(QString::number(title.size())));
    }
    QVERIFY(db.find(QStringLiteral("three")));
}

void quadraticInputsTakeLinearTime()
{
    const md::Options writer = md::Options::forProfile(md::Profile::Writer);
    QElapsedTimer clock;

    // 100,000 link reference definitions in one paragraph, about 1.5 MB.
    QString text;
    for (int i = 0; i < 100000; ++i) text += QStringLiteral("[r%1]: /u%1\n").arg(i);
    text += QStringLiteral("\n[r0] and [r99999]\n");
    clock.start();
    const md::Document refs = md::parse(text, writer);
    const qint64 refsTime = clock.elapsed();
    QVERIFY(md::toHtml(refs).contains(QStringLiteral("<a href=\"/u99999\">r99999</a>")));
    QVERIFY2(refsTime < timeLimit(1500), qPrintable(QString::number(refsTime)));

    // 100,000 unclosed comments, instructions, declarations and CDATA
    // sections, as an HTML block and in a paragraph.
    for (const QString &opening : {QStringLiteral("<!--"), QStringLiteral("<?"), QStringLiteral("<!X"),
                                   QStringLiteral("<![CDATA[")}) {
        for (const QString &before : {QString(), QStringLiteral("Text ")}) {
            const QString unclosed = before + opening.repeated(100000) + u'\n';
            clock.restart();
            const QString html = md::toHtml(md::parse(unclosed, writer));
            const qint64 time = clock.elapsed();
            QVERIFY(!html.isEmpty());
            QVERIFY2(time < timeLimit(1500), qPrintable(before + opening + u' ' + QString::number(time)));
        }
    }
    // What is closed after all is still a comment, which the page leaves out.
    QCOMPARE(md::toHtml(md::parse(QStringLiteral("<!-- a <!-- b -->\n\nAfter\n"), writer)), QStringLiteral("<p>After</p>\n"));
    md::Options standard = md::Options::forProfile(md::Profile::Standard);
    QCOMPARE(md::toHtml(md::parse(QStringLiteral("<div><!-- a <? b <!-- c --> d</div>\n"), standard)),
             QStringLiteral("<p>&lt;div&gt; d&lt;/div&gt;</p>\n"));

    // 40,000 authors, 5,000 to each of eight works (a value holds at most
    // 64 Ki characters), about 500 KB.
    QByteArray bib;
    for (int work = 0; work < 8; ++work) {
        bib += "@book{k" + QByteArray::number(work) + ", title = {x}, author = {";
        for (int i = 0; i < 5000; ++i) bib += (i ? " and A" : "A") + QByteArray::number(i) + " B";
        bib += "}}\n";
    }
    clock.restart();
    const BibliographyDatabase db = BibliographyDatabase::fromData(bib, BibliographyDatabase::Format::BibLaTeX, {});
    const qint64 namesTime = clock.elapsed();
    for (int work = 0; work < 8; ++work) {
        const CslReference *reference = db.find(QStringLiteral("k%1").arg(work));
        QVERIFY(reference);
        QCOMPARE(reference->names.value(QStringLiteral("author")).size(), 5000);
        QCOMPARE(reference->names.value(QStringLiteral("author")).last().given, QStringLiteral("A4999"));
    }
    QVERIFY2(namesTime < timeLimit(1500), qPrintable(QString::number(namesTime)));
}

namespace {

const QString outOfMemory = QStringLiteral("not enough memory");

// Every job a test made fail works again once the test ends, however it ends.
auto failuresUndone()
{
    return qScopeGuard([] { WorkerFailure::failing = 0; });
}

void typeInto(Document *document)
{
    document->buffer()->replace({0, 0}, QStringLiteral("x"));
}

} // namespace

void workerFailuresAreReported()
{
    const auto undo = failuresUndone();
    using WorkerFailure::Job;
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = QFileInfo(files.path()).canonicalFilePath();
    const QString path = root + QStringLiteral("/doc.md");
    writeFile(path, "# Title\n\nSome words.\n");
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QTRY_VERIFY(shell.document() && shell.document()->path() == path);
    Document *document = shell.document();
    DocumentParse *parse = document->parse();
    QTRY_VERIFY(!parse->parsing());

    // The document's parse: one that fails says why, and is not made again
    // for the same text; the text as it is next is parsed as ever.
    WorkerFailure::setFailing(Job::Parse, true);
    QSignalSpy failed(parse, &DocumentParse::failed);
    typeInto(document);
    parse->request();
    QTRY_VERIFY(!failed.isEmpty());
    QVERIFY(!parse->parsing());
    QVERIFY2(parse->failure().contains(outOfMemory), qPrintable(parse->failure()));
    const int parses = parse->parses();
    QVERIFY(!parse->request());
    QTest::qWait(50);
    QVERIFY(!parse->parsing());
    QCOMPARE(parse->parses(), parses);

    // Whatever waits for it is told, says why, and is ready again: Check
    // document, the export dialog's reading, an export and the print preview.
    DocumentChecker checker;
    checker.setBackend(shell.backend.get());
    checker.check(document);
    QTRY_VERIFY(!checker.checking());
    QVERIFY2(checker.summary().contains(outOfMemory), qPrintable(checker.summary()));
    Exports *exports = shell.backend->exports();
    exports->read(document);
    QTRY_VERIFY(!exports->reading());
    QSignalSpy problem(exports, &Exports::problem);
    exports->exportTo(document, QStringLiteral("html"), {}, root + QStringLiteral("/out.html"));
    QTRY_VERIFY(!exports->busy());
    QCOMPARE(problem.size(), 1);
    QVERIFY2(problem.first().at(1).toString().contains(outOfMemory), qPrintable(problem.first().at(1).toString()));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/out.html")));
    exports->preview()->show(document, QStringLiteral("export"), {});
    QTRY_VERIFY(!exports->preview()->busy());

    // Once a parse can be made, all of it works again.
    WorkerFailure::setFailing(Job::Parse, false);
    typeInto(document);
    QTRY_VERIFY(parse->request());
    QVERIFY(parse->failure().isEmpty());
    exports->exportTo(document, QStringLiteral("html"), {}, root + QStringLiteral("/out.html"));
    QTRY_VERIFY(!exports->busy());
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/out.html")));
    checker.check(document);
    QTRY_VERIFY(!checker.checking());
    QVERIFY2(!checker.summary().contains(outOfMemory), qPrintable(checker.summary()));

    // Each one's own work on the parse fails as safely.
    WorkerFailure::setFailing(Job::Check, true);
    checker.check(document);
    QTRY_VERIFY(!checker.checking());
    QVERIFY2(checker.summary().contains(outOfMemory), qPrintable(checker.summary()));
    WorkerFailure::setFailing(Job::Export, true);
    problem.clear();
    exports->exportTo(document, QStringLiteral("html"), {}, root + QStringLiteral("/again.html"));
    QTRY_VERIFY(!exports->busy());
    QCOMPARE(problem.size(), 1);
    QVERIFY2(problem.first().at(1).toString().contains(outOfMemory), qPrintable(problem.first().at(1).toString()));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/again.html")));
    exports->preview()->show(document, QStringLiteral("export"), {});
    QTRY_VERIFY(!exports->preview()->busy());
    WorkerFailure::failing = 0;
    problem.clear();
    exports->exportTo(document, QStringLiteral("html"), {}, root + QStringLiteral("/again.html"));
    QTRY_VERIFY(!exports->busy());
    QVERIFY(problem.isEmpty());
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/again.html")));
}

void previewSurvivesAFailedRead()
{
    const auto undo = failuresUndone();
    using WorkerFailure::Job;
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = QFileInfo(files.path()).canonicalFilePath();
    const QString path = root + QStringLiteral("/doc.md");
    writeFile(path, "# Title\n\nSome words.\n");
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QTRY_VERIFY(shell.document() && shell.document()->path() == path);
    Document *document = shell.document();
    QVERIFY(QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QStringLiteral("viewSplit"))));
    auto *preview = qobject_cast<Preview *>(shell.object(QStringLiteral("preview")));
    QVERIFY(preview);
    QTRY_VERIFY(preview->current());

    // The parse it waits for fails: it says why, in the notice too, and
    // shows the next text it is given.
    WorkerFailure::setFailing(Job::Parse, true);
    typeInto(document);
    QTRY_VERIFY2(preview->failure().contains(outOfMemory), qPrintable(preview->failure()));
    QTRY_VERIFY2(shell.backend->problemDetail().contains(outOfMemory), qPrintable(shell.backend->problemDetail()));
    // A heading a link asks for is not found in text that cannot be read,
    // rather than waited for.
    QSignalSpy headings(preview, &Preview::headingFound);
    const int asked = preview->findHeading(QStringLiteral("title"));
    QTRY_COMPARE(headings.size(), 1);
    QCOMPARE(headings.first().at(0).toInt(), asked);
    QCOMPARE(headings.first().at(2).toInt(), -1);
    WorkerFailure::setFailing(Job::Parse, false);
    typeInto(document);
    QTRY_VERIFY(preview->current());
    QVERIFY(preview->failure().isEmpty());

    // Its own work on the parse fails as safely.
    WorkerFailure::setFailing(Job::Preview, true);
    typeInto(document);
    QTRY_VERIFY2(preview->failure().contains(outOfMemory), qPrintable(preview->failure()));
    WorkerFailure::setFailing(Job::Preview, false);
    typeInto(document);
    QTRY_VERIFY(preview->current());
    QVERIFY(preview->failure().isEmpty());

    // So does reading a picture.
    WorkerFailure::setFailing(Job::Picture, true);
    writeFile(root + QStringLiteral("/picture.png"), "not read");
    PreviewResources resources;
    md::Destination destination;
    destination.kind = md::Destination::Kind::LocalFile;
    destination.path = root + QStringLiteral("/picture.png");
    destination.absolute = true;
    resources.image(destination, destination.path, {root});
    QTRY_COMPARE(resources.pending(), 0);
    const preview::Picture picture = resources.image(destination, destination.path, {root});
    QCOMPARE(picture.state, preview::Picture::Failed);
    QVERIFY2(picture.diagnostic.contains(outOfMemory), qPrintable(picture.diagnostic));
}

void searchesSurviveAFailedJob()
{
    const auto undo = failuresUndone();
    using WorkerFailure::Job;
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString base = QFileInfo(files.path()).canonicalFilePath();
    const QString notes = base + QStringLiteral("/notes");
    writeFile(notes + QStringLiteral("/a.md"), "needle\n");
    writeFile(notes + QStringLiteral("/b.md"), "a needle too\n");
    const AppPaths paths = AppPaths::under(base + QStringLiteral("/app"));
    DocumentStore store(paths);
    Settings settings(paths.settingsFile());
    settings.setLibraryFolders({notes});
    Document *document = store.open(notes + QStringLiteral("/a.md"));
    QVERIFY(document);

    // Finding in the document: the search that fails says why and ends.
    DocumentFind find;
    find.setDocument(document);
    find.setActive(true);
    WorkerFailure::setFailing(Job::Search, true);
    find.setPattern(QStringLiteral("needle"));
    QTRY_VERIFY2(find.error().contains(outOfMemory), qPrintable(find.error()));
    WorkerFailure::setFailing(Job::Search, false);
    find.setPattern(QStringLiteral("need"));
    QTRY_COMPARE(find.count(), 1);
    QVERIFY(find.error().isEmpty());

    // Searching the library: what cannot be searched is counted as unread,
    // and the search ends.
    FolderSearch search(paths, &store, &settings);
    search.setPattern(QStringLiteral("needle"));
    WorkerFailure::setFailing(Job::Search, true);
    search.start();
    QTRY_VERIFY_WITH_TIMEOUT(search.finished(), 10000);
    QVERIFY(!search.searching());
    QVERIFY(search.unreadable() >= 1);
    WorkerFailure::setFailing(Job::Search, false);
    search.start();
    QTRY_VERIFY_WITH_TIMEOUT(search.finished(), 10000);
    QCOMPARE(search.filesMatched(), 2);
    QCOMPARE(search.unreadable(), 0);
}

void restoredTabsAreReadWhenShown()
{
    const auto undo = failuresUndone();
    using WorkerFailure::Job;
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = QFileInfo(files.path()).canonicalFilePath();
    const QString app = root + QStringLiteral("/app");
    QStringList paths;
    SessionState state;
    WindowState window;
    for (const char *name : {"a.md", "b.md", "c.md"}) {
        const QString path = root + u'/' + QString::fromLatin1(name);
        writeFile(path, "# A chapter\n\nSome words.\n");
        paths.append(path);
        ViewState view;
        view.documentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        view.path = path;
        view.mode = ViewState::Split;
        window.tabs.append(view);
    }
    window.activeTab = 1;
    window.size = QSize(1000, 700);
    state.windows.append(window);
    QVERIFY(Session::save(AppPaths::under(app), state));

    // Say the last run ended reading the document in front: this time its
    // parse fails as well. The launch goes on, and the preview says why.
    WorkerFailure::setFailing(Job::Parse, true);
    ShellFixture shell({.settings = R"({"freshDraftOnLaunch": false})", .root = app});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    TabModel *tabs = shell.context()->tabs();
    QTRY_COMPARE(tabs->rowCount(), 3);
    QTRY_VERIFY(shell.document() && shell.document()->path() == paths.at(1));
    Document *shown = shell.document();
    auto *preview = qobject_cast<Preview *>(shell.object(QStringLiteral("preview")));
    QVERIFY(preview);
    QTRY_VERIFY2(preview->failure().contains(outOfMemory), qPrintable(preview->failure()));
    // The tabs behind it are not read at all until they are shown.
    QTest::qWait(200);
    for (int i = 0; i < tabs->rowCount(); ++i) {
        Document *document = tabs->documentAt(i);
        if (!document || document == shown) continue;
        QCOMPARE(document->parse()->parses(), 0);
        QVERIFY(!document->parse()->parsing());
    }
    WorkerFailure::setFailing(Job::Parse, false);
    typeInto(shown);
    QTRY_VERIFY(preview->current());
    QVERIFY(preview->failure().isEmpty());
}

} // namespace HostileTests
