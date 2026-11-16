#pragma once

#include "apppaths.h"
#include "sourcetypes.h"

#include <QDate>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QVariantList>
#include <QtConcurrentRun>
#include <atomic>
#include <functional>

class Document;

// Text that goes into a document once what it refers to is ready, such as a
// file being copied. The place follows every edit made meanwhile, from any
// view or an undo, so the text lands where it was asked for. It belongs to
// the document and goes with it.
class Insertion : public QObject {
    Q_OBJECT
public:
    // `range` is what the text is to replace; empty, it is a place.
    Insertion(Document *document, SourceRange range);
    Document *document() const;
    SourceRange range() const;
    // Puts the text in, in the document's line endings, as one undo step.
    // Block text, such as several paragraphs or an include, is given lines
    // of its own. The range is replaced only while no edit has reached into
    // it; otherwise the text goes at its start. False when the document
    // cannot take text, as a read-only one. The insertion is used up and
    // deleted either way.
    bool insert(const QString &text, bool block);
    // Gives the insertion up.
    void cancel();
    // `text` as it is to stand in `source` at `range`: block text with a
    // blank line between it and the text before and after it on its lines.
    static QString fitted(QStringView source, SourceRange range, const QString &text, bool block);

private:
    void follow(SourceRange edited, const QString &replacement);

    QPointer<Document> m_document;
    QMetaObject::Connection m_following;
    int m_start = 0;
    int m_end = 0;
    bool m_touched = false;
    bool m_used = false;
};

// The files a document refers to: images pasted into it and files dropped on
// it, placed in its assets folder, and the links that point to nothing. A
// named document's assets are in "assets" beside its file; a draft's are in
// its own folder among the drafts, which naming the draft moves beside its
// file. Names are readable and never taken from another file: a file already
// there with the same bytes is used as it is, and otherwise the name takes a
// number. Copies run on workers, never on the UI thread, and stop when the
// application ends.
class Attachments : public QObject {
    Q_OBJECT
public:
    // Files larger than this are copied only once the writer agrees.
    static constexpr qint64 LargeFileBytes = qint64(50) << 20;

    explicit Attachments(const AppPaths &paths, QObject *parent = nullptr);
    ~Attachments() override;

    // The library's folders: a file dropped from one is linked where it is
    // rather than copied.
    void setLibraryFolders(std::function<QStringList()> folders);
    QStringList libraryFolders() const;
    // The day pasted images are named after; today unless a test says.
    void setToday(std::function<QDate()> today);
    QDate today() const;
    void setLargeFileBytes(qint64 bytes);
    qint64 largeFileBytes() const;

    // The folder a document's relative links start from: its file's
    // folder, a draft's own folder, or the resources of one of parch's own.
    QString documentFolder(const Document *document) const;
    // The library folder that holds the document, the nearest when several
    // do; empty for a draft or a document outside every library.
    QString libraryOf(const Document *document) const;

    // Says that something could not be attached; the windows show it as a notice.
    void report(const QString &title, const QString &detail);

    // Runs `work` on a worker and then `done`, on this thread, with what it
    // returned, unless `context` has gone meanwhile.
    template <typename Result>
    void run(QObject *context, std::function<Result()> work, std::function<void(Result)> done)
    {
        auto *watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcherBase::finished, watcher, &QObject::deleteLater);
        connect(watcher, &QFutureWatcherBase::finished, context ? context : this, [watcher, done = std::move(done)] {
            done(watcher->result());
        });
        watcher->setFuture(QtConcurrent::run(&m_pool, std::move(work)));
    }
    // Set once the application ends, so that a copy running stops.
    const std::atomic<bool> *stopping() const { return &m_stopping; }

    // The place a file took, relative to the document's folder, as
    // "assets/name.png"; or why it took none.
    struct Placed {
        QString relative;
        QString error;
    };
    // Saves an image's bytes in <folder>/assets as pasted-<date>-<n>.<extension>,
    // at the first n free or already holding the same bytes.
    static Placed savePasted(const QString &folder, const QDate &date, const QString &extension,
                             const QByteArray &bytes);
    // Copies a file into <folder>/assets under its own name, or with -2, -3
    // and on before its extension.
    static Placed copyIn(const QString &folder, const QString &source, const std::atomic<bool> *stop = nullptr);
    // The extension a pasted image of this type is saved with; empty for a
    // type that is better saved as PNG.
    static QString extensionFor(const QString &mimeType);

    // A file dropped on a document, and what becomes of it.
    struct Dropped {
        QString source;
        QString name;
        qint64 size = 0;
        bool image = false;
        bool markdown = false;
        bool folder = false;
        bool missing = false;
        // The path it is linked by where it is, relative to the document's
        // folder: it is inside that folder, or inside a library while the
        // document is named. Empty when it is copied into the assets.
        QString linked;
        // The path an include names it by: relative to a named document's
        // folder, starting with ./ or ../; empty for a draft, which includes nothing.
        QString included;
    };
    // Looks at the files, off the UI thread: what each is, how large, and
    // where it is linked from or whether it is copied. `folder` is the
    // document's folder; `libraries`, the library's folders.
    static QList<Dropped> plan(const QStringList &paths, const QString &folder, bool draft,
                               const QStringList &libraries);

    static bool isImageFile(const QString &name);
    // A Markdown document another can include.
    static bool isMarkdownFile(const QString &name);
    // Markdown that refers to a file at a path relative to the document,
    // the path encoded as a link destination.
    static QString imageMarkdown(const QString &alt, const QString &relative);
    static QString linkMarkdown(const QString &text, const QString &relative);
    // The line that includes a document, quoted when it has spaces.
    static QString includeLine(const QString &included);

signals:
    void problem(const QString &title, const QString &detail);

private:
    AppPaths m_paths;
    std::function<QStringList()> m_libraries;
    std::function<QDate()> m_today;
    qint64 m_large = LargeFileBytes;
    QThreadPool m_pool;
    std::atomic<bool> m_stopping{false};
};
