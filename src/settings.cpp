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
