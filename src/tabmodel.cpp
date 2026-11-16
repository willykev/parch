#include "tabmodel.h"

#include "documentstore.h"
#include "pathtext.h"
#include "sourcebuffer.h"

#include <QFileInfo>
#include <QtConcurrent>
#include <utility>

TabModel::TabModel(DocumentStore *store, QObject *parent) : QAbstractListModel(parent), m_store(store) {}

int TabModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_tabs.size());
}

QVariant TabModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tabs.size()) return {};
    const Tab &tab = m_tabs.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return titleOf(tab);
    case PathRole:
        return pathOf(tab);
    case StateRole:
        return statusOf(tab);
    case DocumentIdRole:
        return tab.document ? tab.document->id() : tab.view.documentId;
    case DocumentRole:
        return QVariant::fromValue(tab.document.data());
    default:
        return {};
    }
}

QHash<int, QByteArray> TabModel::roleNames() const
{
    // "status" rather than "state", which every QML item already has.
    return {{TitleRole, "title"}, {PathRole, "path"}, {StateRole, "status"}, {DocumentIdRole, "documentId"},
            {DocumentRole, "document"}};
}

int TabModel::activeIndex() const { return m_active; }

Document *TabModel::activeDocument() const
{
    return m_active >= 0 ? m_tabs.at(m_active).document.data() : nullptr;
}

QString TabModel::activeTitle() const { return m_active >= 0 ? titleOf(m_tabs.at(m_active)) : QString(); }
QString TabModel::activePath() const { return m_active >= 0 ? pathOf(m_tabs.at(m_active)) : QString(); }
QString TabModel::activeStatus() const { return m_active >= 0 ? statusOf(m_tabs.at(m_active)) : QString(); }

TabModel::ViewMode TabModel::activeMode() const
{
    return m_active >= 0 ? ViewMode(m_tabs.at(m_active).view.mode) : Source;
}

void TabModel::setActiveMode(ViewMode mode)
{
    if (m_active < 0 || mode < Source || mode > Reading || m_tabs.at(m_active).view.mode == mode) return;
    m_tabs[m_active].view.mode = mode;
    emit activeModeChanged();
    emit changed();
}

qreal TabModel::activeSplit() const { return m_active >= 0 ? m_tabs.at(m_active).view.split : 0.5; }

void TabModel::setActiveSplit(qreal split)
{
    // A value that is no number changes nothing.
    if (m_active < 0 || !(split == split)) return;
    split = std::clamp(split, ViewState::MinSplit, ViewState::MaxSplit);
    if (m_tabs.at(m_active).view.split == split) return;
    m_tabs[m_active].view.split = split;
    emit activeModeChanged();
    emit changed();
}

ViewState TabModel::freshView(const QString &documentId, const QString &path) const
{
    ViewState view{documentId, path};
    if (m_active >= 0) {
        const ViewState &front = m_tabs.at(m_active).view;
        // Reading shows no text to write in, which a new tab is for.
        if (front.mode == ViewState::Split) view.mode = ViewState::Split;
        view.split = front.split;
    }
    return view;
}

void TabModel::setActiveIndex(int index)
{
    if (index < 0 || index >= m_tabs.size() || index == m_active) return;
    emit activeAboutToChange();
    Document *before = activeDocument();
    m_active = index;
    load(index);
    announceActive(before);
}

int TabModel::openDocument(Document *document, const std::optional<ViewState> &view)
{
    if (!document) return -1;
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs.at(i).document == document) {
            setActiveIndex(i);
            return i;
        }
    }
    // A tab waiting for this document, or for its file, takes it.
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (waitsFor(m_tabs.at(i), document)) {
            emit activeAboutToChange();
            Document *before = activeDocument();
            take(i, document);
            m_active = i;
            announceActive(before);
            return i;
        }
    }
    ViewState start = freshView(document->id(), document->path());
    if (view) {
        start.cursor = view->cursor;
        start.anchor = view->anchor;
        start.top = view->top;
        start.mode = view->mode;
        start.split = view->split;
    }
    const int at = insert(int(m_tabs.size()), Tab{nullptr, start, {}, {}});
    take(at, document);
    setActiveIndex(at);
    return at;
}

int TabModel::openInPlaceOf(int index, Document *document)
{
    if (!document || index < 0 || index >= m_tabs.size()) return openDocument(document);
    const int at = insert(index + 1, Tab{nullptr, freshView(document->id(), document->path()), {}, {}});
    take(at, document);
    setActiveIndex(at);
    closeTab(index);
    return index;
}

int TabModel::newView(int index)
{
    if (index < 0 || index >= m_tabs.size() || !m_tabs.at(index).document) return -1;
    // The window first keeps where the view it shows is, so that the new
    // view starts there.
    if (index == m_active) emit activeAboutToChange();
    Tab copy = m_tabs.at(index);
    Document *document = std::exchange(copy.document, nullptr);
    const int at = insert(index + 1, copy);
    take(at, document);
    Document *before = activeDocument();
    m_active = at;
    announceActive(before);
    return at;
}

void TabModel::closeTab(int index)
{
    if (index < 0 || index >= m_tabs.size()) return;
    const bool wasActive = index == m_active;
    const int activeBefore = m_active;
    Document *before = activeDocument();
    beginRemoveRows({}, index, index);
    const Tab closing = m_tabs.takeAt(index);
    endRemoveRows();
    if (m_tabs.isEmpty()) {
        m_active = -1;
    } else if (index < m_active) {
        --m_active;
    } else if (wasActive) {
        // The tab after it, or the one before when it was the last.
        m_active = std::min(index, int(m_tabs.size()) - 1);
        load(m_active);
    }
    // The same tab stays in front when one before it closes, at a new index.
    if (wasActive) announceActive(before);
    else if (m_active != activeBefore) emit activeIndexChanged();
    emit countChanged();
    // The view lets go of the document before the store may close it.
    if (closing.document) release(closing.document);
    emit changed();
}

int TabModel::openNewFile(Document *draft, const QString &path)
{
    if (!draft) return -1;
    const int at = insert(int(m_tabs.size()), Tab{nullptr, freshView(draft->id(), QString()), {}, path});
    take(at, draft);
    setActiveIndex(at);
    return at;
}

bool TabModel::standsForNewFile(int index) const
{
    return index >= 0 && index < m_tabs.size() && !m_tabs.at(index).newFile.isEmpty();
}

void TabModel::forgetNewFile(Document *draft)
{
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs.at(i).document != draft || m_tabs.at(i).newFile.isEmpty()) continue;
        m_tabs[i].newFile.clear();
        emit dataChanged(index(i), index(i));
        if (i == m_active) emit activeChanged();
        emit changed();
    }
}

int TabModel::openMissing(const QString &path)
{
    if (const int found = find(path); found >= 0) {
        setActiveIndex(found);
        return found;
    }
    const int at = insert(int(m_tabs.size()), Tab{nullptr, freshView(QString(), path), {}, {}});
    // Showing it looks for the file, which finds out that it is not there.
    setActiveIndex(at);
    return at;
}

int TabModel::append(Document *document)
{
    if (!document) return -1;
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs.at(i).document == document) return i;
    const int at = insert(int(m_tabs.size()), Tab{nullptr, freshView(document->id(), document->path()), {}, {}});
    take(at, document);
    if (m_active < 0) setActiveIndex(at);
    emit changed();
    return at;
}

int TabModel::find(const QString &path) const
{
    if (path.isEmpty()) return -1;
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs.at(i).view.path == path || m_tabs.at(i).newFile == path || pathOf(m_tabs.at(i)) == path) return i;
    return -1;
}

int TabModel::indexOf(const Document *document) const
{
    if (!document) return -1;
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs.at(i).document == document) return i;
    // A tab not read yet, or whose file was not there, knows the document
    // only by its id and its file.
    for (int i = 0; i < m_tabs.size(); ++i)
        if (waitsFor(m_tabs.at(i), document)) return i;
    return -1;
}

bool TabModel::waitsFor(const Tab &tab, const Document *document)
{
    return !tab.document
           && (tab.view.documentId == document->id() || (!tab.view.path.isEmpty() && tab.view.path == document->path()));
}

int TabModel::viewsOf(const Document *document) const
{
    int views = 0;
    for (const Tab &tab : m_tabs) views += document && tab.document == document ? 1 : 0;
    return views;
}

void TabModel::restore(const QList<ViewState> &views, int active)
{
    if (views.isEmpty()) return;
    QList<Tab> tabs;
    QStringList paths;
    for (const ViewState &view : views) {
        tabs.append(Tab{nullptr, view, {}, {}});
        if (!view.path.isEmpty()) paths.append(view.path);
    }
    // Only whether each file is there is asked now, off the UI thread, since
    // a file on a mount that stopped answering would hold up the start;
    // reading it waits until the tab is shown.
    if (!paths.isEmpty()) {
        QtConcurrent::run(DiskFile::looks(), [paths] {
            QSet<QString> missing;
            for (const QString &path : paths)
                if (!QFileInfo::exists(path)) missing.insert(path);
            return missing;
        }).then(this, [this](const QSet<QString> &missing) { notFound(missing); });
    }
    const int first = int(m_tabs.size());
    beginInsertRows({}, first, first + int(tabs.size()) - 1);
    m_tabs.append(tabs);
    endInsertRows();
    if (m_active < 0) {
        m_active = first + std::clamp(active, 0, int(tabs.size()) - 1);
        emit activeIndexChanged();
        emit activeChanged();
        emit activeModeChanged();
    }
    emit countChanged();
    emit changed();
}

void TabModel::notFound(const QSet<QString> &paths)
{
    for (int i = 0; i < m_tabs.size(); ++i) {
        Tab &tab = m_tabs[i];
        if (tab.document || !tab.problem.isEmpty() || !paths.contains(tab.view.path)) continue;
        tab.problem = tr("File not found");
        emit dataChanged(index(i), index(i));
    }
}

int TabModel::place(Document *document)
{
    if (!document) return 0;
    int taken = 0;
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs.at(i).document || m_tabs.at(i).view.documentId != document->id()) continue;
        Document *before = activeDocument();
        take(i, document);
        ++taken;
        if (i == m_active) announceActive(before);
    }
    return taken;
}

void TabModel::loadActive()
{
    if (m_active < 0 || m_tabs.at(m_active).document) return;
    load(m_active);
    announceActive(nullptr);
}

Document *TabModel::documentAt(int index) const
{
    return index >= 0 && index < m_tabs.size() ? m_tabs.at(index).document.data() : nullptr;
}

ViewState TabModel::viewState(int index) const
{
    if (index < 0 || index >= m_tabs.size()) return {};
    const Tab &tab = m_tabs.at(index);
    ViewState view = tab.view;
    if (tab.document) {
        view.documentId = tab.document->id();
        // A draft is kept as a draft, whatever file it may stand for.
        view.path = tab.document->isDraft() ? QString() : tab.document->path();
        view.title = tab.document->title();
    }
    return view;
}

void TabModel::setViewState(int index, const ViewState &state)
{
    setView(index, state.cursor, state.anchor, state.top);
}

QVariantMap TabModel::viewAt(int index) const
{
    const ViewState view = viewState(index);
    return {{QStringLiteral("cursor"), view.cursor}, {QStringLiteral("anchor"), view.anchor},
            {QStringLiteral("top"), view.top}};
}

void TabModel::setView(int index, int cursor, int anchor, int top)
{
    if (index < 0 || index >= m_tabs.size()) return;
    ViewState &view = m_tabs[index].view;
    view.cursor = std::max(0, cursor);
    view.anchor = std::max(0, anchor);
    view.top = std::max(0, top);
}

void TabModel::clear()
{
    if (m_tabs.isEmpty()) return;
    Document *before = activeDocument();
    beginResetModel();
    const QList<Tab> closing = std::exchange(m_tabs, {});
    m_active = -1;
    endResetModel();
    if (before) emit activeDocumentChanged();
    emit activeIndexChanged();
    emit activeChanged();
    emit activeModeChanged();
    emit countChanged();
    for (const Tab &tab : closing)
        if (tab.document) release(tab.document);
    emit changed();
}

int TabModel::insert(int at, const Tab &tab)
{
    beginInsertRows({}, at, at);
    m_tabs.insert(at, tab);
    endInsertRows();
    if (m_active >= at) {
        ++m_active;
        emit activeIndexChanged();
    }
    emit countChanged();
    emit changed();
    return at;
}

void TabModel::load(int index)
{
    Tab &tab = m_tabs[index];
    if (tab.document) return;
    Document *document = nullptr;
    QString problem;
    if (!tab.view.path.isEmpty()) {
        // A file that is not there is reported; opening never creates it.
        // The path is the one the document had, which may lead elsewhere since.
        if (QFileInfo::exists(tab.view.path)) document = m_store->reopen(tab.view.path, tab.view.documentId, &problem);
        else problem = tr("File not found");
    } else {
        document = m_store->openDraft(tab.view.documentId);
        if (!document) problem = tr("File not found");
    }
    if (document) {
        take(index, document);
    } else if (tab.problem != problem) {
        tab.problem = problem;
        emit dataChanged(this->index(index), this->index(index));
    }
}

void TabModel::take(int index, Document *document)
{
    Tab &tab = m_tabs[index];
    tab.document = document;
    tab.problem.clear();
    m_store->retain(document);
    connect(document, &Document::changed, this, &TabModel::documentChanged, Qt::UniqueConnection);
    connect(document->buffer(), &SourceBuffer::sourceChanged, this, &TabModel::documentEdited, Qt::UniqueConnection);
    emit dataChanged(this->index(index), this->index(index));
    emit documentAdded(document);
    emit changed();
}

void TabModel::release(Document *document)
{
    bool shown = false;
    for (const Tab &tab : std::as_const(m_tabs)) shown = shown || tab.document == document;
    if (!shown) {
        disconnect(document, &Document::changed, this, &TabModel::documentChanged);
        disconnect(document->buffer(), &SourceBuffer::sourceChanged, this, &TabModel::documentEdited);
    }
    m_store->release(document);
}

QString TabModel::titleOf(const Tab &tab) const
{
    if (tab.document) {
        // A new draft is named after the file it stands for.
        if (tab.document->isDraft() && !tab.newFile.isEmpty()) return PathText::fileName(tab.newFile);
        return tab.document->title();
    }
    if (!tab.view.path.isEmpty()) return PathText::fileName(tab.view.path);
    if (!tab.view.title.isEmpty()) return tab.view.title;
    return tr("Untitled");
}

QString TabModel::pathOf(const Tab &tab) const
{
    if (tab.document) return tab.document->isDraft() ? tab.newFile : tab.document->path();
    return tab.view.path;
}

QString TabModel::statusOf(const Tab &tab) const
{
    if (tab.document) return tab.document->statusText();
    return m_shownText ? m_shownText(tab.problem) : tab.problem;
}

void TabModel::setShownText(std::function<QString(const QString &text)> shown)
{
    m_shownText = std::move(shown);
}

void TabModel::announceActive(Document *before)
{
    if (activeDocument() != before) emit activeDocumentChanged();
    emit activeIndexChanged();
    emit activeChanged();
    emit activeModeChanged();
    emit activeViewChanged();
    emit changed();
}

void TabModel::documentEdited(Revision, SourceRange range, const QString &replacement)
{
    // As the editor moves a selection, except that a position at the edit's
    // start stays before what was put there: positions before the edit stay,
    // those after it move by its length, and those inside what it replaced go
    // to its end.
    const int start = range.start;
    const int oldEnd = range.start + range.length;
    const int newEnd = start + int(replacement.size());
    const int delta = newEnd - oldEnd;
    const auto track = [&](int position) {
        if (position <= start) return position;
        if (position >= oldEnd) return position + delta;
        return newEnd;
    };
    auto *buffer = qobject_cast<SourceBuffer *>(sender());
    for (Tab &tab : m_tabs) {
        if (!tab.document || tab.document->buffer() != buffer) continue;
        tab.view.cursor = track(tab.view.cursor);
        tab.view.anchor = track(tab.view.anchor);
        tab.view.top = track(tab.view.top);
    }
}

void TabModel::documentChanged()
{
    auto *document = qobject_cast<Document *>(sender());
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs.at(i).document != document) continue;
        emit dataChanged(index(i), index(i));
        if (i == m_active) emit activeChanged();
    }
    // A document of parch's own is no part of the session, whatever it does.
    if (!document || !document->isScratch()) emit changed();
}

void TabModel::relocate(const QString &from, const QString &to)
{
    bool moved = false;
    for (int i = 0; i < m_tabs.size(); ++i) {
        Tab &tab = m_tabs[i];
        if (tab.document) continue;
        QString &path = tab.view.path;
        if (path != from && !path.startsWith(from + u'/')) continue;
        path = to + path.mid(from.size());
        moved = true;
        emit dataChanged(index(i), index(i));
        if (i == m_active) emit activeChanged();
    }
    if (moved) emit changed();
}
