#pragma once

#include "editordecorations.h"

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringView>
#include <QTimer>

// Focus mode for one view: every part of the text but the sentence or the
// paragraph with the caret is dimmed, through the view's decorations, and
// the focus follows the caret and the text as they change. A paragraph is a
// run of lines between blank lines, a heading's line on its own, and each
// item of a list or line of a quote or table from its own line on; a
// sentence is one as Unicode finds them within its paragraph, soft line
// breaks read as spaces. The caret on a blank line dims everything.
//
// Dimmed text is the normal text colour blended towards the background, as
// far as the strength asks but never so far that it reads at less than
// MinimumContrast against the background. With the strength off, nothing
// is dimmed, and focus mode is otherwise as it was.
class FocusMode : public QObject {
    Q_OBJECT
    Q_PROPERTY(EditorDecorations *decorations READ decorations WRITE setDecorations NOTIFY decorationsChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // "sentence" or "paragraph".
    Q_PROPERTY(QString unit READ unit WRITE setUnit NOTIFY unitChanged)
    // The colour of the text and the background it is drawn on.
    Q_PROPERTY(QColor normal READ normal WRITE setNormal NOTIFY colorsChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY colorsChanged)
    // "off", "light" or "strong".
    Q_PROPERTY(QString strength READ strength WRITE setStrength NOTIFY colorsChanged)
    // The colour of what is not in focus, from the three above.
    Q_PROPERTY(QColor dim READ dim NOTIFY colorsChanged)

public:
    enum class Unit { Sentence, Paragraph };
    enum class Strength { Off, Light, Strong };
    // Dimmed text reads at no less than this against the background (WCAG's
    // ratio for large text and for what is not the text in hand).
    static constexpr double MinimumContrast = 3.0;
    // How far each strength blends the text towards the background, before
    // the floor above stops it.
    static constexpr double LightBlend = 0.35, StrongBlend = 0.55;
    // The dimmed colour of `normal` on `background`. Text that already reads
    // at less than MinimumContrast is not dimmed at all.
    static QColor dimmed(const QColor &normal, const QColor &background, Strength strength);
    // The strength a setting names; Strong for anything unknown.
    static Strength strengthNamed(const QString &name);
    // A sentence is looked for this far around the caret at most, and a
    // paragraph this far, so that one of a whole book costs no more. A
    // sentence that reaches further runs to its paragraph's ends.
    static constexpr int SentenceReach = 1500;
    static constexpr int ParagraphReach = 50000;

    explicit FocusMode(QObject *parent = nullptr);

    EditorDecorations *decorations() const { return m_decorations; }
    void setDecorations(EditorDecorations *decorations);
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    QString unit() const;
    void setUnit(const QString &unit);
    QColor dim() const { return m_dim; }
    QColor normal() const { return m_normal; }
    void setNormal(const QColor &normal);
    QColor background() const { return m_background; }
    void setBackground(const QColor &background);
    QString strength() const;
    void setStrength(const QString &strength);

    // The part of `text` that stays bright with the caret at `position`.
    static SourceRange rangeAt(QStringView text, int position, Unit unit);

signals:
    void decorationsChanged();
    void enabledChanged();
    void unitChanged();
    void colorsChanged();

private:
    void connectBinding();
    void refresh();
    // Works the dim colour out again, and says so when it changed.
    void recolor();

    QPointer<EditorDecorations> m_decorations;
    QPointer<EditorBinding> m_binding;
    QList<QMetaObject::Connection> m_connections, m_bindingConnections;
    bool m_enabled = false;
    Unit m_unit = Unit::Sentence;
    Strength m_strength = Strength::Strong;
    QColor m_dim, m_normal, m_background;
    // An edit that leaves the caret where it was can still change its sentence.
    QTimer m_afterEdit;
};
