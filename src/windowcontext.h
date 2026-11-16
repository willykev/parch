#pragma once

#include "session.h"
// The tabs and the library are properties QML reads, so their types must be complete here.
#include "foldersearch.h"
#include "librarymodel.h"
#include "tabmodel.h"

#include <QObject>
#include <QSize>

class DocumentStore;

// What one window owns: its tabs and the one in front, its library, whether
// the library is shown beside the editor, and its size. The window's QML reads it as its
// `context`; the backend keeps one per window and writes them to the session.
class WindowContext : public QObject {
    Q_OBJECT
    Q_PROPERTY(TabModel *tabs READ tabs CONSTANT)
    // What its library shows, with the window's own filter and expanded folders.
    Q_PROPERTY(LibraryModel *library READ library CONSTANT)
    Q_PROPERTY(bool libraryOpen READ libraryOpen WRITE setLibraryOpen NOTIFY libraryOpenChanged)
    // The library's width in this window, in pixels, which the session
    // keeps; a new window starts at the default the settings keep.
    Q_PROPERTY(int libraryWidth READ libraryWidth WRITE setLibraryWidth NOTIFY libraryWidthChanged)
    // The size the window opens at, set before the window is made; invalid
    // for the default.
    Q_PROPERTY(QSize initialSize READ initialSize CONSTANT)
    Q_PROPERTY(QSize size READ size WRITE setSize NOTIFY sizeChanged)
    // A library folder the window is to show, with the library open on it,
    // until the window has shown it and empties this.
    Q_PROPERTY(QString folderToShow READ folderToShow WRITE setFolderToShow NOTIFY folderToShowChanged)
    // Whether the keys go to that folder's row. They stay with the text when
    // a document was opened with it.
    Q_PROPERTY(bool folderTakesKeys READ folderTakesKeys NOTIFY folderToShowChanged)
    // The window's search across the library, with its query and results;
    // null until the backend gives it one.
    Q_PROPERTY(FolderSearch *folderSearch READ folderSearch CONSTANT)

public:
    // Takes the library's model as its own.
    WindowContext(DocumentStore *store, LibraryModel *library, QObject *parent = nullptr);

    TabModel *tabs() const;
    LibraryModel *library() const;
    bool libraryOpen() const;
    void setLibraryOpen(bool open);
    int libraryWidth() const;
    // Keeps a width within Settings' range for it.
    void setLibraryWidth(int width);
    QSize initialSize() const;
    QSize size() const;
    void setSize(const QSize &size);

    // The window as the session keeps it. Drafts that were never written in
    // hold nothing and are left out.
    WindowState state() const;
    // Takes a session's window: its tabs, waiting to be read, its library and its size.
    void restore(const WindowState &state);

    // Asks the window to put where its editor is into the front tab, before
    // the state is read.
    void captureView();
    // Asks the window to come to the front.
    void raise();
    // Opens the library on one of its folders, now or as soon as the window
    // shows, with the keys on its row unless they are to stay with the text.
    void showFolder(const QString &path, bool takesKeys = true);
    QString folderToShow() const;
    bool folderTakesKeys() const;
    // Asks the window to give the keys to the text in front, as when a
    // document has been opened in it.
    void focusText();
    void setFolderToShow(const QString &path);
    // Asks the window whether to close the document's tab, whose text is in
    // neither its file nor any other place the writer can see; the tab is
    // in front. Discarding, it asks whether to give up text that nothing
    // will keep.
    void askToClose(Document *document, bool discarding = false);
    // Asks the window whether saving the document, read in another
    // encoding, may write UTF-8 over its file.
    void askToConfirmEncoding(Document *document);
    // The window's editor moved its cursor or scrolled.
    Q_INVOKABLE void noteViewMoved();
    FolderSearch *folderSearch() const;
    // Takes the search as its own; given once, before any window shows it.
    void setFolderSearch(FolderSearch *search);
    // Asks the window to show the front tab where its view is now, as when
    // the view was set to a place in the text.
    void showView();

signals:
    void libraryOpenChanged();
    void libraryWidthChanged();
    void sizeChanged();
    void viewCaptureRequested();
    void raiseRequested();
    void folderToShowChanged();
    void textFocusRequested();
    void closeConfirmationRequested(Document *document, bool discarding);
    void encodingConfirmationRequested(Document *document);
    void viewShowRequested();
    // Something the session keeps changed.
    void changed();

private:
    TabModel *m_tabs;
    LibraryModel *m_library;
    bool m_libraryOpen = true;
    int m_libraryWidth = 260;
    QSize m_initialSize;
    QSize m_size;
    QString m_folderToShow;
    bool m_folderTakesKeys = true;
    FolderSearch *m_folderSearch = nullptr;
};
