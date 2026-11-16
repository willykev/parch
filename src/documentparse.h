#pragma once

#include "dialectchoice.h"
#include "documentreading.h"
#include "sourcetypes.h"

#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <utility>

class Document;

// One reading of a document's text: the text as it was at a revision, and
// what the Markdown engine made of it in the document's dialect, with its
// included files and its bibliography's keys (DocumentReading::parse; the
// citations are not formatted, which is the preview's own work). Never
// changed once made, so that every view and worker can read it at once.
// Views of the document's own text read file 0 of the tree; the files it
// includes are the other files.
struct ParsedText {
    Revision revision = 0;
    QString text;
    DocumentReading::Reading reading;

    const md::Document &document() const { return reading.document; }
};
using ParsedTextPtr = std::shared_ptr<const ParsedText>;
Q_DECLARE_METATYPE(ParsedTextPtr)

// The parse of one document that every view of it shares: live styling,
// spelling, the preview, the word count and the outline ask it for the text
// as it is now, and the text is parsed once for each revision, in the
// settings and the place the document is read in, on a worker. Each view
// does its own work from there on its own worker, as before.
//
// At most one parse of a document runs at a time. A request while one runs
// waits for it; when the text or its settings changed meanwhile, that
// parse is dropped, never handed over as current, and the text as it is
// then is parsed at once. Every parse handed over is of the text as it is
// at that moment.
class DocumentParse : public QObject {
    Q_OBJECT

public:
    // Made by the document, as one of its children.
    explicit DocumentParse(Document *document);
    ~DocumentParse() override;

    // The parse of the text as it is now, read as the document is read now;
    // null when there is none yet.
    ParsedTextPtr current() const;
    // Asks for the text as it is now: returns its parse when there is one,
    // and otherwise has it parsed, unless a parse runs, and says parsed()
    // once it is.
    ParsedTextPtr request();
    // Calls `then` with the parse of the text as it is now: at once when
    // there is one, and otherwise once it is parsed, on `context`'s thread.
    // Never when `context` or the document goes first. When the parse
    // fails, `failed` is called with the reason instead, when given;
    // without it, `then` waits for the next parse.
    void whenParsed(QObject *context, std::function<void(const ParsedTextPtr &)> then,
                    std::function<void(const QString &reason)> failed = {});
    // Why the text as it is now could not be parsed, as when memory ran
    // out; empty when it was, or is being parsed. A parse that failed is
    // not made again for the same text and settings: once either changes,
    // the next request makes it.
    QString failure() const;
    // The files the text was read with changed on disk, such as an included
    // chapter or a bibliography: the next request reads the text again.
    void invalidate();
    // A parse runs on the worker.
    bool parsing() const { return m_running; }
    // Where a draft's relative paths are found: its own folder, which the
    // document store gives it.
    void setDraftFolder(const QString &folder) { m_draftFolder = folder; }
    // Where the document's relative paths and `/rooted` includes are found,
    // as {folder, library root}, when the application knows better than the
    // document: its library folders, say. Without, the draft folder or the
    // file's folder, and Document::libraryRoot().
    using Places = std::function<std::pair<QString, QString>(const Document *)>;
    void setPlaces(Places places) { m_places = std::move(places); }

    // For measuring and tests: how many parses ran to the end, and how many
    // of them were dropped since the text or its settings had changed.
    int parses() const { return m_parses; }
    int dropped() const { return m_dropped; }
    // Makes every parse take at least this long, so that a test can edit
    // the text while it is being read.
    static void setWorkerDelay(int milliseconds);

signals:
    // The text as it is now is parsed: said only for the current text.
    void parsed(const ParsedTextPtr &parse);
    // The text as it is now could not be parsed, and why.
    void failed(const QString &reason);

private:
    // What a parse was made from: whenever any of it differs, the parse is
    // no longer the text as the document is read now.
    struct Key {
        Revision revision = 0;
        Dialect::Layers dialect;
        QString path;
        QString directory;
        QString libraryRoot;
        quint64 reads = 0;
        bool operator==(const Key &) const = default;
    };
    Key keyNow() const;
    void start();
    // A parse ended: with the parse, or with why it failed.
    void finished(ParsedTextPtr parse, const Key &key, const QString &failure = {});
    // Lets go of a parse no longer current, on a worker.
    static void letGo(ParsedTextPtr parse);

    Document *m_document;
    QString m_draftFolder;
    Places m_places;
    ParsedTextPtr m_current;
    Key m_currentKey;
    // The last parse that failed: what it was made from, and why.
    Key m_failedKey;
    QString m_failure;
    bool m_running = false;
    quint64 m_reads = 0;
    int m_parses = 0;
    int m_dropped = 0;
};
