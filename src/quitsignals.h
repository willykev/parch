#pragma once

#include <unistd.h>

class AppService;
class Backend;

// SIGTERM and SIGHUP, as the end of the session or a service manager sends
// them, end parch as quitting does, except that nothing can be refused.
namespace QuitSignals {
// How the signal paths leave the process: _exit, unless a test puts in its
// own. It must be safe to call from a signal handler.
using Exit = void (*)(int status);

// Catches both signals. The first writes the signal's number to a pipe, whose
// reading end this returns for the event loop to watch; a second, while parch
// is still ending, leaves at once with 128 plus its number. -1 when the
// signals cannot be caught.
int install(Exit exit = ::_exit);
// Puts back the signals' earlier handling and closes the pipe.
void uninstall();
// What parch does once its event loop has heard the first signal. From then
// on every request is refused with a reason, while the socket stays so that
// no second instance starts. Every text not in its file goes to recovery,
// the session is written and saves get waitMs to land; then the socket is
// closed. True when parch is to leave through its event loop. A save still
// running then has its text in recovery, and waiting for it at exit could
// take for ever, so parch leaves with exit(0) instead, and false is returned
// only by an exit that does not end the process.
bool end(Backend &backend, AppService &service, int waitMs, Exit exit = ::_exit);
} // namespace QuitSignals
