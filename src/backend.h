#pragma once

#include "apppaths.h"
// The attachments, documents, the settings, the commands, the review and the
// organizer are properties QML reads, so their types must be complete here.
#include "attachments.h"
#include "commandregistry.h"
#include "document.h"
#include "documentreview.h"
#include "documentsettingsstore.h"
#include "fileorganizer.h"
#include "history.h"
#include "session.h"
#include "settings.h"
#include "spellchecker.h"

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class Autosave;
class DocumentStore;
class FilePicker;
class FileRequests;
class ReadingPositions;
class QThreadPool;
class RecentFiles;
class Recovery;
class WindowContext;

// What the windows work with: the store of open documents, autosave and
// recovery behind it, local history, the settings, and one WindowContext per
// window. A window's actions call in here and act on the active window, the
// one that last came to the front. What the desktop's file dialogs are asked
// and answer is FileRequests'; looking back at versions and conflicts is
// DocumentReview's, which QML reaches as `review`. The windows and their tabs
// are written to the session two seconds after they change and when the
// application ends, off the UI thread. The application ends only through
// quit(), once every text is safe on disk.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(Settings *settings READ settings CONSTANT)
    // Everything a window can do, with its keys; the application's own
    // actions run here, for the window that asks.
    Q_PROPERTY(CommandRegistry *commands READ commands CONSTANT)
    // Why the last action could not be done, in a few words; empty when
    // nothing is wrong. A refused quit stays until every text it found
    // unsafe is in its file or in recovery; anything else is a notice, which
    // stays until it is dismissed or the next action.
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    // The whole of it.
    Q_PROPERTY(QString problemDetail READ problemDetail NOTIFY problemChanged)
    // The problem is a notice, which dismissNotice() clears, rather than a refused quit.
    Q_PROPERTY(bool notice READ notice NOTIFY problemChanged)
    // What the notice tells of: a failure; a failure whose harm was headed
    // off, such as a draft kept because history could not take it; or news,
    // such as text that came back from recovery.
    Q_PROPERTY(NoticeTone noticeTone READ noticeTone NOTIFY problemChanged)
    // The notice is news, with no failure behind it.
    Q_PROPERTY(bool noticeInforms READ noticeInforms NOTIFY problemChanged)
    // History records nothing new until room is made in it.
    Q_PROPERTY(bool historyPaused READ historyPaused NOTIFY historyPausedChanged)
    // Paused because named versions alone fill its space, rather than
    // because the last version did not fit.
    Q_PROPERTY(bool historyFull READ historyFull NOTIFY historyPausedChanged)
    Q_PROPERTY(DocumentReview *review READ review CONSTANT)
    // Why recovery could not take a document's text, by document id: from a
    // failed write until a write succeeds or the text no longer needs it.
    Q_PROPERTY(QVariantMap recoveryFailures READ recoveryFailures NOTIFY recoveryFailuresChanged)
    // Where pasted images and dropped files are placed; what fails there is a notice.
    Q_PROPERTY(Attachments *attachments READ attachments CONSTANT)
    // Makes, renames, moves and trashes the library's files and folders.
    Q_PROPERTY(FileOrganizer *organizer READ organizer CONSTANT)
    // English spelling for every window.
    Q_PROPERTY(SpellChecker *spelling READ spelling CONSTANT)
    // The Markdown settings of documents, library folders and every
    // document, which every open document is read in.
    Q_PROPERTY(DocumentSettingsStore *documentSettings READ documentSettings CONSTANT)

public:
    enum NoticeTone { Failure, Warning, News };
    Q_ENUM(NoticeTone)

    static constexpr int SessionDelayMs = 2000;
    // How long an end that cannot be refused waits for saves to land.
    static constexpr int EndWaitMs = 3000;

    explicit Backend(const AppPaths &paths, QObject *parent = nullptr);
    ~Backend() override;

    DocumentStore *store() const;
    Recovery *recovery() const;
    Autosave *autosave() const;
    History *history() const;
    Settings *settings() const;
    CommandRegistry *commands() const;
    // The files opened, saved and named most recently.
    RecentFiles *recentFiles() const;
    // Where the writer was in each file, which a file opened again in a new
    // tab starts at. Kept as tabs close and as the session is written.
    ReadingPositions *readingPositions() const;
    FileOrganizer *organizer() const;
    // Where the libraries ask the file system, apart from where documents
    // are saved, so that a folder that never answers cannot hold up a save.
    QThreadPool *libraryPool() const;
    QString problem() const;
    QString problemDetail() const;
    bool notice() const;
    NoticeTone noticeTone() const;
    bool noticeInforms() const;
    bool historyPaused() const;
    bool historyFull() const;
    // The file dialogs' questions and answers.
    FileRequests *fileRequests() const;
    DocumentReview *review() const;
    QVariantMap recoveryFailures() const;
    Attachments *attachments() const;
    SpellChecker *spelling() const { return m_spelling; }
    DocumentSettingsStore *documentSettings() const { return m_documentSettings; }
    QList<WindowContext *> windows() const;
    // The window that came to the front last, or the oldest window when none has yet.
    WindowContext *activeWindow() const;
    // The document in front of the active window.
    Document *document() const;

    // The desktop's open and save dialogs. Without one, the actions that need
    // it say so and change nothing.
    void setFilePicker(FilePicker *picker);
    // As FileRequests::firstFolder().
    static QString firstFolder(const QStringList &candidates, const QString &fallback);
    // A message as the writer reads it: a draft's file and parch's own
    // folders named by what they are for, and the home folder as ~.
    Q_INVOKABLE QString shownText(const QString &text) const;

    // Brings back the last session, then opens the files. The first launch,
    // or a launch with freshDraftOnLaunch, also opens one blank draft. Text
    // that came back from recovery is named in a notice, and so is text
    // that stayed there; when none came back, the notice is titled for
    // that, as a failure headed off.
    void start(const QStringList &files);
    // Restores the text that only recovery holds, then the session's windows
    // with their tabs. Each restored document goes to the tab the session
    // names for it, or to the first window, and each window reads only the
    // document in front; the others wait until they are shown. Returns a
    // line for what came back from recovery, and one for what stayed there
    // because another document has its file, naming the documents, or
    // nothing when recovery held nothing its files did not. `onlyKept`,
    // when given, says whether nothing came back and something stayed.
    QStringList restoreSession(bool *onlyKept = nullptr);
    // Opens full paths and brings the window that shows them to the front.
    // A document already open in any window comes to the front there;
    // anything else opens in the active window. A file that does not exist,
    // in a folder that does, opens as an empty document named after it that
    // becomes the file at its first edit, unless a file has appeared there
    // meanwhile: that one is never replaced, and the text stays a draft. A
    // path in a missing folder is a tab that says the file is not found. A
    // folder joins the library, which the window opens on it.
    Q_INVOKABLE void openFiles(const QStringList &files);
    // The same, for files a later launch handed over: the window that shows
    // them is raised with that launch's activation token, so that a desktop
    // that keeps windows from taking the focus lets it come to the front.
    // Without a token, it is raised as openFiles() raises it.
    void openForwarded(const QStringList &files, const QString &activationToken);
    // A window came to the front.
    Q_INVOKABLE void activate(WindowContext *window);
    // Closes a window, keeping the text of its documents as closing a tab
    // does; text that cannot be kept keeps the window open. A document only
    // this window shows whose save failed or that is in conflict comes to the
    // front and asks first. Closing the last window quits.
    Q_INVOKABLE void closeWindow(WindowContext *window);

    // Where a document lives, as the header names it, its folders joined by
    // " / ": from the library folder that holds it, by that folder's own
    // name, the nearest such folder when several do; otherwise from the
    // home folder, written as ~, or from the root. Drafts for a draft.
    // `libraryFolders` are the library's; the first form takes them from
    // the settings.
    Q_INVOKABLE QString folderLabel(const QString &path) const;
    Q_INVOKABLE QString folderLabel(const QString &path, const QStringList &libraryFolders) const;

    // Shows a new, empty draft. It has no file until something is written in it.
    Q_INVOKABLE void newDraft();
    // Saves the document in front, off the UI thread. A draft is asked a
    // name first, with its first heading or words in the folder last used.
    Q_INVOKABLE void save();
    // Asks where to save the document in front and saves it there, keeping
    // its identity, text and undo.
    Q_INVOKABLE void saveAs();
    // Saves again a document whose save failed, to where it was going.
    Q_INVOKABLE void retrySave();
    // Reads again the file of the document in front, which waits because
    // its file could not be read.
    Q_INVOKABLE void readAgain();
    // Writes recovery again for the document in front, whose last recovery
    // write failed.
    Q_INVOKABLE void retryRecovery();
    // Clears a notice.
    Q_INVOKABLE void dismissNotice();
    // Saves what can be saved, keeps the rest in recovery, waits for every
    // save, for at most EndWaitMs, writes the session, and then asks to end
    // the application with quitRequested(). Text that neither its file nor
    // recovery could take keeps the application open, and problem says why
    // until that text is safe. A save still running then has its text in
    // recovery, and the application ends without waiting for it.
    Q_INVOKABLE void quit();
    // Ends at once, as when the session ends and nothing can be refused:
    // the session is written, every text not in its file goes to recovery,
    // and saves get at most waitMs to land. Returns false when a save is
    // still running then, whose text recovery holds.
    bool end(int waitMs);
    // A new window with one blank draft.
    Q_INVOKABLE void newWindow();
    // A second tab on the active window's front document, with a cursor and
    // scroll of its own.
    Q_INVOKABLE void newView();
    // Closes a tab of the active window. A document whose save failed or
    // that is in conflict asks first, when this is its last view. A named
    // document with unsaved text is saved, and its tab closes once the save
    // has landed, or asks if it failed. Otherwise the last view of a document
    // lets go of it only once its text is in its file or in recovery: a
    // modified draft starts saving and is written to recovery at once, and
    // text neither can take keeps its tab open, with the reason in problem.
    // Closing a window's last tab closes the window.
    Q_INVOKABLE void requestCloseTab(int index);
    // What the window answers when it asked whether to close the document
    // in front: save it again, or ask where to save it, and close it once it
    // is saved; or record its text in history, give up the rest, and close
    // it. Should history be paused when it is asked to keep the text, the
    // window asks again, since the text would then be kept nowhere.
    Q_INVOKABLE void closeTryingAgain();
    Q_INVOKABLE void closeSavingAs();
    Q_INVOKABLE void closeWithoutSaving();
    // The same, when the writer knows that nothing will keep the text: a
    // copy still goes to history if it can, but nothing waits on it.
    Q_INVOKABLE void discardText();

    // Opens a file the library lists, as openFiles() does, in the window
    // in front, except that a file no longer there is a tab that says it is
    // not found: the library never makes a file.
    Q_INVOKABLE void openFromLibrary(const QString &path);
    // Opens an unnamed draft, in the window that has a tab for it, or else
    // in the window in front.
    Q_INVOKABLE void openDraft(const QString &documentId);
    // Opens what a search found, a file or a draft, as the library opens
    // it, with the match selected a few lines below the top of the view:
    // where it was found, or, should the text have changed since, where the
    // same text is now, as TextSearch::relocate() finds it. `line` counts
    // from 1.
    Q_INVOKABLE void openSearchResult(const QString &path, const QString &documentId, int start, int length, int line,
                                      const QString &matched);

    // Asks for files to open, as openFiles() opens them.
    Q_INVOKABLE void open();
    // Asks for a folder, which joins the library and is shown there.
    Q_INVOKABLE void openFolder();

    // As DocumentReview's own, for the document in front. A dialog acts on
    // the document it shows, through `review`.
    Q_INVOKABLE void resolveConflict(int choice);
    Q_INVOKABLE void checkpoint(const QString &name);
    Q_INVOKABLE void restoreVersion(const QString &versionId);
    Q_INVOKABLE void nameVersion(const QString &versionId, const QString &name);
    Q_INVOKABLE void removeVersion(const QString &versionId);

    // Turns spelling on or off, as the writer's choice from now on. Turned
    // on without a dictionary, it says so.
    Q_INVOKABLE void setSpellchecking(bool on);

    // Asks the desktop's file dialog, for the active window, for a file that
    // document settings name: "bibliography" (BibTeX, BibLaTeX, CSL JSON or
    // CSL YAML files, several at once), "style" (a .csl file) or
    // "libraryRoot" (a folder). The answer is settingsFilesChosen().
    Q_INVOKABLE void chooseSettingsFiles(const QString &purpose);

signals:
    void settingsFilesChosen(const QString &purpose, const QStringList &paths);
    void problemChanged();
    void historyPausedChanged();
    void recoveryFailuresChanged();
    // Every text is safe and the application may end; main() quits on it,
    // at once if store()->savesRunning(), since such a save may never end.
    void quitRequested();
    // A window's context, with its tabs, before anything shows it.
    void windowAdded(WindowContext *window);
    // The window is closing; its context is deleted later.
    void windowRemoved(WindowContext *window);

private:
    // Registers every command, with the actions that are the application's.
    void addCommands();
    WindowContext *makeWindow();
    void announce(WindowContext *window);
    void watch(Document *document);
    void documentChanged();
    void openFilesIn(WindowContext *window, const QStringList &files);
    // What opening a path that has no file does.
    enum class Absent { NewFile, NotFound };
    // Returns the window that now shows the last of them.
    WindowContext *openIn(WindowContext *window, const QStringList &files, Absent absent = Absent::NewFile);
    // Adds a folder to the library and opens the window's library on it.
    void showLibraryFolder(WindowContext *window, const QString &path, bool takesKeys = true);
    // Brings the document to the front in the window with a tab that shows
    // it or waits for it, or else in a new tab of the given window. Returns
    // the window that shows it.
    WindowContext *showDocument(WindowContext *window, Document *document);
    // A draft standing for a file becomes that file at its first edit, but
    // only while the file is still as absent as when the tab was made.
    void nameOnFirstEdit(Document *draft, const QString &path, const DiskVersion &absent);
    void name(Document *draft, const QString &path, const DiskVersion &absent);
    void namingEnded(Document *draft, const QString &path);

    void closeTab(WindowContext *window, int index);
    // The window asks whether to close the document's tab, with the tab in
    // front; when discarding, whether to give up text nothing will keep.
    void askToClose(WindowContext *window, Document *document, bool discarding = false);
    // Saves the document and closes its tab once the save has landed; a
    // failure asks instead.
    void closeOnceSaved(WindowContext *window, Document *document);
    void closeGivingUp(bool keepInHistory);
    // Records the text in history and then gives it up, once history has
    // the newest text; when keeping it in history was the point, a failure
    // keeps the tab open and a pause asks again.
    void recordThenGiveUp(WindowContext *window, Document *document, bool keepInHistory);
    // Closes the document's tab and gives up its text.
    void giveUp(WindowContext *window, Document *document);
    // Makes the text of a document whose last view is about to close safe:
    // a modified one starts saving and is written to recovery at once. False,
    // with the reason, when neither its file nor recovery holds the text.
    bool keepText(Document *document, QString *reason);
    // How many tabs of all windows show the document, leaving out one window.
    int viewsOf(const Document *document, const WindowContext *leavingOut = nullptr) const;
    void refuseClosing(const QList<Document *> &documents, const QStringList &reasons);
    void scheduleSession();
    SessionState collectSession() const;
    // Keeps where the window's tabs are in their files, the front one as
    // its editor has it now.
    void rememberPositions(WindowContext *window) const;

    void forgetRecoveryFailure(const QString &documentId);
    // Spelling is wanted as the settings say, or, until the writer chooses,
    // whenever a dictionary is found.
    void followSpelling();
    // Says that spelling is wanted but there is no dictionary to check it with.
    void sayDictionaryIsMissing();

    void setProblem(const QString &problem, const QString &detail);
    // A problem that the next action clears.
    void notify(const QString &problem, const QString &detail);
    // The same for news with no failure behind it.
    void inform(const QString &news, const QString &detail);
    // Shows a notice of any tone, or adds its detail to a refused quit's.
    void post(const QString &problem, const QString &detail, NoticeTone tone);
    void clearNotice();
    // Forgets the unsafe documents that are now clean or closed, and clears
    // the problem once none is left.
    void checkUnsafe();
    void forgetUnsafe(const QString &documentId);

    AppPaths m_paths;
    // Declared in the order they depend on each other, so that autosave goes
    // before the store it saves through, and the store before recovery.
    std::unique_ptr<Recovery> m_recovery;
    std::unique_ptr<DocumentStore> m_store;
    std::unique_ptr<Autosave> m_autosave;
    Settings *m_settings;
    std::unique_ptr<CommandRegistry> m_commands;
    std::unique_ptr<RecentFiles> m_recentFiles;
    std::unique_ptr<QThreadPool> m_libraryPool;
    SessionWriter m_sessionWriter;
    QTimer m_sessionTimer;
    // In the order they were made, which the session keeps.
    QList<WindowContext *> m_windows;
    // The most recently active first.
    QList<WindowContext *> m_recent;
    QString m_problem;
    QString m_problemDetail;
    bool m_notice = false;
    NoticeTone m_tone = Failure;
    // The documents whose text a refused quit found in neither its file nor recovery.
    QSet<QString> m_unsafe;
    // Recovery writes started before the refusal are no evidence that the text is safe now.
    quint64 m_refusedAfter = 0;

    // Both go before the store, whose documents and history they use.
    std::unique_ptr<FileRequests> m_files;
    std::unique_ptr<DocumentReview> m_review;
    std::unique_ptr<ReadingPositions> m_positions;
    // Goes before the store, whose documents it moves.
    std::unique_ptr<FileOrganizer> m_organizer;
    History::Pause m_historyPause = History::Pause::None;
    QVariantMap m_recoveryFailures;
    // The documents whose tab closes once their save lands, by id.
    QSet<QString> m_closing;
    // Documents whose text was given up, whose recovery entries go once they
    // have closed: at once, or once a save that runs has ended.
    QSet<QString> m_givenUp;
    SpellChecker *m_spelling;
    DocumentSettingsStore *m_documentSettings;
    // A dictionary that is missing is said once in a run, unless spelling is turned on again.
    bool m_dictionaryMissingSaid = false;
    // Declared last, so that it goes first and its copies end before the
    // documents they insert into do.
    std::unique_ptr<Attachments> m_attachments;
};
