#pragma once

#include "markdownediting.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <Qt>

class Document;
class QInputMethodEvent;
class QKeyEvent;
class SourceBuffer;

// What Vim keys need of the view they act in. Positions are source offsets;
// EditorBinding is the host.
class VimHost {
public:
    virtual ~VimHost() = default;
    // The document the view shows, or null while it shows none.
    virtual Document *document() const = 0;
    // Whether the view takes edits now.
    virtual bool editable() const = 0;
    // Whether an input method is composing text in the view.
    virtual bool composing() const = 0;
    virtual int cursor() const = 0;
    virtual int anchor() const = 0;
    // Selects from anchor to cursor and keeps the cursor in view.
    virtual void select(int anchor, int cursor) = 0;
    // Makes a change as one undo step and selects as it says; false when
    // the view takes no edits.
    virtual bool apply(const TextChange &change) = 0;
    // Hands a key, or an input method's text, to the view, which takes it
    // as it takes typing.
    virtual void type(int key, Qt::KeyboardModifiers modifiers, const QString &text) = 0;
    virtual void commit(const QString &text) = 0;
    // Whether the view takes text from input methods.
    virtual void setInputMethodEnabled(bool enabled) = 0;
};

// Vim keys for one view: Normal mode, where keys move and edit by Vim's
// commands; Insert mode, where they type as they do without Vim keys;
// Visual mode, by character or by line; and a command line for search and
// for :w, :q, :wq and :x. Every edit goes through the document's
// SourceBuffer, and a command, or a stay in Insert mode with the command
// that began it, is one undo step. Registers, the last search and the last
// f, t, F or T are shared by every view, as in one Vim; "." repeats this
// view's last change.
//
// Keys with Ctrl, other than Ctrl+R and Ctrl+[, are left to the view and to
// the window's shortcuts in every mode, so that the command palette, the
// menu and the conventional editing keys keep working. Escape always ends
// in Normal mode.
class VimMode : public QObject {
    Q_OBJECT
    // Whether the view takes Vim keys at all.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(Mode mode READ mode NOTIFY modeChanged)
    // The mode in words, as the status line shows it; empty while inactive.
    Q_PROPERTY(QString modeName READ modeName NOTIFY modeChanged)
    // The keys of a command not yet complete, such as `2d` or `"a`.
    Q_PROPERTY(QString pending READ pending NOTIFY stateChanged)
    // The command line being typed, with its `:`, `/` or `?`; empty when there is none.
    Q_PROPERTY(QString commandLine READ commandLine NOTIFY stateChanged)
    // What the last command had to say, such as a search that found nothing;
    // gone at the next key.
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    // The character the cursor is on, from cursor to cursorEnd; the two are
    // equal at the end of a line.
    Q_PROPERTY(int cursor READ cursor NOTIFY cursorChanged)
    Q_PROPERTY(int cursorEnd READ cursorEnd NOTIFY cursorChanged)

public:
    enum Mode { Normal, Insert, Visual, VisualLine };
    Q_ENUM(Mode)

    explicit VimMode(VimHost *host, QObject *parent = nullptr);
    ~VimMode() override;
    // Zero-based line reached by a count followed by %, or -1 for an invalid count.
    static int percentageLine(int percent, int lines);

    bool active() const { return m_active; }
    // Turned on, the view starts in Normal mode; turned off, it types again.
    void setActive(bool active);
    Mode mode() const { return m_mode; }
    QString modeName() const;
    QString pending() const { return m_typed; }
    QString commandLine() const { return m_commandLine; }
    QString message() const { return m_message; }
    int cursor() const { return m_cursor; }
    int cursorEnd() const;

    // A key the view received: true when Vim keys took it, false when it
    // goes on as it would without them, as typing in Insert mode does.
    bool handleKey(QKeyEvent *event);
    // Whether this key chord belongs to Vim before a window shortcut sees it.
    bool takesShortcut(const QKeyEvent *event) const;
    // The same for an input method's text: outside Insert mode it is
    // taken as keys, never typed.
    bool handleInputMethod(QInputMethodEvent *event);
    // The view shows another document, or none: back to Normal mode, with
    // nothing pending.
    void reset();
    // The view's cursor or selection moved: by an edit, the mouse, or keys
    // Vim keys let through. A selection made so is Visual mode's.
    void selectionMoved();
    // The view takes no input while a dialog is open, whose choices, such
    // as reloading the file, are edits of their own: what Insert mode typed
    // before is one undo step, and what it types after is another.
    void setBlocked(bool blocked);

signals:
    void activeChanged();
    void modeChanged();
    void stateChanged();
    void cursorChanged();
    // A window command, by its id in the command registry: "save" or
    // "closeTab". Sent once the key that asked for it has been handled.
    void commandRequested(const QString &command);

private:
    // A key typed in Insert mode, or an input method's text when commit is set.
    struct Key {
        int key = 0;
        Qt::KeyboardModifiers modifiers;
        QString text;
        bool commit = false;
    };
    struct Motion;
    struct Region;
    // A change "." repeats: the keys of the command, without its count or
    // register, and what Insert mode then typed. A Visual mode command is
    // repeated on as much text from the cursor.
    struct Change {
        QStringList keys;
        int count = 0;
        QChar reg;
        bool visual = false;
        bool visualLines = false;
        int lines = 1;
        int columns = 0;
        QList<Key> inserted;
    };
    enum class Await { None, Register, Char, G, Object };
    enum class Use { Move, Operator, Visual };
    enum class InsertKind { Plain, OpenBelow, OpenAbove };

    QString tokenOf(const QKeyEvent *event) const;
    // One key from the view, a whole command or part of one; true when taken.
    bool run(const QString &token);
    bool press(const QString &token);
    void normalKey(const QString &k);
    void visualKey(const QString &k);
    void operatorKey(const QString &k);
    void awaited(const QString &k);
    void commandLineKey(const QString &k);
    bool escape();
    // Ends a command, keeping it for "." when it changed the text.
    void done(bool change);
    // Drops whatever was pending.
    void cancel();
    int count() const;
    bool hasCount() const { return m_count1 > 0 || m_count2 > 0; }
    void showTyped(const QString &k);

    QString text() const;
    SourceBuffer *buffer() const;
    QString newline() const;
    bool motion(const QString &k, const QString &argument, Use use, Motion *out);
    void moveTo(const Motion &motion);
    void visualMove(const Motion &motion);
    void operate(QChar op, const Region &region, int cursorAfterYank);
    void finishOperator(const Motion &motion);
    void recordInsert(const Key &key);
    Region regionFor(int from, const Motion &motion, bool forDelete) const;
    bool textObject(QChar type, bool inner, int *start, int *end, bool *linewise);

    bool edit(int start, int end, const QString &replacement, int cursor);
    void deleteRegion(const Region &region, bool keep = true);
    void yankRegion(const Region &region, int cursor);
    void changeRegion(const Region &region);
    void shiftLines(int from, int to, int times, bool outdent);
    bool joinLines(int from, int lines);
    void changeCase(int start, int end, int how, int cursor);
    bool replaceChars(int start, int end, const QString &with, int cursor);
    bool put(bool before);
    void visualPut(bool before);
    void undo(bool redo);
    void repeat();

    bool enterInsert(int position, InsertKind kind = InsertKind::Plain, int count = 1);
    void leaveInsert();
    void openLine(bool below);
    void replayInsertKey(const Key &key);

    void setMode(Mode mode);
    void leaveVisual();
    void showVisual();
    Region visualRegion(bool lines) const;
    void startVisual(Mode mode);
    void rememberVisualSize();
    void setCursor(int position, bool updateWant = true);
    int clamped(int position) const;
    void setMessage(const QString &message);
    void openCommandLine(QChar kind);
    void executeCommandLine(const QString &line);
    void exCommand(const QString &command);
    bool search(bool forward, int count, int from, int *found);
    void request(const QString &command);

    VimHost *m_host;
    bool m_active = false;
    Mode m_mode = Normal;
    int m_cursor = 0;
    // The column j and k keep to, in display columns; -1 for the end of the line.
    int m_want = 0;
    int m_visualAnchor = 0;

    // The command being typed.
    int m_count1 = 0;
    int m_count2 = 0;
    QChar m_register;
    QChar m_operator;
    Await m_await = Await::None;
    QString m_awaitFor;
    int m_deadKey = 0;
    QStringList m_keys;
    QString m_typed;
    // The size of the Visual area a command acts on, for ".".
    Change m_visualSize;

    QString m_commandLine;
    QString m_message;

    Change m_last;
    bool m_recordingInsert = false;
    int m_replaying = 0;
    // Set while Vim keys move the view's cursor themselves.
    int m_moving = 0;
    // Undo and redo move through steps that must stay as they are.
    bool m_skipJoin = false;

    // Insert mode: the undo depth to join back to, the buffer it is in, what
    // began it and what was typed.
    qsizetype m_keyMark = 0;
    qsizetype m_insertMark = 0;
    QPointer<SourceBuffer> m_insertBuffer;
    InsertKind m_insertKind = InsertKind::Plain;
    int m_insertCount = 1;
    QList<Key> m_insertKeys;
    bool m_insertOverflow = false;
};
