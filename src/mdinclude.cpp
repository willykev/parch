#include "mdinclude.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

namespace md {

QString includeTarget(const QString &written, const QString &includingDirectory, const QString &libraryRoot)
{
    if (written.startsWith(u'/')) {
        if (libraryRoot.isEmpty()) return QString();
        return QDir::cleanPath(libraryRoot + written);
    }
    if (includingDirectory.isEmpty()) return QString();
    return QDir::cleanPath(includingDirectory + u'/' + written);
}

IncludeResolver::File FileIncludeResolver::read(const QString &path, qint64 maxBytes)
{
    File result;
    const QFileInfo info(path);
    if (!info.exists()) {
        result.missing = true;
        result.error = QStringLiteral("The file does not exist");
        return result;
    }
    if (!info.isFile()) {
        result.error = QStringLiteral("This is not a file");
        return result;
    }
    result.canonicalPath = info.canonicalFilePath();
    if (info.size() > maxBytes) {
        result.tooLarge = true;
        result.bytes = info.size();
        return result;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        result.error = file.errorString();
        return result;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    result.text = decoder(bytes);
    if (decoder.hasError()) {
        result.text.clear();
        result.error = QStringLiteral("The file is not UTF-8 text");
        return result;
    }
    result.bytes = bytes.size();
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
