// Adapted from sprawl's src/theme.h (MIT, copyright 2026 willykev), without
// the board-only parts: note paper, swatches, ink and the bar height.
#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QFont>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <utility>

class HyprlandOptions;

// The colors the window is drawn with. A plain value, so the theme can tell
// whether anything changed by comparing two of them.
struct Palette {
    bool dark = true;
    QColor background{"#1a1b26"};
    QColor panel{"#13141c"}; // toolbars and the status line
    QColor raised{"#24283b"};
    QColor foreground{"#a9b1d6"};
    QColor bright{"#c0caf5"};
    QColor muted{"#414868"};
    QColor selection{"#292e42"}; // text selection
    QColor accent{"#7aa2f7"};
    QColor accentInk{"#1a1b26"};

    static Palette fallback();
    bool operator==(const Palette &other) const = default;
};

// How Omarchy looks right now, followed live: the whole palette of the
// current theme (colors.toml), the control states and type size the Omarchy
// shell draws its own panels with (the theme's shell.toml, and the person's
// own over it), and Hyprland's window rounding, border width, and whether it
// animates. Hyprland is asked over its own sockets once at the start, again
// whenever it reloads its configuration or the theme files change, and again
// when a window of the application comes to the front, since a setting
// changed by `hyprctl keyword` sends no event. Nothing is asked on a timer.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor panel READ panel NOTIFY changed)
    Q_PROPERTY(QColor raised READ raised NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor bright READ bright NOTIFY changed)
    Q_PROPERTY(QColor dim READ dim NOTIFY changed)
    Q_PROPERTY(QColor faint READ faint NOTIFY changed)
    Q_PROPERTY(QColor muted READ muted NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor selection READ selection NOTIFY changed)
    Q_PROPERTY(QColor accentInk READ accentInk NOTIFY changed)
    Q_PROPERTY(QColor danger READ danger NOTIFY changed)
    Q_PROPERTY(QColor good READ good NOTIFY changed)
    Q_PROPERTY(QColor warning READ warning NOTIFY changed)
    // The states every control in the Omarchy shell's own kit shares.
    Q_PROPERTY(QColor controlFill READ controlFill NOTIFY changed)
    Q_PROPERTY(QColor hoverFill READ hoverFill NOTIFY changed)
    Q_PROPERTY(QColor selectedFill READ selectedFill NOTIFY changed)
    Q_PROPERTY(QColor pressedFill READ pressedFill NOTIFY changed)
    Q_PROPERTY(QColor controlBorder READ controlBorder NOTIFY changed)
    Q_PROPERTY(QColor focusBorder READ focusBorder NOTIFY changed)
    // Menus, as the shell draws its own.
    Q_PROPERTY(QColor menuBackground READ menuBackground NOTIFY changed)
    Q_PROPERTY(QColor menuBorder READ menuBorder NOTIFY changed)
    Q_PROPERTY(QColor scrim READ scrim NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(int borderWidth READ borderWidth NOTIFY changed)
    Q_PROPERTY(int controlBorderWidth READ controlBorderWidth NOTIFY changed)
    Q_PROPERTY(bool reduceMotion READ reduceMotion NOTIFY changed)
    Q_PROPERTY(QString fontFamily READ fontFamily CONSTANT)
    Q_PROPERTY(int fontSize READ fontSize NOTIFY changed)
    // The shell's type scale, by token. A property and not a function, because
    // a binding is evaluated again when a property it read changes, and never
    // because a function would now answer otherwise.
    Q_PROPERTY(QVariantMap sizes READ sizes NOTIFY changed)
    // How many times the theme has changed: another number every time, so a
    // property that holds it passes every change on as well.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    // The desktop's: Omarchy's current theme, Hyprland, and the person's own
    // settings for the shell. A folder given to stand in for Omarchy's current
    // one (another theme, or a test's) comes without Hyprland or the person's
    // settings, unless they are given too: `hyprland` is the folder of
    // Hyprland's sockets, as HyprlandOptions::sessionFolder() finds it.
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprland = {},
                   const QString &userShell = {});
    ~Theme() override;

    const Palette &palette() const { return m_palette; }

    QColor background() const { return m_palette.background; }
    QColor panel() const { return m_palette.panel; }
    QColor raised() const { return m_palette.raised; }
    QColor foreground() const { return m_palette.foreground; }
    QColor bright() const { return m_palette.bright; }
    QColor dim() const { return m_dim; }
    QColor faint() const { return m_faint; }
    QColor muted() const { return m_palette.muted; }
    QColor accent() const { return m_palette.accent; }
    QColor selection() const { return m_palette.selection; }
    QColor accentInk() const { return m_palette.accentInk; }
    QColor danger() const { return m_danger; }
    QColor good() const { return m_good; }
    QColor warning() const { return m_warning; }
    QColor controlFill() const { return m_controlFill; }
    QColor hoverFill() const { return m_hoverFill; }
    QColor selectedFill() const { return m_selectedFill; }
    QColor pressedFill() const { return m_pressedFill; }
    QColor controlBorder() const { return m_controlBorder; }
    QColor focusBorder() const { return m_focusBorder; }
    QColor menuBackground() const { return m_menuBackground; }
    QColor menuBorder() const { return m_menuBorder; }
    QColor scrim() const { return m_scrim; }
    bool dark() const { return m_palette.dark; }
    int radius() const { return m_radius; }
    int borderWidth() const { return m_borderWidth; }
    int controlBorderWidth() const { return m_controlBorderWidth; }
    bool reduceMotion() const { return m_reduceMotion; }
    // The fontconfig alias `omarchy font set` points at the Omarchy font.
    QString fontFamily() const { return QStringLiteral("monospace"); }
    int fontSize() const { return m_fontSize; }
    // The Omarchy font at the size of the shell's own text, for the whole
    // application: a text takes it as it is made.
    QFont font() const;
    QVariantMap sizes() const;
    int revision() const { return m_revision; }

    // A size from the shell's type scale: caption, small, body, subtitle,
    // title, heading, display.
    int size(const QString &token) const;
    // The person's own settings for the shell, which it lays over whichever
    // theme is current, key by key: `omarchy display text size` keeps the
    // size there.
    static QString userShellFile();
    // A TOML file as "section.key" -> value; keys before any section have no
    // prefix. Enough of TOML for theme files: strings, numbers, comments.
    static QHash<QString, QString> readToml(const QString &path);
    // A shell.toml as the shell itself reads one (Color.parseShell): every
    // "section.key" and its value, in the order the file has them, and only
    // what the shell keeps. A value is a quoted string that is not empty, one
    // to four numbers, or a word; a key before any [section] is no key, and
    // a header the shell does not take for one leaves the keys after it in
    // the section before.
    static QList<std::pair<QString, QString>> readShell(const QString &path);
    static QColor mix(const QColor &base, const QColor &other, double amount);
    static double luminance(const QColor &color);

signals:
    void changed();

private:
    void reload();
    void watch();
    void askHyprland();
    void takeHyprland(const QString &option, const QJsonObject &reply);

    QString m_directory;
    QString m_userShell;
    Palette m_palette = Palette::fallback();
    QColor m_dim, m_faint, m_danger, m_good, m_warning;
    QColor m_controlFill, m_hoverFill, m_selectedFill, m_pressedFill, m_controlBorder, m_focusBorder;
    QColor m_menuBackground, m_menuBorder, m_scrim;
    QHash<QString, int> m_sizes;
    QVariantMap m_sizesForQml;
    int m_fontSize = 12;
    int m_controlBorderWidth = 1;
    int m_radius = 0;
    int m_borderWidth = 2;
    bool m_reduceMotion = false;
    bool m_loaded = false;
    int m_revision = 0;

    QFileSystemWatcher m_watcher;
    QTimer m_debounce{this};
    // Coming to the front asks Hyprland once, however many windows it passes.
    QTimer m_activation{this};
    HyprlandOptions *m_hyprland = nullptr;
};
