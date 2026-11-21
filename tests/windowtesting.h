#pragma once

#include "apppaths.h"
#include "backend.h"
#include "theme.h"

#include <QAccessible>
#include <QKeySequence>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QPoint>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <map>
#include <memory>

class EditorBinding;
class WindowContext;

// Keys, input and items in a window, as a person reaches them.

// A window delivers keys and input methods to its focused item only while it
// is the active window, and the window shown last took that over.
void activate(QQuickWindow *window);
void sendKey(QQuickWindow *window, QChar key);
void sendKey(QQuickWindow *window, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
void sendPreedit(QQuickWindow *window, const QString &preedit);
void typeText(QQuickWindow *window, const QString &text);
void selectRange(QQuickItem *editor, int start, int end);
QQuickItem *findItem(QQuickItem *item, const QString &name);
QPoint centreOf(QQuickItem *item);
// Compare the drawn caret with the current layout at a source character.
bool caretDrawnAt(EditorBinding *binding, QQuickItem *editor, int source);
// Gives the window a new width and waits until its content has taken it,
// since a resize reaches the window's items only through the event loop.
void resizeWindow(QQuickWindow *window, int width);

// What the window holds.

// Everything a window holds: its own children, and the items it shows with
// what they own, since delegates and popup contents belong to items without
// being the window's children.
QList<QObject *> everyObjectIn(QQuickWindow *window);
// A parch window makes its menu and dialogs the first time each is asked
// for: this makes them all, and says whether the window has any.
bool makePopups(QQuickWindow *window);
// An object by name; a menu or dialog not made yet, or something in one,
// is made first.
QObject *findObject(QQuickWindow *window, const QString &name);
QString describe(const QObject *object);
// The items a window shows, depth first: those visible and not faded out.
QList<QQuickItem *> visibleItems(QQuickWindow *window);

// Checks of the whole window. Each returns true when the window passes and
// otherwise names what failed in `problems`.

// Nothing the window shows reaches past its edges, except inside something
// that clips it, and no text is drawn past its own box: a label that has too
// little room must elide or wrap rather than run into what is beside it.
bool everyVisibleItemFitsWithin(QQuickWindow *window, QStringList *problems = nullptr);
// Every visible item that assistive technology is told about, and that says
// what it is, also tells it a name. Plain containers have no role of their
// own and need none; what is marked ignored is not in the tree at all.
QStringList unnamedAccessibleItems(QQuickWindow *window);
// What a person operates, and so what the keyboard must reach.
bool isInteractiveRole(QAccessible::Role role);
// Tab, pressed from the first control in the window, reaches every visible
// control that takes focus or that a person operates, and each control it
// reaches draws its background otherwise than without focus. The walk ends
// where focus comes round again or stays put, as it does in an editor, which
// types the Tab.
bool tabKeyReachesEveryControl(QQuickWindow *window, QStringList *problems = nullptr);
// How a control's background is drawn: its fill and its border.
QString backgroundLook(QQuickItem *control);
// A shortcut carries Ctrl, Alt or Super, or is a function key or Escape, so
// that every other key types. Shift alone is typing too.
bool isModifiedOrFunctionKey(const QKeySequence &sequence);
QList<QObject *> shortcutsIn(QQuickWindow *window);
// Every key sequence a Shortcut answers, from `sequence` and `sequences`.
QList<QKeySequence> shortcutSequences(const QObject *shortcut);
QObject *shortcutFor(QQuickWindow *window, const QKeySequence &sequence);
// Every animation the window holds, those inside Behaviors and Transitions too.
QList<QObject *> animationsIn(QQuickWindow *window);
// With reduced motion every animation the window holds takes no time:
// anything with a duration has 0, and an animation timed by its velocity or
// a spring cannot be made instant at all. `moving` names the rest.
bool allBehaviorDurationsAreZero(QQuickWindow *window, QStringList *moving = nullptr, int *animations = nullptr);
// Colours written into QML or JavaScript rather than taken from the theme:
// #hex, Qt.rgba() and its kin, and a quoted colour name other than
// "transparent". Each is reported with its file and line.
QStringList colourLiterals(const QString &source, const QString &file);

// The tooltip a control shows, if it has one.
QObject *toolTipOf(QQuickItem *control);
// A colour drawn over another, as the screen shows it.
QColor over(const QColor &top, const QColor &bottom);
// The WCAG contrast ratio of two opaque colours, from 1 to 21.
double contrastRatio(const QColor &a, const QColor &b);
// How far apart two colours are, summed over red, green and blue.
int colourDistance(const QColor &a, const QColor &b);

// A small window holding one QML item, for checking the checks themselves;
// null, with the error, when the QML does not load.
std::unique_ptr<QQuickWindow> sceneWindow(QQmlEngine &engine, const QByteArray &qml, QString *error);

// Stands in for Hyprland's two sockets, in a folder of its own, so that no
// test reaches the real compositor. The request socket answers
// "j/getoption <option>" with the JSON set for the option and closes, as
// Hyprland does; the event socket sends lines to whoever listens.
class FakeHyprland : public QObject {
public:
    explicit FakeHyprland(const QString &folder);
    QString folder() const { return m_folder; }
    // Answers {"option": <option>, "int": <value>, "set": true}.
    void setInt(const QString &option, int value);
    // Sends a line, such as "configreloaded>>", to every listener.
    void send(const QByteArray &line);
    // Closes every listener's connection, as Hyprland going away does.
    void dropListeners();
    int questions() const { return m_questions; }
    int listeners() const;
    // Bytes any listener has sent, which parch never does.
    qint64 heardFromListeners() const { return m_heard; }
    // Event bytes no listener has taken yet.
    qint64 untaken() const;

private:
    QString m_folder;
    QLocalServer m_requests, m_events;
    QHash<QString, QByteArray> m_answers;
    QList<QPointer<QLocalSocket>> m_listeners;
    int m_questions = 0;
    qint64 m_heard = 0;
};

// What a window test can set before the window opens: the folder of a
// FakeHyprland, the theme's shell.toml, the settings file, a folder of its
// own for the application's files, so that a second fixture can start where
// one ended, the files and folders the launch names, and the folder spelling
// finds its dictionary in, which is none unless one is given: never the
// system's.
struct ShellOptions {
    QString hyprland = {};
    QByteArray shell = {};
    QByteArray settings = {};
    QString root = {};
    QStringList files = {};
    QString dictionaries = {};
};

// The application as main() makes it: a theme on a temporary folder, a
// backend on temporary paths started as a launch with the options' files, and one Main.qml
// window over each of the backend's windows.
struct ShellFixture {
    QTemporaryDir root;
    QTemporaryDir themeFolder;
    std::unique_ptr<Theme> theme;
    std::unique_ptr<Backend> backend;
    QQmlEngine engine;
    // The first window.
    std::unique_ptr<QQuickWindow> window;
    // Every window after the first, by its context.
    std::map<WindowContext *, std::unique_ptr<QQuickWindow>> others;
    QString error;

    explicit ShellFixture(const ShellOptions &options = {});
    ~ShellFixture();

    bool ok() const;
    AppPaths paths() const { return AppPaths::under(m_root); }
    QQuickItem *item(const QString &name) const;
    QObject *object(const QString &name) const;
    Settings *settings() const { return backend->settings(); }
    Document *document() const { return backend->document(); }
    EditorBinding *binding() const;
    // The first window's context, and the window shown for a context.
    WindowContext *context() const;
    QQuickWindow *windowFor(WindowContext *context) const;

private:
    void open(WindowContext *context);
    void close(WindowContext *context);

    QString m_root;
    WindowContext *m_first = nullptr;
};
