#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class Theme;

// The writing settings, kept in AppPaths::settingsFile(). A missing file, one
// that cannot be read, or a value out of its range gives that value's
// default, so a damaged file never costs a window. Changing a setting rewrites
// the file atomically with every key it held, those this version does not
// know included.
//
// A file that cannot be read is never written over: the first change moves
// it aside as <name>.bad (or .bad.2 and on, beside earlier ones) and writes
// the settings anew.
//
// The editor's text size follows the shell's own text size, four pixels
// larger, until it is zoomed; a zoom keeps its size whatever the theme does,
// until the zoom is reset.
class Settings : public QObject {
    Q_OBJECT
    Q_PROPERTY(int fontPixelSize READ fontPixelSize NOTIFY changed)
    // True while the text size is a zoom rather than the theme's.
    Q_PROPERTY(bool zoomed READ zoomed NOTIFY changed)
    // The shell's text size, as followTheme() keeps it.
    Q_PROPERTY(int themeFontSize READ themeFontSize NOTIFY changed)
    Q_PROPERTY(qreal lineHeight READ lineHeight NOTIFY changed)
    Q_PROPERTY(int maxLineLength READ maxLineLength NOTIFY changed)
    Q_PROPERTY(QString editorFontFamily READ editorFontFamily NOTIFY changed)
    // True while the editor's family is the theme's, the default.
    Q_PROPERTY(bool themeFontFamily READ themeFontFamily NOTIFY changed)
    // The heading commands underline levels one and two (Setext) rather
    // than marking them with # (ATX), which they always do for three to six.
    Q_PROPERTY(bool setextHeadings READ setextHeadings NOTIFY changed)
    // Whether windows show the formatting toolbar; a narrow window still
    // hides it until asked.
    Q_PROPERTY(bool showToolbar READ showToolbar NOTIFY changed)
    // The ranges of the writing settings, for controls that set them:
    // minFontPixelSize, maxFontPixelSize, minLineHeight, maxLineHeight,
    // minLineLength and maxLineLength.
    Q_PROPERTY(QVariantMap limits READ limits CONSTANT)
    Q_PROPERTY(bool freshDraftOnLaunch READ freshDraftOnLaunch NOTIFY changed)
    Q_PROPERTY(int libraryWidth READ libraryWidth NOTIFY changed)
    // The folders the library lists, in the order they were added.
    Q_PROPERTY(QStringList libraryFolders READ libraryFolders NOTIFY changed)

public:
    static constexpr int MinFontPixelSize = 10, MaxFontPixelSize = 40;
    static constexpr int MinLineLength = 20, MaxLineLength = 200;
    static constexpr qreal MinLineHeight = 1.0, MaxLineHeight = 3.0;
    // How much larger the editor's text is than the shell's.
    static constexpr int ThemeFontOffset = 4;

    explicit Settings(const QString &file, QObject *parent = nullptr);

    int fontPixelSize() const;
    bool zoomed() const { return m_zoomedSize > 0; }
    int themeFontSize() const { return m_themeFontSize; }
    void setThemeFontSize(int size);
    // Takes the shell's text size from the theme now and whenever it changes,
    // so that the editor is laid out at the right size from the first.
    void followTheme(const Theme *theme);
    qreal lineHeight() const { return m_lineHeight; }
    int maxLineLength() const { return m_maxLineLength; }
    QString editorFontFamily() const { return m_editorFontFamily; }
    bool freshDraftOnLaunch() const { return m_freshDraftOnLaunch; }
    int libraryWidth() const { return m_libraryWidth; }
    QStringList libraryFolders() const { return m_libraryFolders; }
    // Keeps the library's folders: full paths only, each once, in order.
    void setLibraryFolders(const QStringList &folders);
    // The folder a file was last opened from, saved to or named into, where
    // the next save dialog starts; empty until there is one.
    QString lastFolder() const { return m_lastFolder; }
    // Keeps a full path; anything else is ignored.
    void setLastFolder(const QString &folder);

    // Makes the text `steps` pixels larger, or smaller for a negative number,
    // within MinFontPixelSize and MaxFontPixelSize, and keeps that size. A
    // step that changes nothing, at either limit, keeps nothing either.
    Q_INVOKABLE void zoom(int steps);
    // Returns the text size to following the theme.
    Q_INVOKABLE void resetZoom();
    // Zooms to a size, within the same limits.
    Q_INVOKABLE void setFontPixelSize(int size);

    bool themeFontFamily() const;
    QVariantMap limits() const;
    bool setextHeadings() const { return m_setextHeadings; }
    bool showToolbar() const { return m_showToolbar; }
    // Each setter keeps a value within its range, ignores one outside it,
    // and writes nothing when nothing changes. An empty family, or the
    // theme's own, returns the editor to the theme's family.
    Q_INVOKABLE void setEditorFontFamily(const QString &family);
    Q_INVOKABLE void setLineHeight(qreal lineHeight);
    Q_INVOKABLE void setMaxLineLength(int characters);
    Q_INVOKABLE void setSetextHeadings(bool setext);
    Q_INVOKABLE void setShowToolbar(bool show);

    // The writer's own key bindings, by command id, each replacing all of
    // that command's default keys; an empty list leaves the command without
    // keys. Kept as portable key sequence text, ids this version does not
    // know included.
    QHash<QString, QStringList> shortcutOverrides() const { return m_shortcutOverrides; }
    void setShortcutOverride(const QString &command, const QStringList &sequences);
    void clearShortcutOverride(const QString &command);
    void clearShortcutOverrides();

signals:
    void changed();

private:
    void load();
    void write();
    void writeShortcuts();

    QString m_file;
    // What the file held, kept whole and written back with each change.
    QJsonObject m_stored;
    // The file is there but could not be read, so it must not be written over.
    bool m_damaged = false;
    int m_themeFontSize = 12;
    // 0 while the size follows the theme.
    int m_zoomedSize = 0;
    qreal m_lineHeight = 1.5;
    int m_maxLineLength = 72;
    QString m_editorFontFamily = QStringLiteral("monospace");
    bool m_setextHeadings = false;
    bool m_showToolbar = true;
    QHash<QString, QStringList> m_shortcutOverrides;
    bool m_freshDraftOnLaunch = false;
    int m_libraryWidth = 260;
    QStringList m_libraryFolders;
    QString m_lastFolder;
};
