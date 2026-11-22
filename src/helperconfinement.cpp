#include "helperconfinement.h"
#include "renderhelpers.h"

#include <QFile>
#include <QScopeGuard>
#include <cerrno>
#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace HelperConfinement {
namespace {

int abi()
{
    static const int version = int(::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION));
    return version;
}

constexpr __u64 readFile = LANDLOCK_ACCESS_FS_READ_FILE;
constexpr __u64 readTree = readFile | LANDLOCK_ACCESS_FS_READ_DIR;
constexpr __u64 runFile = readFile | LANDLOCK_ACCESS_FS_EXECUTE;
constexpr __u64 homeAccess = readTree | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
    LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
    LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_SOCK;

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif
#ifndef LANDLOCK_ACCESS_NET_BIND_TCP
#define LANDLOCK_ACCESS_NET_BIND_TCP (1ULL << 0)
#define LANDLOCK_ACCESS_NET_CONNECT_TCP (1ULL << 1)
#endif
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15)
#endif
#ifndef LANDLOCK_ACCESS_FS_RESOLVE_UNIX
#define LANDLOCK_ACCESS_FS_RESOLVE_UNIX (1ULL << 16)
#endif
#ifndef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
#define LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET (1ULL << 0)
#define LANDLOCK_SCOPE_SIGNAL (1ULL << 1)
#endif
#ifndef LANDLOCK_ACCESS_NET_BIND_UDP
#define LANDLOCK_ACCESS_NET_BIND_UDP (1ULL << 2)
#define LANDLOCK_ACCESS_NET_CONNECT_SEND_UDP (1ULL << 3)
#endif

} // namespace

Restrictions restrictionsFor(int version)
{
    Restrictions rights{};
    if (version < 1) return rights;
    rights.handled_access_fs = homeAccess | LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_CHAR;
    if (version >= 2) rights.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
    if (version >= 3) rights.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
    if (version >= 4) rights.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
    if (version >= 5) rights.handled_access_fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    if (version >= 6) rights.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
    if (version >= 9) rights.handled_access_fs |= LANDLOCK_ACCESS_FS_RESOLVE_UNIX;
    if (version >= 10) rights.handled_access_net |= LANDLOCK_ACCESS_NET_BIND_UDP | LANDLOCK_ACCESS_NET_CONNECT_SEND_UDP;
    return rights;
}

namespace {

int newRules()
{
    const auto rights = restrictionsFor(abi());
    return int(::syscall(SYS_landlock_create_ruleset, &rights, sizeof rights, 0));
}

bool allow(int rules, const QByteArray &path, __u64 access, bool directory, bool optional = false)
{
    const int fd = ::open(path.constData(), O_PATH | O_CLOEXEC);
    if (fd < 0) return optional && errno == ENOENT;
    const auto close = qScopeGuard([fd] { ::close(fd); });
    struct stat info{};
    if (::fstat(fd, &info) != 0 || (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))) return false;
    const landlock_path_beneath_attr rule{access, fd};
    return ::syscall(SYS_landlock_add_rule, rules, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) == 0;
}

} // namespace

bool enter(int rules)
{
    return ::syscall(SYS_landlock_restrict_self, rules, 0) == 0;
}

bool available()
{
    static const bool works = [] {
        if (abi() < 1) return false;
        const int rules = newRules();
        if (rules < 0) return false;
        const auto close = qScopeGuard([rules] { ::close(rules); });
        const pid_t child = ::fork();
        if (child == 0) ::_exit(::prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) == 0 && enter(rules) ? 0 : 1);
        if (child < 0) return false;
        int status;
        while (::waitpid(child, &status, 0) < 0)
            if (errno != EINTR) return false;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }();
    return works;
}

QStringList missingProtections()
{
    const auto rights = restrictionsFor(available() ? abi() : 0);
    QStringList missing;
    if (!rights.handled_access_fs) missing << QStringLiteral("filesystem");
    const auto tcp = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
    const auto udp = LANDLOCK_ACCESS_NET_BIND_UDP | LANDLOCK_ACCESS_NET_CONNECT_SEND_UDP;
    if ((rights.handled_access_net & tcp) != tcp) missing << QStringLiteral("tcp");
    if (!(rights.scoped & LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET)) missing << QStringLiteral("abstract-unix-sockets");
    if (!(rights.scoped & LANDLOCK_SCOPE_SIGNAL)) missing << QStringLiteral("signals");
    if (!(rights.handled_access_fs & LANDLOCK_ACCESS_FS_RESOLVE_UNIX)) missing << QStringLiteral("pathname-unix-sockets");
    if ((rights.handled_access_net & udp) != udp) missing << QStringLiteral("udp");
    return missing;
}

int rulesFor(const HelperRun &run, const QString &home)
{
    const int rules = newRules();
    if (rules < 0) return -1;
    __u64 writable = homeAccess;
    if (abi() >= 2) writable |= LANDLOCK_ACCESS_FS_REFER;
    if (abi() >= 3) writable |= LANDLOCK_ACCESS_FS_TRUNCATE;
    bool ok = allow(rules, QFile::encodeName(home), writable, true) &&
              allow(rules, QFile::encodeName(run.program), runFile, false);
    for (const char *lib : {"/usr/lib", "/lib", "/lib64", "/usr/lib64"})
        ok = ok && allow(rules, lib, readTree | LANDLOCK_ACCESS_FS_EXECUTE, true, true);
    ok = ok && allow(rules, "/etc/ld.so.cache", readFile, false, true);
    if (run.readSystemFonts)
        ok = ok && allow(rules, "/usr/share/fonts", readTree, true, true);
    for (qsizetype i = 0; ok && i < run.arguments.size(); ++i) {
        if (run.arguments.at(i) == u"--font-file")
            ok = ++i < run.arguments.size() && allow(rules, QFile::encodeName(run.arguments.at(i)), readFile, false);
    }
    if (ok) return rules;
    ::close(rules);
    return -1;
}

} // namespace HelperConfinement
