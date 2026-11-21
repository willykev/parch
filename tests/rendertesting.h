#pragma once

#include "renderhelpers.h"
#include "renderrequest.h"
#include "renderservice.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

// What the render tests share: stand-in helpers, the real helpers when a
// development build has them, waiting for results, and reading PDFs.

// Writes stand-ins for render-svg and merman-cli into the folder. Each
// appends a line to `log` for every run ("start <pid>", then "end <pid>";
// merman-cli also "run <format> <scale>"). The math stand-in draws a box as
// wide as the formula is long, 20 high with its baseline at 14, in the
// colour asked for, unless the formula starts with a word that asks for
// something else:
//   slow      takes 300 ms first
//   sleep     starts a child that sleeps for a minute and waits for it,
//             writing the child's process id to <log>.child
//   flood     writes 4 MB of output
//   environ   writes its environment to <log>.environ, its network devices
//             to <log>.net and its working folder to <log>.folder
//   refuse    refuses the formula as render-svg refuses one
//   crash     kills itself with SIGSEGV
// The diagram stand-in draws an SVG with one label and, for PNG, copies
// <folder>/picture.png (80 × 40); a source starting with "refuse" is refused.
// Asked for --version, it says "merman-cli 0.0.7".
void writeFakeHelpers(const QString &folder, const QString &log);

// The development build's helpers, or empty paths when bin/fetch-helpers
// has not fetched them.
HelperSet realHelpers();

// Collects every result a service delivers.
class RenderResults : public QObject {
public:
    explicit RenderResults(RenderService &service);
    // Waits until the ticket's result arrives.
    bool wait(quint64 ticket, int timeoutMs = 30000);
    RenderResult operator[](quint64 ticket) const { return m_results.value(ticket); }
    bool has(quint64 ticket) const { return m_results.contains(ticket); }
    int count() const { return int(m_results.size()); }

private:
    QHash<quint64, RenderResult> m_results;
};

// A service with the given helpers whose cache and temporary folders are
// beneath `folder`.
RenderService::Options renderOptions(const HelperSet &helpers, const QString &folder);

// What a PDF draws: path-construction and painting operators, text objects
// and images, counted in its (inflated) content streams.
struct PdfContents {
    int pathOperators = 0;
    int textObjects = 0;
    int images = 0;
};
PdfContents inspectPdf(const QByteArray &pdf);

// Runs a helper as the service would and measures it: wall time, and the
// peak resident memory of the helper process itself.
struct HelperMeasure {
    int exitCode = -1;
    double milliseconds = 0;
    double peakMiB = 0;
    QByteArray output;
};
HelperMeasure measureHelper(const HelperRun &run, const QString &folder);

// A fixture corpus from tests/fixtures/<name>/corpus.json.
QJsonArray renderFixtures(const QString &name);

// A green SVG with a reference to a magenta tile. Only the test process
// knows the search path for the tile.
class SvgSwatch {
public:
    SvgSwatch();
    ~SvgSwatch();
    QByteArray svg() const;
private:
    QTemporaryDir m_folder;
    QStringList m_previous;
};

// Pixels of the image within `tolerance` (summed over the channels) of the colour.
int pixelsNear(const QImage &image, const QColor &color, int tolerance);

// Whether this kernel lets an unprivileged process make a user and a network
// namespace, which is how the helpers are kept off the network. Some
// containers and hardened kernels refuse; helpers then run with the network.
bool kernelGivesNetworkNamespaces();
