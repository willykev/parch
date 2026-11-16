#include "filerequests.h"

#include "document.h"
#include "documentstore.h"
#include "filepicker.h"
#include "pathtext.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "windowcontext.h"

#include <QDir>
#include <QFileInfo>
#include <QSemaphore>
#include <QStandardPaths>
#include <QThreadPool>
#include <memory>
#include <utility>

FileRequests::FileRequests(DocumentStore *store, Settings *settings, QObject *parent)
    : QObject(parent), m_store(store), m_settings(settings),
      m_fallbacks{QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), QDir::homePath()}
{
    // A name that failed before is no longer where the next save goes, once
    // a save has landed or the document has gone.
    connect(store, &DocumentStore::documentSaved, this,
            [this](Document *document) { m_failedNames.remove(document->id()); });
    connect(store, &DocumentStore::documentClosed, this, [this](const QString &documentId) {
        m_failedNames.remove(documentId);
        m_namings.remove(documentId);
    });
    connect(store, &DocumentStore::saveFinished, this, &FileRequests::saveFinished);
}

void FileRequests::setPicker(FilePicker *picker)
{
    if (m_picker) disconnect(m_picker, nullptr, this, nullptr);
    m_picker = picker;
    m_request = {};
    if (!picker) return;
    connect(picker, &FilePicker::documentsSelected, this, &FileRequests::documentsPicked);
    connect(picker, &FilePicker::filesSelected, this, &FileRequests::filesPicked);
    connect(picker, &FilePicker::folderSelected, this, &FileRequests::folderPicked);
    connect(picker, &FilePicker::saveSelected, this, &FileRequests::saveTargetPicked);
    connect(picker, &FilePicker::failed, this, &FileRequests::pickerFailed);
}

void FileRequests::setFallbackFolders(const QStringList &folders)
{
    if (!folders.isEmpty()) m_fallbacks = folders;
}

QString FileRequests::firstFolder(const QStringList &candidates, const QString &fallback)
{
    for (const QString &candidate : candidates)
        if (PathText::isFullPath(candidate) && QFileInfo(candidate).isDir()) return candidate;
    return fallback;
}

QString FileRequests::startFolder() const
{
    const QStringList candidates = QStringList{m_settings->lastFolder()} + m_fallbacks.mid(0, m_fallbacks.size() - 1);
    const QString fallback = m_fallbacks.last();
    // Shared with the look, which may end long after the dialog has opened.
    struct Answer {
        QSemaphore given;
        QString folder;
    };
    const auto answer = std::make_shared<Answer>();
    DiskFile::looks()->start([answer, candidates, fallback] {
        answer->folder = firstFolder(candidates, fallback);
        answer->given.release();
    });
    return answer->given.tryAcquire(1, StartFolderWaitMs) ? answer->folder : fallback;
}

bool FileRequests::askForDocuments(WindowContext *window)
{
    if (!ask({Asked::Documents, window, nullptr, false})) return false;
    m_picker->setStartFolder(startFolder());
    m_picker->openDocuments();
    return true;
}

bool FileRequests::askForFolder(WindowContext *window)
{
    if (!ask({Asked::Folder, window, nullptr, false})) return false;
    m_picker->setStartFolder(startFolder());
    m_picker->openFolder();
    return true;
}

bool FileRequests::askForName(WindowContext *window, Document *document, bool closeAfter)
{
    if (!ask({Asked::SaveTarget, window, document, closeAfter})) return false;
    m_picker->saveDocument(QUrl::fromLocalFile(suggestedPath(window, document)));
    return true;
}

bool FileRequests::askForFiles(WindowContext *window, const QString &purpose, const QString &title,
                               const QString &filterName, const QStringList &patterns, bool multiple)
{
    const bool folder = patterns.isEmpty();
    if (!ask({folder ? Asked::FolderFor : Asked::Files, window, nullptr, false, purpose})) return false;
    m_picker->setStartFolder(startFolder());
    if (folder) m_picker->openFolder();
    else m_picker->openFiles(title, filterName, patterns, multiple);
    return true;
}

bool FileRequests::askForSaveFile(WindowContext *window, const QString &purpose, const QString &title,
                                  const QString &suggested, const QString &filterName, const QStringList &patterns)
{
    if (!ask({Asked::SaveFile, window, nullptr, false, purpose})) return false;
    m_picker->saveFile(title, QUrl::fromLocalFile(suggested), filterName, patterns);
    return true;
}

void FileRequests::filesPicked(const QList<QUrl> &urls)
{
    if (m_request.asked != Asked::Files) return;
    const Request request = std::exchange(m_request, {});
    emit answered();
    QStringList files;
    for (const QUrl &url : urls)
        if (url.isLocalFile() && PathText::isFullPath(url.toLocalFile())) files.append(url.toLocalFile());
    if (files.isEmpty()) {
        emit problem(tr("No file chosen"), tr("Only local files can be chosen."));
        return;
    }
    emit filesChosen(request.window, request.purpose, files);
}

bool FileRequests::retryNaming(WindowContext *window, Document *document, bool closeAfter)
{
    const QString failed = m_failedNames.value(document->id());
    if (failed.isEmpty()) return false;
    saveTo(window, document, failed, closeAfter);
    return true;
}

bool FileRequests::ask(const Request &request)
{
    if (!m_picker) {
        emit problem(tr("No file dialog"), tr("The desktop's file dialog cannot be reached, so parch cannot ask for a file."));
        return false;
    }
    // A newer question replaces one still open.
    m_request = request;
    return true;
}

QString FileRequests::suggestedPath(WindowContext *window, Document *document) const
{
    if (!document->isDraft()) return document->path();
    // A draft standing for a file that does not exist yet is offered that file.
    TabModel *tabs = window->tabs();
    const QString standsFor = tabs->data(tabs->index(tabs->indexOf(document)), TabModel::PathRole).toString();
    if (!standsFor.isEmpty()) return standsFor;
    return startFolder() + u'/' + Document::suggestedFileName(document->buffer()->text());
}

void FileRequests::documentsPicked(const QList<QUrl> &urls)
{
    if (m_request.asked != Asked::Documents) return;
    const Request request = std::exchange(m_request, {});
    emit answered();
    QStringList files, refused;
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) files.append(url.toLocalFile());
        else refused.append(url.toDisplayString());
    }
    emit documentsChosen(request.window, files);
    if (!refused.isEmpty())
        emit problem(tr("Some files could not be opened"),
                     tr("Only local files can be opened:\n%1").arg(refused.join(u'\n')));
}

void FileRequests::folderPicked(const QUrl &url)
{
    if (m_request.asked == Asked::FolderFor) {
        const Request request = std::exchange(m_request, {});
        emit answered();
        if (!url.isLocalFile() || !PathText::isFullPath(url.toLocalFile())) {
            emit problem(tr("No folder chosen"), tr("Only a local folder can be chosen."));
            return;
        }
        emit filesChosen(request.window, request.purpose, {url.toLocalFile()});
        return;
    }
    if (m_request.asked != Asked::Folder) return;
    const Request request = std::exchange(m_request, {});
    emit answered();
    const QString path = url.toLocalFile();
    if (!url.isLocalFile() || !PathText::isFullPath(path)) {
        emit problem(tr("Folder not opened"), tr("Only a local folder can join the library."));
        return;
    }
    m_settings->setLastFolder(path);
    emit folderChosen(request.window, path);
}

void FileRequests::saveTargetPicked(const QUrl &url)
{
    if (m_request.asked == Asked::SaveFile) {
        const Request request = std::exchange(m_request, {});
        emit answered();
        const QString path = url.toLocalFile();
        if (!url.isLocalFile() || !PathText::isFullPath(path)) {
            emit problem(tr("Nothing written"), tr("Only a local file can be chosen."));
            return;
        }
        emit saveFileChosen(request.window, request.purpose, path);
        return;
    }
    if (m_request.asked != Asked::SaveTarget) return;
    const Request request = std::exchange(m_request, {});
    if (!request.document) return;
    emit answered();
    const QString path = url.toLocalFile();
    if (!url.isLocalFile() || !PathText::isFullPath(path)) {
        emit problem(tr("Not saved"), tr("A document can be saved only to a local file."));
        return;
    }
    saveTo(request.window, request.document, path, request.closeAfter);
}

void FileRequests::pickerFailed(const QString &message)
{
    m_request = {};
    emit problem(tr("The file dialog failed"), message);
}

void FileRequests::saveTo(WindowContext *window, Document *document, const QString &path, bool closeAfter)
{
    m_failedNames.remove(document->id());
    // The naming's end is the store's to say: a document in conflict shows
    // the conflict, not the save, while a naming runs.
    m_namings.insert(document->id(), Naming{window, path, closeAfter});
    m_store->saveAs(document, path);
}

void FileRequests::saveFinished(Document *document, const QString &name, SaveStatus status)
{
    const auto naming = m_namings.constFind(document->id());
    if (naming == m_namings.cend() || naming->path != name) return;
    const Naming ended = *naming;
    m_namings.erase(naming);
    // A name that could not be taken fails the save; the document is where it was.
    if (status != SaveStatus::Saved) {
        m_failedNames.insert(document->id(), name);
        return;
    }
    if (document->isDraft()) return;
    m_settings->setLastFolder(PathText::folderOf(document->path()));
    emit named(ended.window, document, ended.closeAfter);
}
