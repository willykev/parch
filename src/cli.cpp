#include "cli.h"

#include "clicommands.h"
#include "diskfile.h"

#include <QCoreApplication>

#include <algorithm>

QString versionLine()
{
    return QStringLiteral("parch ") + QStringLiteral(PARCH_VERSION);
}

QString openHelpText()
{
    // Translated as one block so a catalog can keep the columns aligned.
    return QCoreApplication::translate("cli",
        "Usage: parch [open] [--theme DIR] [--benchmark OUT] [FILE...]\n"
        "\n"
        "  open             Open the files that follow (optional; bare files also open)\n"
        "  --theme DIR      Read theme tokens from DIR instead of the Omarchy theme\n"
        "  --benchmark OUT  Measure the window on a copy of one FILE; write JSON to OUT\n"
        "  --version        Print the version and exit\n"
        "  --help           Print this help and exit\n");
}

QString helpText()
{
    QString text = openHelpText();
    text.insert(text.indexOf(u'\n'), QCoreApplication::translate("cli", "\n       parch COMMAND [options]"));
    return text + QCoreApplication::translate("cli", "\nCommands that need no display (parch help COMMAND for its options):\n") +
           Cli::summary();
}

QStringList absoluteFiles(const QStringList &files, const QString &workingDirectory)
{
    QStringList absolute;
    const QString folder = workingDirectory.endsWith(u'/') ? workingDirectory : workingDirectory + u'/';
    for (const QString &file : files) {
        if (file.isEmpty()) continue;
        // Spelled as the files parch records are, so that "./" or ".."
        // reach the same document.
        absolute.append(DiskFile::cleanPath(file.startsWith(u'/') ? file : folder + file));
    }
    return absolute;
}

QStringList withoutQtOptions(const QStringList &arguments)
{
    // The options QCoreApplication and QGuiApplication read and take out of
    // the arguments themselves, as they spell them: those that take the
    // next argument as their value, those that stand alone, and those that
    // carry their value after '='. Qt accepts each with one dash or two.
    static const QStringList takingValue = {
        QStringLiteral("-platform"),        QStringLiteral("-platformpluginpath"), QStringLiteral("-platformtheme"),
        QStringLiteral("-plugin"),          QStringLiteral("-qwindowgeometry"),    QStringLiteral("-qwindowicon"),
        QStringLiteral("-qwindowtitle"),    QStringLiteral("-style"),              QStringLiteral("-session"),
        QStringLiteral("-qmljsdebugger")};
    static const QStringList alone = {QStringLiteral("-reverse"), QStringLiteral("-testability")};
    static const QStringList withEquals = {QStringLiteral("-style="), QStringLiteral("-qmljsdebugger=")};

    QStringList kept;
    // The first entry is the program name.
    if (!arguments.isEmpty()) kept.append(arguments.first());
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        const QString option = argument.startsWith(QLatin1String("--")) ? argument.mid(1) : argument;
        if (takingValue.contains(option)) {
            ++i;
            continue;
        }
        if (alone.contains(option)) continue;
        if (std::any_of(withEquals.cbegin(), withEquals.cend(),
                        [&option](const QString &prefix) { return option.startsWith(prefix); }))
            continue;
        kept.append(argument);
    }
    return kept;
}

Launch parseLaunch(const QStringList &given)
{
    Launch launch;
    const auto refuse = [&launch](const QString &error) {
        launch.kind = Launch::Invalid;
        launch.error = error;
        return launch;
    };
    // Qt's own options are Qt's, whichever place they have.
    const QStringList arguments = withoutQtOptions(given);
    bool onlyFiles = false;
    // The first entry is the program name.
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        // After "--" every argument is a file, even one that starts with a dash.
        if (onlyFiles) {
            launch.files.append(argument);
            continue;
        }
        if (argument == QLatin1String("--")) {
            onlyFiles = true;
            continue;
        }
        if (argument == QLatin1String("--version")) {
            launch.kind = Launch::Version;
            return launch;
        }
        if (argument == QLatin1String("--help") || argument == QLatin1String("-h")) {
            launch.kind = Launch::Help;
            return launch;
        }
        // An option with a value takes it as the next argument or after '='.
        const qsizetype equals = argument.indexOf(u'=');
        const QString name = equals > 0 ? argument.left(equals) : argument;
        if (name == QLatin1String("--theme") || name == QLatin1String("--benchmark")) {
            QString value;
            if (equals > 0) {
                value = argument.mid(equals + 1);
            } else if (i + 1 < arguments.size() && !arguments.at(i + 1).startsWith(u'-')) {
                value = arguments.at(++i);
            }
            if (value.isEmpty())
                return refuse(QCoreApplication::translate("cli", "parch: %1 needs a value").arg(name));
            (name == QLatin1String("--theme") ? launch.themeDirectory : launch.benchmarkOutput) = value;
            continue;
        }
        // Anything else that looks like an option is refused rather than
        // opened as a file; "--" before a name that starts with a dash opens it.
        if (argument.startsWith(u'-') && argument.size() > 1)
            return refuse(QCoreApplication::translate("cli", "parch: unknown option %1").arg(argument));
        // The "open" word is only a subcommand before any file; afterwards it is a file name.
        if (argument == QLatin1String("open") && i == 1)
            continue;
        // So is a command's name, which runs it.
        if (i == 1 && Cli::isCommand(argument)) {
            launch.kind = Launch::Command;
            launch.commandArguments = arguments;
            return launch;
        }
        launch.files.append(argument);
    }
    return launch;
}
