#pragma once

#include "documentcheck.h"
#include "mdtree.h"
#include "renderrequest.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include <vector>

class RenderService;
struct HelperSet;

// The formulas and diagrams of a document, checked by drawing them with
// their helpers: what RaTeX or Merman refuses, or cannot draw, is a problem
// at the formula's or diagram's place, as the preview would show it. The
// drawing happens on the render service's own thread; collecting what is
// to be drawn is plain work on a tree, for any thread.
namespace RenderCheck {

// A formula or a diagram, where it is written.
struct Item {
    int file = 0;
    md::Range range;
    DocumentCheck::Position position;
    RenderKind kind = RenderKind::DisplayMath;
    QString source;

    bool isMath() const { return kind != RenderKind::Diagram; }
};

// Every formula and diagram of a tree, in file and source order; one
// written in a file included twice is there once.
std::vector<Item> items(const md::Document &document);

// How an item is asked of the helper: drawn as the preview draws it, at
// the size of body text, in the default colours.
RenderRequest request(const Item &item);

// What is wrong with an item, given what its helper said; nothing when it
// was drawn. Codes "invalid-math" and "invalid-diagram" for what the helper
// refused, "math-not-drawn" and "diagram-not-drawn" for a helper that
// failed; kinds "math" and "diagram"; all warnings.
std::optional<DocumentCheck::Problem> problem(const Item &item, const RenderResult &result);

// Whether the helpers can draw an item of this kind at all.
bool canDraw(const HelperSet &helpers, RenderKind kind);

// Draws items with a service and gathers what is wrong with them. A batch
// lives on the thread the service answers on; finished() follows once
// every item is answered, at once (from the event loop) when there are none.
class Batch : public QObject {
    Q_OBJECT

public:
    Batch(RenderService *service, std::vector<Item> items, QObject *parent = nullptr);
    ~Batch() override;

    void start();
    // Stops waiting: nothing more is gathered, and finished() never follows.
    void cancel();
    bool done() const { return m_started && m_waiting.isEmpty(); }
    // In the items' order.
    std::vector<DocumentCheck::Problem> problems() const;

signals:
    void finished();

private:
    void answered(quint64 ticket, const RenderResult &result);

    QPointer<RenderService> m_service;
    std::vector<Item> m_items;
    std::vector<std::optional<DocumentCheck::Problem>> m_problems;
    QHash<quint64, size_t> m_waiting;
    bool m_started = false;
};

// Draws the items and waits for every answer, running the event loop
// meanwhile, as the command line does.
std::vector<DocumentCheck::Problem> wait(RenderService &service, const std::vector<Item> &items);

} // namespace RenderCheck
