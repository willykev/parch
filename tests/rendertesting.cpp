#include "rendertesting.h"
#include <sched.h>

#include "testfiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMutex>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTest>

#include <cmath>
#include <chrono>
#include <fcntl.h>
#include <csignal>
#include <sys/ptrace.h>
#include <unistd.h>
#include <sys/wait.h>

namespace {
QMutex fixtureMutex;
QHash<QString, QString> fixtureLogs;

// The test driver supplies observations and sample drawings inside the helper's
// home before it runs. The helper receives the same filesystem policy as a real drawing.
void prepareDrawingFixture(const HelperRun &run, const QString &home)
{
    const QDir fixture = QFileInfo(run.program).absoluteDir();
    QString log;
    {
        QMutexLocker lock(&fixtureMutex);
        log = fixtureLogs.value(fixture.absolutePath());
    }
    if (log.isEmpty()) return;
    QStringList suffixes{QString()};
    if (run.input.startsWith("sleep")) suffixes << QStringLiteral(".child");
    if (run.input.startsWith("environ")) suffixes << QStringLiteral(".environ") << QStringLiteral(".folder");
    for (const auto &suffix : suffixes) {
        QFile shared(log + suffix);
        if (!shared.open(QIODevice::WriteOnly | QIODevice::Append)) qFatal("cannot prepare drawing log");
        if (::link(QFile::encodeName(shared.fileName()).constData(),
                   QFile::encodeName(home + QStringLiteral("/log") + suffix).constData()) != 0) qFatal("cannot link drawing log");
    }
    for (const QString &name : {QStringLiteral("picture.png"), QStringLiteral("formula.svg")}) {
        const auto source = fixture.filePath(name);
        if (QFileInfo::exists(source) && !QFile::copy(source, home + u'/' + name)) qFatal("cannot prepare sample drawing");
    }
}

} // namespace

void installDrawingFixture(const QString &program)
{
    const QString fixture = QCoreApplication::applicationDirPath() + QStringLiteral("/drawing-fixture");
    const QByteArray bytes = readFile(fixture);
    QVERIFY2(!bytes.isEmpty(), qPrintable(fixture));
    QVERIFY(QDir().mkpath(QFileInfo(program).absolutePath()));
    QSaveFile target(program);
    QVERIFY2(target.open(QIODevice::WriteOnly), qPrintable(target.errorString()));
    QCOMPARE(target.write(bytes), bytes.size());
    QVERIFY2(target.setPermissions(QFile::permissions(fixture)), qPrintable(target.errorString()));
    QVERIFY2(target.commit(), qPrintable(target.errorString()));
}

void writeFakeHelpers(const QString &folder, const QString &log)
{
    HelperProcess::setHomeObserver(prepareDrawingFixture);
    installDrawingFixture(folder + QStringLiteral("/render-svg"));
    installDrawingFixture(folder + QStringLiteral("/merman-cli"));
    {
        QMutexLocker lock(&fixtureMutex);
        fixtureLogs.insert(QDir(folder).absolutePath(), log);
    }
    QImage picture(80, 40, QImage::Format_ARGB32);
    picture.fill(Qt::darkCyan);
    picture.save(folder + QStringLiteral("/picture.png"));
}

HelperSet realHelpers()
{
    const HelperSet helpers =
        HelperSet::in(QStringLiteral(PARCH_SOURCE_DIR "/build/helpers/") + HelperSet::architecture());
    if (!QFileInfo(helpers.ratex).isExecutable() || !QFileInfo(helpers.merman).isExecutable()) return {};
    return helpers;
}

RenderResults::RenderResults(RenderService &service)
{
    connect(&service, &RenderService::finished, this,
            [this](quint64 ticket, const RenderResult &result) { m_results.insert(ticket, result); });
}

bool RenderResults::wait(quint64 ticket, int timeoutMs)
{
    return QTest::qWaitFor([&] { return m_results.contains(ticket); }, QDeadlineTimer(timeoutMs));
}

RenderService::Options renderOptions(const HelperSet &helpers, const QString &folder)
{
    RenderService::Options options;
    options.helpers = helpers;
    options.findHelpers = false;
    options.cacheFolder = folder + QStringLiteral("/cache");
    options.temporaryFolder = folder + QStringLiteral("/tmp");
    return options;
}

namespace {

QByteArray inflate(const QByteArray &data)
{
    QByteArray sized(4, '\0');
    const quint32 hint = quint32(std::min<qsizetype>(data.size() * 16 + 4096, 64 * 1024 * 1024));
    sized[0] = char(hint >> 24);
    sized[1] = char(hint >> 16);
    sized[2] = char(hint >> 8);
    sized[3] = char(hint);
    return qUncompress(sized + data);
}

} // namespace

PdfContents inspectPdf(const QByteArray &pdf)
{
    PdfContents contents;
    static const QRegularExpression image(QStringLiteral(R"(/Subtype\s*/Image)"));
    qsizetype at = 0;
    while ((at = pdf.indexOf("stream", at)) >= 0) {
        if (at >= 3 && pdf.mid(at - 3, 3) == "end") {
            at += 6;
            continue;
        }
        const qsizetype dictionary = pdf.lastIndexOf("<<", at);
        const QString header = QString::fromLatin1(pdf.mid(dictionary, at - dictionary));
        qsizetype start = at + 6;
        if (start < pdf.size() && pdf.at(start) == '\r') ++start;
        if (start < pdf.size() && pdf.at(start) == '\n') ++start;
        const qsizetype end = pdf.indexOf("endstream", start);
        if (end < 0) break;
        at = end + 9;
        if (header.contains(image)) {
            ++contents.images;
            continue;
        }
        const QByteArray raw = pdf.mid(start, end - start);
        const QByteArray content = header.contains(QLatin1String("/FlateDecode")) ? inflate(raw) : raw;
        for (const QByteArray &line : content.split('\n')) {
            const QList<QByteArray> words = line.simplified().split(' ');
            if (words.isEmpty()) continue;
            const QByteArray &last = words.last();
            if (last == "m" || last == "l" || last == "c" || last == "v" || last == "y" || last == "re")
                ++contents.pathOperators;
            for (const QByteArray &word : words)
                if (word == "BT") ++contents.textObjects;
        }
    }
    return contents;
}

HelperMeasure measureHelper(const HelperRun &run, const QString &folder)
{
    HelperMeasure measure;
    QDir().mkpath(folder);
    for (const auto &[name, contents] : run.files) writeFile(folder + u'/' + name, contents);
    const QString input = folder + QStringLiteral("/input"), output = folder + QStringLiteral("/output");
    writeFile(input, run.input);
    QList<QByteArray> arguments{QFile::encodeName(run.program)};
    for (const QString &argument : run.arguments) arguments << argument.toUtf8();
    QList<char *> argv;
    for (QByteArray &argument : arguments) argv << argument.data();
    argv << nullptr;
    QList<QByteArray> environment{"HOME=" + QFile::encodeName(folder), "TMPDIR=" + QFile::encodeName(folder),
                                  "LANG=C.UTF-8"};
    QList<char *> envp;
    for (QByteArray &entry : environment) envp << entry.data();
    envp << nullptr;
    const QByteArray inputPath = QFile::encodeName(input), outputPath = QFile::encodeName(output),
                     folderPath = QFile::encodeName(folder);
    const auto started = std::chrono::steady_clock::now();
    // The helper is traced so that it stops just before it exits, when its
    // own high-water mark of resident memory can still be read. Measuring
    // through wait4() would also count the memory of the test process it
    // was forked from.
    const pid_t pid = ::fork();
    if (pid < 0) return measure;
    if (pid == 0) {
        const int in = ::open(inputPath.constData(), O_RDONLY);
        const int out = ::open(outputPath.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int null = ::open("/dev/null", O_WRONLY);
        if (in < 0 || out < 0 || null < 0 || ::dup2(in, 0) < 0 || ::dup2(out, 1) < 0 || ::dup2(null, 2) < 0 ||
            ::chdir(folderPath.constData()) != 0)
            ::_exit(126);
        ::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
        ::execve(argv.first(), argv.data(), envp.data());
        ::_exit(127);
    }
    int status = 0;
    // Stopped at its exec.
    if (::waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) return measure;
    ::ptrace(PTRACE_SETOPTIONS, pid, nullptr, reinterpret_cast<void *>(PTRACE_O_TRACEEXIT | PTRACE_O_EXITKILL));
    ::ptrace(PTRACE_CONT, pid, nullptr, nullptr);
    for (;;) {
        if (::waitpid(pid, &status, 0) != pid) return measure;
        if (WIFEXITED(status) || WIFSIGNALED(status)) break;
        if (WIFSTOPPED(status) && (status >> 8) == (SIGTRAP | (PTRACE_EVENT_EXIT << 8))) {
            for (const QString &line : QString::fromLatin1(readFile(QStringLiteral("/proc/%1/status").arg(pid))).split(u'\n'))
                if (line.startsWith(QLatin1String("VmHWM:")))
                    measure.peakMiB = line.section(u':', 1).trimmed().section(u' ', 0, 0).toDouble() / 1024.0;
            ::ptrace(PTRACE_CONT, pid, nullptr, nullptr);
        } else if (WIFSTOPPED(status)) {
            const int signal = WSTOPSIG(status) == SIGTRAP ? 0 : WSTOPSIG(status);
            ::ptrace(PTRACE_CONT, pid, nullptr, reinterpret_cast<void *>(qintptr(signal)));
        }
    }
    measure.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    measure.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    measure.output = readFile(output);
    return measure;
}

QJsonArray renderFixtures(const QString &name)
{
    QFile file(QStringLiteral(PARCH_SOURCE_DIR "/tests/fixtures/") + name + QStringLiteral("/corpus.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).array();
}

SvgSwatch::SvgSwatch() : m_previous(QDir::searchPaths(QStringLiteral("parchswatch")))
{
    QImage tile(8, 8, QImage::Format_RGB32);
    tile.fill(Qt::magenta);
    if (!tile.save(m_folder.filePath(QStringLiteral("tile.png")))) qFatal("could not make the colour tile");
    QDir::setSearchPaths(QStringLiteral("parchswatch"), {m_folder.path()});
}

SvgSwatch::~SvgSwatch()
{
    QDir::setSearchPaths(QStringLiteral("parchswatch"), m_previous);
}

QByteArray SvgSwatch::svg() const
{
    return R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 16 8"><rect width="8" height="8" fill="#008000"/><image x="8" width="8" height="8" xlink:href="parchswatch:tile.png"/></svg>)";
}

int pixelsNear(const QImage &image, const QColor &color, int tolerance)
{
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32);
    int count = 0;
    for (int y = 0; y < pixels.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
        for (int x = 0; x < pixels.width(); ++x) {
            const QRgb p = line[x];
            if (std::abs(qRed(p) - color.red()) + std::abs(qGreen(p) - color.green()) +
                    std::abs(qBlue(p) - color.blue()) <=
                tolerance)
                ++count;
        }
    }
    return count;
}

bool kernelGivesNetworkNamespaces()
{
    static const bool given = [] {
        const pid_t child = ::fork();
        if (child < 0) return false;
        if (child == 0) ::_exit(::unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0 ? 0 : 1);
        int status = 0;
        if (::waitpid(child, &status, 0) != child) return false;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }();
    return given;
}
