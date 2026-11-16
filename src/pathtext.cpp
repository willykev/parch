#include "pathtext.h"

bool PathText::isFullPath(const QString &path)
{
    return path.startsWith(u'/') && path.size() <= MaxLength && !path.contains(QChar(0));
}

QString PathText::folderOf(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    return slash <= 0 ? QStringLiteral("/") : path.left(slash);
}

QString PathText::fileName(const QString &path)
{
    return path.mid(path.lastIndexOf(u'/') + 1);
}
