#include "cli.h"

#include "diskfile.h"

#include <QCoreApplication>

#include <algorithm>

QString versionLine()
{
    return QStringLiteral("parch ") + QStringLiteral(PARCH_VERSION);
}

QString helpText()
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
    // Qt's own options are Qt's, whichever place they have.
    const QStringList arguments = withoutQtOptions(given);
    // The first entry is the program name.
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == QLatin1String("--version")) {
            launch.kind = Launch::Version;
            return launch;
        }
        if (argument == QLatin1String("--help") || argument == QLatin1String("-h")) {
            launch.kind = Launch::Help;
            return launch;
        }
        if (argument == QLatin1String("--theme") || argument == QLatin1String("--benchmark")) {
            if (i + 1 >= arguments.size()) {
                launch.kind = Launch::Invalid;
                launch.error = QCoreApplication::translate("cli", "parch: %1 needs a value").arg(argument);
                return launch;
            }
            const QString &value = arguments.at(++i);
            (argument == QLatin1String("--theme") ? launch.themeDirectory : launch.benchmarkOutput) = value;
            continue;
        }
        if (argument.startsWith(QLatin1String("--"))) {
            launch.kind = Launch::Invalid;
            launch.error = QCoreApplication::translate("cli", "parch: unknown option %1").arg(argument);
            return launch;
        }
        // The "open" word is only a subcommand before any file; afterwards it is a file name.
        if (argument == QLatin1String("open") && i == 1)
            continue;
        launch.files.append(argument);
    }
    return launch;
}
