// Adapted from sprawl's src/filepicker.h (MIT).
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

// The desktop's own open and save dialogs. Abstract, so that tests can stand
// in a picker that answers without a desktop. Each request answers at most
// once, with a selection or failed(); a dialog the person cancels answers
// nothing, and a newer request replaces one still open.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    ~FilePicker() override = default;

    // Where the open dialogs start, a full path; empty for the dialog's own choice.
    void setStartFolder(const QString &folder) { m_startFolder = folder; }
    QString startFolder() const { return m_startFolder; }

    // One or more Markdown or text files to open.
    virtual void openDocuments() = 0;
    // A folder for the library.
    virtual void openFolder() = 0;
    // Where to save a document, starting from the suggested file.
    virtual void saveDocument(const QUrl &suggested) = 0;
    // Files of one kind to read, such as a bibliography: those `patterns`
    // match (globs, "*.bib"), named `filterName`, one or several. A picker
    // that cannot ask says failed().
    virtual void openFiles(const QString &title, const QString &filterName, const QStringList &patterns, bool multiple)
    {
        Q_UNUSED(title);
        Q_UNUSED(filterName);
        Q_UNUSED(patterns);
        Q_UNUSED(multiple);
        emit failed(tr("The desktop's file dialog cannot choose these files."));
    }

signals:
    void documentsSelected(const QList<QUrl> &urls);
    void filesSelected(const QList<QUrl> &urls);
    void folderSelected(const QUrl &url);
    void saveSelected(const QUrl &url);
    void failed(const QString &message);

private:
    QString m_startFolder;
};
