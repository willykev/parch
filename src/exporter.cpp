#include "exporter.h"

#include "docxexport.h"
#include "epubexport.h"
#include "exportpictures.h"
#include "htmlexport.h"
#include "markdownexport.h"
#include "pdfexport.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRandomGenerator>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

QJsonObject ExportRecipe::toJson() const
{
    QJsonObject given;
    for (auto it = options.cbegin(); it != options.cend(); ++it)
        given.insert(it.key(), it.value().isEmpty() ? QJsonValue(true) : QJsonValue(it.value()));
    return QJsonObject{
        {QStringLiteral("source"), source},
        {QStringLiteral("format"), format},
        {QStringLiteral("output"), output},
        {QStringLiteral("dialect"), dialect.toJson()},
        {QStringLiteral("options"), given},
    };
}

QString Exporter::option(const ExportRecipe &recipe, const QString &name) const
{
    if (const auto given = recipe.options.constFind(name); given != recipe.options.cend()) return *given;
    for (const ExportOption &option : options())
        if (option.name == name) return option.defaultValue;
    return {};
}

namespace Exporters {

QList<const Exporter *> all()
{
    static const MarkdownExporter markdown;
    static const HtmlExporter html;
    static const PdfExporter pdf;
    static const DocxExporter docx;
    static const EpubExporter epub;
    return {&markdown, &html, &pdf, &docx, &epub};
}

QStringList formats()
{
    QStringList names;
    for (const Exporter *exporter : all()) names.append(exporter->format());
    return names;
}

const Exporter *find(const QString &format)
{
    for (const Exporter *exporter : all())
        if (exporter->format().compare(format, Qt::CaseInsensitive) == 0) return exporter;
    return nullptr;
}

const Exporter *forFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty()) return nullptr;
    for (const Exporter *exporter : all())
        if (exporter->suffixes().contains(suffix)) return exporter;
    return nullptr;
}

bool isSource(const md::Document &document, const QString &path)
{
    // The file it would replace, through any link, or the path itself when
    // nothing is there yet.
    const QFileInfo info(path);
    const QString target = info.exists() ? info.canonicalFilePath() : QDir::cleanPath(info.absoluteFilePath());
    for (const md::SourceFile &file : document.files)
        if (!file.path.isEmpty() && file.path == target) return true;
    return false;
}

QString linkRefusal(const QString &path)
{
    struct stat status;
    if (::lstat(QFile::encodeName(path).constData(), &status) != 0 || !S_ISLNK(status.st_mode)) return {};
    const QString target = QFile::symLinkTarget(path);
    return QStringLiteral("%1 is a link to %2; parch never writes through a link")
        .arg(path, target.isEmpty() ? QStringLiteral("another file") : target);
}

PendingFile::PendingFile(const QString &path, bool replaceLink) : m_path(path), m_replaceLink(replaceLink) {}

PendingFile::~PendingFile()
{
    if (!m_temporary.isEmpty()) ::unlinkat(m_folder, m_temporary.constData(), 0);
    if (m_folder >= 0) ::close(m_folder);
}

bool PendingFile::fail(const QString &why, QString *error)
{
    *error = QStringLiteral("cannot write %1: %2").arg(m_path, why);
    return false;
}

bool PendingFile::write(const QByteArray &bytes, QString *error)
{
    if (!m_replaceLink) {
        if (const QString why = linkRefusal(m_path); !why.isEmpty()) {
            *error = why;
            return false;
        }
    }
    const QFileInfo info(m_path);
    m_name = QFile::encodeName(info.fileName());
    if (m_name.isEmpty()) return fail(QStringLiteral("it names no file"), error);
    m_folder = ::open(QFile::encodeName(info.absolutePath()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (m_folder < 0) return fail(QString::fromLocal8Bit(std::strerror(errno)), error);
    struct stat there;
    const bool exists = ::fstatat(m_folder, m_name.constData(), &there, AT_SYMLINK_NOFOLLOW) == 0;
    if (exists && S_ISDIR(there.st_mode)) return fail(QStringLiteral("it is a folder"), error);
    // The temporary file is made anew, never through anything already there.
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
        const QByteArray name = ".parch-" + QByteArray::number(QRandomGenerator::global()->generate64(), 16) + ".part";
        fd = ::openat(m_folder, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
        if (fd >= 0) m_temporary = name;
        else if (errno != EEXIST) break;
    }
    if (fd < 0) return fail(QString::fromLocal8Bit(std::strerror(errno)), error);
    // A file replaced keeps its permissions; a new one has the usual.
    int why = 0;
    if (exists && S_ISREG(there.st_mode) && ::fchmod(fd, there.st_mode & 07777) != 0) why = errno;
    for (qsizetype done = 0; why == 0 && done < bytes.size();) {
        const ssize_t wrote = ::write(fd, bytes.constData() + done, size_t(bytes.size() - done));
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) why = wrote < 0 ? errno : EIO;
        else done += wrote;
    }
    if (why == 0 && ::fsync(fd) != 0) why = errno;
    ::close(fd);
    if (why != 0) return fail(QString::fromLocal8Bit(std::strerror(why)), error);
    return true;
}

bool PendingFile::commit(QString *error)
{
    if (m_temporary.isEmpty()) return fail(QStringLiteral("nothing was written"), error);
    // A link that took the file's place meanwhile is refused as before.
    if (!m_replaceLink) {
        if (const QString why = linkRefusal(m_path); !why.isEmpty()) {
            *error = why;
            return false;
        }
    }
    // The name itself is replaced, whatever is there: never what a link leads to.
    if (::renameat(m_folder, m_temporary.constData(), m_folder, m_name.constData()) != 0)
        return fail(QString::fromLocal8Bit(std::strerror(errno)), error);
    m_temporary.clear();
    return true;
}

bool writeFile(const QString &path, const QByteArray &bytes, QString *error, bool replaceLink)
{
    PendingFile file(path, replaceLink);
    return file.write(bytes, error) && file.commit(error);
}

QString relativeTo(const QString &outputFolder, const QString &absolutePath)
{
    return QDir(outputFolder).relativeFilePath(absolutePath);
}

QString count(int n, const QString &one, const QString &many)
{
    return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
}

namespace {

bool holdsPictures(const md::Node &node)
{
    if (node.type == md::NodeType::MathInline || node.type == md::NodeType::MathBlock ||
        node.type == md::NodeType::MermaidBlock)
        return true;
    return std::any_of(node.children.cbegin(), node.children.cend(), holdsPictures);
}

} // namespace

std::unique_ptr<ExportPictures> drawPictures(const md::Document &document, const ExportRun &run, qreal fontSize,
                                             qreal share, bool *cancelled)
{
    *cancelled = run.stopped();
    if (*cancelled || !run.pictures || !holdsPictures(document.root)) return nullptr;
    ExportPictures::Options options;
    options.service = run.render;
    options.fontSize = fontSize;
    auto pictures = std::make_unique<ExportPictures>(options);
    run.report(0, QStringLiteral("Drawing formulas and diagrams"));
    const bool done = pictures->draw(document, run.cancelled, [&](int drawn, int total) {
        run.report(share * drawn / std::max(total, 1), QStringLiteral("Drawing formulas and diagrams"));
    });
    if (!done) {
        *cancelled = true;
        return nullptr;
    }
    return pictures;
}

ExportResult cancelled()
{
    ExportResult result;
    result.cancelled = true;
    result.error = QStringLiteral("the export was cancelled");
    return result;
}

QString frontMatterText(const md::Document &document, const QString &key)
{
    const md::YamlValue *value = document.frontMatter().data.find(key);
    if (!value) return {};
    if (value->isScalar()) return value->scalar.simplified();
    // A list of names, as `author:` often is.
    if (value->isList()) {
        QStringList items;
        for (const md::YamlValue &item : value->list)
            if (item.isScalar() && !item.scalar.trimmed().isEmpty()) items.append(item.scalar.simplified());
        return items.join(QStringLiteral(", "));
    }
    return {};
}

QString title(const md::Document &document, const ExportRecipe &recipe)
{
    QString title = recipe.options.value(QStringLiteral("title")).simplified();
    if (title.isEmpty()) title = frontMatterText(document, QStringLiteral("title"));
    if (title.isEmpty() && !document.headings.empty()) title = document.headings.front().text.simplified();
    if (title.isEmpty()) title = QFileInfo(recipe.source).completeBaseName();
    if (title.isEmpty()) title = QStringLiteral("Untitled");
    return title;
}

QString language(const md::Document &document)
{
    QString language = frontMatterText(document, QStringLiteral("lang"));
    if (language.isEmpty()) language = frontMatterText(document, QStringLiteral("language"));
    return language;
}

} // namespace Exporters
