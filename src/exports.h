#pragma once

#include "apppaths.h"
#include "dialectchoice.h"
#include "documentparse.h"
#include "printpreview.h"
#include "renderservice.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

class Document;
class Exporter;
class FileRequests;
class Printer;
class WindowContext;
namespace DocumentReading {
struct Reading;
}

// The window's exports and printing, each in the background. An export
// takes the document's parse of its text as it is when it starts (which
// every view of it shares: its includes, bibliography and front matter, in
// the document's own settings) and formats its citations on a worker; then the format's exporter (exporter.h), the same code as `parch
// export`, draws its formulas and diagrams and writes it, atomically, never
// over the document or a file it includes. Its progress shows in the notice
// bar, where it can be cancelled; when it ends the notice says what was
// written and what the format could not carry over, or why nothing was.
//
// Before anything is asked, read() reads the document for the Export
// dialog, which then lists, format by format, what that format leaves out
// of this document.
//
// What each export was asked to do, its recipe, is kept for its document,
// so that Export again repeats it, to the same file with the same options.
// Printing makes the PDF in the paper the print dialog chose and hands it to
// the desktop's printing (Printer).
class Exports : public QObject {
    Q_OBJECT
    // Every format, as the dialog offers it: {format, title, suffix,
    // options: [{name, label, kind ("switch", "choice", "number", "text"),
    // choices, defaultValue, minimum, maximum}]}.
    Q_PROPERTY(QVariantList formats READ formats CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // How far the export in progress has come, from 0 to 1, and what it is doing.
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString status READ status NOTIFY progressChanged)
    // The document read() reads is still being read.
    Q_PROPERTY(bool reading READ reading NOTIFY readChanged)
    // Changes each time a reading lands, for bindings that list limitations.
    Q_PROPERTY(int revision READ revision NOTIFY readChanged)
    // The pages of a PDF or a print before it is made, for the dialog.
    Q_PROPERTY(PrintPreview *preview READ preview CONSTANT)

public:
    explicit Exports(const AppPaths &paths, FileRequests *files, QObject *parent = nullptr);
    ~Exports() override;

    // Without a printer, printing says it cannot.
    void setPrinter(Printer *printer);
    // Where the helpers are and drawings are kept; found as the application
    // finds them unless set.
    void setRenderOptions(const RenderService::Options &options);

    QVariantList formats() const;
    bool busy() const { return m_job != nullptr; }
    qreal progress() const { return m_progress; }
    QString status() const { return m_status; }
    bool reading() const { return m_reading; }
    int revision() const { return m_revision; }
    PrintPreview *preview() const { return m_preview; }
    // The margins and text size a PDF template sets, by option name, for a
    // dialog to show until others are chosen.
    Q_INVOKABLE QVariantMap paperDefaults(const QString &templateName) const;

    // Reads the document as an export would, for limitations().
    Q_INVOKABLE void read(Document *document);
    // What the format, with these options, leaves out of the document
    // read() read; empty until it has been read.
    Q_INVOKABLE QStringList limitations(const QString &format, const QVariantMap &options) const;
    // Asks where to write the document in the format, for the window, then
    // exports it there.
    Q_INVOKABLE void exportDocument(Document *document, const QString &format, const QVariantMap &options,
                                    WindowContext *window = nullptr);
    // The window's answer to replaceAsked(): writes over the file, or leaves
    // it as it is and exports nothing.
    Q_INVOKABLE void answerReplace(bool replace);
    // Exports it to a path the writer chose.
    Q_INVOKABLE void exportTo(Document *document, const QString &format, const QVariantMap &options, const QString &path);
    // Asks the desktop how to print, then prints the document's PDF.
    Q_INVOKABLE void print(Document *document, const QVariantMap &options);
    Q_INVOKABLE void cancel();
    // Cancels the export or print that runs and waits, for at most waitMs,
    // until it has stopped and taken away what it had begun to write; false
    // when it is still running then.
    bool stop(int waitMs);
    // The format and options the document was last exported with, for the
    // dialog to start from; empty when it never was.
    Q_INVOKABLE QString lastFormat(Document *document) const;
    Q_INVOKABLE QVariantMap lastOptions(Document *document) const;
    // Repeats the document's last export: the same format and options, to
    // the same file. False, said in a notice, when there is none.
    Q_INVOKABLE bool exportAgain(Document *document);
    // The recipe of the document's last export, as the notice reports it.
    QJsonObject lastRecipe(Document *document) const;

    // An export's options as a recipe holds them: a switch that is off is
    // left out, a value given as it is.
    static QHash<QString, QString> recipeOptions(const QString &format, const QVariantMap &options);

signals:
    void busyChanged();
    void progressChanged();
    void readChanged();
    // An export or a print ended.
    void finished(bool ok);
    // The file dialog was given a name without the format's suffix, and the
    // name with it, `path`, is a file that is there already, which the
    // dialog never asked about. The window that asked asks whether to write
    // over it; answerReplace() is its answer.
    void replaceAsked(WindowContext *window, const QString &path);
    // For the notice bar: news, or a problem.
    void news(const QString &news, const QString &detail);
    void problem(const QString &problem, const QString &detail);

private:
    struct Job;
    struct Outcome;
    void start(Document *document, const QString &format, const QHash<QString, QString> &options, const QString &output,
               bool printing, quint32 token = 0);
    void done(const Outcome &outcome);
    void poll();
    void remember(const QString &documentId, const QJsonObject &recipe);
    void loadRecipes();
    QString recipesFile() const;
    // Exports the document's parse of its text, on the export's worker.
    void exportParsed(const std::shared_ptr<Job> &job, const ParsedTextPtr &parse, const Exporter *exporter,
                      const QHash<QString, QString> &options, const QString &output, bool printing, quint32 token,
                      const QString &path, const QString &documentId, const QString &title,
                      const RenderService::Options &render);

    AppPaths m_paths;
    FileRequests *m_files;
    PrintPreview *m_preview;
    QPointer<Printer> m_printer;
    RenderService::Options m_render;
    // Exports run one at a time.
    QThreadPool m_pool;
    QTimer m_poll;

    std::shared_ptr<Job> m_job;
    qreal m_progress = 0;
    QString m_status;

    // The reading the dialog lists limitations from.
    std::shared_ptr<const DocumentReading::Reading> m_read;
    // Why the document could not be read, when it could not.
    QString m_readFailure;
    QPointer<Document> m_readDocument;
    bool m_reading = false;
    int m_revision = 0;
    quint64 m_readGeneration = 0;

    // What is waiting for the file dialog's or the print dialog's answer,
    // or for the window's about replacing a file.
    struct Asked {
        QPointer<Document> document;
        QString format;
        QHash<QString, QString> options;
        QString output = {};
    };
    Asked m_asked;
    Asked m_printing;
    Asked m_replacing;

    // Each document's last recipe, by document id.
    QHash<QString, QJsonObject> m_recipes;
};
