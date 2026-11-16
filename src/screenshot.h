#pragma once

#include "cli.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

class Backend;
class QQuickItem;
class QQuickWindow;

// What `parch --screenshot FILE` does once its window is made (main() gives
// the window its size as it makes it, and keeps the caret from blinking).
// It waits until the window holds its document, then sets the window up as
// the Picture asks, one step at a time, each once what the step before
// asked for is drawn: the view, the library and focus mode; then the place
// at the top and the selection; then the panel. The picture is taken once
// nothing is left to draw and two looks at the window, a moment apart, are
// the same: the preview built and its formulas, diagrams and pictures in
// place, live formatting and spelling applied, the text holding the keys
// with its caret lit, and any panel's own content (the pages of the print
// preview, the versions of the history) shown.
//
// It only reads and shows: nothing it does edits the document.
class Screenshot : public QObject {
    Q_OBJECT
public:
    // The longest wait for everything to be drawn.
    static constexpr int WaitMs = 30000;
    // How often the window is looked at meanwhile.
    static constexpr int PollMs = 100;

    Screenshot(const Picture &picture, Backend *backend, int waitMs = WaitMs, QObject *parent = nullptr);

    void start(QQuickWindow *window);
    // What the window is still drawing, in words; empty once it is all drawn.
    QStringList pending() const;

signals:
    // The picture is saved, with code 0 and its file; or it could not be
    // taken, with code 3 when it could not be written and 4 when the window
    // was not drawn in time, and the reason. Always from the event loop.
    void finished(int code, const QString &message);

private:
    enum class Step { Loading, Arranging, Placing, Opening, Settling };

    void poll();
    // The view, the library and focus mode, and the keys to the text.
    void arrange();
    // The place at the top and the selection; asked again as the window
    // settles, since what is drawn late can move the text.
    void place();
    // The panel; false when the window could not open it.
    bool open();
    // Scrolls the Export dialog to what it leaves out and to its pages.
    void frameExport();
    // Takes the pointer out of the window, so that nothing shows it hovered.
    void leave();
    // Drawn by the processor, a shape (an icon, a wavy underline) that a
    // clipping view has scrolled wholly out of sight is still drawn, over
    // whatever is beside the view, where a GPU draws nothing of it. Such
    // shapes are hidden for the picture, so that it shows what the window
    // shows on screen, and shown again once they are back in the view.
    void hideClippedShapes();
    void finish(int code, const QString &message);

    QQuickItem *item(const QString &name) const;
    QObject *object(const QString &name) const;

    Picture m_picture;
    QPointer<Backend> m_backend;
    QPointer<QQuickWindow> m_window;
    int m_waitMs;
    Step m_step = Step::Loading;
    // Looks in a row at which nothing was left to draw.
    int m_quiet = 0;
    QImage m_last;
    QList<QPointer<QQuickItem>> m_hidden;
    QTimer m_poll;
    QElapsedTimer m_clock;
    bool m_done = false;
};
