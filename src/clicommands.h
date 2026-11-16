#pragma once

#include "apppaths.h"
#include "renderhelpers.h"

#include <QString>
#include <QStringList>

#include <optional>

class QIODevice;

// The commands parch runs from the terminal without a window: check,
// render, export, stats, skill and help. None of them needs a display or
// reads what the window was last set to; each prints text for people, or
// JSON with --json, and ends with one of the exit codes below.
//
//   0  done, and nothing wrong
//   1  the command line is wrong
//   2  the document has problems (check), or is incomplete (export)
//   3  a file could not be read or written, or would be replaced unasked
//   4  the export could not be made
namespace Cli {

enum Exit { Ok = 0, Usage = 1, Problems = 2, Io = 3, ExportFailed = 4 };

// What a command runs in. Every location is given, so that tests run each
// command beneath temporary folders, and in the test process.
struct Context {
    QString workingDirectory; // relative paths are taken from here
    QString home;             // where `skill install` installs
    AppPaths paths;           // parch's own files, read only with --use-saved-settings
    QIODevice *out = nullptr;
    QIODevice *err = nullptr;
    // The helpers `check` draws formulas and diagrams with; unset, they are
    // found where HelperSet::find() looks.
    std::optional<HelperSet> helpers;
    // Where `check` keeps what it drew, so that another check draws only
    // what changed; empty, the cache the window shares
    // (RenderCache::standardFolder()).
    QString renderCache;
};

// Whether a first argument is one of these commands, rather than a file to open.
bool isCommand(const QString &word);

// Runs a command. `arguments` are the program's name, the command, and
// what follows it.
int run(const QStringList &arguments, const Context &context);

// The commands, a line each, for parch --help.
QString summary();

// Text as it can be shown in a terminal: control characters other than
// line breaks and tabs become U+FFFD, so a document cannot drive the terminal.
QString printable(const QString &text);

} // namespace Cli
