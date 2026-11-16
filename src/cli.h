#pragma once

#include <QSize>
#include <QString>
#include <QStringList>
#include <optional>

// What `--screenshot` asks for: a picture of the window, set up first as
// the options that go with it say. Each is unset unless given.
struct Picture {
    // Where the picture is saved; empty when none is asked for.
    QString file;
    // The window's size in pixels of a screen at normal scale.
    QSize size;
    // How the document is shown: "source", "split" or "reading".
    QString view;
    // A panel or dialog to open, one of panels().
    QString show;
    // Focus mode is on.
    bool focus = false;
    // The place at the top of the text and of the preview beside it: a
    // fraction of the way down, from 0 to 1, or a line counted from 1.
    double scroll = -1;
    int line = 0;
    // A selection, from one source offset to another.
    int selectFrom = -1;
    int selectTo = -1;
    // The library shown, or hidden.
    std::optional<bool> library;

    // What --show can open.
    static QStringList panels();
};

// What a command line asks parch to do.
struct Launch {
    // Command: one of the commands that need no display (clicommands.h),
    // run with `commandArguments`.
    enum Kind { Gui, Version, Help, Command, Invalid } kind = Gui;
    QStringList files;
    QString themeDirectory;
    // Omarchy's default theme as it ships, rather than the desktop's.
    bool defaultTheme = false;
    QString benchmarkOutput;
    Picture picture;
    QString error;
    // The program's name, the command, and what follows it.
    QStringList commandArguments;
};

// Parses a full argument list, program name first. Qt's own options, such
// as -platform, -style or -qwindowgeometry, are left to Qt. A first argument
// that names a command (check, render, export, stats, skill, help) runs it;
// a file of that name opens with `parch open NAME` or `parch -- NAME`. The
// options that set the window up for a picture are refused without
// --screenshot, and every value is checked here, before any window.
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
