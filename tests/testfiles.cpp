#include "testfiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtTest>
#include <cstdio>

void writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(path));
    file.write(contents);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

namespace {
QByteArray colorsToml(const QString &mode, const QString &background, const QString &foreground, const QString &accent)
{
    return QStringLiteral("mode = \"%1\"\nbackground = \"%2\"\nforeground = \"%3\"\naccent = \"%4\"\n")
        .arg(mode, background, foreground, accent)
        .toUtf8();
}
} // namespace

void writeTheme(const QString &dir, const QString &mode, const QString &background, const QString &foreground,
                const QString &accent)
{
    writeFile(dir + "/theme/colors.toml", colorsToml(mode, background, foreground, accent));
}

// A theme switch swaps the whole theme directory. rename(2) cannot replace a
// directory that has files in it, so the new theme is written beside the old
// one, the old one is moved aside and the new one is renamed into its place.
void replaceTheme(const QString &dir, const QString &mode, const QString &background, const QString &foreground,
                  const QString &accent)
{
    const QString fresh = dir + "/theme.next", old = dir + "/theme.previous";
    QDir(fresh).removeRecursively();
    QDir(old).removeRecursively();
    writeFile(fresh + "/colors.toml", colorsToml(mode, background, foreground, accent));
    QVERIFY(std::rename(qPrintable(dir + "/theme"), qPrintable(old)) == 0);
    QVERIFY(std::rename(qPrintable(fresh), qPrintable(dir + "/theme")) == 0);
    QDir(old).removeRecursively();
}
