#include "cli.h"

#include "clicommands.h"
#include "diskfile.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

QStringList Picture::panels()
{
    return {QStringLiteral("export"),  QStringLiteral("print"),   QStringLiteral("document-settings"),
            QStringLiteral("history"), QStringLiteral("palette"), QStringLiteral("outline"),
            QStringLiteral("preferences"), QStringLiteral("shortcuts")};
}

QString versionLine()
{
    return QStringLiteral("parch ") + QStringLiteral(PARCH_VERSION);
}

QString openHelpText()
{
    // Translated as one block so a catalog can keep the columns aligned.
    return QCoreApplication::translate("cli",
        "Usage: parch [open] [--theme DIR] [--benchmark OUT] [FILE...]\n"
        "       parch --screenshot PNG [OPTION...] [FILE...]\n"
        "\n"
        "  open             Open the files that follow (optional; bare files also open)\n"
        "  --theme DIR      Show the Omarchy theme in DIR, or the installed one so named\n"
        "  --default-theme  Show Omarchy's default theme as it ships, not the desktop's\n"
        "  --benchmark OUT  Measure the window on a copy of one FILE; write JSON to OUT\n"
        "  --screenshot     Draw the window off screen, save it as PNG once all of it\n"
        "                   is drawn, and exit; nothing parch keeps is changed\n"
        "  --force          Replace an existing screenshot or benchmark output file\n"
        "  --version        Print the version and exit\n"
        "  --help           Print this help and exit\n"
        "\n"
        "With --screenshot:\n"
        "  --size WxH       The window's size, such as 1280x800\n"
        "  --view VIEW      The text (source), both (split) or the preview (reading)\n"
        "  --show PANEL     Open export, print, document-settings, history, palette,\n"
        "                   outline, preferences or shortcuts\n"
        "  --library        Show the library\n"
        "  --no-library     Hide the library\n"
        "  --focus          Turn focus mode on\n"
        "  --line N         Put line N at the top of the text and of the preview\n"
        "  --scroll AMOUNT  Scroll that far down instead, from 0 (the top) to 1\n"
        "  --select A,B     Select the text from offset A to offset B\n"
        "\n"
        "With --benchmark:\n"
        "  --view VIEW      As with --screenshot; source unless given\n"
        "  --windows N      1, or 2 windows showing the same FILE\n"
        "  --adversarial    Also a 1 MB paste and its undo, deep nesting and rapid keys\n");
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

namespace {

// Takes the value of one of the options that set a window up for a
// picture; returns why it cannot, or nothing.
QString takePictureValue(Picture &picture, const QString &name, const QString &value)
{
    const auto said = [](const char *text) { return QCoreApplication::translate("cli", text); };
    if (name == u"--size") {
        static const QRegularExpression size(QStringLiteral("^([0-9]{1,5})x([0-9]{1,5})$"));
        const QRegularExpressionMatch match = size.match(value);
        const int width = match.captured(1).toInt(), height = match.captured(2).toInt();
        if (!match.hasMatch() || width < 100 || height < 100 || width > 10000 || height > 10000)
            return said("parch: --size takes WIDTHxHEIGHT in pixels, each from 100 to 10000, such as 1280x800");
        picture.size = QSize(width, height);
    } else if (name == u"--view") {
        if (value != u"source" && value != u"split" && value != u"reading")
            return said("parch: --view takes source, split or reading");
        picture.view = value;
    } else if (name == u"--show") {
        if (!Picture::panels().contains(value))
            return QCoreApplication::translate("cli", "parch: --show takes one of %1").arg(Picture::panels().join(u", "));
        picture.show = value;
    } else if (name == u"--scroll") {
        bool number = false;
        const double fraction = value.toDouble(&number);
        if (!number || !std::isfinite(fraction) || fraction < 0 || fraction > 1)
            return said("parch: --scroll takes a fraction from 0 to 1, such as 0.4");
        picture.scroll = fraction;
    } else if (name == u"--line") {
        bool number = false;
        const int line = value.toInt(&number);
        if (!number || line < 1) return said("parch: --line takes a line number from 1");
        picture.line = line;
    } else if (name == u"--select") {
        const QStringList ends = value.split(u',');
        bool from = false, to = false;
        const int start = ends.value(0).toInt(&from), end = ends.value(1).toInt(&to);
        if (ends.size() != 2 || !from || !to || start < 0 || end < 0)
            return said("parch: --select takes A,B, two offsets in the text such as 120,180");
        picture.selectFrom = start;
        picture.selectTo = end;
    }
    return {};
}

} // namespace

Launch parseLaunch(const QStringList &arguments)
{
    Launch launch;
    const auto refuse = [&launch](const QString &error) {
        launch.kind = Launch::Invalid;
        launch.error = error;
        return launch;
    };
    // The options that take a value, and those that stand alone.
    static const QStringList valued = {
        QStringLiteral("--theme"), QStringLiteral("--benchmark"), QStringLiteral("--screenshot"),
        QStringLiteral("--size"),  QStringLiteral("--view"),      QStringLiteral("--show"),
        QStringLiteral("--scroll"), QStringLiteral("--line"),     QStringLiteral("--select"),
        QStringLiteral("--windows")};
    static const QStringList alone = {QStringLiteral("--default-theme"), QStringLiteral("--focus"),
                                      QStringLiteral("--library"), QStringLiteral("--no-library"),
                                      QStringLiteral("--adversarial"), QStringLiteral("--force")};
    // The options given that set the window up for a picture, in order.
    QStringList forPicture;
    // The options given that say how a benchmark measures.
    QStringList forBenchmark;
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
        if (valued.contains(name)) {
            QString value;
            if (equals > 0) {
                value = argument.mid(equals + 1);
            } else if (i + 1 < arguments.size() && !arguments.at(i + 1).startsWith(u'-')) {
                value = arguments.at(++i);
            }
            if (value.isEmpty())
                return refuse(QCoreApplication::translate("cli", "parch: %1 needs a value").arg(name));
            if (name == QLatin1String("--theme")) {
                launch.themeDirectory = value;
            } else if (name == QLatin1String("--benchmark")) {
                launch.benchmarkOutput = value;
            } else if (name == QLatin1String("--screenshot")) {
                launch.picture.file = value;
            } else if (name == QLatin1String("--windows")) {
                if (value != QLatin1String("1") && value != QLatin1String("2"))
                    return refuse(QCoreApplication::translate("cli", "parch: --windows takes 1 or 2"));
                launch.benchmarkWindows = value.toInt();
                forBenchmark.append(name);
            } else {
                const QString why = takePictureValue(launch.picture, name, value);
                if (!why.isEmpty()) return refuse(why);
                forPicture.append(name);
            }
            continue;
        }
        if (alone.contains(name)) {
            if (equals > 0)
                return refuse(QCoreApplication::translate("cli", "parch: %1 takes no value").arg(name));
            if (name == u"--force") {
                launch.force = true;
            } else if (name == QLatin1String("--default-theme")) {
                launch.defaultTheme = true;
            } else if (name == QLatin1String("--adversarial")) {
                launch.benchmarkAdversarial = true;
                forBenchmark.append(name);
            } else {
                if (name == QLatin1String("--focus")) launch.picture.focus = true;
                else launch.picture.library = name == QLatin1String("--library");
                forPicture.append(name);
            }
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
    const auto together = [&refuse](const char *one, const char *other) {
        return refuse(QCoreApplication::translate("cli", "parch: %1 and %2 cannot both be given")
                          .arg(QLatin1StringView(one), QLatin1StringView(other)));
    };
    if (launch.force && launch.picture.file.isEmpty() && launch.benchmarkOutput.isEmpty())
        return refuse(QCoreApplication::translate("cli", "parch: --force works only with --screenshot or --benchmark"));
    if (!launch.themeDirectory.isEmpty() && launch.defaultTheme) return together("--theme", "--default-theme");
    if (!launch.picture.file.isEmpty() && !launch.benchmarkOutput.isEmpty()) return together("--screenshot", "--benchmark");
    if (forPicture.contains(QLatin1String("--line")) && forPicture.contains(QLatin1String("--scroll")))
        return together("--line", "--scroll");
    if (forPicture.contains(QLatin1String("--library")) && forPicture.contains(QLatin1String("--no-library")))
        return together("--library", "--no-library");
    // A benchmark shows the document in the view --view names too; what
    // else measures it goes with it alone.
    if (!launch.benchmarkOutput.isEmpty()) {
        if (!launch.picture.view.isEmpty()) launch.benchmarkView = launch.picture.view;
        forPicture.removeAll(QStringLiteral("--view"));
    }
    if (launch.picture.file.isEmpty() && !forPicture.isEmpty()) {
        const QString first = forPicture.first();
        return refuse(first == QLatin1String("--view")
                          ? QCoreApplication::translate("cli", "parch: --view works only with --screenshot or --benchmark")
                          : QCoreApplication::translate("cli", "parch: %1 works only with --screenshot").arg(first));
    }
    if (launch.benchmarkOutput.isEmpty() && !forBenchmark.isEmpty())
        return refuse(QCoreApplication::translate("cli", "parch: %1 works only with --benchmark").arg(forBenchmark.first()));
    return launch;
}
