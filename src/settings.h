#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <optional>

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
    // The editor takes Vim keys: Normal, Insert and Visual modes and a
    // command line. Off until chosen.
    Q_PROPERTY(bool vimKeys READ vimKeys NOTIFY changed)
    // The ranges of the writing settings, for controls that set them:
    // minFontPixelSize, maxFontPixelSize, minLineHeight, maxLineHeight,
    // minLineLength, maxLineLength, minLibraryWidth, maxLibraryWidth and
    // the history limits' ranges.
    Q_PROPERTY(QVariantMap limits READ limits CONSTANT)
    Q_PROPERTY(bool freshDraftOnLaunch READ freshDraftOnLaunch NOTIFY changed)
    Q_PROPERTY(int libraryWidth READ libraryWidth NOTIFY changed)
    // The folders the library lists, in the order they were added.
    Q_PROPERTY(QStringList libraryFolders READ libraryFolders NOTIFY changed)
    // The library shows the outline of the document in front beneath its folders.
    Q_PROPERTY(bool outlineShown READ outlineShown WRITE setOutlineShown NOTIFY changed)
    // The preview: its reading face, empty for the default one; its text
    // size, which is the editor's and two pixels until one is chosen; its
    // line height and the most characters its lines hold.
    Q_PROPERTY(QString previewFontFamily READ previewFontFamily NOTIFY changed)
    Q_PROPERTY(int previewFontPixelSize READ previewFontPixelSize NOTIFY changed)
    Q_PROPERTY(bool previewSizeChosen READ previewSizeChosen NOTIFY changed)
    Q_PROPERTY(qreal previewLineHeight READ previewLineHeight NOTIFY changed)
    Q_PROPERTY(int previewLineLength READ previewLineLength NOTIFY changed)
    // The editor and the preview beside it scroll together.
    Q_PROPERTY(bool scrollSync READ scrollSync WRITE setScrollSync NOTIFY changed)
    // The preview fetches the pictures a document names by web address.
    Q_PROPERTY(bool remoteImages READ remoteImages NOTIFY changed)
    // What focus mode keeps bright: "sentence" or "paragraph".
    Q_PROPERTY(QString focusUnit READ focusUnit NOTIFY changed)
    // How strongly focus mode dims the rest: "off", "light" or "strong".
    Q_PROPERTY(QString focusDimming READ focusDimming NOTIFY changed)
    // Whether the editor styles its Markdown as it is written; on by default.
    Q_PROPERTY(bool liveFormatting READ liveFormatting NOTIFY changed)
    // How much history keeps: at most historyVersions unnamed versions of
    // each document, none older than historyDays, and every document's
    // versions together within historyGigabytes (GiB, in tenths). Versions
    // the writer named are never removed.
    Q_PROPERTY(int historyVersions READ historyVersions NOTIFY changed)
    Q_PROPERTY(int historyDays READ historyDays NOTIFY changed)
    Q_PROPERTY(qreal historyGigabytes READ historyGigabytes NOTIFY changed)

public:
    static constexpr int MinFontPixelSize = 10, MaxFontPixelSize = 40;
    static constexpr int MinLineLength = 20, MaxLineLength = 200;
    static constexpr qreal MinLineHeight = 1.0, MaxLineHeight = 3.0;
    // How much larger the editor's text is than the shell's.
    static constexpr int ThemeFontOffset = 4;
    // The library's width, in pixels.
    static constexpr int MinLibraryWidth = 160, MaxLibraryWidth = 640;
    static constexpr int MinHistoryVersions = 1, MaxHistoryVersions = 1000;
    static constexpr int MinHistoryDays = 1, MaxHistoryDays = 3650;
    static constexpr qreal MinHistoryGigabytes = 0.1, MaxHistoryGigabytes = 100.0;
    // How much larger the preview's text is than the editor's.
    static constexpr int PreviewFontOffset = 2;

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
    // A launch that reopens the session also opens a new draft in front.
    Q_INVOKABLE void setFreshDraftOnLaunch(bool fresh);
    // The library's width a new window starts with: each window has its own
    // from then on, and the width chosen last in any window is kept here.
    int libraryWidth() const { return m_libraryWidth; }
    // Keeps a width within its range; ignores one outside it.
    Q_INVOKABLE void setLibraryWidth(int width);
    QStringList libraryFolders() const { return m_libraryFolders; }
    // Keeps the library's folders: full paths only, each once, in order.
    void setLibraryFolders(const QStringList &folders);
    // The folder a file was last opened from, saved to or named into, where
    // the next save dialog starts; empty until there is one.
    QString lastFolder() const { return m_lastFolder; }
    // Keeps a full path; anything else is ignored.
    void setLastFolder(const QString &folder);
    bool outlineShown() const { return m_outlineShown; }
    void setOutlineShown(bool shown);
    // Whether spelling is checked, once the writer has chosen; until then
    // nothing, and it is checked when a dictionary is found.
    std::optional<bool> spellcheck() const { return m_spellcheck; }
    Q_INVOKABLE void setSpellcheck(bool on);
    QString focusUnit() const { return m_focusUnit; }
    // Takes "sentence" or "paragraph"; anything else is ignored.
    Q_INVOKABLE void setFocusUnit(const QString &unit);
    QString focusDimming() const { return m_focusDimming; }
    // Takes "off", "light" or "strong"; anything else is ignored.
    Q_INVOKABLE void setFocusDimming(const QString &strength);
    bool liveFormatting() const { return m_liveFormatting; }
    Q_INVOKABLE void setLiveFormatting(bool on);
    int historyVersions() const { return m_historyVersions; }
    int historyDays() const { return m_historyDays; }
    qreal historyGigabytes() const { return m_historyGigabytes; }
    // Each keeps a value within its range, the space rounded to a tenth, and
    // ignores one outside it.
    Q_INVOKABLE void setHistoryVersions(int versions);
    Q_INVOKABLE void setHistoryDays(int days);
    Q_INVOKABLE void setHistoryGigabytes(qreal gigabytes);

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
    bool vimKeys() const { return m_vimKeys; }
    // Each setter keeps a value within its range, ignores one outside it,
    // and writes nothing when nothing changes. An empty family, or the
    // theme's own, returns the editor to the theme's family.
    Q_INVOKABLE void setEditorFontFamily(const QString &family);
    Q_INVOKABLE void setLineHeight(qreal lineHeight);
    Q_INVOKABLE void setMaxLineLength(int characters);
    Q_INVOKABLE void setSetextHeadings(bool setext);
    Q_INVOKABLE void setShowToolbar(bool show);
    Q_INVOKABLE void setVimKeys(bool on);

    QString previewFontFamily() const { return m_previewFontFamily; }
    int previewFontPixelSize() const;
    bool previewSizeChosen() const { return m_previewSize > 0; }
    qreal previewLineHeight() const { return m_previewLineHeight; }
    int previewLineLength() const { return m_previewLineLength; }
    bool scrollSync() const { return m_scrollSync; }
    bool remoteImages() const { return m_remoteImages; }
    // An empty family returns the preview to the default reading face.
    Q_INVOKABLE void setPreviewFontFamily(const QString &family);
    Q_INVOKABLE void setPreviewFontPixelSize(int size);
    // Returns the preview's text size to following the editor's.
    Q_INVOKABLE void resetPreviewFontPixelSize();
    Q_INVOKABLE void setPreviewLineHeight(qreal lineHeight);
    Q_INVOKABLE void setPreviewLineLength(int characters);
    Q_INVOKABLE void setScrollSync(bool sync);
    Q_INVOKABLE void setRemoteImages(bool load);

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
    bool m_vimKeys = false;
    QHash<QString, QStringList> m_shortcutOverrides;
    bool m_freshDraftOnLaunch = false;
    int m_libraryWidth = 260;
    QStringList m_libraryFolders;
    QString m_lastFolder;
    bool m_outlineShown = false;
    QString m_previewFontFamily;
    // 0 while the size follows the editor's.
    int m_previewSize = 0;
    qreal m_previewLineHeight = 1.6;
    int m_previewLineLength = 68;
    bool m_scrollSync = true;
    bool m_remoteImages = true;
    std::optional<bool> m_spellcheck;
    QString m_focusUnit = QStringLiteral("sentence");
    QString m_focusDimming = QStringLiteral("strong");
    bool m_liveFormatting = true;
    int m_historyVersions = 100;
    int m_historyDays = 30;
    qreal m_historyGigabytes = 1.0;
};
