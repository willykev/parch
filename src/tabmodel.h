#pragma once

#include "session.h"
// The document is a property QML reads, so its type must be complete here.
#include "document.h"
#include "sourcetypes.h"

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <optional>

class DocumentStore;

// The tabs of one window, in order, and the one in front. Each tab holds one
// view of a document: the store's document, shared with any other view of
// it, and the view's own cursor, selection and scroll. A tab retains its
// document in the store while it holds it and releases it when it closes,
// so that the store keeps a document as long as some tab shows it.
//
// A tab restored from a session waits without its document until it is
// first shown. A tab whose file is gone holds no document and says so; it
// never creates the file. Where each view was follows edits made through any
// other view, so a tab comes back to the same place in the text.
//
// QML reads the tabs and brings one to the front; opening and closing tabs
// go through the backend, which keeps their text.
class TabModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int activeIndex READ activeIndex WRITE setActiveIndex NOTIFY activeIndexChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    // The front tab's document; null while it has none.
    Q_PROPERTY(Document *activeDocument READ activeDocument NOTIFY activeDocumentChanged)
    // What the front tab shows as its name, its file and its state, also
    // while it has no document.
    Q_PROPERTY(QString activeTitle READ activeTitle NOTIFY activeChanged)
    Q_PROPERTY(QString activePath READ activePath NOTIFY activeChanged)
    Q_PROPERTY(QString activeStatus READ activeStatus NOTIFY activeChanged)
    // How the front tab shows its document, and the part of the width its
    // source takes beside the preview. Each tab keeps its own; a new tab
    // starts beside its preview when the tab in front is, at that tab's
    // divider, and as source alone otherwise.
    Q_PROPERTY(ViewMode activeMode READ activeMode WRITE setActiveMode NOTIFY activeModeChanged)
    Q_PROPERTY(qreal activeSplit READ activeSplit WRITE setActiveSplit NOTIFY activeModeChanged)

public:
    enum Role { TitleRole = Qt::UserRole + 1, PathRole, StateRole, DocumentIdRole, DocumentRole };
    Q_ENUM(Role)
    // The values of ViewState::Mode, by the names QML uses.
    enum ViewMode { Source = ViewState::Source, Split = ViewState::Split, Reading = ViewState::Reading };
    Q_ENUM(ViewMode)

    explicit TabModel(DocumentStore *store, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int activeIndex() const;
    // Brings a tab to the front, reading its document if it has not been yet.
    void setActiveIndex(int index);
    Document *activeDocument() const;
    QString activeTitle() const;
    QString activePath() const;
    QString activeStatus() const;
    ViewMode activeMode() const;
    void setActiveMode(ViewMode mode);
    qreal activeSplit() const;
    // Kept within ViewState::MinSplit and MaxSplit.
    void setActiveSplit(qreal split);

    // Shows the document in front, in the tab that already shows it here or
    // in a new tab at the end. A new tab starts at `view`'s cursor, selection
    // and scroll when one is given.
    int openDocument(Document *document, const std::optional<ViewState> &view = std::nullopt);
    // A new tab for the document, in front, in place of the tab at `index`,
    // which closes as closeTab() closes it.
    int openInPlaceOf(int index, Document *document);
    // A second tab on the same document, after the given one and in front,
    // starting where that one is.
    int newView(int index);
    // Closes the tab; the tab after it comes to the front, or the one before
    // when it was the last.
    void closeTab(int index);
    // A new draft standing for a file that does not exist yet: the tab is
    // named after the file until the draft becomes it or forgetNewFile().
    int openNewFile(Document *draft, const QString &path);
    // The draft no longer stands for a file and is shown as the draft it is.
    void forgetNewFile(Document *draft);
    // Whether the tab's draft stands for a file that does not exist yet.
    bool standsForNewFile(int index) const;
    // A tab for a file that cannot be shown, such as one in a folder that
    // does not exist. It holds no document and creates nothing.
    int openMissing(const QString &path);
    // Adds a document's tab at the end without bringing it to the front,
    // unless no tab is in front yet.
    int append(Document *document);
    // The tab already showing this file here, or -1.
    int find(const QString &path) const;
    // The first tab showing the document here, or else the first waiting
    // for it, not read yet or with its file not found; -1 for none.
    int indexOf(const Document *document) const;
    // How many tabs here show the document.
    int viewsOf(const Document *document) const;
    // A file, or a folder and everything in it, moved from one path to the
    // other: a tab that waits for a file there, not read yet or not found,
    // waits for it at its new path.
    void relocate(const QString &from, const QString &to);

    // The tabs of a session, none of them read yet. The tab in front is read
    // by loadActive(), the others when first shown. Whether the others' files
    // are there is asked off the UI thread, and a tab whose file is not there
    // says so once the answer is in.
    void restore(const QList<ViewState> &views, int active);
    // Gives a document that is already open, as one restored from recovery,
    // to every tab here that waits for it. Returns how many took it.
    int place(Document *document);
    // Reads the front tab's document if it waits for it.
    void loadActive();

    Document *documentAt(int index) const;
    ViewState viewState(int index) const;
    // Keeps the view's cursor, anchor and scroll. Which document the tab
    // shows is the tab's own and does not change.
    void setViewState(int index, const ViewState &state);
    // The same for the window, as {cursor, anchor, top}.
    Q_INVOKABLE QVariantMap viewAt(int index) const;
    Q_INVOKABLE void setView(int index, int cursor, int anchor, int top);
    // Closes every tab.
    void clear();
    // How a tab that holds no document words why, which can name a path:
    // as the application shows a path in its notices. Without it, the
    // reason is given as it came.
    void setShownText(std::function<QString(const QString &text)> shown);

signals:
    void activeIndexChanged();
    void countChanged();
    void activeDocumentChanged();
    // The front tab's name, file or state changed, or another tab came to the front.
    void activeChanged();
    // Another tab is about to come to the front: the window keeps where the
    // current one is.
    void activeAboutToChange();
    // Another tab came to the front, or the front tab's document was read:
    // the window shows its view.
    void activeViewChanged();
    // The front tab's mode or divider changed, or another tab came to the front.
    void activeModeChanged();
    // A tab took a document.
    void documentAdded(Document *document);
    // The tabs, the front tab, or a document's name or file changed.
    void changed();

private:
    struct Tab {
        QPointer<Document> document;
        // Where the view is. The document id and path describe the tab
        // while it holds no document.
        ViewState view;
        // Why the tab holds no document, once it has tried to read one.
        QString problem;
        // The file a new draft stands for until it becomes that file.
        QString newFile;
    };

    // The tab holds no document and stands for this one, by its id or its file.
    static bool waitsFor(const Tab &tab, const Document *document);
    // A new tab's view of a document, with the mode and divider it starts at.
    ViewState freshView(const QString &documentId, const QString &path) const;
    int insert(int at, const Tab &tab);
    // Reads the tab's document, or finds out why it cannot.
    void load(int index);
    // The restored tabs still waiting for these files, which are not there, say so.
    void notFound(const QSet<QString> &paths);
    void take(int index, Document *document);
    void release(Document *document);
    QString titleOf(const Tab &tab) const;
    QString pathOf(const Tab &tab) const;
    QString statusOf(const Tab &tab) const;
    // Announces that another tab, or another document, is in front.
    void announceActive(Document *before);
    void documentChanged();
    void documentEdited(Revision revision, SourceRange range, const QString &replacement);

    DocumentStore *m_store;
    std::function<QString(const QString &text)> m_shownText;
    QList<Tab> m_tabs;
    int m_active = -1;
};
