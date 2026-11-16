// Adapted from sprawl's src/launch.h (MIT, copyright 2026 willykev): the
// lookup of the theme that --theme asks for.
#pragma once

#include <QDir>
#include <QFile>
#include <QStringList>

// Where Omarchy keeps its themes: the person's own, then those it comes with.
inline QStringList themesFolders()
{
    return {QDir::homePath() + "/.config/omarchy/themes", "/usr/share/omarchy/themes"};
}

// The folder of the theme that --theme asks for, or nothing when there is
// none. A theme's name is that theme whatever the current folder holds, and
// anything else is a path: what has a slash in it, or is the name of no
// theme. Callers say where themes are kept so that a test needs none of the
// person's.
inline QString findTheme(const QString &asked, const QStringList &folders = themesFolders())
{
    // Nothing named is no theme, not a folder themes are kept in.
    if (asked.isEmpty())
        return {};
    QStringList candidates;
    // A name is the name of a folder among the themes, never a way out of them.
    if (!asked.contains('/') && asked != "." && asked != "..")
        for (const QString &folder : folders)
            candidates << folder + "/" + asked;
    candidates << asked;
    for (const QString &candidate : std::as_const(candidates))
        if (QFile::exists(candidate + "/colors.toml"))
            return QDir::cleanPath(QDir::current().absoluteFilePath(candidate));
    return {};
}
