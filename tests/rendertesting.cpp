#include "rendertesting.h"
#include <sched.h>

#include "testfiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTest>

#include <cmath>
#include <chrono>
#include <fcntl.h>
#include <csignal>
#include <sys/ptrace.h>
#include <unistd.h>
#include <sys/wait.h>

namespace {

void writeScript(const QString &path, const QString &text)
{
    writeFile(path, text.toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

// Quoted for the shell.
QString quoted(QString text) { return u'\'' + text.replace(u'\'', QStringLiteral("'\\''")) + u'\''; }

} // namespace

void writeFakeHelpers(const QString &folder, const QString &log)
{
    writeScript(folder + QStringLiteral("/render-svg"), QStringLiteral(R"(#!/bin/sh
PATH=/usr/bin:/bin
LOG=%1
echo "start $$" >> "$LOG"
color=black
size=16
while [ $# -gt 0 ]; do
    case "$1" in
    --color) color=$2; shift 2 ;;
    --font-size) size=$2; shift 2 ;;
    *) shift ;;
    esac
done
IFS= read -r formula
IFS= read -r ruled
case "$formula" in
slow*) sleep 0.3 ;;
sleep*) sleep 60 & echo $! > "$LOG.child"; wait ;;
flood*) head -c 4000000 /dev/zero; echo "end $$" >> "$LOG"; exit 0 ;;
environ*)
    tr '\000' '\n' < /proc/$$/environ > "$LOG.environ"
    cat /proc/net/dev > "$LOG.net"
    pwd > "$LOG.folder" ;;
refuse*)
    printf 'ERR    1 %s \342\200\224 Parse error: fake refusal of %s\n' "$formula" "$formula" >&2
    echo "end $$" >> "$LOG"
    exit 1 ;;
crash*) echo "end $$" >> "$LOG"; kill -SEGV $$ ;;
esac
width=${#formula}
deep=$((14 + 100 * size))
printf '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %s 20" width="%spt" height="20pt"><rect width="%s" height="14" fill="%s"/></svg>\n' "$width" "$width" "$width" "$color"
printf '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %s %s" width="%spt" height="%spt"></svg>\n' "$width" "$deep" "$width" "$deep"
printf 'OK     1 %s\nOK     2 %s\n' "$formula" "$ruled" >&2
echo "end $$" >> "$LOG"
)")
                                                          .arg(quoted(log)));
    writeScript(folder + QStringLiteral("/merman-cli"), QStringLiteral(R"(#!/bin/sh
PATH=/usr/bin:/bin
LOG=%1
DIR=%2
if [ "$1" = --version ]; then echo "merman-cli 0.0.7"; exit 0; fi
echo "start $$" >> "$LOG"
format=svg
scale=1
while [ $# -gt 0 ]; do
    case "$1" in
    --format) format=$2; shift 2 ;;
    --scale) scale=$2; shift 2 ;;
    *) shift ;;
    esac
done
source=$(cat)
echo "run $format $scale" >> "$LOG"
case "$source" in
refuse*)
    echo "Diagram parse error (flowchart-v2): fake refusal" >&2
    echo "end $$" >> "$LOG"
    exit 1 ;;
slow*) sleep 0.3 ;;
esac
if [ "$format" = svg ]; then
    printf '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 40 20" aria-roledescription="flowchart-v2"><text x="1" y="12">Fake label</text></svg>'
else
    cat "$DIR/picture.png"
fi
echo "end $$" >> "$LOG"
)")
                                                          .arg(quoted(log), quoted(folder)));
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
