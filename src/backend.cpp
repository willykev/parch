#include "backend.h"

#include "autosave.h"
#include "document.h"
#include "documentreview.h"
#include "documentstore.h"
#include "filerequests.h"
#include "history.h"
#include "librarymodel.h"
#include "pathtext.h"
#include "recentfiles.h"
#include "recovery.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "windowcontext.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QThreadPool>
#include <memory>
#include <utility>

namespace {
// Closing the last view of such a document asks first: its text is in
// neither its file nor anywhere else the writer can see.
bool asksBeforeClosing(const Document *document)
{
    // A document with no text of its own has none to lose.
    if (document->isTextless()) return false;
    return document->state() == Document::State::SaveFailed || document->state() == Document::State::Conflict;
}

// What is said of text that came back from recovery, by what became of it;
// nothing of text its file held already.
QString recoveredLine(DocumentStore::Recovered how, const QStringList &titles)
{
    const QString names = titles.join(QStringLiteral(", "));
    if (how == DocumentStore::Recovered::Shown)
        return Backend::tr("parch ended before this text was in its file, and it came back from recovery: %1").arg(names);
    if (how == DocumentStore::Recovered::InHistory)
        return Backend::tr("The file had newer text, so the text from recovery is in history: %1").arg(names);
    return {};
}

// A full path that names a folder, or no file, spelled clean for the
// library or a tab to hold, whatever sent it: as given when it is spelled
// so, else with its folder resolved, which is asked of the kernel once.
QString spelledClean(const QString &path)
{
    return QDir::cleanPath(path) == path ? path : DiskFile::literalPath(path);
}
} // namespace

Backend::Backend(const AppPaths &paths, QObject *parent)
    : QObject(parent), m_paths(paths), m_recovery(std::make_unique<Recovery>(paths)),
      m_store(std::make_unique<DocumentStore>(paths)),
      m_autosave(std::make_unique<Autosave>(m_store.get(), m_recovery.get())),
      m_settings(new Settings(paths.settingsFile(), this)), m_recentFiles(std::make_unique<RecentFiles>(paths)),
      m_libraryPool(std::make_unique<QThreadPool>()), m_sessionWriter(paths),
      m_files(std::make_unique<FileRequests>(m_store.get(), m_settings)),
      m_review(std::make_unique<DocumentReview>(m_store.get()))
{
    // Two workers: enough to list a folder while another is slow, and few
    // enough that a folder that never answers costs little.
    m_libraryPool->setMaxThreadCount(2);
    // Text in recovery is safe, but only a write started after a refused quit
    // shows that the text it refused is; a document closes only once it is clean.
    connect(m_autosave.get(), &Autosave::recoveryWritten, this, [this](const QString &documentId, quint64 write) {
        if (write > m_refusedAfter) forgetUnsafe(documentId);
    });
    connect(m_store.get(), &DocumentStore::documentClosed, this, &Backend::forgetUnsafe);
    // Keep both opens the local text as a copy beside the file, in the
    // window in front, where the writer was in that text: the view of the
    // original in front of the window in front, or else its first tab in the
    // most recent window that shows it.
    connect(m_store.get(), &DocumentStore::conflictCopyOpened, this, [this](Document *copy, Document *original) {
        WindowContext *window = activeWindow();
        if (!window) return;
        std::optional<ViewState> view;
        for (WindowContext *holder : std::as_const(m_recent)) {
            TabModel *tabs = holder->tabs();
            const int index = tabs->activeDocument() == original ? tabs->activeIndex() : tabs->indexOf(original);
            if (index < 0) continue;
            holder->captureView();
            view = tabs->viewState(index);
            break;
        }
        window->tabs()->openDocument(copy, view);
    });
    // A document whose tab closed while its save ran, and whose save then
    // failed or met a conflict, comes back as a tab of the window in front,
    // behind the one being written in, and a notice says so.
    connect(m_store.get(), &DocumentStore::needsView, this, [this](Document *document) {
        WindowContext *window = activeWindow();
        if (!window) return;
        window->tabs()->append(document);
        const QString reason = document->lastError();
        if (document->state() == Document::State::Conflict)
            notify(tr("Changed on disk: %1").arg(document->title()),
                   tr("%1 changed on disk after its tab was closed, so its tab is open again: %2")
                       .arg(document->title(), reason));
        else
            notify(tr("Not saved: %1").arg(document->title()),
                   tr("%1 could not be saved after its tab was closed, so its tab is open again: %2")
                       .arg(document->title(), reason));
    });
    // A draft named while its own file held other text, which history could
    // not keep, leaves that text as a draft, and says where.
    connect(m_store.get(), &DocumentStore::draftKept, this,
            [this](Document *named, const QString &draftId, const QString &title) {
        const QString where = draftId.isEmpty()
                                  ? tr("It stays in %1.").arg(m_paths.drafts() + u'/' + named->id())
                                  : tr("It stays in the library's Drafts as %1.").arg(title);
        // The text is safe, but history failed to take it: a failure headed off.
        post(tr("Draft kept: %1").arg(title),
               tr("%1 was saved, but its draft held other text, which history could not keep. %2")
                   .arg(named->title(), where),
             Warning);
    });
    // The store names folders in its messages as the header does.
    m_store->setFolderNames([this](const QString &path) { return folderLabel(path); });
    // A draft named where its assets cannot go says where they stay.
    connect(m_store.get(), &DocumentStore::assetsKept, this, [this](Document *named, const QString &folder) {
        post(tr("Assets not moved: %1").arg(named->title()),
             tr("“assets” beside %1 is not a folder of its own, so nothing was put there. The draft's assets stay in %2.")
                 .arg(named->path(), folder),
             Warning);
    });
    // A copy Keep both made for a document that has moved on since is said.
    connect(m_store.get(), &DocumentStore::conflictCopyLeft, this, [this](const QString &title, const QString &copy) {
        post(tr("Conflict copy kept: %1").arg(PathText::fileName(copy)),
             tr("“%1” in “%2” holds the text as it was when Keep both was chosen. %3 had changed or closed since, "
                "so it was left as it is.")
                 .arg(PathText::fileName(copy), folderLabel(copy), title),
             News);
    });
    // Text recovery held since the launch, which came back when its path
    // was opened and led to its own file again, is said as the launch says
    // what came back, and kept safe from here as any unsaved text is.
    connect(m_store.get(), &DocumentStore::heldRestored, this,
            [this](Document *document, DocumentStore::Recovered how) {
        watch(document);
        const QString line = recoveredLine(how, {document->title()});
        if (!line.isEmpty()) inform(tr("Restored from recovery"), line);
    });
    // A file saved, or a draft named, is the newest recent file.
    connect(m_store.get(), &DocumentStore::documentSaved, this, [this](Document *document) {
        if (!document->isDraft()) m_recentFiles->note(document->path());
    });
    connect(m_store.get(), &DocumentStore::documentClosed, this, [this](const QString &documentId) {
        m_closing.remove(documentId);
        forgetRecoveryFailure(documentId);
    });
    // A recovery write that failed is shown until one succeeds.
    connect(m_autosave.get(), &Autosave::recoveryFailed, this, [this](const QString &documentId, const QString &error) {
        if (m_recoveryFailures.value(documentId).toString() == error) return;
        m_recoveryFailures.insert(documentId, error);
        emit recoveryFailuresChanged();
    });
    connect(m_autosave.get(), &Autosave::recoveryWritten, this,
            [this](const QString &documentId) { forgetRecoveryFailure(documentId); });
    // History says when it pauses, and why, from the thread that recorded,
    // so it is heard here in turn; whether it is paused already is asked off
    // the UI thread, since it reads every document's versions.
    connect(history(), &History::pausedChanged, this, [this](History::Pause pause) {
        if (pause == std::exchange(m_historyPause, pause)) return;
        emit historyPausedChanged();
    }, Qt::QueuedConnection);
    m_review->checkPaused();
    // The file dialogs' answers, and the dialogs' actions, are actions too:
    // each clears a notice, and what goes wrong is one.
    connect(m_files.get(), &FileRequests::answered, this, &Backend::clearNotice);
    connect(m_files.get(), &FileRequests::problem, this, &Backend::notify);
    connect(m_files.get(), &FileRequests::documentsChosen, this, [this](WindowContext *window, const QStringList &files) {
        openFilesIn(m_windows.contains(window) ? window : activeWindow(), files);
    });
    connect(m_files.get(), &FileRequests::folderChosen, this, [this](WindowContext *window, const QString &path) {
        if (!m_windows.contains(window)) window = activeWindow();
        if (!window) return;
        showLibraryFolder(window, path);
        window->raise();
    });
    connect(m_files.get(), &FileRequests::named, this, [this](WindowContext *window, Document *document, bool closeAfter) {
        if (!closeAfter || !m_windows.contains(window)) return;
        const int index = window->tabs()->indexOf(document);
        if (index >= 0) closeTab(window, index);
    });
    connect(m_review.get(), &DocumentReview::acting, this, &Backend::clearNotice);
    connect(m_review.get(), &DocumentReview::problem, this, &Backend::notify);
    m_sessionTimer.setSingleShot(true);
    m_sessionTimer.setInterval(SessionDelayMs);
    connect(&m_sessionTimer, &QTimer::timeout, this, [this] { m_sessionWriter.write(collectSession()); });
}

Backend::~Backend()
{
    m_sessionTimer.stop();
    // History and the store must outlive the reads and records still running.
    m_review.reset();
    // The windows let go of their tabs before the store goes.
    qDeleteAll(std::exchange(m_windows, {}));
    m_recent.clear();
    // The libraries went with their windows, and their jobs' results with
    // them. A job still waiting on a folder that does not answer is left to
    // end with the process rather than holding up its end.
    m_libraryPool->clear();
    if (!m_libraryPool->waitForDone(1000)) (void)m_libraryPool.release();
}

DocumentStore *Backend::store() const { return m_store.get(); }
Recovery *Backend::recovery() const { return m_recovery.get(); }
Autosave *Backend::autosave() const { return m_autosave.get(); }
History *Backend::history() const { return m_store->history(); }
Settings *Backend::settings() const { return m_settings; }
RecentFiles *Backend::recentFiles() const { return m_recentFiles.get(); }
QThreadPool *Backend::libraryPool() const { return m_libraryPool.get(); }
QString Backend::problem() const { return m_problem; }
QString Backend::problemDetail() const { return m_problemDetail; }
bool Backend::notice() const { return m_notice && !m_problem.isEmpty(); }
Backend::NoticeTone Backend::noticeTone() const { return notice() ? m_tone : Failure; }
bool Backend::noticeInforms() const { return noticeTone() == News; }
FileRequests *Backend::fileRequests() const { return m_files.get(); }
DocumentReview *Backend::review() const { return m_review.get(); }
bool Backend::historyPaused() const { return m_historyPause != History::Pause::None; }
bool Backend::historyFull() const { return m_historyPause == History::Pause::NamedVersionsFill; }
QVariantMap Backend::recoveryFailures() const { return m_recoveryFailures; }
QList<WindowContext *> Backend::windows() const { return m_windows; }
WindowContext *Backend::activeWindow() const { return m_recent.value(0); }

Document *Backend::document() const
{
    WindowContext *window = activeWindow();
    return window ? window->tabs()->activeDocument() : nullptr;
}

QString Backend::folderLabel(const QString &path) const
{
    return folderLabel(path, m_settings->libraryFolders());
}

QString Backend::folderLabel(const QString &path, const QStringList &libraryFolders) const
{
    if (path.isEmpty()) return tr("Drafts");
    const qsizetype slash = path.lastIndexOf(u'/');
    if (slash < 0) return {};
    const QString folder = slash == 0 ? QStringLiteral("/") : path.left(slash);
    const QString home = QDir::homePath();
    const auto within = [&folder](const QString &outer) {
        return folder == outer || folder.startsWith(outer + u'/');
    };
    // The nearest library folder that holds the document, by its own name.
    QString library;
    for (const QString &candidate : libraryFolders)
        if (candidate.size() > 1 && within(candidate) && candidate.size() > library.size()) library = candidate;
    QString first, rest;
    if (!library.isEmpty()) {
        first = library == home ? QStringLiteral("~") : QFileInfo(library).fileName();
        rest = folder.mid(library.size());
    } else if (within(home)) {
        first = QStringLiteral("~");
        rest = folder.mid(home.size());
    } else {
        first = QStringLiteral("/");
        rest = folder;
    }
    const QStringList parts = rest.split(u'/', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return first;
    // The root is a separator itself, and is shown as one.
    return (first == u'/' ? first + u' ' : first + QStringLiteral(" / ")) + parts.join(QStringLiteral(" / "));
}

void Backend::start(const QStringList &files)
{
    bool onlyKept = false;
    const QStringList recovered = restoreSession(&onlyKept);
    if (m_windows.isEmpty()) {
        // The first launch, or one whose session could not be read.
        WindowContext *window = makeWindow();
        openIn(window, files);
        if (window->tabs()->rowCount() == 0) window->tabs()->openDocument(m_store->newDraft());
        announce(window);
    } else {
        if (m_settings->freshDraftOnLaunch()) activeWindow()->tabs()->openDocument(m_store->newDraft());
        if (!files.isEmpty()) openFilesIn(activeWindow(), files);
    }
    if (recovered.isEmpty()) return;
    // Text that came back from recovery is named, beside any notice the
    // files opened gave, until it is dismissed.
    const QString detail = recovered.join(u'\n');
    if (notice()) setProblem(m_problem, m_problemDetail + u'\n' + detail);
    // Nothing came back: the text is safe where it is, but not shown.
    else if (onlyKept) post(tr("Text kept in recovery"), detail, Warning);
    else inform(tr("Restored from recovery"), detail);
}

QStringList Backend::restoreSession(bool *onlyKept)
{
    // Text that only recovery holds comes back first. An entry that cannot be
    // taken stays where it is.
    QList<Document *> restored;
    QStringList shown, inHistory, held;
    for (const Recovery::Entry &entry : m_recovery->pending()) {
        DocumentStore::Recovered how = DocumentStore::Recovered::AlreadySaved;
        if (Document *document = m_store->restore(entry, &how)) {
            watch(document);
            restored.append(document);
            if (how == DocumentStore::Recovered::Shown) shown.append(document->title());
            else if (how == DocumentStore::Recovered::InHistory) inHistory.append(document->title());
            continue;
        }
        // Its file, or the file its path leads to, is another document's:
        // the store holds the entry in recovery as it is, and it is said.
        if (m_store->holdsRecovered(entry.documentId))
            held.append(entry.path.isEmpty() ? Document::draftTitle(entry.text) : PathText::fileName(entry.path));
    }
    // The saves noted for documents that are neither open nor in recovery,
    // left by a crash while their files held their text, tell nothing now.
    QStringList open;
    for (Document *document : m_store->documents()) open.append(document->id());
    m_recovery->sweepSaves(open);
    QList<WindowContext *> made;
    for (const WindowState &state : Session::load(m_paths).windows) {
        WindowContext *window = makeWindow();
        window->restore(state);
        made.append(window);
    }
    for (Document *document : std::as_const(restored)) {
        int placed = 0;
        for (WindowContext *window : std::as_const(made)) placed += window->tabs()->place(document);
        if (placed > 0) continue;
        if (made.isEmpty()) made.append(makeWindow());
        made.first()->tabs()->append(document);
    }
    // Only the document in front of each window is read now.
    for (WindowContext *window : std::as_const(made)) window->tabs()->loadActive();
    for (WindowContext *window : std::as_const(made)) announce(window);
    QStringList recovered;
    if (!shown.isEmpty()) recovered.append(recoveredLine(DocumentStore::Recovered::Shown, shown));
    if (!inHistory.isEmpty()) recovered.append(recoveredLine(DocumentStore::Recovered::InHistory, inHistory));
    if (!held.isEmpty())
        recovered.append(tr("Text from recovery stays there, since another document has the file its path leads to: %1")
                             .arg(held.join(QStringLiteral(", "))));
    if (onlyKept) *onlyKept = !held.isEmpty() && shown.isEmpty() && inHistory.isEmpty();
    return recovered;
}

void Backend::openFiles(const QStringList &files)
{
    clearNotice();
    openFilesIn(activeWindow(), files);
}

void Backend::openForwarded(const QStringList &files, const QString &activationToken)
{
    if (activationToken.isEmpty()) {
        openFiles(files);
        return;
    }
    // Qt's Wayland platform raises a window with the token in
    // XDG_ACTIVATION_TOKEN, which it reads and clears as it does. The raise
    // comes before openFiles() returns; a token it left unused is cleared
    // then, so that no later raise uses it.
    qputenv("XDG_ACTIVATION_TOKEN", activationToken.toUtf8());
    openFiles(files);
    qunsetenv("XDG_ACTIVATION_TOKEN");
}

void Backend::openFilesIn(WindowContext *window, const QStringList &files)
{
    if (!window) {
        window = makeWindow();
        openIn(window, files);
        if (window->tabs()->rowCount() == 0) window->tabs()->openDocument(m_store->newDraft());
        announce(window);
        window->raise();
        return;
    }
    openIn(window, files)->raise();
}

void Backend::activate(WindowContext *window)
{
    if (!m_windows.contains(window) || m_recent.value(0) == window) return;
    m_recent.removeOne(window);
    m_recent.prepend(window);
}

void Backend::closeWindow(WindowContext *window)
{
    if (!m_windows.contains(window)) return;
    // A document only this window shows whose save failed, or whose file
    // changed or went, comes to the front and asks first.
    for (int i = 0; i < window->tabs()->rowCount(); ++i) {
        Document *document = window->tabs()->documentAt(i);
        if (document && viewsOf(document, window) == 0 && asksBeforeClosing(document)) {
            askToClose(window, document);
            return;
        }
    }
    // The last window ends the application, but only once every text is safe.
    if (m_windows.size() == 1) {
        quit();
        return;
    }
    clearNotice();
    // The documents only this window shows must be kept before it goes.
    QList<Document *> unsafe;
    QStringList reasons;
    for (int i = 0; i < window->tabs()->rowCount(); ++i) {
        Document *document = window->tabs()->documentAt(i);
        if (!document || unsafe.contains(document) || viewsOf(document, window) > 0) continue;
        QString reason;
        if (keepText(document, &reason)) continue;
        unsafe.append(document);
        reasons.append(reason);
    }
    if (!unsafe.isEmpty()) {
        refuseClosing(unsafe, reasons);
        return;
    }
    m_windows.removeOne(window);
    m_recent.removeOne(window);
    // The window goes first, then its tabs let go of their documents.
    emit windowRemoved(window);
    window->tabs()->clear();
    window->deleteLater();
    scheduleSession();
}

void Backend::newDraft()
{
    clearNotice();
    Document *draft = m_store->newDraft();
    if (WindowContext *window = activeWindow()) {
        window->tabs()->openDocument(draft);
        return;
    }
    WindowContext *window = makeWindow();
    window->tabs()->openDocument(draft);
    announce(window);
}

void Backend::save()
{
    clearNotice();
    WindowContext *window = activeWindow();
    Document *document = this->document();
    if (!window || !document) return;
    if (document->isDraft()) m_files->askForName(window, document, false);
    else m_store->save(document);
}

void Backend::saveAs()
{
    clearNotice();
    WindowContext *window = activeWindow();
    Document *document = this->document();
    if (!window || !document) return;
    if (document->state() == Document::State::ReadOnly) {
        notify(tr("Read-only: not saved"),
               tr("%1 is not valid UTF-8 text. It is open read-only so that its bytes stay as they are, and it "
                  "cannot be saved as text.")
                   .arg(document->title()));
        return;
    }
    m_files->askForName(window, document, false);
}

void Backend::retrySave()
{
    clearNotice();
    WindowContext *window = activeWindow();
    Document *document = this->document();
    if (!window || !document) return;
    if (!m_files->retryNaming(window, document, false)) m_store->save(document);
    if (m_recoveryFailures.contains(document->id())) retryRecovery();
}

void Backend::readAgain()
{
    clearNotice();
    if (Document *document = this->document()) m_store->readAgain(document);
}

void Backend::retryRecovery()
{
    clearNotice();
    Document *document = this->document();
    if (!document) return;
    // Text that needs no recovery any more has nothing left to fail.
    if (!m_autosave->writeNow(document)) forgetRecoveryFailure(document->id());
}

void Backend::dismissNotice()
{
    clearNotice();
}

void Backend::quit()
{
    QString problems;
    QStringList unkept;
    // Flushing waits for every save, those already running included.
    m_autosave->flush(&problems, &unkept);
    // The session is kept either way, since a refused quit can still end in a crash.
    m_sessionTimer.stop();
    m_sessionWriter.finish(collectSession());
    m_unsafe = QSet<QString>(unkept.begin(), unkept.end());
    if (m_unsafe.isEmpty()) {
        m_notice = false;
        setProblem({}, {});
        emit quitRequested();
        return;
    }
    m_refusedAfter = m_autosave->recoveryWrites();
    // A save that succeeds later makes the text safe as well.
    for (Document *document : m_store->documents())
        if (m_unsafe.contains(document->id()))
            connect(document, &Document::changed, this, &Backend::checkUnsafe, Qt::UniqueConnection);
    m_notice = false;
    setProblem(tr("Quit refused: not saved"),
               tr("Some text is in neither its file nor recovery, so parch stays open until it is in one of them.\n%1")
                   .arg(problems));
}

bool Backend::end(int waitMs)
{
    m_sessionTimer.stop();
    m_sessionWriter.finish(collectSession());
    return m_autosave->secure(waitMs);
}

void Backend::newWindow()
{
    clearNotice();
    WindowContext *window = makeWindow();
    window->tabs()->openDocument(m_store->newDraft());
    announce(window);
}

void Backend::newView()
{
    clearNotice();
    if (WindowContext *window = activeWindow()) window->tabs()->newView(window->tabs()->activeIndex());
}

void Backend::requestCloseTab(int index)
{
    clearNotice();
    if (WindowContext *window = activeWindow()) closeTab(window, index);
}

void Backend::closeTryingAgain()
{
    clearNotice();
    WindowContext *window = activeWindow();
    Document *document = this->document();
    if (!window || !document) return;
    if (!m_files->retryNaming(window, document, true)) closeOnceSaved(window, document);
}

void Backend::closeSavingAs()
{
    clearNotice();
    WindowContext *window = activeWindow();
    Document *document = this->document();
    if (window && document) m_files->askForName(window, document, true);
}

void Backend::closeWithoutSaving()
{
    closeGivingUp(true);
}

void Backend::discardText()
{
    closeGivingUp(false);
}

void Backend::closeGivingUp(bool keepInHistory)
{
    clearNotice();
    WindowContext *window = activeWindow();
    Document *document = this->document();
    if (!window || !document) return;
    if (!asksBeforeClosing(document)) {
        closeTab(window, window->tabs()->activeIndex());
        return;
    }
    recordThenGiveUp(window, document, keepInHistory);
}

void Backend::recordThenGiveUp(WindowContext *window, Document *document, bool keepInHistory)
{
    // The text goes to history first, off the UI thread, as a copy the writer
    // can go back to; giving the text up waits until history has it.
    m_review->record(document, [this, keepInHistory, holder = QPointer(window), target = QPointer(document)](
                                   History::CaptureStatus status, Revision recorded) {
        if (!target || !holder || !m_windows.contains(holder)) return;
        // Written to while history took the text: the newer text is recorded too.
        if (target->buffer()->revision() != recorded) {
            recordThenGiveUp(holder, target, keepInHistory);
            return;
        }
        if (keepInHistory && status == History::CaptureStatus::Failed) {
            notify(tr("Not closed: history could not keep it"),
                   tr("The text of %1 could not be written to history, so it stays open rather than be lost.")
                       .arg(target->title()));
            return;
        }
        // History paused: closing was to keep the text there, and now would
        // keep it nowhere, so the window asks again.
        if (keepInHistory && status == History::CaptureStatus::Paused) {
            askToClose(holder, target, true);
            return;
        }
        giveUp(holder, target);
    });
}

void Backend::openFromLibrary(const QString &path)
{
    clearNotice();
    if (WindowContext *window = activeWindow()) openIn(window, {path}, Absent::NotFound)->raise();
}

void Backend::openDraft(const QString &documentId)
{
    clearNotice();
    WindowContext *window = activeWindow();
    if (!window) return;
    Document *draft = m_store->openDraft(documentId);
    if (!draft) {
        notify(tr("Draft not found"), tr("This draft is no longer among parch's drafts."));
        return;
    }
    showDocument(window, draft)->raise();
}

void Backend::setFilePicker(FilePicker *picker)
{
    m_files->setPicker(picker);
}

QString Backend::firstFolder(const QStringList &candidates, const QString &fallback)
{
    return FileRequests::firstFolder(candidates, fallback);
}

void Backend::open()
{
    clearNotice();
    m_files->askForDocuments(activeWindow());
}

void Backend::openFolder()
{
    clearNotice();
    m_files->askForFolder(activeWindow());
}

void Backend::resolveConflict(int choice)
{
    m_review->resolveConflict(document(), choice);
}

void Backend::checkpoint(const QString &name)
{
    m_review->checkpoint(document(), name);
}

void Backend::restoreVersion(const QString &versionId)
{
    m_review->restoreVersion(document(), versionId);
}

void Backend::nameVersion(const QString &versionId, const QString &name)
{
    m_review->nameVersion(document(), versionId, name);
}

void Backend::removeVersion(const QString &versionId)
{
    m_review->removeVersion(document(), versionId);
}

QString Backend::shownText(const QString &text) const
{
    // A draft's file and the folders parch keeps for itself are no place
    // the writer knows, so they are named by what they are for.
    const QString drafts = QRegularExpression::escape(m_paths.drafts());
    static const QString id = QStringLiteral("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
    QString shown = text;
    shown.replace(QRegularExpression(drafts + u'/' + id + QStringLiteral("/draft\\.md")), tr("the draft"));
    shown.replace(QRegularExpression(drafts + QStringLiteral("(/") + id + QStringLiteral(")?")), tr("the drafts folder"));
    shown.replace(m_paths.recovery(), tr("the recovery folder"));
    shown.replace(m_paths.history(), tr("the history folder"));
    // Elsewhere, the home folder is ~, as the header writes it.
    const QString home = QDir::homePath();
    if (home.size() > 1) shown.replace(QRegularExpression(QRegularExpression::escape(home) + QStringLiteral("(?=/|$)")),
                                       QStringLiteral("~"));
    return shown;
}

void Backend::forgetRecoveryFailure(const QString &documentId)
{
    if (m_recoveryFailures.remove(documentId) > 0) emit recoveryFailuresChanged();
}

WindowContext *Backend::makeWindow()
{
    auto *window = new WindowContext(
        m_store.get(),
        new LibraryModel(m_paths, m_store.get(), m_recentFiles.get(), m_settings, m_libraryPool.get()), this);
    // Why a tab holds no document is worded as a notice is, a path from ~.
    window->tabs()->setShownText([this](const QString &text) { return shownText(text); });
    connect(window, &WindowContext::changed, this, &Backend::scheduleSession);
    connect(window->tabs(), &TabModel::documentAdded, this, &Backend::watch);
    m_windows.append(window);
    m_recent.append(window);
    return window;
}

void Backend::announce(WindowContext *window)
{
    emit windowAdded(window);
    scheduleSession();
}

void Backend::watch(Document *document)
{
    m_autosave->watch(document);
    // A file opened in a tab is the newest recent file.
    if (!document->isDraft()) m_recentFiles->note(document->path());
    connect(document, &Document::changed, this, &Backend::documentChanged, Qt::UniqueConnection);
}

void Backend::documentChanged()
{
    // Text that its file holds needs no recovery, so a failed write of it no longer matters.
    auto *document = qobject_cast<Document *>(sender());
    if (!document) return;
    const Document::State state = document->state();
    if (state == Document::State::Clean || state == Document::State::ReadOnly) forgetRecoveryFailure(document->id());
}

WindowContext *Backend::openIn(WindowContext *window, const QStringList &files, Absent absent)
{
    WindowContext *shown = window;
    // Whether the tab that came to the front last for one of the files shows
    // text. A tab whose file is not found shows none, and takes no keys.
    bool documentShown = false;
    // Brings a tab to the front in whichever window has it.
    const auto showIn = [&](WindowContext *holder, int index) {
        holder->tabs()->setActiveIndex(index);
        shown = holder;
        documentShown = holder->tabs()->documentAt(index) != nullptr;
    };
    QStringList failures;
    // How many of them are files that are there but were not opened.
    int refused = 0;
    // Where the next save dialog starts: the folder of what was opened last.
    QString lastFolder;
    // Folders join the library once the files are open, since a file opened
    // with them keeps the keys in its text.
    QStringList folders;
    for (const QString &given : files) {
        if (!PathText::isFullPath(given)) {
            failures.append(tr("%1: not a full path").arg(given));
            continue;
        }
        const QFileInfo info(given);
        if (info.isDir()) {
            folders.append(spelledClean(given));
            lastFolder = info.canonicalFilePath();
            continue;
        }
        if (info.exists()) {
            QString error;
            // The store finds the file parch records, however this is spelled.
            Document *document = m_store->open(given, &error);
            if (!document) {
                // Each refusal in words of its own, which name the file and
                // its folder as the header names a folder.
                failures.append(error);
                ++refused;
                continue;
            }
            documentShown = true;
            shown = showDocument(window, document);
            if (!document->isDraft()) lastFolder = PathText::folderOf(document->path());
            continue;
        }
        // A tab already waiting for this file comes to the front rather than a second one.
        const QString path = spelledClean(given);
        bool waiting = false;
        for (WindowContext *holder : std::as_const(m_windows)) {
            const int found = holder->tabs()->find(path);
            if (found < 0) continue;
            showIn(holder, found);
            waiting = true;
            break;
        }
        if (waiting) continue;
        // Its absence is what the first edit checks before creating the file.
        const DiskVersion none = DiskFile::fingerprint(path);
        if (absent == Absent::NewFile && !path.endsWith(u'/') && !none.exists
            && QFileInfo(PathText::folderOf(path)).isDir()) {
            Document *draft = m_store->newDraft();
            showIn(window, window->tabs()->openNewFile(draft, path));
            nameOnFirstEdit(draft, path, none);
        } else {
            showIn(window, window->tabs()->openMissing(path));
        }
    }
    for (const QString &folder : std::as_const(folders)) showLibraryFolder(window, folder, !documentShown);
    if (!folders.isEmpty() && !documentShown) shown = window;
    if (documentShown) shown->focusText();
    if (!lastFolder.isEmpty()) m_settings->setLastFolder(lastFolder);
    // One file that was not opened is said in one line; several, one to a line beneath.
    if (failures.size() == 1 && refused == 1) notify(failures.first(), {});
    else if (!failures.isEmpty()) notify(tr("Some files could not be opened"), failures.join(u'\n'));
    return shown;
}

void Backend::showLibraryFolder(WindowContext *window, const QString &path, bool takesKeys)
{
    // The library keeps the kernel's name for it, so the window looks for that.
    window->library()->addFolder(path);
    const QString canonical = QFileInfo(path).canonicalFilePath();
    window->showFolder(canonical.isEmpty() ? path : canonical, takesKeys);
}

WindowContext *Backend::showDocument(WindowContext *window, Document *document)
{
    // A document open in any window comes to the front there: in a tab that
    // shows it, or else in one that waits for it. New view is the way to a
    // second tab on it.
    for (const bool shown : {true, false}) {
        for (WindowContext *holder : std::as_const(m_windows)) {
            if (shown ? holder->tabs()->viewsOf(document) == 0 : holder->tabs()->indexOf(document) < 0) continue;
            holder->tabs()->openDocument(document);
            return holder;
        }
    }
    // An empty draft in front that nothing was ever written in, and that no
    // other tab shows, gives its tab to the document, and goes as it would
    // if it were closed; not one that stands for a file a launch named.
    TabModel *tabs = window->tabs();
    const int front = tabs->activeIndex();
    const Document *draft = tabs->documentAt(front);
    if (draft && draft != document && draft->isDraft() && !draft->everSaved() && !tabs->standsForNewFile(front)
        && draft->state() == Document::State::Clean && draft->buffer()->length() == 0
        && !draft->buffer()->canUndo() && !draft->buffer()->canRedo() && viewsOf(draft) == 1) {
        tabs->openInPlaceOf(front, document);
        return window;
    }
    tabs->openDocument(document);
    return window;
}

void Backend::nameOnFirstEdit(Document *draft, const QString &path, const DiskVersion &absent)
{
    auto edited = std::make_shared<QMetaObject::Connection>();
    *edited = connect(draft->buffer(), &SourceBuffer::sourceChanged, this,
                      [this, edited, draft = QPointer(draft), path, absent] {
        disconnect(*edited);
        // Naming is left until the keystroke that made the edit is done.
        QTimer::singleShot(0, this, [this, draft, path, absent] {
            if (draft && draft->isDraft()) name(draft, path, absent);
        });
    });
}

void Backend::name(Document *draft, const QString &path, const DiskVersion &absent)
{
    // The store says when this naming ends, before saveAs returns when it
    // ends at once; the connection goes with the draft.
    auto finished = std::make_shared<QMetaObject::Connection>();
    *finished = connect(m_store.get(), &DocumentStore::saveFinished, draft,
                        [this, finished, draft, path](Document *saved, const QString &name) {
        if (saved != draft || name != path) return;
        disconnect(*finished);
        namingEnded(draft, path);
    });
    m_store->saveAs(draft, path, absent);
}

void Backend::namingEnded(Document *draft, const QString &path)
{
    for (WindowContext *window : std::as_const(m_windows)) window->tabs()->forgetNewFile(draft);
    if (!draft->isDraft()) {
        m_settings->setLastFolder(PathText::folderOf(draft->path()));
        return;
    }
    // The file was not created, most likely because one appeared there. That
    // file is left as it is; the text is kept as the draft it still is, in
    // Drafts, and shown as one.
    const QString reason = draft->lastError();
    m_store->save(draft);
    const QString name = PathText::fileName(path);
    if (DiskFile::fingerprint(path).exists)
        notify(tr("Kept as a draft: %1 already exists").arg(name),
               tr("%1 appeared after it was opened as a new document, so it was left as it is and what was "
                  "written here was kept as a draft. Open %1 to see it, or name the draft to keep both.")
                   .arg(path));
    else
        notify(tr("Kept as a draft: %1 could not be created").arg(name),
               tr("%1 could not be created (%2), so what was written here was kept as a draft.").arg(path, reason));
}

void Backend::closeTab(WindowContext *window, int index)
{
    TabModel *tabs = window->tabs();
    if (index < 0 || index >= tabs->rowCount()) return;
    Document *document = tabs->documentAt(index);
    // Only the last view of a document has to keep its text.
    const bool last = document && viewsOf(document) == 1;
    if (last && asksBeforeClosing(document)) {
        askToClose(window, document);
        return;
    }
    // A named document's own file takes the text before its tab goes.
    const Document::State state = last ? document->state() : Document::State::Clean;
    if (last && !document->isDraft() && (state == Document::State::Modified || state == Document::State::Saving)) {
        closeOnceSaved(window, document);
        return;
    }
    if (tabs->rowCount() == 1) {
        closeWindow(window);
        return;
    }
    QString reason;
    if (last && !keepText(document, &reason)) {
        refuseClosing({document}, {reason});
        return;
    }
    tabs->closeTab(index);
}

void Backend::askToClose(WindowContext *window, Document *document, bool discarding)
{
    activate(window);
    window->tabs()->setActiveIndex(window->tabs()->indexOf(document));
    window->raise();
    window->askToClose(document, discarding);
}

void Backend::closeOnceSaved(WindowContext *window, Document *document)
{
    const QString id = document->id();
    // Asked again while it waits: the save is tried again, and the tab still
    // closes once it lands.
    if (m_closing.contains(id)) {
        m_store->save(document);
        return;
    }
    m_closing.insert(id);
    auto settled = std::make_shared<QMetaObject::Connection>();
    // Once the save has landed the tab is closed again, as it was asked: a
    // clean document closes, and a failed save or a conflict asks. Whether
    // a save still runs is asked, since a conflict hides it from state().
    const auto check = [this, settled, holder = QPointer(window), target = QPointer(document), id] {
        if (!target || target->isSaving()) return;
        if (target->state() == Document::State::Modified) {
            // Written to meanwhile: the newer text is saved too.
            m_store->save(target);
            return;
        }
        disconnect(*settled);
        // After everything else that hears of the change has taken it.
        QMetaObject::invokeMethod(this, [this, holder, target, id] {
            if (!m_closing.remove(id) || !target || !holder || !m_windows.contains(holder)) return;
            const int index = holder->tabs()->indexOf(target);
            if (index >= 0) closeTab(holder, index);
        }, Qt::QueuedConnection);
    };
    // Each save that ends is the store's to say, the one running now included.
    *settled = connect(m_store.get(), &DocumentStore::saveFinished, document, [document, check](Document *saved) {
        if (saved == document) check();
    });
    m_store->save(document);
    // A save that had nothing to write ends without a word.
    check();
}

void Backend::giveUp(WindowContext *window, Document *document)
{
    TabModel *tabs = window->tabs();
    const int index = tabs->indexOf(document);
    if (index < 0) return;
    const QString id = document->id();
    tabs->closeTab(index);
    // A view opened meanwhile still shows the text, which stays.
    if (m_store->drop(document)) m_autosave->discard(id);
    if (tabs->rowCount() > 0) return;
    // The window went with its last tab; it stays only if quitting is
    // refused, and then with a draft to write in.
    closeWindow(window);
    if (m_windows.contains(window) && tabs->rowCount() == 0) tabs->openDocument(m_store->newDraft());
}

bool Backend::keepText(Document *document, QString *reason)
{
    // A document with no text of its own has none to keep, and recovery is not touched.
    if (document->isTextless()) return true;
    switch (document->state()) {
    case Document::State::Clean:
    case Document::State::ReadOnly:
        return true;
    case Document::State::Modified:
        // The store keeps a document whose last view closed until it is
        // clean; this starts its save at once.
        m_store->save(document);
        break;
    default:
        break;
    }
    // Recovery holds the text whatever the save does.
    if (m_autosave->keep(document, reason)) return true;
    // A save that is running may still put the text in its file.
    m_store->waitForSaves();
    return document->state() == Document::State::Clean;
}

int Backend::viewsOf(const Document *document, const WindowContext *leavingOut) const
{
    int views = 0;
    for (const WindowContext *window : m_windows)
        if (window != leavingOut) views += window->tabs()->viewsOf(document);
    return views;
}

void Backend::refuseClosing(const QList<Document *> &documents, const QStringList &reasons)
{
    QStringList lines;
    for (qsizetype i = 0; i < documents.size(); ++i)
        lines.append(tr("%1: %2").arg(documents.at(i)->title(), reasons.value(i)));
    notify(tr("Not closed: not saved"),
           tr("This text is in neither its file nor recovery, so it stays open until it is in one of them.\n%1")
               .arg(lines.join(u'\n')));
}

void Backend::scheduleSession()
{
    // Two seconds after the first change, not after the last, so that
    // continuous work is still written.
    if (!m_sessionTimer.isActive()) m_sessionTimer.start();
}

SessionState Backend::collectSession() const
{
    SessionState state;
    for (WindowContext *window : m_windows) {
        window->captureView();
        const WindowState kept = window->state();
        if (!kept.tabs.isEmpty()) state.windows.append(kept);
    }
    return state;
}

void Backend::checkUnsafe()
{
    const QSet<QString> unsafe = m_unsafe;
    for (const QString &id : unsafe) {
        Document *found = nullptr;
        for (Document *document : m_store->documents())
            if (document->id() == id) found = document;
        if (!found || found->state() == Document::State::Clean) forgetUnsafe(id);
    }
}

void Backend::forgetUnsafe(const QString &documentId)
{
    if (!m_unsafe.remove(documentId) || !m_unsafe.isEmpty()) return;
    setProblem({}, {});
}

void Backend::setProblem(const QString &problem, const QString &detail)
{
    if (problem == m_problem && detail == m_problemDetail) return;
    m_problem = problem;
    m_problemDetail = detail;
    emit problemChanged();
}

void Backend::notify(const QString &problem, const QString &detail)
{
    post(problem, detail, Failure);
}

void Backend::inform(const QString &news, const QString &detail)
{
    post(news, detail, News);
}

void Backend::post(const QString &problem, const QString &detail, NoticeTone tone)
{
    // A refused quit stays in view until its text is safe; the notice joins its detail.
    if (!m_unsafe.isEmpty()) {
        setProblem(m_problem, m_problemDetail + u'\n' + detail);
        return;
    }
    // Everything about the notice is set first, and said once.
    const bool same = m_notice && m_tone == tone && problem == m_problem && detail == m_problemDetail;
    m_notice = true;
    m_tone = tone;
    m_problem = problem;
    m_problemDetail = detail;
    if (!same) emit problemChanged();
}

void Backend::clearNotice()
{
    if (!std::exchange(m_notice, false)) return;
    m_tone = Failure;
    setProblem({}, {});
}
