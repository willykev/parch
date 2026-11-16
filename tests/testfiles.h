#pragma once

#include <QByteArray>
#include <QString>

// Files and theme folders the tests write, always beneath a temporary folder.

// Writes a file, making its folder first.
void writeFile(const QString &path, const QByteArray &contents);
// The file's bytes, or nothing when it cannot be read.
QByteArray readFile(const QString &path);
// Omarchy keeps the active theme in <dir>/theme/colors.toml.
void writeTheme(const QString &dir, const QString &mode, const QString &background, const QString &foreground,
                const QString &accent);
// A theme switch swaps the whole theme directory, as Omarchy does.
void replaceTheme(const QString &dir, const QString &mode, const QString &background, const QString &foreground,
                  const QString &accent);
