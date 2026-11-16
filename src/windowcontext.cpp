#include "windowcontext.h"

#include "document.h"
#include "sourcebuffer.h"

WindowContext::WindowContext(DocumentStore *store, LibraryModel *library, QObject *parent)
    : QObject(parent), m_tabs(new TabModel(store, this)), m_library(library)
{
    m_library->setParent(this);
    connect(m_tabs, &TabModel::changed, this, &WindowContext::changed);
}

TabModel *WindowContext::tabs() const { return m_tabs; }
LibraryModel *WindowContext::library() const { return m_library; }
bool WindowContext::libraryOpen() const { return m_libraryOpen; }
QSize WindowContext::initialSize() const { return m_initialSize; }
QSize WindowContext::size() const { return m_size.isValid() ? m_size : m_initialSize; }

void WindowContext::setLibraryOpen(bool open)
{
    if (open == m_libraryOpen) return;
    m_libraryOpen = open;
    emit libraryOpenChanged();
    emit changed();
}

void WindowContext::setSize(const QSize &size)
{
    if (size == m_size) return;
    m_size = size;
    emit sizeChanged();
    emit changed();
}

WindowState WindowContext::state() const
{
    WindowState state;
    state.libraryOpen = m_libraryOpen;
    state.size = size();
    const int active = m_tabs->activeIndex();
    for (int i = 0; i < m_tabs->rowCount(); ++i) {
        // A draft nobody wrote in has no file and nothing to bring back.
        const Document *document = m_tabs->documentAt(i);
        if (document && document->isDraft() && document->buffer()->length() == 0 && !document->buffer()->modified()
            && document->diskVersion().canonicalPath.isEmpty())
            continue;
        if (i <= active) state.activeTab = int(state.tabs.size());
        state.tabs.append(m_tabs->viewState(i));
    }
    return state;
}

void WindowContext::restore(const WindowState &state)
{
    setLibraryOpen(state.libraryOpen);
    if (state.size.isValid()) m_initialSize = state.size;
    m_tabs->restore(state.tabs, state.activeTab);
}

void WindowContext::captureView() { emit viewCaptureRequested(); }
void WindowContext::raise() { emit raiseRequested(); }
QString WindowContext::folderToShow() const { return m_folderToShow; }
void WindowContext::askToClose(Document *document, bool discarding)
{
    emit closeConfirmationRequested(document, discarding);
}

void WindowContext::showFolder(const QString &path, bool takesKeys)
{
    setLibraryOpen(true);
    // Said again for the same folder, since the window emptied it once shown.
    m_folderToShow = path;
    m_folderTakesKeys = takesKeys;
    emit folderToShowChanged();
}

bool WindowContext::folderTakesKeys() const { return m_folderTakesKeys; }
void WindowContext::focusText() { emit textFocusRequested(); }

void WindowContext::setFolderToShow(const QString &path)
{
    if (path == m_folderToShow) return;
    m_folderToShow = path;
    emit folderToShowChanged();
}
void WindowContext::noteViewMoved() { emit changed(); }
