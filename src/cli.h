#pragma once

#include <QString>
#include <QStringList>

// What a command line asks parch to do.
struct Launch {
    // Command: one of the commands that need no display (clicommands.h),
    // run with `commandArguments`.
    enum Kind { Gui, Version, Help, Command, Invalid } kind = Gui;
    QStringList files;
    QString themeDirectory;
    QString benchmarkOutput;
    QString error;
    // The program's name, the command, and what follows it.
    QStringList commandArguments;
};

// Parses a full argument list, program name first. Qt's own options, such
// as -platform, -style or -qwindowgeometry, are left to Qt. A first argument
// that names a command (check, render, export, stats, skill, help) runs it;
// a file of that name opens with `parch open NAME` or `parch -- NAME`.
Launch parseLaunch(const QStringList &arguments);

// The arguments without the options Qt reads and removes itself, with
// their values, so that parch sees what it will once Qt has started.
QStringList withoutQtOptions(const QStringList &arguments);

// The files as full paths, each relative one taken from `workingDirectory`.
// Nothing else is resolved, so that links and ".." are left to the kernel.
QStringList absoluteFiles(const QStringList &files, const QString &workingDirectory);

QString versionLine();
// Everything: opening files, and the commands.
QString helpText();
// Opening files in the window: `parch help open`.
QString openHelpText();
