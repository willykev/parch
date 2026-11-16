#include "settings.h"

#include "diskfile.h"
#include "pathtext.h"
#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStringList>
#include <QtGlobal>

namespace {
const auto fontPixelSizeKey = QStringLiteral("fontPixelSize");
const auto lineHeightKey = QStringLiteral("lineHeight");
const auto maxLineLengthKey = QStringLiteral("maxLineLength");
const auto editorFontFamilyKey = QStringLiteral("editorFontFamily");
const auto freshDraftOnLaunchKey = QStringLiteral("freshDraftOnLaunch");
const auto libraryWidthKey = QStringLiteral("libraryWidth");
const auto libraryFoldersKey = QStringLiteral("libraryFolders");
const auto lastFolderKey = QStringLiteral("lastFolder");
const auto headingSyntaxKey = QStringLiteral("headingSyntax");
const auto toolbarKey = QStringLiteral("toolbar");
const auto shortcutsKey = QStringLiteral("shortcuts");
const auto vimKeysKey = QStringLiteral("vimKeys");
// The family the editor has until another is chosen: the alias the theme's
// font is set through.
const auto themeFamily = QStringLiteral("monospace");
const auto outlineShownKey = QStringLiteral("outlineShown");
const auto spellcheckKey = QStringLiteral("spellcheck");
const auto focusUnitKey = QStringLiteral("focusUnit");

bool isFocusUnit(const QString &unit) { return unit == u"sentence" || unit == u"paragraph"; }
// More folders than anyone lists, so that a damaged file costs little.
constexpr qsizetype maxLibraryFolders = 1000;

// Full paths only, each once, in order.
QStringList usableFolders(const QStringList &folders)
{
    QStringList usable;
    for (const QString &folder : folders)
        if (PathText::isFullPath(folder) && !usable.contains(folder) && usable.size() < maxLibraryFolders)
            usable.append(folder);
    return usable;
}

// A whole number within its range, as JSON writes one.
bool wholeIn(const QJsonValue &value, int low, int high, int *out)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (number != qRound64(number) || number < low || number > high) return false;
    *out = int(number);
    return true;
}
} // namespace

Settings::Settings(const QString &file, QObject *parent) : QObject(parent), m_file(file) { load(); }

int Settings::fontPixelSize() const
{
    return zoomed() ? m_zoomedSize : qBound(MinFontPixelSize, m_themeFontSize + ThemeFontOffset, MaxFontPixelSize);
}

void Settings::setThemeFontSize(int size)
{
    if (size <= 0 || size == m_themeFontSize) return;
    m_themeFontSize = size;
    emit changed();
}

void Settings::followTheme(const Theme *theme)
{
    setThemeFontSize(theme->fontSize());
    connect(theme, &Theme::changed, this, [this, theme] { setThemeFontSize(theme->fontSize()); });
}

void Settings::zoom(int steps)
{
    const int size = qBound(MinFontPixelSize, fontPixelSize() + steps, MaxFontPixelSize);
    // A zoom past either limit changes nothing, so nothing is written, and
    // a size that follows the theme goes on following it.
    if (size == fontPixelSize()) return;
    m_zoomedSize = size;
    m_stored.insert(fontPixelSizeKey, m_zoomedSize);
    write();
    emit changed();
}

void Settings::resetZoom()
{
    if (!zoomed()) return;
    m_zoomedSize = 0;
    m_stored.remove(fontPixelSizeKey);
    write();
    emit changed();
}

void Settings::setFontPixelSize(int size)
{
    zoom(qBound(MinFontPixelSize, size, MaxFontPixelSize) - fontPixelSize());
}

bool Settings::themeFontFamily() const { return !m_stored.contains(editorFontFamilyKey); }

QVariantMap Settings::limits() const
{
    return {{QStringLiteral("minFontPixelSize"), MinFontPixelSize}, {QStringLiteral("maxFontPixelSize"), MaxFontPixelSize},
            {QStringLiteral("minLineHeight"), MinLineHeight},       {QStringLiteral("maxLineHeight"), MaxLineHeight},
            {QStringLiteral("minLineLength"), MinLineLength},       {QStringLiteral("maxLineLength"), MaxLineLength}};
}

void Settings::setEditorFontFamily(const QString &family)
{
    const QString chosen = family.trimmed();
    const bool theme = chosen.isEmpty() || chosen == themeFamily;
    if (theme == themeFontFamily() && (theme || chosen == m_editorFontFamily)) return;
    m_editorFontFamily = theme ? themeFamily : chosen;
    if (theme) m_stored.remove(editorFontFamilyKey);
    else m_stored.insert(editorFontFamilyKey, chosen);
    write();
    emit changed();
}

void Settings::setLineHeight(qreal lineHeight)
{
    if (!(lineHeight >= MinLineHeight && lineHeight <= MaxLineHeight) || qFuzzyCompare(lineHeight, m_lineHeight)) return;
    m_lineHeight = lineHeight;
    m_stored.insert(lineHeightKey, lineHeight);
    write();
    emit changed();
}

void Settings::setMaxLineLength(int characters)
{
    if (characters < MinLineLength || characters > MaxLineLength || characters == m_maxLineLength) return;
    m_maxLineLength = characters;
    m_stored.insert(maxLineLengthKey, characters);
    write();
    emit changed();
}

void Settings::setSetextHeadings(bool setext)
{
    if (setext == m_setextHeadings) return;
    m_setextHeadings = setext;
    m_stored.insert(headingSyntaxKey, setext ? QStringLiteral("setext") : QStringLiteral("atx"));
    write();
    emit changed();
}

void Settings::setShowToolbar(bool show)
{
    if (show == m_showToolbar) return;
    m_showToolbar = show;
    m_stored.insert(toolbarKey, show);
    write();
    emit changed();
}

void Settings::setVimKeys(bool on)
{
    if (on == m_vimKeys) return;
    m_vimKeys = on;
    m_stored.insert(vimKeysKey, on);
    write();
    emit changed();
}

void Settings::setShortcutOverride(const QString &command, const QStringList &sequences)
{
    if (command.isEmpty() || (m_shortcutOverrides.contains(command) && m_shortcutOverrides.value(command) == sequences))
        return;
    m_shortcutOverrides.insert(command, sequences);
    writeShortcuts();
}

void Settings::clearShortcutOverride(const QString &command)
{
    if (m_shortcutOverrides.remove(command) > 0) writeShortcuts();
}

void Settings::clearShortcutOverrides()
{
    if (m_shortcutOverrides.isEmpty()) return;
    m_shortcutOverrides.clear();
    writeShortcuts();
}

void Settings::writeShortcuts()
{
    // Ids this version does not know were loaded with the rest, so they stay.
    QJsonObject stored;
    for (auto it = m_shortcutOverrides.cbegin(); it != m_shortcutOverrides.cend(); ++it)
        stored.insert(it.key(), QJsonArray::fromStringList(it.value()));
    if (stored.isEmpty()) m_stored.remove(shortcutsKey);
    else m_stored.insert(shortcutsKey, stored);
    write();
    emit changed();
}

void Settings::setLibraryFolders(const QStringList &folders)
{
    const QStringList usable = usableFolders(folders);
    if (usable == m_libraryFolders) return;
    m_libraryFolders = usable;
    if (usable.isEmpty()) m_stored.remove(libraryFoldersKey);
    else m_stored.insert(libraryFoldersKey, QJsonArray::fromStringList(usable));
    write();
    emit changed();
}

void Settings::setLastFolder(const QString &folder)
{
    if (!PathText::isFullPath(folder) || folder == m_lastFolder) return;
    m_lastFolder = folder;
    m_stored.insert(lastFolderKey, folder);
    write();
    emit changed();
}

void Settings::setOutlineShown(bool shown)
{
    if (shown == m_outlineShown) return;
    m_outlineShown = shown;
    m_stored.insert(outlineShownKey, shown);
    write();
    emit changed();
}

void Settings::setSpellcheck(bool on)
{
    if (m_spellcheck == on) return;
    m_spellcheck = on;
    m_stored.insert(spellcheckKey, on);
    write();
    emit changed();
}

void Settings::setFocusUnit(const QString &unit)
{
    if (!isFocusUnit(unit) || unit == m_focusUnit) return;
    m_focusUnit = unit;
    m_stored.insert(focusUnitKey, unit);
    write();
    emit changed();
}

void Settings::load()
{
    QFile file(m_file);
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("parch: the settings in %s cannot be read (%s); using the defaults", qPrintable(m_file),
                 qPrintable(file.errorString()));
        m_damaged = true;
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning("parch: the settings in %s cannot be read (%s); using the defaults", qPrintable(m_file),
                 qPrintable(error.error != QJsonParseError::NoError ? error.errorString()
                                                                     : QStringLiteral("not an object")));
        m_damaged = true;
        return;
    }
    m_stored = document.object();

    // Each value is taken on its own, so one bad value costs only itself.
    QStringList ignored;
    const auto present = [this](const QString &key) { return m_stored.contains(key); };
    int whole = 0;
    if (present(fontPixelSizeKey)) {
        if (wholeIn(m_stored.value(fontPixelSizeKey), MinFontPixelSize, MaxFontPixelSize, &whole))
            m_zoomedSize = whole;
        else
            ignored << fontPixelSizeKey;
    }
    if (present(lineHeightKey)) {
        const QJsonValue value = m_stored.value(lineHeightKey);
        if (value.isDouble() && value.toDouble() >= 1.0 && value.toDouble() <= 3.0)
            m_lineHeight = value.toDouble();
        else
            ignored << lineHeightKey;
    }
    if (present(maxLineLengthKey)) {
        if (wholeIn(m_stored.value(maxLineLengthKey), 20, 200, &whole))
            m_maxLineLength = whole;
        else
            ignored << maxLineLengthKey;
    }
    if (present(editorFontFamilyKey)) {
        const QString family = m_stored.value(editorFontFamilyKey).toString().trimmed();
        if (!family.isEmpty())
            m_editorFontFamily = family;
        else
            ignored << editorFontFamilyKey;
    }
    if (present(headingSyntaxKey)) {
        const QString syntax = m_stored.value(headingSyntaxKey).toString();
        if (syntax == u"atx" || syntax == u"setext")
            m_setextHeadings = syntax == u"setext";
        else
            ignored << headingSyntaxKey;
    }
    if (present(toolbarKey)) {
        if (m_stored.value(toolbarKey).isBool())
            m_showToolbar = m_stored.value(toolbarKey).toBool();
        else
            ignored << toolbarKey;
    }
    if (present(vimKeysKey)) {
        if (m_stored.value(vimKeysKey).isBool())
            m_vimKeys = m_stored.value(vimKeysKey).toBool();
        else
            ignored << vimKeysKey;
    }
    if (present(shortcutsKey)) {
        // Each command's keys on their own: a list of strings, or nothing of it.
        const QJsonObject shortcuts = m_stored.value(shortcutsKey).toObject();
        bool dropped = !m_stored.value(shortcutsKey).isObject();
        for (auto it = shortcuts.constBegin(); it != shortcuts.constEnd(); ++it) {
            const QJsonArray list = it.value().toArray();
            QStringList sequences;
            for (const QJsonValue &sequence : list)
                if (sequence.isString()) sequences.append(sequence.toString());
            if (!it.value().isArray() || sequences.size() != list.size()) {
                dropped = true;
                continue;
            }
            m_shortcutOverrides.insert(it.key(), sequences);
        }
        if (dropped) ignored << shortcutsKey;
    }
    if (present(freshDraftOnLaunchKey)) {
        if (m_stored.value(freshDraftOnLaunchKey).isBool())
            m_freshDraftOnLaunch = m_stored.value(freshDraftOnLaunchKey).toBool();
        else
            ignored << freshDraftOnLaunchKey;
    }
    if (present(libraryWidthKey)) {
        if (wholeIn(m_stored.value(libraryWidthKey), 160, 640, &whole))
            m_libraryWidth = whole;
        else
            ignored << libraryWidthKey;
    }
    if (present(libraryFoldersKey)) {
        const QJsonValue value = m_stored.value(libraryFoldersKey);
        QStringList folders;
        for (const QJsonValue &folder : value.toArray()) folders.append(folder.toString());
        m_libraryFolders = usableFolders(folders);
        // A folder left out is said, but the others are kept.
        if (!value.isArray() || m_libraryFolders.size() != value.toArray().size()) ignored << libraryFoldersKey;
    }
    if (present(lastFolderKey)) {
        const QString folder = m_stored.value(lastFolderKey).toString();
        if (PathText::isFullPath(folder))
            m_lastFolder = folder;
        else
            ignored << lastFolderKey;
    }
    if (present(outlineShownKey)) {
        if (m_stored.value(outlineShownKey).isBool())
            m_outlineShown = m_stored.value(outlineShownKey).toBool();
        else
            ignored << outlineShownKey;
    }
    if (present(spellcheckKey)) {
        if (m_stored.value(spellcheckKey).isBool())
            m_spellcheck = m_stored.value(spellcheckKey).toBool();
        else
            ignored << spellcheckKey;
    }
    if (present(focusUnitKey)) {
        if (isFocusUnit(m_stored.value(focusUnitKey).toString()))
            m_focusUnit = m_stored.value(focusUnitKey).toString();
        else
            ignored << focusUnitKey;
    }
    if (!ignored.isEmpty())
        qWarning("parch: ignoring settings out of range in %s: %s", qPrintable(m_file), qPrintable(ignored.join(", ")));
}

void Settings::write()
{
    // A setting that cannot be kept still applies for as long as parch runs.
    QString error;
    if (m_damaged) {
        if (DiskFile::moveAside(m_file))
            m_damaged = false;
        else
            error = QStringLiteral("the unreadable file cannot be moved aside");
    }
    if (error.isEmpty() && !QDir().mkpath(QFileInfo(m_file).absolutePath())) {
        error = QStringLiteral("cannot make its folder");
    } else if (error.isEmpty()) {
        QSaveFile file(m_file);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(m_stored).toJson()) < 0 || !file.commit())
            error = file.errorString();
    }
    if (!error.isEmpty())
        qWarning("parch: the settings could not be saved to %s (%s)", qPrintable(m_file), qPrintable(error));
}
