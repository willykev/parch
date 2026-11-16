#include "recovery.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimeZone>
#include <QRegularExpression>
#include <algorithm>

namespace {
const QString suffix = QStringLiteral(".recovery");
const QString savesSuffix = QStringLiteral(".saves");
// Enough saves to cover any that can land between two snapshots.
constexpr qsizetype keptSaves = 32;
// Room for that many lines, each two SHA-256 hashes in hex, a revision and
// the mark of a save that has not landed.
constexpr qint64 savesLimit = keptSaves * 160;
// Written after a save that has not yet landed; a line without it has.
const QByteArray pendingMark("pending");

QByteArray hexOf(const QByteArray &hash)
{
    return hash.isEmpty() ? QByteArray("-") : hash.toHex();
}

// UTF-8 is used whenever it returns the same text, which keeps an entry
// readable by hand. Text with unpaired surrogates cannot make that trip, so
// it is kept as raw UTF-16 code units and still comes back exactly.
bool survivesUtf8(const QString &text)
{
    return DiskFile::decodeUtf8(text.toUtf8()) == text;
}

QByteArray utf16Bytes(const QString &text)
{
    QByteArray bytes;
    bytes.reserve(text.size() * 2);
    for (const QChar unit : text) {
        bytes.append(char(unit.unicode() & 0xff));
        bytes.append(char(unit.unicode() >> 8));
    }
    return bytes;
}

QString fromUtf16Bytes(const QByteArray &bytes)
{
    QString text;
    text.reserve(bytes.size() / 2);
    for (qsizetype i = 0; i + 1 < bytes.size(); i += 2)
        text.append(QChar(ushort(uchar(bytes.at(i)) | (uchar(bytes.at(i + 1)) << 8))));
    return text;
}
} // namespace

Recovery::Recovery(const AppPaths &paths) : m_paths(paths), m_made(QDateTime::currentDateTimeUtc()) {}

QString Recovery::fileFor(const QString &documentId) const
{
    return m_paths.recovery() + u'/' + documentId + suffix;
}

bool Recovery::write(const QString &documentId, const QString &path, const SourceSnapshot &snapshot,
                     const DiskVersion &base, QString *error) const
{
    return writeEntry(documentId, path, snapshot, base, std::nullopt, error);
}

bool Recovery::write(const QString &documentId, const QString &path, const SourceSnapshot &snapshot,
                     const DiskVersion &base, const TextConvention &convention, QString *error) const
{
    return writeEntry(documentId, path, snapshot, base, convention, error);
}

bool Recovery::writeEntry(const QString &documentId, const QString &path, const SourceSnapshot &snapshot,
                          const DiskVersion &base, const std::optional<TextConvention> &convention,
                          QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!AppPaths::isDocumentId(documentId))
        return fail(QCoreApplication::translate("Recovery", "Not a document id: %1").arg(documentId));
    // The folder is parch's own, so it is named by what it is for, not where it is.
    QString reason;
    if (!DiskFile::makeFolders(m_paths.recovery(), &reason))
        return fail(QCoreApplication::translate("Recovery", "The recovery folder can't be written (%1)").arg(reason));

    const bool utf8 = survivesUtf8(snapshot.text);
    const QByteArray payload = utf8 ? snapshot.text.toUtf8() : utf16Bytes(snapshot.text);
    QJsonObject header;
    header.insert(QStringLiteral("version"), 1);
    header.insert(QStringLiteral("id"), documentId);
    header.insert(QStringLiteral("path"), path);
    // A string, since a JSON number cannot hold every 64-bit revision.
    header.insert(QStringLiteral("revision"), QString::number(snapshot.revision));
    header.insert(QStringLiteral("written"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    QJsonObject baseObject;
    baseObject.insert(QStringLiteral("path"), base.canonicalPath);
    // Strings, since a JSON number cannot hold every 64-bit value.
    baseObject.insert(QStringLiteral("device"), QString::number(base.device));
    baseObject.insert(QStringLiteral("inode"), QString::number(base.inode));
    baseObject.insert(QStringLiteral("sha256"), QString::fromLatin1(base.sha256.toHex()));
    baseObject.insert(QStringLiteral("exists"), base.exists);
    // The file's permission bits, when they are known, for a restart that
    // makes the file again before it has read it.
    if (base.mode != 0) baseObject.insert(QStringLiteral("mode"), int(base.mode & 0777));
    header.insert(QStringLiteral("base"), baseObject);
    if (convention) {
        header.insert(QStringLiteral("bom"), convention->bom);
        header.insert(QStringLiteral("newline"), convention->newline);
    }
    header.insert(QStringLiteral("encoding"), utf8 ? QStringLiteral("utf8") : QStringLiteral("utf16"));
    header.insert(QStringLiteral("bytes"), payload.size());

    QSaveFile file(fileFor(documentId));
    if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
    // The compact header is one line, so the first line break ends it.
    if (file.write(QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n') < 0
        || file.write(payload) != payload.size() || !file.commit())
        return fail(file.errorString());
    return true;
}

void Recovery::discard(const QString &documentId) const
{
    if (AppPaths::isDocumentId(documentId)) QFile::remove(fileFor(documentId));
}

QString Recovery::savesFor(const QString &documentId) const
{
    return m_paths.recovery() + u'/' + documentId + savesSuffix;
}

QList<Recovery::Link> Recovery::saves(const QString &documentId) const
{
    QList<Link> links;
    QFile file(savesFor(documentId));
    if (!file.open(QIODevice::ReadOnly)) return links;
    // A file longer than parch writes is not its own, and tells nothing.
    const QByteArray text = file.read(savesLimit + 1);
    if (text.size() > savesLimit || file.error() != QFileDevice::NoError) return links;
    // One save to a line: the hash replaced, the hash written and the
    // revision written, each after a space, and the pending mark while the
    // save has not landed.
    for (const QByteArray &line : text.split('\n')) {
        const QList<QByteArray> parts = line.split(' ');
        const bool marked = parts.size() == 4 && parts.at(3) == pendingMark;
        bool revisionOk = false;
        const Revision revision = parts.size() == 3 || marked ? parts.at(2).toULongLong(&revisionOk) : 0;
        if (revisionOk && !parts.at(0).isEmpty() && !parts.at(1).isEmpty())
            links.append({parts.at(0), parts.at(1), revision, !marked});
    }
    return links;
}

bool Recovery::writeSaves(const QString &documentId, const QList<Link> &links) const
{
    QByteArray text;
    for (const Link &link : links)
        text += link.from + ' ' + link.to + ' ' + QByteArray::number(link.revision) + (link.landed ? QByteArray() : " " + pendingMark)
                + '\n';
    QSaveFile file(savesFor(documentId));
    return file.open(QIODevice::WriteOnly) && file.write(text) == text.size() && file.commit();
}

bool Recovery::noteSave(const QString &documentId, const QByteArray &from, const QByteArray &to,
                        Revision revision) const
{
    if (!AppPaths::isDocumentId(documentId) || !DiskFile::makeFolders(m_paths.recovery())) return false;
    QList<Link> links = saves(documentId);
    links.append({hexOf(from), hexOf(to), revision, false});
    if (links.size() > keptSaves) links.remove(0, links.size() - keptSaves);
    return writeSaves(documentId, links);
}

bool Recovery::noteLanded(const QString &documentId, const QByteArray &to, Revision revision) const
{
    if (!AppPaths::isDocumentId(documentId)) return false;
    QList<Link> links = saves(documentId);
    if (links.isEmpty() || links.last().landed || links.last().to != hexOf(to) || links.last().revision != revision)
        return false;
    links.last().landed = true;
    return writeSaves(documentId, links);
}

void Recovery::dropSave(const QString &documentId, const QByteArray &to, Revision revision) const
{
    if (!AppPaths::isDocumentId(documentId)) return;
    QList<Link> links = saves(documentId);
    if (links.isEmpty() || links.last().landed || links.last().to != hexOf(to) || links.last().revision != revision)
        return;
    links.removeLast();
    // A note that cannot be rewritten cannot be trusted either.
    if (!writeSaves(documentId, links)) forgetSaves(documentId);
}

std::optional<qsizetype> Recovery::heldBy(const QList<Link> &links, const QByteArray &content)
{
    if (links.isEmpty()) return std::nullopt;
    const Link &last = links.last();
    if (last.to == content) return links.size() - 1;
    // A save that had not landed may have left the file as it found it,
    // which the save before it wrote.
    if (last.landed || last.from != content) return std::nullopt;
    if (links.size() > 1 && links.at(links.size() - 2).to == content) return links.size() - 2;
    return std::nullopt;
}

bool Recovery::savedSince(const QString &documentId, const QByteArray &from, const QByteArray &to) const
{
    if (!AppPaths::isDocumentId(documentId)) return false;
    QList<Link> links = saves(documentId);
    const QByteArray target = hexOf(to);
    // Only what parch wrote last can still be its own. Whatever it wrote
    // before could have come back from somewhere else since.
    const std::optional<qsizetype> held = heldBy(links, target);
    if (!held || target == hexOf(from)) return false;
    // Every content reached from `from` through parch's own saves up to that
    // one, in the order they were made; a save that went back to an earlier
    // content only reaches what is reached already.
    QList<QByteArray> reached{hexOf(from)};
    for (qsizetype i = 0; i <= *held; ++i)
        if (reached.contains(links.at(i).from) && !reached.contains(links.at(i).to)) reached.append(links.at(i).to);
    return reached.contains(target);
}

bool Recovery::savedLast(const QString &documentId, const QByteArray &content) const
{
    return AppPaths::isDocumentId(documentId) && heldBy(saves(documentId), hexOf(content)).has_value();
}

std::optional<Recovery::Save> Recovery::lastSave(const QString &documentId) const
{
    if (!AppPaths::isDocumentId(documentId)) return std::nullopt;
    const QList<Link> links = saves(documentId);
    if (links.isEmpty()) return std::nullopt;
    const Link &last = links.last();
    return Save{last.to == "-" ? QByteArray() : QByteArray::fromHex(last.to), last.revision};
}

std::optional<Recovery::Save> Recovery::saveHeld(const QString &documentId, const QByteArray &content) const
{
    if (!AppPaths::isDocumentId(documentId)) return std::nullopt;
    const QList<Link> links = saves(documentId);
    const std::optional<qsizetype> held = heldBy(links, hexOf(content));
    if (!held) return std::nullopt;
    const Link &link = links.at(*held);
    return Save{link.to == "-" ? QByteArray() : QByteArray::fromHex(link.to), link.revision};
}

void Recovery::forgetSaves(const QString &documentId) const
{
    if (AppPaths::isDocumentId(documentId)) QFile::remove(savesFor(documentId));
}

void Recovery::sweepSaves(const QStringList &openIds) const
{
    const QDir folder(m_paths.recovery());
    for (const QString &name : folder.entryList({QStringLiteral("*") + savesSuffix}, QDir::Files)) {
        const QString id = name.chopped(savesSuffix.size());
        if (!AppPaths::isDocumentId(id) || openIds.contains(id) || QFileInfo::exists(fileFor(id))) continue;
        QFile::remove(folder.filePath(name));
    }
    // QSaveFile writes beside its file under the file's name and six more
    // characters, and a crash leaves that temporary. One older than this
    // Recovery, with a margin for coarse file times, is no write of ours.
    static const QRegularExpression temporary(
        QStringLiteral("^([0-9a-f-]{36})\\.(recovery|saves)\\.[A-Za-z0-9]{6}$"));
    const QDateTime before = m_made.addSecs(-2);
    for (const QFileInfo &info : folder.entryInfoList(QDir::Files | QDir::Hidden)) {
        const QRegularExpressionMatch match = temporary.match(info.fileName());
        if (!match.hasMatch() || !AppPaths::isDocumentId(match.captured(1))) continue;
        if (info.lastModified(QTimeZone::UTC) < before) QFile::remove(info.filePath());
    }
}

QList<Recovery::Entry> Recovery::pending() const
{
    QList<Entry> entries;
    const QStringList names = QDir(m_paths.recovery()).entryList({QStringLiteral("*") + suffix}, QDir::Files);
    for (const QString &name : names) {
        QFile file(m_paths.recovery() + u'/' + name);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray all = file.readAll();
        const qsizetype end = all.indexOf('\n');
        if (end < 0) continue;
        const QJsonObject header = QJsonDocument::fromJson(all.left(end)).object();
        const QByteArray payload = all.mid(end + 1);
        const QString id = header.value(QStringLiteral("id")).toString();
        const QString encoding = header.value(QStringLiteral("encoding")).toString();
        bool revisionOk = false;
        const Revision revision = header.value(QStringLiteral("revision")).toString().toULongLong(&revisionOk);
        // The name must agree with the id inside, and the length with the payload.
        if (!AppPaths::isDocumentId(id) || id + suffix != name || !revisionOk
            || header.value(QStringLiteral("bytes")).toInteger(-1) != payload.size()
            || (encoding != QLatin1String("utf8") && encoding != QLatin1String("utf16"))
            // UTF-16 comes in pairs of bytes; an odd one out is a damaged entry.
            || (encoding == QLatin1String("utf16") && payload.size() % 2 != 0))
            continue;
        Entry entry;
        entry.documentId = id;
        entry.path = header.value(QStringLiteral("path")).toString();
        entry.text = encoding == QLatin1String("utf8") ? DiskFile::decodeUtf8(payload) : fromUtf16Bytes(payload);
        entry.revision = revision;
        entry.written = QDateTime::fromString(header.value(QStringLiteral("written")).toString(), Qt::ISODateWithMs);
        const QJsonObject base = header.value(QStringLiteral("base")).toObject();
        entry.hasBase = header.contains(QStringLiteral("base"));
        entry.base.canonicalPath = base.value(QStringLiteral("path")).toString();
        entry.base.device = base.value(QStringLiteral("device")).toString().toULongLong();
        entry.base.inode = base.value(QStringLiteral("inode")).toString().toULongLong();
        entry.base.sha256 = QByteArray::fromHex(base.value(QStringLiteral("sha256")).toString().toLatin1());
        entry.base.exists = base.value(QStringLiteral("exists")).toBool();
        // Not known for an entry that did not record it.
        entry.base.mode = quint32(base.value(QStringLiteral("mode")).toInt()) & 0777;
        // Only the two line endings parch writes are taken.
        const QString newline = header.value(QStringLiteral("newline")).toString();
        if (header.value(QStringLiteral("bom")).isBool() && (newline == u"\n" || newline == u"\r\n")) {
            entry.hasConvention = true;
            entry.convention.bom = header.value(QStringLiteral("bom")).toBool();
            entry.convention.newline = newline;
        }
        entries.append(entry);
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry &a, const Entry &b) { return a.written < b.written; });
    return entries;
}
