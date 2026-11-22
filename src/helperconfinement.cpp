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

landlock_ruleset_attr restrictions()
{
    landlock_ruleset_attr rights{};
    rights.handled_access_fs = homeAccess | LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_CHAR;
    if (abi() >= 2) rights.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
    if (abi() >= 3) rights.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi() >= 4) rights.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi() >= 5) rights.handled_access_fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
    return rights;
}

int newRules()
{
    const auto rights = restrictions();
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
