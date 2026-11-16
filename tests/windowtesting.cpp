#include "windowtesting.h"

#include "editorbinding.h"
#include "filerequests.h"
#include "settings.h"
#include "testfiles.h"
#include "windowcontext.h"

#include <QColor>
#include <QCoreApplication>
#include <QHash>
#include <QInputMethodEvent>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlListReference>
#include <QRegularExpression>
#include <QSet>
#include <QtTest>
#include <functional>
#include <linux/sockios.h>
#include <sys/ioctl.h>

FakeHyprland::FakeHyprland(const QString &folder) : m_folder(folder)
{
    QObject::connect(&m_requests, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_requests.nextPendingConnection()) {
            socket->setParent(this);
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket] {
                const QByteArray request = socket->readAll();
                ++m_questions;
                const QByteArray asked("j/getoption ");
                const QString option = QString::fromUtf8(request.startsWith(asked) ? request.mid(asked.size()) : request);
                socket->write(m_answers.value(option, QByteArrayLiteral("no such option")));
                socket->disconnectFromServer();
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    QObject::connect(&m_events, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_events.nextPendingConnection()) {
            socket->setParent(this);
            m_listeners.append(socket);
            QObject::connect(socket, &QLocalSocket::readyRead, socket,
                             [this, socket] { m_heard += socket->readAll().size(); });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    QVERIFY(m_requests.listen(folder + QStringLiteral("/.socket.sock")));
    QVERIFY(m_events.listen(folder + QStringLiteral("/.socket2.sock")));
}

void FakeHyprland::setInt(const QString &option, int value)
{
    m_answers.insert(option, QStringLiteral("{\"option\":\"%1\",\"int\":%2,\"set\":true}").arg(option).arg(value).toUtf8());
}

void FakeHyprland::send(const QByteArray &line)
{
    for (const QPointer<QLocalSocket> &socket : std::as_const(m_listeners))
        if (socket && socket->state() == QLocalSocket::ConnectedState) {
            socket->write(line + '\n');
            socket->flush();
        }
}

void FakeHyprland::dropListeners()
{
    for (const QPointer<QLocalSocket> &socket : std::as_const(m_listeners))
        if (socket) socket->disconnectFromServer();
}

qint64 FakeHyprland::untaken() const
{
    qint64 bytes = 0;
    for (const QPointer<QLocalSocket> &socket : std::as_const(m_listeners)) {
        if (!socket || socket->state() != QLocalSocket::ConnectedState) continue;
        int queued = 0;
        // What the listener's own side still holds, and what the kernel holds for it.
        bytes += socket->bytesToWrite();
        if (::ioctl(int(socket->socketDescriptor()), SIOCOUTQ, &queued) == 0) bytes += queued;
    }
    return bytes;
}

int FakeHyprland::listeners() const
{
    return int(std::count_if(m_listeners.cbegin(), m_listeners.cend(), [](const QPointer<QLocalSocket> &socket) {
        return socket && socket->state() == QLocalSocket::ConnectedState;
    }));
}

void activate(QQuickWindow *window)
{
    if (window->isActive()) return;
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));
}

void sendKey(QQuickWindow *window, QChar key)
{
    activate(window);
    if (key.unicode() >= 0x20 && key.unicode() < 0x7f) {
        QTest::keyClick(window, char(key.unicode()));
        return;
    }
    QKeyEvent event(QEvent::KeyPress, 0, Qt::NoModifier, QString(key));
    QCoreApplication::sendEvent(window->activeFocusItem(), &event);
}

void sendKey(QQuickWindow *window, Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    activate(window);
    QTest::keyClick(window, key, modifiers);
}

void sendPreedit(QQuickWindow *window, const QString &preedit)
{
    activate(window);
    QInputMethodEvent event(preedit, {});
    QCoreApplication::sendEvent(window->activeFocusItem(), &event);
}

QQuickItem *findItem(QQuickItem *item, const QString &name)
{
    for (QQuickItem *child : item->childItems()) {
        if (child->objectName() == name) return child;
        if (QQuickItem *found = findItem(child, name)) return found;
    }
    return nullptr;
}

// Everything a window holds: its own children, and the items it shows with
// what they own, since delegates and popup contents belong to items without
// being the window's children.
QList<QObject *> everyObjectIn(QQuickWindow *window)
{
    QList<QObject *> objects;
    QSet<QObject *> seen;
    const auto add = [&](QObject *object) {
        if (seen.contains(object)) return;
        seen.insert(object);
        objects.append(object);
    };
    for (QObject *child : window->findChildren<QObject *>()) add(child);
    std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        add(item);
        for (QObject *child : item->findChildren<QObject *>()) add(child);
        for (QQuickItem *child : item->childItems()) visit(child);
    };
    visit(window->contentItem());
    return objects;
}

QObject *findObject(QQuickWindow *window, const QString &name)
{
    for (QObject *object : everyObjectIn(window))
        if (object->objectName() == name) return object;
    return nullptr;
}

QString describe(const QObject *object)
{
    QString name = QString::fromLatin1(object->metaObject()->className());
    if (!object->objectName().isEmpty()) name += u' ' + object->objectName();
    const QVariant text = object->property("text");
    if (text.metaType().id() == QMetaType::QString && !text.toString().isEmpty())
        name += QStringLiteral(" \"%1\"").arg(text.toString().left(40));
    return name;
}

// The items a window shows, depth first: those visible and not faded out.
QList<QQuickItem *> visibleItems(QQuickWindow *window)
{
    QList<QQuickItem *> items;
    std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (!item->isVisible() || item->opacity() <= 0) return;
        items.append(item);
        for (QQuickItem *child : item->childItems()) visit(child);
    };
    visit(window->contentItem());
    return items;
}

// Nothing the window shows reaches past its edges, except inside something
// that clips it, and no text is drawn past its own box: a label that has too
// little room must elide or wrap rather than run into what is beside it.
bool everyVisibleItemFitsWithin(QQuickWindow *window, QStringList *problems)
{
    QStringList found;
    const QRectF bounds = QRectF(0, 0, window->width(), window->height()).adjusted(-0.5, -0.5, 0.5, 0.5);
    std::function<void(QQuickItem *, bool)> visit = [&](QQuickItem *item, bool clipped) {
        if (!item->isVisible() || item->opacity() <= 0) return;
        const QRectF box = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        if (!clipped && !box.isEmpty() && !bounds.contains(box))
            found << QStringLiteral("%1 at %2,%3 %4x%5").arg(describe(item)).arg(box.x()).arg(box.y()).arg(box.width()).arg(box.height());
        if (item->inherits("QQuickText")) {
            const qreal contentWidth = item->property("contentWidth").toReal();
            const qreal contentHeight = item->property("contentHeight").toReal();
            if (contentWidth > item->width() + 0.5 || contentHeight > item->height() + 0.5)
                found << QStringLiteral("%1 draws %2x%3 in %4x%5").arg(describe(item)).arg(contentWidth).arg(contentHeight)
                             .arg(item->width()).arg(item->height());
        }
        for (QQuickItem *child : item->childItems()) visit(child, clipped || item->clip());
    };
    visit(window->contentItem(), false);
    if (problems) *problems = found;
    return found.isEmpty();
}

// What a person operates, and so what the keyboard must reach.
bool isInteractiveRole(QAccessible::Role role)
{
    switch (role) {
    case QAccessible::PushButton:
    case QAccessible::CheckBox:
    case QAccessible::RadioButton:
    case QAccessible::ComboBox:
    case QAccessible::PageTab:
    case QAccessible::EditableText:
    case QAccessible::Slider:
    case QAccessible::SpinBox:
    case QAccessible::Link:
    case QAccessible::ButtonMenu:
    case QAccessible::ButtonDropDown:
    case QAccessible::MenuItem:
        return true;
    default:
        return false;
    }
}

QStringList unnamedAccessibleItems(QQuickWindow *window)
{
    // The tree assistive technology reads, which leaves out what is ignored.
    QStringList unnamed;
    std::function<void(QAccessibleInterface *, int)> visit = [&](QAccessibleInterface *access, int depth) {
        if (!access || depth > 64) return;
        auto *item = qobject_cast<QQuickItem *>(access->object());
        const QAccessible::Role role = access->role();
        if (item && item->isVisible() && !access->state().invisible && role != QAccessible::NoRole
            && role != QAccessible::Client && access->text(QAccessible::Name).trimmed().isEmpty())
            unnamed << QStringLiteral("%1 (role %2)").arg(describe(item)).arg(int(role));
        for (int i = 0; i < access->childCount(); ++i) visit(access->child(i), depth + 1);
    };
    visit(QAccessible::queryAccessibleInterface(window), 0);
    return unnamed;
}

// How a control's background is drawn: its fill and its border.
QString backgroundLook(QQuickItem *control)
{
    auto *background = control->property("background").value<QQuickItem *>();
    if (!background || !background->isVisible()) return {};
    QString look = background->property("color").value<QColor>().name(QColor::HexArgb);
    if (QObject *border = background->property("border").value<QObject *>())
        look += QStringLiteral(" %1 %2").arg(border->property("width").toReal())
                    .arg(border->property("color").value<QColor>().name(QColor::HexArgb));
    return look;
}

// Tab, pressed from the first control in the window, reaches every visible
// control that takes focus or that a person operates, and each control it
// reaches draws its background otherwise than without focus. The walk ends
// where focus comes round again or stays put, as it does in an editor, which
// types the Tab.
bool tabKeyReachesEveryControl(QQuickWindow *window, QStringList *problems)
{
    QStringList found;
    QList<QQuickItem *> controls;
    for (QQuickItem *item : visibleItems(window)) {
        if (!item->isEnabled() || item->width() <= 0 || item->height() <= 0) continue;
        QAccessibleInterface *access = QAccessible::queryAccessibleInterface(item);
        if (item->activeFocusOnTab() || (access && isInteractiveRole(access->role()))) controls << item;
    }
    QQuickItem *first = window->contentItem()->nextItemInFocusChain(true);
    if (controls.isEmpty() || !first) {
        found << QStringLiteral("no control takes focus from Tab");
    } else {
        window->contentItem()->forceActiveFocus();
        QHash<QQuickItem *, QString> unfocused;
        for (QQuickItem *control : std::as_const(controls)) unfocused.insert(control, backgroundLook(control));
        QList<QQuickItem *> reached;
        const auto look = [&](QQuickItem *item) {
            reached << item;
            // A control of Qt Quick Controls shows focus on its background; an
            // editor shows it with its caret.
            if (!item->property("visualFocus").isValid()) return;
            if (!item->property("visualFocus").toBool()) found << describe(item) + QStringLiteral(" takes focus unshown");
            const QString focused = backgroundLook(item);
            if (focused.isEmpty() || focused == unfocused.value(item)) found << describe(item) + QStringLiteral(" draws no focus");
        };
        first->forceActiveFocus(Qt::TabFocusReason);
        look(window->activeFocusItem());
        for (int i = 0; i <= controls.size(); ++i) {
            QTest::keyClick(window, Qt::Key_Tab);
            QQuickItem *now = window->activeFocusItem();
            if (!now || reached.contains(now)) break;
            look(now);
        }
        for (QQuickItem *control : std::as_const(controls))
            if (!reached.contains(control)) found << describe(control) + QStringLiteral(" is not reached by Tab");
    }
    if (problems) *problems = found;
    return found.isEmpty();
}

// A shortcut carries Ctrl, Alt or Super, or is a function key or Escape, so
// that every other key types. Shift alone is typing too.
bool isModifiedOrFunctionKey(const QKeySequence &sequence)
{
    if (sequence.isEmpty()) return false;
    for (int i = 0; i < sequence.count(); ++i) {
        const QKeyCombination combination = sequence[i];
        const Qt::Key key = combination.key();
        const bool modified = combination.keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        const bool function = key >= Qt::Key_F1 && key <= Qt::Key_F35;
        if (!modified && !function && key != Qt::Key_Escape) return false;
    }
    return true;
}

QList<QObject *> shortcutsIn(QQuickWindow *window)
{
    QList<QObject *> shortcuts;
    for (QObject *object : everyObjectIn(window))
        if (object->inherits("QQuickShortcut")) shortcuts << object;
    return shortcuts;
}

// Every key sequence a Shortcut answers, from `sequence` and `sequences`.
QList<QKeySequence> shortcutSequences(const QObject *shortcut)
{
    QList<QKeySequence> sequences;
    const auto take = [&](const QVariant &value) {
        if (!value.isValid()) return;
        if (value.metaType().id() == QMetaType::QString) {
            if (!value.toString().isEmpty())
                sequences << QKeySequence::fromString(value.toString(), QKeySequence::PortableText);
        } else if (value.metaType().id() == QMetaType::Int) {
            sequences += QKeySequence::keyBindings(QKeySequence::StandardKey(value.toInt()));
        } else if (value.canConvert<QKeySequence>()) {
            sequences << value.value<QKeySequence>();
        }
    };
    take(shortcut->property("sequence"));
    for (const QVariant &value : shortcut->property("sequences").toList()) take(value);
    return sequences;
}

QObject *shortcutFor(QQuickWindow *window, const QKeySequence &sequence)
{
    for (QObject *shortcut : shortcutsIn(window))
        if (shortcutSequences(shortcut).contains(sequence)) return shortcut;
    return nullptr;
}

namespace {
bool isMotion(const QObject *object)
{
    return object->inherits("QQuickAbstractAnimation") || object->inherits("QQuickTransition")
           || object->inherits("QQuickBehavior");
}
} // namespace

// Every animation the window holds. A Behavior, a Transition or a group holds
// its animations in properties rather than as children, so the search
// follows every property, single or list, that holds motion. A Behavior and a
// popup make theirs only when first needed, so they are made here first.
QList<QObject *> animationsIn(QQuickWindow *window)
{
    QList<QObject *> queue = everyObjectIn(window);
    QSet<QObject *> seen(queue.begin(), queue.end());
    QList<QObject *> animations;
    const auto take = [&](QObject *object) {
        if (!object || seen.contains(object) || !isMotion(object)) return;
        seen.insert(object);
        queue.append(object);
    };
    for (qsizetype i = 0; i < queue.size(); ++i) {
        QObject *object = queue.at(i);
        qmlExecuteDeferred(object);
        if (object->inherits("QQuickAbstractAnimation")) animations.append(object);
        const QMetaObject *meta = object->metaObject();
        for (int p = 0; p < meta->propertyCount(); ++p) {
            const QMetaProperty property = meta->property(p);
            const QQmlListReference list(object, property.name());
            if (list.isValid()) {
                for (qsizetype e = 0; e < list.count(); ++e) take(list.at(e));
            } else if (property.metaType().flags() & QMetaType::PointerToQObject) {
                take(property.read(object).value<QObject *>());
            }
        }
    }
    return animations;
}

// With reduced motion every animation the window holds takes no time:
// anything with a duration has 0, and an animation timed by its velocity or
// a spring cannot be made instant at all. `moving` names the rest.
bool allBehaviorDurationsAreZero(QQuickWindow *window, QStringList *moving, int *animations)
{
    QStringList found;
    int count = 0;
    for (QObject *object : animationsIn(window)) {
        ++count;
        if (object->inherits("QQuickSpringAnimation"))
            found << describe(object) + QStringLiteral(" springs");
        else if (object->metaObject()->indexOfProperty("duration") >= 0 && object->property("duration").toInt() != 0)
            found << QStringLiteral("%1 takes %2 ms").arg(describe(object)).arg(object->property("duration").toInt());
    }
    if (moving) *moving = found;
    if (animations) *animations = count;
    return found.isEmpty();
}

// Colours written into QML or JavaScript rather than taken from the theme:
// #hex, Qt.rgba() and its kin, and a quoted colour name other than
// "transparent". Each is reported with its file and line.
QStringList colourLiterals(const QString &source, const QString &file)
{
    static const QRegularExpression hex(QStringLiteral("#[0-9A-Fa-f]{3,8}\\b"));
    static const QRegularExpression constructor(QStringLiteral("\\bQt\\.(rgba|hsla|hsva|rgb)\\s*\\("));
    static const QRegularExpression quoted(QStringLiteral("([\"'])([A-Za-z]+)\\1"));
    QStringList found;
    const QStringList lines = source.split(u'\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        bool literal = line.contains(hex) || line.contains(constructor);
        for (auto match = quoted.globalMatch(line); match.hasNext();) {
            const QString word = match.next().captured(2);
            if (word.compare(u"transparent", Qt::CaseInsensitive) != 0 && QColor::isValidColorName(word)) literal = true;
        }
        if (literal) found << QStringLiteral("%1:%2: %3").arg(file).arg(i + 1).arg(line.trimmed());
    }
    return found;
}

// Gives the window a new width and waits until its content has taken it,
// since a resize reaches the window's items only through the event loop.
void resizeWindow(QQuickWindow *window, int width)
{
    window->setWidth(width);
    QTRY_COMPARE(window->contentItem()->width(), qreal(width));
}

void typeText(QQuickWindow *window, const QString &text)
{
    for (const QChar c : text) sendKey(window, c);
}

void selectRange(QQuickItem *editor, int start, int end)
{
    QMetaObject::invokeMethod(editor, "select", Q_ARG(int, start), Q_ARG(int, end));
}

QPoint centreOf(QQuickItem *item)
{
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

QObject *toolTipOf(QQuickItem *control)
{
    for (QObject *child : control->children())
        if (child->inherits("QQuickToolTip")) return child;
    return nullptr;
}

QColor over(const QColor &top, const QColor &bottom)
{
    const double a = top.alphaF();
    return QColor::fromRgbF(float(a * top.redF() + (1 - a) * bottom.redF()), float(a * top.greenF() + (1 - a) * bottom.greenF()),
                            float(a * top.blueF() + (1 - a) * bottom.blueF()));
}

double contrastRatio(const QColor &a, const QColor &b)
{
    const double first = Theme::luminance(a), second = Theme::luminance(b);
    return (qMax(first, second) + 0.05) / (qMin(first, second) + 0.05);
}

int colourDistance(const QColor &a, const QColor &b)
{
    return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue());
}

std::unique_ptr<QQuickWindow> sceneWindow(QQmlEngine &engine, const QByteArray &qml, QString *error)
{
    auto window = std::make_unique<QQuickWindow>();
    window->resize(200, 100);
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nimport QtQuick.Controls\n" + qml, QUrl());
    auto *item = qobject_cast<QQuickItem *>(component.create());
    if (!item) {
        *error = component.errorString();
        return nullptr;
    }
    item->setParent(window.get());
    item->setParentItem(window->contentItem());
    window->show();
    QCoreApplication::processEvents();
    return window;
}

ShellFixture::ShellFixture(const ShellOptions &options) : m_root(options.root.isEmpty() ? root.path() : options.root)
{
    writeTheme(themeFolder.path(), "light", "#F8F1E3", "#342B27", "#A7353A");
    if (!options.shell.isEmpty()) writeFile(themeFolder.filePath("theme/shell.toml"), options.shell);
    if (!options.settings.isEmpty()) writeFile(paths().settingsFile(), options.settings);
    theme = std::make_unique<Theme>(themeFolder.path(), nullptr, options.hyprland);
    backend = std::make_unique<Backend>(paths());
    // A save dialog with no folder used last starts here, never in the real Documents or home.
    backend->fileRequests()->setFallbackFolders({m_root});
    backend->settings()->followTheme(theme.get());
    backend->spelling()->setDictionaryFolders(
        {options.dictionaries.isEmpty() ? m_root + QStringLiteral("/no-dictionaries") : options.dictionaries});
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), theme.get());
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), backend.get());
    engine.rootContext()->setContextProperty(QStringLiteral("settings"), backend->settings());
    QObject::connect(backend.get(), &Backend::windowAdded, backend.get(), [this](WindowContext *context) { open(context); });
    QObject::connect(backend.get(), &Backend::windowRemoved, backend.get(), [this](WindowContext *context) { close(context); });
    backend->start(options.files);
    QCoreApplication::processEvents();
}

ShellFixture::~ShellFixture()
{
    // Windows go before the engine and the backend they show.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    others.clear();
    window.reset();
}

void ShellFixture::open(WindowContext *context)
{
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/Main.qml")));
    QObject *made = component.createWithInitialProperties({{QStringLiteral("context"), QVariant::fromValue(context)}});
    auto *shown = qobject_cast<QQuickWindow *>(made);
    if (!shown) {
        delete made;
        error = component.errorString();
        return;
    }
    shown->show();
    if (!window) {
        window.reset(shown);
        m_first = context;
    } else {
        others[context].reset(shown);
    }
}

void ShellFixture::close(WindowContext *context)
{
    // The window may be in the middle of handling its own close.
    if (context == m_first) {
        if (window) window.release()->deleteLater();
        m_first = nullptr;
    } else if (auto found = others.find(context); found != others.end()) {
        found->second.release()->deleteLater();
        others.erase(found);
    }
}

WindowContext *ShellFixture::context() const { return m_first; }

QQuickWindow *ShellFixture::windowFor(WindowContext *context) const
{
    if (context && context == m_first) return window.get();
    const auto found = others.find(context);
    return found == others.end() ? nullptr : found->second.get();
}

bool ShellFixture::ok() const
{
    return window && item(QStringLiteral("editor")) && item(QStringLiteral("editorPane"));
}

QQuickItem *ShellFixture::item(const QString &name) const { return findItem(window->contentItem(), name); }

QObject *ShellFixture::object(const QString &name) const { return findObject(window.get(), name); }

EditorBinding *ShellFixture::binding() const
{
    for (QObject *object : everyObjectIn(window.get()))
        if (auto *binding = qobject_cast<EditorBinding *>(object)) return binding;
    return nullptr;
}
