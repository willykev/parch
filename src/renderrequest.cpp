#include "renderrequest.h"

#include "theme.h"

#include <QStringList>

RenderColors RenderColors::fromTheme(const Theme &theme)
{
    RenderColors colors;
    colors.dark = theme.dark();
    colors.foreground = theme.foreground();
    colors.background = theme.background();
    colors.accent = theme.accent();
    colors.muted = theme.muted();
    colors.surface = theme.raised();
    const QColor good = theme.good(), warning = theme.warning(), danger = theme.danger();
    colors.series = {theme.accent(),
                     good,
                     warning,
                     danger,
                     Theme::mix(theme.accent(), good, .5),
                     Theme::mix(warning, danger, .5),
                     Theme::mix(theme.accent(), danger, .5),
                     Theme::mix(good, warning, .5)};
    return colors;
}

QString RenderColors::identity() const
{
    QStringList parts{dark ? QStringLiteral("dark") : QStringLiteral("light"),
                      foreground.name(QColor::HexArgb), background.name(QColor::HexArgb),
                      accent.name(QColor::HexArgb), muted.name(QColor::HexArgb), surface.name(QColor::HexArgb)};
    for (const QColor &color : series) parts << color.name(QColor::HexArgb);
    return parts.join(u',');
}
