#include "commandregistry.h"

#include "document.h"
#include "settings.h"
#include "tabmodel.h"
#include "windowcontext.h"

#include <QCoreApplication>
#include <QKeyCombination>
#include <QSet>

namespace {

QString tr(const char *text) { return QCoreApplication::translate("Commands", text); }

CommandInfo command(const char *id, const char *title, const char *category, QStringList keys = {},
                    CommandInfo::Needs needs = CommandInfo::Needs::Nothing)
{
    return {QString::fromLatin1(id), tr(title), tr(category), std::move(keys), needs};
}

// The commands that change a document's text.
CommandInfo editing(const char *id, const char *title, const char *category, QStringList keys = {})
{
    return command(id, title, category, std::move(keys), CommandInfo::Needs::EditableText);
}

} // namespace

// Every key of every command, one row each: a window makes a shortcut of
// each, so that each shortcut names its own key.
class KeyBindings : public QAbstractListModel {
public:
    // A row's command is CommandRegistry::IdRole, as in the registry.
    enum Role { SequenceRole = Qt::UserRole + 100 };

    explicit KeyBindings(QObject *parent) : QAbstractListModel(parent) {}

    void reload(const QList<std::pair<QString, QString>> &bindings)
    {
        if (bindings == m_bindings) return;
        beginResetModel();
        m_bindings = bindings;
        endResetModel();
    }
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(m_bindings.size()); }
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_bindings.size()) return {};
        if (role == CommandRegistry::IdRole) return m_bindings[index.row()].first;
        if (role == SequenceRole) return m_bindings[index.row()].second;
        return {};
    }
    QHash<int, QByteArray> roleNames() const override
    {
        return {{CommandRegistry::IdRole, "commandId"}, {SequenceRole, "keySequence"}};
    }

private:
    QList<std::pair<QString, QString>> m_bindings;
};

QList<CommandInfo> standardCommands()
{
    using Needs = CommandInfo::Needs;
    return {
        command("newDraft", "New draft", "File", {QStringLiteral("Ctrl+N")}),
        command("newWindow", "New window", "File", {QStringLiteral("Ctrl+Shift+N")}),
        command("newView", "New view", "File", {}, Needs::Document),
        command("open", "Open…", "File", {QStringLiteral("Ctrl+O")}),
        command("openFolder", "Open folder…", "File"),
        command("findDocument", "Find a document…", "File", {QStringLiteral("Ctrl+Shift+O")}),
        command("findText", "Find…", "Find", {QStringLiteral("Ctrl+F")}, Needs::Document),
        command("replace", "Find and replace…", "Find", {QStringLiteral("Ctrl+H")}, Needs::Document),
        command("findNext", "Find next", "Find", {QStringLiteral("F3")}, Needs::Document),
        command("findPrevious", "Find previous", "Find", {QStringLiteral("Shift+F3")}, Needs::Document),
        command("searchLibrary", "Search the library…", "Find", {QStringLiteral("Ctrl+Shift+F")}),
        command("goToHeading", "Go to heading…", "Find", {QStringLiteral("Ctrl+R")}, Needs::Document),
        command("save", "Save", "File", {QStringLiteral("Ctrl+S")}, Needs::Document),
        command("saveAs", "Save as…", "File", {QStringLiteral("Ctrl+Shift+S")}, Needs::Document),
        command("history", "Version history…", "File", {}, Needs::Document),
        command("checkLinks", "Check links…", "File", {}, Needs::Document),
        command("closeTab", "Close tab", "File", {QStringLiteral("Ctrl+W")}),
        command("quit", "Quit", "File", {QStringLiteral("Ctrl+Q")}),
        editing("undo", "Undo", "Edit", {QStringLiteral("Ctrl+Z")}),
        editing("redo", "Redo", "Edit", {QStringLiteral("Ctrl+Shift+Z"), QStringLiteral("Ctrl+Y")}),
        command("checkSpelling", "Check spelling", "Edit"),
        command("palette", "Command palette…", "View", {QStringLiteral("Ctrl+Shift+P")}),
        command("library", "Show or hide library", "View", {QStringLiteral("F9")}),
        command("outline", "Show or hide outline", "View", {QStringLiteral("Ctrl+Alt+O")}),
        command("toolbar", "Show or hide toolbar", "View"),
        // The three views of a document, and what goes with its preview.
        command("viewSource", "Source view", "View", {QStringLiteral("Ctrl+Alt+1")}, Needs::Document),
        command("viewSplit", "Split view", "View", {QStringLiteral("Ctrl+Alt+2")}, Needs::Document),
        command("viewReading", "Reading view", "View", {QStringLiteral("Ctrl+Alt+3")}, Needs::Document),
        command("togglePreview", "Show or hide preview", "View", {QStringLiteral("Ctrl+Alt+P")}, Needs::Document),
        command("otherPane", "Switch between text and preview", "View", {QStringLiteral("Ctrl+Alt+E")}, Needs::Document),
        command("scrollSync", "Scroll text and preview together", "View", {}, Needs::Document),
        command("previewBack", "Back in the preview", "View", {QStringLiteral("Alt+Left")}, Needs::Document),
        command("liveFormatting", "Live formatting", "View"),
        command("focusMode", "Focus mode", "View", {QStringLiteral("Ctrl+Shift+D")}),
        command("distractionFree", "Distraction-free writing", "View", {QStringLiteral("F11")}),
        command("nextTab", "Next tab", "View", {QStringLiteral("Ctrl+Tab"), QStringLiteral("Ctrl+PgDown")}),
        command("previousTab", "Previous tab", "View", {QStringLiteral("Ctrl+Shift+Tab"), QStringLiteral("Ctrl+PgUp")}),
        command("zoomIn", "Larger text", "View", {QStringLiteral("Ctrl+="), QStringLiteral("Ctrl++")}),
        command("zoomOut", "Smaller text", "View", {QStringLiteral("Ctrl+-")}),
        command("resetZoom", "Theme text size", "View", {QStringLiteral("Ctrl+0")}),
        command("menu", "Window menu", "View", {QStringLiteral("F10")}),
        command("nextRegion", "Next part of the window", "View", {QStringLiteral("F6")}),
        command("previousRegion", "Previous part of the window", "View", {QStringLiteral("Shift+F6")}),
        command("preferences", "Preferences…", "Settings", {QStringLiteral("Ctrl+,")}),
        command("documentSettings", "Document settings…", "Settings", {}, Needs::Document),
        command("shortcuts", "Keyboard shortcuts…", "Settings"),
        command("vimKeys", "Turn Vim keys on or off", "Settings"),
        editing("bold", "Bold", "Format", {QStringLiteral("Ctrl+B")}),
        editing("italic", "Italic", "Format", {QStringLiteral("Ctrl+I")}),
        editing("strikethrough", "Strikethrough", "Format", {QStringLiteral("Ctrl+Shift+X")}),
        editing("highlight", "Highlight", "Format", {QStringLiteral("Ctrl+Shift+H")}),
        editing("code", "Inline code", "Format", {QStringLiteral("Ctrl+E")}),
        editing("comment", "Comment", "Format", {QStringLiteral("Ctrl+/")}),
        editing("heading1", "Heading 1", "Format", {QStringLiteral("Ctrl+1")}),
        editing("heading2", "Heading 2", "Format", {QStringLiteral("Ctrl+2")}),
        editing("heading3", "Heading 3", "Format", {QStringLiteral("Ctrl+3")}),
        editing("heading4", "Heading 4", "Format", {QStringLiteral("Ctrl+4")}),
        editing("heading5", "Heading 5", "Format", {QStringLiteral("Ctrl+5")}),
        editing("heading6", "Heading 6", "Format", {QStringLiteral("Ctrl+6")}),
        editing("paragraph", "Paragraph", "Format"),
        editing("quote", "Quote", "Format", {QStringLiteral("Ctrl+'")}),
        editing("bulletList", "Bulleted list", "Format", {QStringLiteral("Ctrl+Shift+L")}),
        editing("numberedList", "Numbered list", "Format", {QStringLiteral("Ctrl+Alt+L")}),
        editing("taskList", "Task list", "Format"),
        editing("toggleTask", "Check or uncheck task", "Format", {QStringLiteral("Ctrl+L")}),
        editing("codeBlock", "Code block", "Format", {QStringLiteral("Ctrl+Shift+K")}),
        editing("formatTable", "Format table", "Format"),
        editing("link", "Link", "Insert", {QStringLiteral("Ctrl+K")}),
        editing("image", "Image", "Insert", {QStringLiteral("Ctrl+Shift+I")}),
        editing("footnote", "Footnote", "Insert", {QStringLiteral("Ctrl+Alt+F")}),
        editing("table", "Table", "Insert", {QStringLiteral("Ctrl+T")}),
        editing("inlineMath", "Inline math", "Insert", {QStringLiteral("Ctrl+M")}),
        editing("displayMath", "Display math", "Insert", {QStringLiteral("Ctrl+Shift+M")}),
        editing("thematicBreak", "Thematic break", "Insert"),
        editing("pageBreak", "Page break", "Insert", {QStringLiteral("Ctrl+Return")}),
    };
}

CommandRegistry::CommandRegistry(Settings *settings, QObject *parent)
    : QAbstractListModel(parent), m_settings(settings), m_bound(new KeyBindings(this))
{
}

CommandRegistry::~CommandRegistry() = default;

int CommandRegistry::indexOf(const QString &id) const
{
    for (int i = 0; i < m_commands.size(); ++i)
        if (m_commands[i].info.id == id) return i;
    return -1;
}

void CommandRegistry::add(const CommandInfo &info, Action action)
{
    if (info.id.isEmpty()) return;
    if (const int at = indexOf(info.id); at >= 0) {
        m_commands[at].info = info;
        if (action) m_commands[at].action = std::move(action);
    } else {
        beginInsertRows({}, int(m_commands.size()), int(m_commands.size()));
        m_commands.append({info, std::move(action), {}});
        endInsertRows();
    }
    refresh();
}

void CommandRegistry::setAction(const QString &id, Action action)
{
    if (const int at = indexOf(id); at >= 0) m_commands[at].action = std::move(action);
}

bool CommandRegistry::contains(const QString &id) const { return indexOf(id) >= 0; }

CommandInfo CommandRegistry::info(const QString &id) const
{
    const int at = indexOf(id);
    return at >= 0 ? m_commands[at].info : CommandInfo{};
}

QList<QKeySequence> CommandRegistry::shortcuts(const QString &id) const
{
    const int at = indexOf(id);
    return at >= 0 ? m_commands[at].shortcuts : QList<QKeySequence>{};
}

QVariantMap CommandRegistry::keys() const
{
    QVariantMap keys;
    for (const Command &command : m_commands)
        if (!command.shortcuts.isEmpty())
            keys.insert(command.info.id, command.shortcuts.first().toString(QKeySequence::NativeText));
    return keys;
}

QAbstractItemModel *CommandRegistry::bound() const { return m_bound; }

QString CommandRegistry::title(const QString &id) const { return info(id).title; }
QString CommandRegistry::category(const QString &id) const { return info(id).category; }

QString CommandRegistry::needs(const QString &id) const
{
    switch (info(id).needs) {
    case CommandInfo::Needs::Document:
        return QStringLiteral("document");
    case CommandInfo::Needs::EditableText:
        return QStringLiteral("text");
    case CommandInfo::Needs::Nothing:
        break;
    }
    return {};
}

QStringList CommandRegistry::ids() const
{
    QStringList ids;
    for (const Command &command : m_commands) ids.append(command.info.id);
    return ids;
}

bool CommandRegistry::canRun(const QString &id, WindowContext *window) const
{
    const int at = indexOf(id);
    if (at < 0) return false;
    const Document *document = window ? window->tabs()->activeDocument() : nullptr;
    switch (m_commands[at].info.needs) {
    case CommandInfo::Needs::Nothing:
        return true;
    case CommandInfo::Needs::Document:
        return document;
    case CommandInfo::Needs::EditableText:
        return document && document->state() != Document::State::ReadOnly && !document->isTextless();
    }
    return false;
}

bool CommandRegistry::run(const QString &id, WindowContext *window)
{
    const int at = indexOf(id);
    if (at < 0 || !m_commands[at].action) return false;
    if (canRun(id, window)) {
        // A copy, since the action may change the registry.
        const Action action = m_commands[at].action;
        action(window);
    }
    return true;
}

bool CommandRegistry::isTypingKey(const QKeySequence &sequence)
{
    for (int i = 0; i < sequence.count(); ++i) {
        const QKeyCombination combination = sequence[i];
        const Qt::Key key = combination.key();
        const bool modified = combination.keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        if (!modified && !(key >= Qt::Key_F1 && key <= Qt::Key_F35)) return true;
    }
    return false;
}

QString CommandRegistry::reservedFor(const QKeySequence &sequence) const
{
    // The keys the editor takes before any shortcut sees them, as Qt's text
    // controls do (QInputControl::isCommonTextEditShortcut), and the word
    // deletions, which edit text.
    static const QList<QKeySequence::StandardKey> editing{
        QKeySequence::Copy, QKeySequence::Cut, QKeySequence::Paste, QKeySequence::SelectAll,
        QKeySequence::MoveToNextWord, QKeySequence::MoveToPreviousWord, QKeySequence::MoveToStartOfDocument,
        QKeySequence::MoveToEndOfDocument, QKeySequence::SelectNextWord, QKeySequence::SelectPreviousWord,
        QKeySequence::SelectStartOfLine, QKeySequence::SelectEndOfLine, QKeySequence::SelectStartOfBlock,
        QKeySequence::SelectEndOfBlock, QKeySequence::SelectStartOfDocument, QKeySequence::SelectEndOfDocument,
        QKeySequence::DeleteStartOfWord, QKeySequence::DeleteEndOfWord};
    for (const QKeySequence::StandardKey key : editing)
        if (QKeySequence::keyBindings(key).contains(sequence)) return QStringLiteral("editing");
    if (QKeySequence::keyBindings(QKeySequence::Undo).contains(sequence)) return QStringLiteral("undo");
    if (QKeySequence::keyBindings(QKeySequence::Redo).contains(sequence)) return QStringLiteral("redo");
    if (sequence == QKeySequence(Qt::Key_Escape)) return QStringLiteral("escape");
    return {};
}

QVariantMap CommandRegistry::check(const QString &id, const QString &text) const
{
    const QKeySequence sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
    QVariantMap result{{QStringLiteral("allowed"), false},
                       {QStringLiteral("problem"), QString()},
                       {QStringLiteral("takenBy"), QString()},
                       {QStringLiteral("takenByTitle"), QString()},
                       {QStringLiteral("keys"), sequence.toString(QKeySequence::NativeText)}};
    const auto refuse = [&](const char *problem) {
        result[QStringLiteral("problem")] = QString::fromLatin1(problem);
        return result;
    };
    if (indexOf(id) < 0 || sequence.isEmpty() || sequence[0].key() == Qt::Key_unknown) return refuse("invalid");
    const QString reserved = reservedFor(sequence);
    if (reserved == u"escape") return refuse("reserved");
    if (isTypingKey(sequence)) return refuse("typing");
    if (!reserved.isEmpty() && reserved != id) return refuse("reserved");
    for (const Command &command : m_commands) {
        if (command.info.id == id || !command.shortcuts.contains(sequence)) continue;
        result[QStringLiteral("takenBy")] = command.info.id;
        result[QStringLiteral("takenByTitle")] = command.info.title;
        return refuse("taken");
    }
    result[QStringLiteral("allowed")] = true;
    return result;
}

void CommandRegistry::keep(const QString &id, const QList<QKeySequence> &sequences)
{
    const int at = indexOf(id);
    if (at < 0) return;
    QList<QKeySequence> defaults;
    for (const QString &key : m_commands[at].info.defaultKeys) defaults.append(QKeySequence::fromString(key, QKeySequence::PortableText));
    if (sequences == defaults) m_settings->clearShortcutOverride(id);
    else m_settings->setShortcutOverride(id, portable(sequences));
}

bool CommandRegistry::bind(const QString &id, const QString &text)
{
    if (!check(id, text).value(QStringLiteral("allowed")).toBool()) return false;
    keep(id, {QKeySequence::fromString(text, QKeySequence::PortableText)});
    refresh();
    return true;
}

bool CommandRegistry::swap(const QString &id, const QString &text)
{
    const QVariantMap checked = check(id, text);
    if (checked.value(QStringLiteral("allowed")).toBool()) return bind(id, text);
    if (checked.value(QStringLiteral("problem")).toString() != u"taken") return false;
    const QString other = checked.value(QStringLiteral("takenBy")).toString();
    const QKeySequence sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
    const QList<QKeySequence> mine = shortcuts(id);
    QList<QKeySequence> theirs = shortcuts(other);
    const qsizetype at = theirs.indexOf(sequence);
    // The other command takes this one's main key where the swapped key was,
    // unless it may not have it.
    if (!mine.isEmpty() && !theirs.contains(mine.first()) && reservedFor(mine.first()).isEmpty()) theirs[at] = mine.first();
    else theirs.removeAt(at);
    keep(other, theirs);
    keep(id, {sequence});
    refresh();
    return true;
}

void CommandRegistry::unbind(const QString &id)
{
    if (indexOf(id) < 0) return;
    keep(id, {});
    refresh();
}

void CommandRegistry::reset(const QString &id)
{
    const int at = indexOf(id);
    if (at < 0) return;
    // A command given one of these keys gives it back.
    QList<QKeySequence> defaults;
    for (const QString &key : m_commands[at].info.defaultKeys) defaults.append(QKeySequence::fromString(key, QKeySequence::PortableText));
    for (const Command &command : std::as_const(m_commands)) {
        if (command.info.id == id) continue;
        QList<QKeySequence> kept = command.shortcuts;
        if (kept.removeIf([&](const QKeySequence &sequence) { return defaults.contains(sequence); }) > 0)
            keep(command.info.id, kept);
    }
    m_settings->clearShortcutOverride(id);
    refresh();
}

void CommandRegistry::resetAll()
{
    m_settings->clearShortcutOverrides();
    refresh();
}

QString CommandRegistry::sequenceOf(int key, int modifiers) const
{
    switch (key) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_Hyper_L:
    case Qt::Key_Hyper_R:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_unknown:
    case 0:
        return {};
    default:
        break;
    }
    Qt::KeyboardModifiers held = Qt::KeyboardModifiers(modifiers)
        & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    // Shift+Tab arrives as Backtab, which Qt's shortcuts know as Shift+Tab.
    if (key == Qt::Key_Backtab) {
        key = Qt::Key_Tab;
        held |= Qt::ShiftModifier;
    }
    return QKeySequence(QKeyCombination(held, Qt::Key(key))).toString(QKeySequence::PortableText);
}

QStringList CommandRegistry::portable(const QList<QKeySequence> &sequences) const
{
    QStringList texts;
    for (const QKeySequence &sequence : sequences) texts.append(sequence.toString(QKeySequence::PortableText));
    return texts;
}

void CommandRegistry::refresh()
{
    const QHash<QString, QStringList> overrides = m_settings->shortcutOverrides();
    QList<QList<QKeySequence>> chosen(m_commands.size());
    QSet<QKeySequence> claimed;
    const auto usable = [&](const QString &id, const QKeySequence &sequence) {
        if (sequence.isEmpty() || isTypingKey(sequence) || claimed.contains(sequence)) return false;
        const QString reserved = reservedFor(sequence);
        return reserved.isEmpty() || reserved == id;
    };
    // The writer's own keys first, then the defaults of the other commands
    // where no command has taken them, each key once.
    for (int i = 0; i < m_commands.size(); ++i) {
        const QString &id = m_commands[i].info.id;
        if (!overrides.contains(id)) continue;
        for (const QString &text : overrides.value(id)) {
            const QKeySequence sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
            if (!usable(id, sequence)) continue;
            chosen[i].append(sequence);
            claimed.insert(sequence);
        }
    }
    for (int i = 0; i < m_commands.size(); ++i) {
        const QString &id = m_commands[i].info.id;
        if (overrides.contains(id)) continue;
        for (const QString &text : m_commands[i].info.defaultKeys) {
            const QKeySequence sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
            if (!usable(id, sequence)) continue;
            chosen[i].append(sequence);
            claimed.insert(sequence);
        }
    }
    bool changed = false;
    QList<std::pair<QString, QString>> bindings;
    for (int i = 0; i < m_commands.size(); ++i) {
        for (const QKeySequence &sequence : chosen[i])
            bindings.append({m_commands[i].info.id, sequence.toString(QKeySequence::PortableText)});
        // Whether the keys are the writer's own can change when they do not.
        emit dataChanged(index(i), index(i), {CustomizedRole});
        if (m_commands[i].shortcuts == chosen[i]) continue;
        m_commands[i].shortcuts = chosen[i];
        changed = true;
        emit dataChanged(index(i), index(i), {ShortcutsRole, KeysRole});
    }
    m_bound->reload(bindings);
    if (changed) emit shortcutsChanged();
}

int CommandRegistry::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_commands.size()); }

QVariant CommandRegistry::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_commands.size()) return {};
    const Command &command = m_commands[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return command.info.title;
    case IdRole:
        return command.info.id;
    case CategoryRole:
        return command.info.category;
    case ShortcutsRole:
        return portable(command.shortcuts);
    case KeysRole: {
        QStringList keys;
        for (const QKeySequence &sequence : command.shortcuts) keys.append(sequence.toString(QKeySequence::NativeText));
        return keys.join(QStringLiteral(", "));
    }
    case CustomizedRole:
        return m_settings->shortcutOverrides().contains(command.info.id);
    case NeedsRole:
        return needs(command.info.id);
    default:
        return {};
    }
}

QHash<int, QByteArray> CommandRegistry::roleNames() const { return commandRoles(); }

QHash<int, QByteArray> CommandRegistry::commandRoles()
{
    return {{IdRole, "commandId"}, {TitleRole, "title"},           {CategoryRole, "category"}, {ShortcutsRole, "shortcuts"},
            {KeysRole, "keyText"},    {CustomizedRole, "customized"}, {NeedsRole, "needs"}};
}
