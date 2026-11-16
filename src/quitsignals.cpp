#include "quitsignals.h"

#include "appservice.h"
#include "backend.h"

#include <QCoreApplication>
#include <cerrno>
#include <csignal>
#include <fcntl.h>

namespace {
// The handler does nothing but write one byte, which is safe in a signal handler.
int quitPipe[2] = {-1, -1};
volatile std::sig_atomic_t quitting = 0;
QuitSignals::Exit exitNow = ::_exit;
struct sigaction earlierTerm {}, earlierHangUp {};

void onQuitSignal(int number)
{
    // A second signal while parch is ending ends it at once.
    if (quitting) {
        exitNow(128 + number);
        return;
    }
    quitting = 1;
    const int saved = errno;
    const unsigned char byte = static_cast<unsigned char>(number);
    [[maybe_unused]] const ssize_t written = ::write(quitPipe[1], &byte, 1);
    errno = saved;
}
} // namespace

namespace QuitSignals {
int install(Exit exit)
{
    if (quitPipe[0] >= 0 || ::pipe2(quitPipe, O_CLOEXEC | O_NONBLOCK) != 0) return -1;
    exitNow = exit;
    quitting = 0;
    struct sigaction action {};
    action.sa_handler = onQuitSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (::sigaction(SIGTERM, &action, &earlierTerm) != 0) {
        uninstall();
        return -1;
    }
    if (::sigaction(SIGHUP, &action, &earlierHangUp) != 0) {
        ::sigaction(SIGTERM, &earlierTerm, nullptr);
        uninstall();
        return -1;
    }
    return quitPipe[0];
}

void uninstall()
{
    if (quitPipe[0] < 0) return;
    ::sigaction(SIGTERM, &earlierTerm, nullptr);
    ::sigaction(SIGHUP, &earlierHangUp, nullptr);
    ::close(quitPipe[0]);
    ::close(quitPipe[1]);
    quitPipe[0] = quitPipe[1] = -1;
    quitting = 0;
    exitNow = ::_exit;
}

bool end(Backend &backend, AppService &service, int waitMs, Exit exit)
{
    service.refuse(QCoreApplication::translate("QuitSignals", "parch is closing. Open the files again once it has."));
    const bool settled = backend.end(waitMs);
    service.close();
    if (!settled) {
        // Leaving at once: an export stops first, and the files' ids are written.
        backend.endJobsBeforeLeaving(waitMs);
        exit(0);
    }
    return settled;
}
} // namespace QuitSignals
