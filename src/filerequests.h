#pragma once

#include "diskfile.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

class Document;
class DocumentStore;
class FilePicker;
class Settings;
class WindowContext;

// What parch asks the desktop's file dialogs, and what it does with their
// answers: which files to open, which folder joins the library, and where a
// document is saved under a new name. A question remembers the window that
// asked it and, for a save, the document; its answer goes back to that
// window while it is open, and an answer to a question not asked, or no
// longer asked, is ignored. A name that could not be taken is remembered,
// so that trying again names the document there.
class FileRequests : public QObject {
    Q_OBJECT
public:
    FileRequests(DocumentStore *store, Settings *settings, QObject *parent = nullptr);

    // Without a picker, every question says so and changes nothing.
    void setPicker(FilePicker *picker);
    // The folders a dialog starts in when none was used last, in order; the
    // last is taken whether it is there or not. Documents, then home, unless
    // set otherwise.
    void setFallbackFolders(const QStringList &folders);
    // How long a dialog waits, at most, to learn which folder it starts in.
    static constexpr int StartFolderWaitMs = 250;
    // The first of the candidates that is a folder, or else the fallback.
    static QString firstFolder(const QStringList &candidates, const QString &fallback);
    // Where a dialog starts: the folder last opened from or saved to, or a
    // fallback. Which of them is a folder is asked off the UI thread, and
    // waited for at most StartFolderWaitMs: without an answer by then, as
    // from a mount that stopped answering, the last fallback.
    QString startFolder() const;

    // Each is false, with a problem said, when there is no file dialog.
    bool askForDocuments(WindowContext *window);
    bool askForFolder(WindowContext *window);
    // Suggests the document's own file, the file its tab stands for, or its
    // first heading or words as a name in the folder last used.
    bool askForName(WindowContext *window, Document *document, bool closeAfter);
    // Saves the document under the path and, once that has landed, keeps
    // its folder as the one last used and says named().
    void saveTo(WindowContext *window, Document *document, const QString &path, bool closeAfter);
    WindowContext *namingWindow(Document *document) const;
    // Names the document again where its last naming failed; false when none did.
    bool retryNaming(WindowContext *window, Document *document, bool closeAfter);
    // Files to read for a purpose of the asker's own, such as a document's
    // bibliography, as FilePicker::openFiles() asks for them; or a folder
    // for one (a folder with `patterns` empty). The answer is filesChosen().
    bool askForFiles(WindowContext *window, const QString &purpose, const QString &title, const QString &filterName,
                     const QStringList &patterns, bool multiple);
    // Where to write a file for a purpose of the asker's own, such as an
    // export, starting from the suggested path, as FilePicker::saveFile()
    // asks. The answer is saveFileChosen().
    bool askForSaveFile(WindowContext *window, const QString &purpose, const QString &title, const QString &suggested,
                        const QString &filterName, const QStringList &patterns);

signals:
    // A question was answered, before what it does.
    void answered();
    // The window is the one that asked, or null once it has gone.
    void documentsChosen(WindowContext *window, const QStringList &files);
    void folderChosen(WindowContext *window, const QString &path);
    // The answer to askForFiles(): local full paths, at least one.
    void filesChosen(WindowContext *window, const QString &purpose, const QStringList &paths);
    // The answer to askForSaveFile(): a local full path.
    void saveFileChosen(WindowContext *window, const QString &purpose, const QString &path);
    // The document took the name it was given; its tab is to close if asked.
    void named(WindowContext *window, Document *document, bool closeAfter);
    void problem(const QString &problem, const QString &detail);

private:
    // Files and FolderFor are asked for a purpose of the asker's own.
    enum class Asked { Nothing, Documents, Folder, SaveTarget, Files, FolderFor, SaveFile };
    struct Request {
        Asked asked = Asked::Nothing;
        QPointer<WindowContext> window;
        QPointer<Document> document;
        // The document's tab closes once it is saved where the dialog says.
        bool closeAfter = false;
        QString purpose;
    };

    bool ask(const Request &request);
    QString suggestedPath(WindowContext *window, Document *document) const;
    void documentsPicked(const QList<QUrl> &urls);
    void filesPicked(const QList<QUrl> &urls);
    void folderPicked(const QUrl &url);
    void saveTargetPicked(const QUrl &url);
    void pickerFailed(const QString &message);
    // A save the store has finished, which may be a naming saveTo asked for.
    void saveFinished(Document *document, const QString &name, SaveStatus status);

    // A naming asked for and not yet ended.
    struct Naming {
        QPointer<WindowContext> window;
        QString path;
        bool closeAfter = false;
    };

    DocumentStore *m_store;
    Settings *m_settings;
    QPointer<FilePicker> m_picker;
    Request m_request;
    QStringList m_fallbacks;
    // Where a document's last naming failed to save it, by document id.
    QHash<QString, QString> m_failedNames;
    // The naming each document waits for, by document id; a newer one replaces it.
    QHash<QString, Naming> m_namings;
};
