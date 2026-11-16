#pragma once

#include "mdtree.h"
#include "sourcetypes.h"
// The document is a property QML reads, so its type must be complete here.
#include "document.h"

#include <QAbstractListModel>
#include <QList>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>

// One heading of a document, as an outline lists it.
struct Heading {
    // 1 to 6.
    int level = 1;
    // What it says, without its marks, formatting or a trailing [label].
    QString text;
    // Where its first line starts, as a source offset.
    int position = 0;
    // Which line that is, from 0.
    int line = 0;
    // The anchor a `#slug` link reaches it by, numbered as the document alone
    // numbers its headings, and its Writer label, if it has one.
    QString slug;
    QString label;
    // A heading an include brought in: the file it is written in, and its
    // line there, from 0. Its position is then the start of the line in the
    // document's own text that includes it. Empty, and -1, for the
    // document's own headings.
    QString file;
    int fileLine = -1;
};

namespace Outline {
// The headings of a Markdown text as the Markdown engine reads them, the
// outline that {{TOC}} and anchors are made from: ATX and Setext headings,
// wherever they stand, quotes and list items too, but never in code, front
// matter, HTML or comments. Headings with no text are left out.
// `options` is the dialect the text is read in (Document::markdownOptions()).
QList<Heading> headings(const QString &text, const md::Options &options = {});
// The same with the headings of the files it includes, which `source` reads.
QList<Heading> headings(const QString &text, const md::Options &options, const md::Source &source);
} // namespace Outline

// The headings of one document, for a list that goes to them: found off the
// UI thread, from a snapshot of the text, once it has rested after an edit,
// and taken only while the text is still at that revision. Nothing is
// looked for while the model is not active, so that typing pays for nothing
// while no list shows it. A filter keeps the headings whose text holds it,
// ignoring case.
class OutlineModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(Document *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    // The headings shown are those of the text as it is now.
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    // QML reads a heading's text as "heading", since a delegate's "text" is its own.
    enum Role { TextRole = Qt::UserRole + 1, LevelRole, PositionRole, LineRole, SlugRole, LabelRole, FileRole, FileLineRole };
    Q_ENUM(Role)

    // How long the text must rest after an edit before it is looked at again.
    static constexpr int EditSettleMs = 300;

    explicit OutlineModel(QObject *parent = nullptr);
    ~OutlineModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Document *document() const;
    void setDocument(Document *document);
    bool active() const;
    void setActive(bool active);
    QString filter() const;
    void setFilter(const QString &filter);
    bool ready() const;
    QList<Heading> headings() const;
    // Where the heading in this row starts, as a source offset; -1 for no row.
    Q_INVOKABLE int positionAt(int row) const;
    // The file a heading in this row is written in when an include brought
    // it in, and its line there; empty, and -1, for the document's own.
    Q_INVOKABLE QString fileAt(int row) const;
    Q_INVOKABLE int fileLineAt(int row) const;
    // The row of the heading a source offset lies under: the last that
    // starts at or before it; -1 for none.
    Q_INVOKABLE int rowAt(int position) const;

signals:
    void documentChanged();
    void activeChanged();
    void filterChanged();
    void countChanged();
    void readyChanged();

private:
    struct Scan {
        QList<Heading> headings;
        Revision revision = 0;
    };
    static Scan scanJob(QSharedPointer<const SourceSnapshot> snapshot, md::Options options, QString path,
                        QString directory, QString libraryRoot);
    void schedule(int delayMs);
    void scan();
    void take(const QList<Heading> &headings);
    void show();
    void setReady(bool ready);

    QPointer<Document> m_document;
    QMetaObject::Connection m_edits;
    QMetaObject::Connection m_dialect;
    bool m_active = false;
    QString m_filter;
    QList<Heading> m_all;
    QList<Heading> m_shown;
    bool m_ready = false;
    QTimer m_timer;
    bool m_running = false;
    bool m_scanAgain = false;
    quint64 m_generation = 0;
    QThreadPool m_pool;
};
