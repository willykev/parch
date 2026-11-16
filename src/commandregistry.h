#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>

class KeyBindings;
class Settings;
class WindowContext;

// What one command is: its id, the title menus and the palette show, the
// category it is listed under, the keys it has until the writer chooses
// others, and what it needs to run.
struct CommandInfo {
    enum class Needs {
        Nothing,
        // A document in front of the window.
        Document,
        // A document in front whose text can be edited: the command edits it.
        EditableText,
    };
    QString id;
    QString title;
    QString category;
    QStringList defaultKeys;
    Needs needs = Needs::Nothing;
};

// Every command parch has, in the order the palette lists them. A module
// that adds a command adds it here, and its action where it lives.
QList<CommandInfo> standardCommands();

// The one list of everything the application can do, from which the windows
// make their shortcuts, menus, palette and toolbar. Each command has keys:
// its defaults, or the writer's own, which the settings keep. No key belongs
// to two commands, and none is a key that types or that the editor needs
// for editing text. A command's action is the application's, set here, or
// else the window's: run() reports which.
//
// As a list model each row is a command, in the order they were added.
class CommandRegistry : public QAbstractListModel {
    Q_OBJECT
    // The text of each command's first key as the desktop writes it, by
    // command id, for menus and tooltips; a command without keys is absent.
    Q_PROPERTY(QVariantMap keys READ keys NOTIFY shortcutsChanged)
    // Every key of every command, a row each with its commandId and its
    // keySequence, from which the windows make their shortcuts.
    Q_PROPERTY(QAbstractItemModel *bound READ bound CONSTANT)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        CategoryRole,
        // Portable text of every key the command has, the first the main one.
        ShortcutsRole,
        // Every key as the desktop writes it, joined.
        KeysRole,
        // True when the keys are the writer's own rather than the defaults.
        CustomizedRole,
        NeedsRole,
    };
    using Action = std::function<void(WindowContext *)>;

    explicit CommandRegistry(Settings *settings, QObject *parent = nullptr);
    ~CommandRegistry() override;

    // Adds a command, or replaces one with the same id.
    void add(const CommandInfo &info, Action action = {});
    void setAction(const QString &id, Action action);
    bool contains(const QString &id) const;
    CommandInfo info(const QString &id) const;
    QList<QKeySequence> shortcuts(const QString &id) const;

    QVariantMap keys() const;
    QAbstractItemModel *bound() const;

    Q_INVOKABLE QString title(const QString &id) const;
    Q_INVOKABLE QString category(const QString &id) const;
    // "document", "text", or empty for a command that needs nothing.
    Q_INVOKABLE QString needs(const QString &id) const;
    Q_INVOKABLE QStringList ids() const;
    // Whether the command can run now in the window.
    Q_INVOKABLE bool canRun(const QString &id, WindowContext *window) const;
    // Runs the command's own action for the window, when it can run there.
    // False for a command whose action is the window's.
    Q_INVOKABLE bool run(const QString &id, WindowContext *window);

    // Whether `sequence` (portable text) can become the command's key:
    // {allowed, problem, takenBy, takenByTitle, keys}. The problem is
    // "invalid" for no key, "typing" for a key without Ctrl, Alt or Super
    // that is no function key, "reserved" for a key the editor keeps for
    // editing text or Escape, and "taken" for a key another command has,
    // named by takenBy; a taken key can be swapped. keys is the sequence as
    // the desktop writes it.
    Q_INVOKABLE QVariantMap check(const QString &id, const QString &sequence) const;
    // Makes `sequence` the command's only key, when check() allows it.
    Q_INVOKABLE bool bind(const QString &id, const QString &sequence);
    // Gives the command a key another command has; that command takes this
    // one's main key in its place, or loses the key when this one had none.
    Q_INVOKABLE bool swap(const QString &id, const QString &sequence);
    // Leaves the command without keys.
    Q_INVOKABLE void unbind(const QString &id);
    // Gives the command its default keys back, taking them from any command
    // that was given one of them.
    Q_INVOKABLE void reset(const QString &id);
    Q_INVOKABLE void resetAll();
    // The portable text of a key press, for a dialog that records one; empty
    // for a press of a modifier alone.
    Q_INVOKABLE QString sequenceOf(int key, int modifiers) const;
    // True for a sequence that a key without Ctrl, Alt or Super and no
    // function key is part of: such a key types, or moves in the text.
    static bool isTypingKey(const QKeySequence &sequence);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    // The names of the roles, for models that list commands.
    static QHash<int, QByteArray> commandRoles();

signals:
    void shortcutsChanged();

private:
    struct Command {
        CommandInfo info;
        Action action;
        QList<QKeySequence> shortcuts;
    };
    int indexOf(const QString &id) const;
    // The editing key a sequence is, or empty; Undo and Redo keys belong to
    // the commands that undo and redo.
    QString reservedFor(const QKeySequence &sequence) const;
    // Works out every command's keys from the defaults and the writer's own.
    void refresh();
    QStringList portable(const QList<QKeySequence> &sequences) const;
    // Keeps the command's keys as the writer's own, or as the defaults when they are the same.
    void keep(const QString &id, const QList<QKeySequence> &sequences);

    Settings *m_settings;
    QList<Command> m_commands;
    KeyBindings *m_bound;
};
