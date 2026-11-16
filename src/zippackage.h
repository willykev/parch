#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// A ZIP archive made in memory with libzip, as DOCX and EPUB files are: the
// files in the order they were added, each deflated or stored as asked, with
// one fixed time so that the same document packs the same way. The caller
// writes the bytes out in one step.
class ZipPackage {
public:
    void add(const QString &name, const QByteArray &data, bool compress = true);
    bool contains(const QString &name) const;
    // The archive; empty, with why, when libzip could not make it.
    QByteArray bytes(QString *error) const;

    // The files of an archive, for tests: names in order, with whether each
    // is stored, and their contents. False when it cannot be read.
    struct Entry {
        QString name;
        bool stored = false;
        QByteArray data;
    };
    static bool read(const QByteArray &archive, QList<Entry> *entries, QString *error = nullptr);

private:
    struct File {
        QString name;
        QByteArray data;
        bool compress = true;
    };
    QList<File> m_files;
};
