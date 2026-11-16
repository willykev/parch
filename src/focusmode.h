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
class FocusMode : public QObject {
    Q_OBJECT
    Q_PROPERTY(EditorDecorations *decorations READ decorations WRITE setDecorations NOTIFY decorationsChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // "sentence" or "paragraph".
    Q_PROPERTY(QString unit READ unit WRITE setUnit NOTIFY unitChanged)
    Q_PROPERTY(QColor dim READ dim WRITE setDim NOTIFY colorsChanged)
    Q_PROPERTY(QColor normal READ normal WRITE setNormal NOTIFY colorsChanged)

public:
    enum class Unit { Sentence, Paragraph };
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
    void setDim(const QColor &dim);
    QColor normal() const { return m_normal; }
    void setNormal(const QColor &normal);

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

    QPointer<EditorDecorations> m_decorations;
    QPointer<EditorBinding> m_binding;
    QList<QMetaObject::Connection> m_connections, m_bindingConnections;
    bool m_enabled = false;
    Unit m_unit = Unit::Sentence;
    QColor m_dim, m_normal;
    // An edit that leaves the caret where it was can still change its sentence.
    QTimer m_afterEdit;
};
