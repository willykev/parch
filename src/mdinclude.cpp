#include "mdinclude.h"

#include "saferead.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>

namespace md {

bool isUrlSpelling(QStringView path)
{
    if (path.startsWith(u"www.", Qt::CaseInsensitive) || path.contains(u"://")) return true;
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]+:"));
    return path.contains(u':') && scheme.matchView(path).hasMatch();
}

bool isBareIncludePath(QStringView path)
{
    if (path.isEmpty() || QStringView(u"\"'#!(=").contains(path.front()) || path.startsWith(u"~~~")) return false;
    for (const QChar ch : path)
        if (ch <= u' ' || QStringView(u"<>[]*`|\\").contains(ch)) return false;
    return true;
}

QString includeTarget(const QString &written, const QString &includingDirectory, const QString &libraryRoot)
{
    if (written.startsWith(u'/')) {
        if (libraryRoot.isEmpty()) return QString();
        return QDir::cleanPath(libraryRoot + written);
    }
    if (includingDirectory.isEmpty()) return QString();
    return QDir::cleanPath(includingDirectory + u'/' + written);
}

bool isWithin(const QString &path, const QString &folder)
{
    if (folder.isEmpty() || path.isEmpty()) return false;
    if (path == folder) return true;
    const QString base = folder.endsWith(u'/') ? folder : folder + u'/';
    return path.startsWith(base);
}

QString IncludeResolver::canonicalPath(const QString &path)
{
    return QDir::cleanPath(path);
}

QString FileIncludeResolver::canonicalPath(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

IncludeResolver::File FileIncludeResolver::read(const QString &path, qint64 maxBytes)
{
    File result;
    // No file is read larger than a document may be, whatever room the
    // include limits leave; a smaller limit is that room, said as such.
    const bool roomLimits = maxBytes <= SafeRead::DocumentBytes;
    const SafeRead::Result read = SafeRead::read(path, roomLimits ? std::max<qint64>(0, maxBytes) : SafeRead::DocumentBytes);
    result.canonicalPath = read.canonicalPath;
    if (!read.ok) {
        result.missing = read.missing;
        if (read.missing) {
            result.error = QStringLiteral("The file does not exist");
        } else if (read.tooLarge && roomLimits) {
            result.tooLarge = true;
            result.bytes = read.size;
        } else {
            result.error = read.error;
        }
        return result;
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    result.text = decoder(read.bytes);
    if (decoder.hasError()) {
        result.text.clear();
        result.error = QStringLiteral("The file is not UTF-8 text");
        return result;
    }
    result.bytes = read.bytes.size();
    result.ok = true;
    return result;
}

void MemoryIncludeResolver::add(const QString &path, const QString &text)
{
    m_files.insert(QDir::cleanPath(path), text);
}

IncludeResolver::File MemoryIncludeResolver::read(const QString &path, qint64 maxBytes)
{
    ++m_reads;
    File result;
    const QString clean = QDir::cleanPath(path);
    const auto it = m_files.constFind(clean);
    if (it == m_files.constEnd()) {
        result.missing = true;
        result.error = QStringLiteral("The file does not exist");
        return result;
    }
    result.canonicalPath = clean;
    result.bytes = it->toUtf8().size();
    if (result.bytes > maxBytes) {
        result.tooLarge = true;
        return result;
    }
    result.text = *it;
    result.ok = true;
    return result;
}

} // namespace md
