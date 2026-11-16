#pragma once

#include <QString>
#include <QStringList>

// What a command line asks parch to do.
struct Launch {
    enum Kind { Gui, Version, Help, Invalid } kind = Gui;
    QStringList files;
    QString themeDirectory;
    QString benchmarkOutput;
    QString error;
};

// Parses a full argument list, program name first. Qt's own options, such
// as -platform, -style or -qwindowgeometry, are left to Qt.
Launch parseLaunch(const QStringList &arguments);

// The arguments without the options Qt reads and removes itself, with
// their values, so that parch sees what it will once Qt has started.
QStringList withoutQtOptions(const QStringList &arguments);

// The files as full paths, each relative one taken from `workingDirectory`.
// Nothing else is resolved, so that links and ".." are left to the kernel.
QStringList absoluteFiles(const QStringList &files, const QString &workingDirectory);

QString versionLine();
QString helpText();
