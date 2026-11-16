#pragma once

// The backend and the document are properties QML sets, so their types
// must be complete here.
#include "backend.h"
#include "document.h"
#include "documentcheck.h"
#include "documentparse.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QVariantList>

#include <memory>

namespace RenderCheck {
class Batch;
}

// Check document: everything wrong in the document in front, as `parch
// check` finds it, listed for the window. It reads the document's parse of
// its text as it is when the check starts (DocumentParse, shared by every
// view of the document, which reads it as every output reads it: its
// settings, its front matter, its includes from disk), formats its
// citations and checks it on a worker: the engine's diagnostics, missing
// images, links to files and headings that are not there, and what is
// wrong with its citations. Then its formulas
// and diagrams are drawn by the render service, on the service's own
// thread, and what their helpers refuse is added in its place. Nothing of
// it runs on the UI thread but taking the results.
//
// Each problem is a row: {text ("file:line:column: message"), place
// ("file:line:column"), message, severity ("error", "warning" or "info"),
// code, kind, file (as shown), path (empty for a draft's own text),
// documentId (the draft's), start, length and line (from 1) in that file,
// matched (the text there, for finding it again should the file change),
// included (in a file the document includes)}, in file and source order.
class DocumentChecker : public QObject {
    Q_OBJECT
    Q_PROPERTY(Backend *backend READ backend WRITE setBackend NOTIFY backendChanged)
    // A check is going on.
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(QVariantList problems READ problems NOTIFY checked)
    // In a sentence or two: how many problems and notes, and what could not
    // be checked for want of a helper.
    Q_PROPERTY(QString summary READ summary NOTIFY checked)

public:
    explicit DocumentChecker(QObject *parent = nullptr);
    ~DocumentChecker() override;

    Backend *backend() const { return m_backend; }
    void setBackend(Backend *backend);
    bool checking() const { return m_checking; }
    QVariantList problems() const { return m_problems; }
    QString summary() const { return m_summary; }

    // Checks the document as its text is now; checked() follows.
    Q_INVOKABLE void check(Document *document);
    // What is going on is dropped, and nothing follows.
    Q_INVOKABLE void cancel();
    // Opens the place of a problem, a row of problems(): in the document,
    // or in the file it is written in, in a tab of the window in front, with
    // the place selected.
    Q_INVOKABLE void open(int row);

    // Makes every worker take at least this long, so that a test can change
    // the text while it is being checked.
    static void setWorkerDelay(int milliseconds);

signals:
    void backendChanged();
    void checkingChanged();
    void checked();

private:
    struct Found;
    static std::shared_ptr<Found> work(ParsedTextPtr parse);
    void read(quint64 request, const std::shared_ptr<Found> &found);
    void finish();
    QVariantMap row(const DocumentCheck::Problem &problem) const;
    void setChecking(bool checking);

    QPointer<Backend> m_backend;
    QThreadPool m_pool;
    quint64 m_request = 0;
    bool m_checking = false;
    // The check under way or done: the document, what was found, and the
    // formulas and diagrams being drawn.
    QPointer<Document> m_document;
    QString m_documentId;
    std::shared_ptr<Found> m_found;
    std::unique_ptr<RenderCheck::Batch> m_batch;
    int m_skippedFormulas = 0;
    int m_skippedDiagrams = 0;
    QVariantList m_problems;
    QString m_summary;
};
