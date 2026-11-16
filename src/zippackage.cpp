#include "zippackage.h"

#include <zip.h>

namespace {

QString libzipError(int code)
{
    zip_error_t error;
    zip_error_init_with_code(&error, code);
    const QString text = QString::fromUtf8(zip_error_strerror(&error));
    zip_error_fini(&error);
    return text;
}

QString archiveError(zip_t *archive) { return QString::fromUtf8(zip_strerror(archive)); }

// 1 January 2020, local midnight: a time ZIP's DOS dates can hold, the same
// for every export.
constexpr time_t kFixedTime = 1577836800;

} // namespace

void ZipPackage::add(const QString &name, const QByteArray &data, bool compress)
{
    m_files.append({name, data, compress});
}

bool ZipPackage::contains(const QString &name) const
{
    return std::any_of(m_files.cbegin(), m_files.cend(), [&](const File &file) { return file.name == name; });
}

QByteArray ZipPackage::bytes(QString *error) const
{
    zip_error_t failure;
    zip_error_init(&failure);
    zip_source_t *buffer = zip_source_buffer_create(nullptr, 0, 0, &failure);
    if (!buffer) {
        if (error) *error = QString::fromUtf8(zip_error_strerror(&failure));
        zip_error_fini(&failure);
        return {};
    }
    zip_t *archive = zip_open_from_source(buffer, ZIP_TRUNCATE, &failure);
    if (!archive) {
        if (error) *error = QString::fromUtf8(zip_error_strerror(&failure));
        zip_error_fini(&failure);
        zip_source_free(buffer);
        return {};
    }
    zip_error_fini(&failure);
    // Kept past zip_close(), which writes the archive into it.
    zip_source_keep(buffer);
    for (const File &file : m_files) {
        // libzip reads the data when it closes the archive; m_files holds it until then.
        zip_source_t *source = zip_source_buffer(archive, file.data.constData(), zip_uint64_t(file.data.size()), 0);
        const zip_int64_t index =
            source ? zip_file_add(archive, file.name.toUtf8().constData(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) : -1;
        if (index < 0) {
            if (source) zip_source_free(source);
            if (error) *error = archiveError(archive);
            zip_discard(archive);
            zip_source_free(buffer);
            return {};
        }
        zip_set_file_compression(archive, zip_uint64_t(index), file.compress ? ZIP_CM_DEFLATE : ZIP_CM_STORE, 9);
        zip_file_set_mtime(archive, zip_uint64_t(index), kFixedTime, 0);
    }
    if (zip_close(archive) < 0) {
        if (error) *error = archiveError(archive);
        zip_discard(archive);
        zip_source_free(buffer);
        return {};
    }
    QByteArray bytes;
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_source_open(buffer) < 0 || zip_source_stat(buffer, &stat) < 0) {
        if (error) *error = QStringLiteral("the archive cannot be read back");
        zip_source_free(buffer);
        return {};
    }
    bytes.resize(qsizetype(stat.size));
    const zip_int64_t read = zip_source_read(buffer, bytes.data(), stat.size);
    zip_source_close(buffer);
    zip_source_free(buffer);
    if (read < 0 || zip_uint64_t(read) != stat.size) {
        if (error) *error = QStringLiteral("the archive cannot be read back");
        return {};
    }
    return bytes;
}

bool ZipPackage::read(const QByteArray &archive, QList<Entry> *entries, QString *error)
{
    zip_error_t failure;
    zip_error_init(&failure);
    zip_source_t *source = zip_source_buffer_create(archive.constData(), zip_uint64_t(archive.size()), 0, &failure);
    zip_t *zip = source ? zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, &failure) : nullptr;
    if (!zip) {
        if (error) *error = QString::fromUtf8(zip_error_strerror(&failure));
        zip_error_fini(&failure);
        if (source) zip_source_free(source);
        return false;
    }
    zip_error_fini(&failure);
    const zip_int64_t count = zip_get_num_entries(zip, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        zip_stat_t stat;
        if (zip_stat_index(zip, zip_uint64_t(i), 0, &stat) < 0) continue;
        Entry entry;
        entry.name = QString::fromUtf8(stat.name);
        entry.stored = stat.comp_method == ZIP_CM_STORE;
        zip_file_t *file = zip_fopen_index(zip, zip_uint64_t(i), 0);
        if (!file) {
            if (error) *error = libzipError(ZIP_ER_READ);
            zip_close(zip);
            return false;
        }
        entry.data.resize(qsizetype(stat.size));
        zip_fread(file, entry.data.data(), stat.size);
        zip_fclose(file);
        entries->append(entry);
    }
    zip_close(zip);
    return true;
}
