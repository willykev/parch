#pragma once

#include "dialectchoice.h"
#include "diskfile.h"
// The buffer is a property QML reads, so its type must be complete here.
#include "sourcebuffer.h"

#include <QObject>
#include <QString>
#include <optional>

// One open document: its authoritative text, where it lives, and how its last
// save went. Only DocumentStore creates documents and moves them, so that a
// file never has two documents writing to it.
class Document : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(SourceBuffer *buffer READ buffer CONSTANT)
    Q_PROPERTY(QString newline READ newline NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString path READ path NOTIFY changed)
    Q_PROPERTY(bool draft READ isDraft NOTIFY changed)
    Q_PROPERTY(State state READ state NOTIFY changed)
    // The state in words, as the status line and a tab say it. A draft with
    // no text is an empty draft rather than saved, whatever is on disk, and a
    // conflict over a file that went says the file is missing, and one over a
    // file that cannot be read, that it cannot be read.
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
    Q_PROPERTY(bool fileMissing READ fileMissing NOTIFY changed)
    Q_PROPERTY(bool fileUnreadable READ fileUnreadable NOTIFY changed)
    Q_PROPERTY(QString leadsTo READ leadsTo NOTIFY changed)
    Q_PROPERTY(bool nameTaken READ nameTaken NOTIFY changed)
    Q_PROPERTY(bool textless READ isTextless NOTIFY changed)
    // Whether the text has ever had a file of its own; false for a draft
    // that was never saved, which nothing on disk leads back to.
    Q_PROPERTY(bool everSaved READ everSaved NOTIFY changed)
    // A document of parch's own, such as its guide, under a title of its
    // own: it has no file and is no draft that the library lists, it is
    // never saved or kept in recovery, history or the session, and it goes
    // when its last tab closes.
    Q_PROPERTY(bool scratch READ isScratch CONSTANT)
    // The encoding the text was read in, as the writer chose for a file that
    // is not UTF-8, until the writer confirms that saving writes UTF-8 over
    // the file: nothing saves it meanwhile. Empty otherwise.
    Q_PROPERTY(QString convertedFrom READ convertedFrom NOTIFY changed)
public:
    enum class State { Clean, Modified, Saving, SaveFailed, Conflict, ReadOnly };
    Q_ENUM(State)

    // A UUID, kept when a draft is named and across restarts.
    QString id() const;
    SourceBuffer *buffer() const;
    // The canonical path of a named file; empty for a draft.
    QString path() const;
    bool isDraft() const;
    QString title() const;
    State state() const;
    QString statusText() const;
    // A draft's title, from its text: the first line with text, without its
    // heading marks and cut short between characters, or Untitled. Only the
    // start of the text is read. Safe on any thread.
    static QString draftTitle(const QString &text);
    // The file name a draft is offered when it is first saved: its first
    // heading, or else its first words, at most 60 characters and cut
    // between words, with .md. Nothing in it can reach another folder or
    // hide the file: no '/', no control characters, no leading dot. Untitled
    // when nothing is left.
    static QString suggestedFileName(const QString &text);
    // The text as a save writes it, in the document's own line endings and
    // byte-order mark.
    QByteArray bytes() const;
    QString lastError() const;
    // The document's line ending, for Enter.
    QString newline() const;
    // The line ending and byte-order mark its text is saved with.
    TextConvention convention() const;
    DiskVersion diskVersion() const;
    // The document is in conflict because its file is gone, so parch may hold
    // the only copy of the text.
    bool fileMissing() const;
    // The document is in conflict because its file is there but parch cannot
    // read it, so parch can neither show it nor tell what saving would
    // replace: it waits until the file is read again, or the text is saved
    // somewhere else.
    bool fileUnreadable() const;
    // The file the document's path leads to now, while that is not the
    // document's own: a link put in its place, or a folder on its path
    // changed, leads to another file. The document waits in conflict, and
    // that file is neither written nor taken unless the writer follows the
    // link. Empty otherwise.
    QString leadsTo() const;
    // The document waits because, when its file was to be made again,
    // something else had taken the file's name, which parch left as it is.
    // Its reason says all there is to say.
    bool nameTaken() const;
    bool everSaved() const;
    // Whether a save of it runs, which state() does not say during a
    // conflict, since a naming can run while the conflict waits.
    bool isSaving() const;
    // Whether the text is other than what parch last put in its file: edited
    // since, or undone back to a point the buffer counts as saved while a
    // later save, which landed after the text moved on, is in the file.
    bool differsFromFile() const;

    // The Markdown settings kept for the document, its library and every
    // document (documentsettings.h), which the window keeps current as they
    // or the document's place change. Their flags and front matter layers
    // are empty: the front matter is the text's own, read with the text.
    Dialect::Layers savedDialect() const { return m_dialect; }
    // Says dialectChanged() when they differ from those it has.
    void setSavedDialect(const Dialect::Layers &layers);
    // What every view of the window parses the text with: the profile and
    // extensions those layers choose. Read it with the snapshot a worker
    // parses, and parse again when dialectChanged() says it changed.
    md::Options markdownOptions() const { return m_choice.options; }
    // Where the text's `/rooted` includes are found: the library root those
    // layers choose, the library folder holding it by default; empty for none.
    QString libraryRoot() const { return m_choice.libraryRoot; }
    // The document was opened again at a path parch recorded as its file,
    // which led to another file by then, and nothing kept its text: it has
    // none of its own. It shows no text and takes none, and has nothing to
    // save, until a version from its history is restored or the link is
    // followed.
    bool isTextless() const;
    bool isScratch() const;
    QString convertedFrom() const;

signals:
    void changed();
    // The settings its text is read in changed (savedDialect()): every view
    // parses it again.
    void dialectChanged();

private:
    friend class DocumentStore;

    // How the last save ended. A failure stays until a later save succeeds, so
    // that further typing does not hide it.
    enum class Problem { None, Failed, Conflict };
    // What changed() announces, kept to tell a real change from a repeat.
    struct Shown {
        State state;
        QString status;
        QString title;
        QString path;
        QString lastError;
        bool fileMissing;
        bool fileUnreadable;
        QString leadsTo;
        bool nameTaken;
        bool textless;
        QString convertedFrom;
        bool operator==(const Shown &) const = default;
    };

    Document(const QString &id, const QString &path, const QString &text, const TextConvention &convention,
             const DiskVersion &version, bool readOnly, QObject *parent);
    Shown shown() const;
    // Emits changed() when anything the interface shows is different.
    void refresh();

    QString m_id;
    SourceBuffer *m_buffer;
    QString m_path;
    TextConvention m_convention;
    // What parch last read or wrote; a save only replaces a file still at this version.
    DiskVersion m_diskVersion;
    bool m_readOnly;
    Problem m_problem = Problem::None;
    QString m_lastError;
    bool m_saving = false;
    bool m_fileMissing = false;
    bool m_fileUnreadable = false;
    QString m_leadsTo;
    bool m_nameTaken = false;
    // A save finished after the text had moved on, so the file holds an older
    // revision than the buffer, whatever the buffer's saved position says.
    bool m_diskAhead = false;
    // The file holds bytes parch read rather than wrote, as when it was
    // opened or reloaded, and no save has replaced them yet: the first save
    // keeps them in history before it does.
    bool m_diskUnkept = false;
    // The name a naming of the document's own file was asked under, taken
    // by the save that naming starts, which says it again when it ends.
    QString m_askedName;
    // Requests made while a save ran; at most one more save follows it.
    bool m_saveAgain = false;
    QString m_pendingSaveAs;
    // The version the waiting name must still find, when the name asked for one.
    std::optional<DiskVersion> m_pendingExpected;
    // The file is being compared with what parch last read, off the UI thread.
    bool m_checking = false;
    // A choice made in a conflict runs off the UI thread: Keep both reads
    // the file and makes the copy, or Follow reads the file the link leads
    // to and keeps this text in history.
    bool m_resolving = false;
    bool m_textless = false;
    // A document of parch's own, and its title.
    bool m_scratch = false;
    QString m_scratchTitle;
    QString m_convertedFrom;
    // The file may have changed again while a save or a comparison ran.
    bool m_checkAgain = false;
    Shown m_shown;
    Dialect::Layers m_dialect;
    Dialect::Choice m_choice;
};
