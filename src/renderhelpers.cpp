#include "renderhelpers.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>

#include <csignal>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace {

// What a helper's child process writes first on its error output when the
// kernel would not give it a network namespace. Taken off again before the
// errors are read.
constexpr char kNoNetworkMarker[] = "\x01parch: no network namespace\n";

} // namespace

QString HelperSet::architecture()
{
    utsname name{};
    if (::uname(&name) != 0) return QStringLiteral("unknown");
    const QString machine = QString::fromLatin1(name.machine);
    if (machine == QLatin1String("arm64")) return QStringLiteral("aarch64");
    if (machine == QLatin1String("amd64")) return QStringLiteral("x86_64");
    return machine;
}

QStringList HelperSet::searchFolders()
{
    const QString chosen = qEnvironmentVariable("PARCH_HELPERS");
    if (!chosen.isEmpty()) return {QDir::cleanPath(chosen)};
    const QString application = QCoreApplication::applicationDirPath();
    const QString development = QStringLiteral("/helpers/") + architecture();
    return {application, QStringLiteral("/usr/lib/parch"), QDir::cleanPath(application + development),
            QDir::cleanPath(application + QStringLiteral("/..") + development)};
}

HelperSet HelperSet::in(const QString &folder)
{
    return {folder + QStringLiteral("/render-svg"), folder + QStringLiteral("/merman-cli")};
}

namespace {

// The version the `.pins` file beside a helper records for it ("ratex
// 0.1.15 <checksum>" a line), or nothing.
QString pinnedVersion(const QString &helper, QLatin1StringView name)
{
    QFile pins(QFileInfo(helper).absolutePath() + QStringLiteral("/.pins"));
    if (!pins.open(QIODevice::ReadOnly)) return {};
    const QStringList lines = QString::fromUtf8(pins.read(64 * 1024)).split(u'\n');
    for (const QString &line : lines) {
        const QStringList words = line.split(u' ', Qt::SkipEmptyParts);
        if (words.size() >= 2 && words.first() == name) return words.at(1);
    }
    return {};
}

} // namespace

QString HelperSet::ratexVersion() const
{
    if (ratex.isEmpty()) return {};
    const QString pinned = pinnedVersion(ratex, QLatin1StringView("ratex"));
    return pinned.isEmpty() ? QStringLiteral("RaTeX (version unknown)") : QStringLiteral("RaTeX ") + pinned;
}

QString HelperSet::mermanVersion() const
{
    if (merman.isEmpty()) return {};
    QString version = pinnedVersion(merman, QLatin1StringView("merman"));
    if (version.isEmpty()) {
        // "merman-cli 0.10.0"
        QProcess process;
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start(merman, {QStringLiteral("--version")});
        process.closeWriteChannel();
        if (process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
            const QStringList words = QString::fromUtf8(process.readAllStandardOutput().left(256)).split(
                QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (words.size() >= 2 && words.first() == u"merman-cli") version = words.at(1);
        } else {
            process.kill();
            process.waitForFinished(1000);
        }
    }
    return version.isEmpty() ? QStringLiteral("Merman (version unknown)") : QStringLiteral("Merman ") + version;
}

HelperSet HelperSet::find()
{
    HelperSet found;
    auto locate = [](const QString &name) {
        for (const QString &folder : searchFolders()) {
            const QFileInfo candidate(folder + u'/' + name);
            if (candidate.isFile() && candidate.isExecutable()) return candidate.absoluteFilePath();
        }
        return QString();
    };
    found.ratex = locate(QStringLiteral("render-svg"));
    found.merman = locate(QStringLiteral("merman-cli"));
    return found;
}

HelperProcess::HelperProcess(const HelperRun &run, const RenderLimits &limits, const QString &temporaryRoot,
                             QObject *parent)
    : QObject(parent), m_run(run), m_limits(limits), m_temporaryRoot(temporaryRoot)
{
}

HelperProcess::~HelperProcess()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        // Nothing waits for this one any more: stop it and its session, and
        // reap it here so that no zombie outlives the owner.
        m_done = true;
        if (const qint64 pid = m_process->processId(); pid > 0) ::kill(-pid_t(pid), SIGKILL);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
    delete m_process;
    delete m_home;
}

qint64 HelperProcess::processId() const { return m_process ? m_process->processId() : 0; }

void HelperProcess::start()
{
    m_clock.start();
    QDir().mkpath(m_temporaryRoot);
    m_home = new QTemporaryDir(m_temporaryRoot + QStringLiteral("/parch-helper-XXXXXX"));
    if (!m_home->isValid()) {
        m_outcome.errors = QStringLiteral("could not make a temporary folder in %1: %2")
                               .arg(m_temporaryRoot, m_home->errorString())
                               .toUtf8();
        m_done = true;
        QMetaObject::invokeMethod(this, [this] { emit finished(m_outcome); }, Qt::QueuedConnection);
        return;
    }
    const QString home = m_home->path();
    QDir(home).mkdir(QStringLiteral("tmp"));
    for (const auto &[name, contents] : std::as_const(m_run.files)) {
        QFile file(home + u'/' + name);
        if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) {
            m_outcome.errors = QStringLiteral("could not write %1 for the helper").arg(name).toUtf8();
            m_done = true;
            QMetaObject::invokeMethod(this, [this] { emit finished(m_outcome); }, Qt::QueuedConnection);
            return;
        }
    }

    m_process = new QProcess;
    m_process->setProgram(m_run.program);
    m_process->setArguments(m_run.arguments);
    m_process->setWorkingDirectory(home);
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("TMPDIR"), home + QStringLiteral("/tmp"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    m_process->setProcessEnvironment(environment);
    // A session of its own, so that the helper and anything it starts are
    // stopped together; no descriptors but the three pipes; default signal
    // handling; no core dumps; and no privileges it was not started with.
    m_process->setUnixProcessParameters(
        {QProcess::UnixProcessFlag::ResetSignalHandlers | QProcess::UnixProcessFlag::CloseFileDescriptors |
             QProcess::UnixProcessFlag::CreateNewSession | QProcess::UnixProcessFlag::DisconnectControllingTerminal |
             QProcess::UnixProcessFlag::ResetIds | QProcess::UnixProcessFlag::DisableCoreDumps,
         3});
    const rlim_t fileLimit = rlim_t(m_limits.maxOutputBytes);
    // Runs in the child between fork and exec: system calls only.
    m_process->setChildProcessModifier([fileLimit] {
        // Dies with the thread that started it, so that a helper never
        // outlives parch.
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        const rlimit files{fileLimit, fileLimit};
        ::setrlimit(RLIMIT_FSIZE, &files);
        // A user namespace makes a network namespace possible without
        // privileges; the new network namespace has no way out.
        if (::unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) {
            const ssize_t written = ::write(STDERR_FILENO, kNoNetworkMarker, sizeof kNoNetworkMarker - 1);
            (void)written;
        }
    });
    connect(m_process, &QProcess::readyReadStandardOutput, this, &HelperProcess::readOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &HelperProcess::readErrors);
    connect(m_process, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus status) { conclude(code, status == QProcess::CrashExit); });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_outcome.errors = QStringLiteral("could not start %1: %2")
                                   .arg(QFileInfo(m_run.program).fileName(), m_process->errorString())
                                   .toUtf8();
            conclude(-1, true);
        }
    });
    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    m_deadline->setInterval(m_limits.timeoutMs);
    connect(m_deadline, &QTimer::timeout, this, [this] { stop(HelperOutcome::TimedOut); });
    m_deadline->start();
    m_process->start(QIODevice::ReadWrite);
    if (m_process->state() != QProcess::NotRunning) {
        m_outcome.processId = m_process->processId();
        if (!m_run.input.isEmpty()) m_process->write(m_run.input);
        m_process->closeWriteChannel();
    }
}

void HelperProcess::cancel() { stop(HelperOutcome::Cancelled); }

void HelperProcess::stop(HelperOutcome::Status status)
{
    if (m_done || m_stopped || !m_process) return;
    m_stopped = true;
    m_stopReason = status;
    if (m_process->state() == QProcess::NotRunning) return;
    // The whole session: the helper is its leader and not yet reaped, so the
    // group cannot belong to anyone else.
    if (const qint64 pid = m_process->processId(); pid > 0) ::kill(-pid_t(pid), SIGKILL);
    m_process->kill();
}

bool HelperProcess::takeOutput()
{
    m_outcome.output += m_process->readAllStandardOutput();
    if (m_outcome.output.size() <= m_limits.maxOutputBytes) return false;
    m_outcome.output.truncate(m_limits.maxOutputBytes);
    return true;
}

void HelperProcess::takeErrors()
{
    // Read on to the end so that the helper never blocks on a full pipe,
    // keeping only the first part.
    const QByteArray more = m_process->readAllStandardError();
    const qint64 room = m_limits.maxErrorBytes - m_outcome.errors.size();
    if (room > 0) m_outcome.errors += more.left(room);
}

void HelperProcess::readOutput()
{
    if (!m_done && takeOutput()) stop(HelperOutcome::TooMuchOutput);
}

void HelperProcess::readErrors()
{
    if (!m_done) takeErrors();
}

void HelperProcess::conclude(int exitCode, bool crashed)
{
    if (m_done) return;
    m_done = true;
    if (m_deadline) m_deadline->stop();
    // What is still in the pipes; the helper is gone, so nothing is stopped.
    if (m_process) {
        if (takeOutput() && !m_stopped) {
            m_stopped = true;
            m_stopReason = HelperOutcome::TooMuchOutput;
        }
        takeErrors();
    }
    m_outcome.elapsedMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    m_outcome.exitCode = exitCode;
    if (m_stopped)
        m_outcome.status = m_stopReason;
    else if (crashed)
        m_outcome.status = HelperOutcome::Failed;
    else
        m_outcome.status = HelperOutcome::Finished;
    const QByteArray marker(kNoNetworkMarker);
    if (m_outcome.errors.startsWith(marker)) {
        m_outcome.errors.remove(0, marker.size());
        m_outcome.networkIsolated = false;
    } else {
        m_outcome.networkIsolated = m_outcome.processId > 0;
    }
    if (m_outcome.status == HelperOutcome::Failed && m_outcome.errors.isEmpty() && m_process)
        m_outcome.errors = QStringLiteral("%1 stopped: %2")
                               .arg(QFileInfo(m_run.program).fileName(), m_process->errorString())
                               .toUtf8();
    // The private home goes as soon as the helper has.
    delete m_home;
    m_home = nullptr;
    emit finished(m_outcome);
}

namespace {
struct KnownIdentity {
    qint64 size = -1;
    qint64 modifiedNs = 0;
    quint64 inode = 0;
    QString hash;
};
} // namespace

QString HelperIdentity::of(const QString &path)
{
    static QMutex mutex;
    static QHash<QString, KnownIdentity> known;
    struct stat status{};
    if (path.isEmpty() || ::stat(QFile::encodeName(path).constData(), &status) != 0) return {};
    const qint64 modified = qint64(status.st_mtim.tv_sec) * 1000000000 + status.st_mtim.tv_nsec;
    {
        QMutexLocker lock(&mutex);
        const auto it = known.constFind(path);
        if (it != known.constEnd() && it->size == status.st_size && it->modifiedNs == modified &&
            it->inode == quint64(status.st_ino))
            return it->hash;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    const QString result = QString::fromLatin1(hash.result().toHex());
    QMutexLocker lock(&mutex);
    known.insert(path, {qint64(status.st_size), modified, quint64(status.st_ino), result});
    return result;
}
