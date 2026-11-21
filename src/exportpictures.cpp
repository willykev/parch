#include "exportpictures.h"

#include "picturedata.h"
#include "svgpaper.h"

#include <QBuffer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QJsonObject>
#include <QSet>
#include <QTimer>

namespace {

enum Kind { Formula, Diagram };

struct Wanted {
    Kind kind;
    QString text;
    bool display;
};

void collect(const md::Node &node, QList<Wanted> &wanted, QSet<QString> &seen,
             const std::function<QString(int, const QString &, bool)> &key)
{
    if (!node.resolved && (node.type == md::NodeType::MathInline || node.type == md::NodeType::MathBlock ||
                           node.type == md::NodeType::MermaidBlock)) return;
    switch (node.type) {
    case md::NodeType::MathInline:
    case md::NodeType::MathBlock: {
        const bool display = node.type == md::NodeType::MathBlock || node.display;
        const QString k = key(Formula, node.text, display);
        if (!seen.contains(k)) {
            seen.insert(k);
            wanted.append({Formula, node.text, display});
        }
        break;
    }
    case md::NodeType::MermaidBlock: {
        const QString k = key(Diagram, node.text, false);
        if (!seen.contains(k)) {
            seen.insert(k);
            wanted.append({Diagram, node.text, false});
        }
        break;
    }
    default: break;
    }
    for (const md::Node &child : node.children) collect(child, wanted, seen, key);
}

void count(const md::Node &node, const std::function<void(const md::Node &)> &visit)
{
    if (node.type == md::NodeType::MathInline || node.type == md::NodeType::MathBlock ||
        node.type == md::NodeType::MermaidBlock)
        visit(node);
    for (const md::Node &child : node.children) count(child, visit);
}

} // namespace

RenderColors ExportPictures::paperColors()
{
    RenderColors colors;
    colors.dark = false;
    colors.foreground = QColor(0x1f, 0x23, 0x28);
    colors.background = QColor(Qt::white);
    colors.accent = QColor(0x0b, 0x57, 0xd0);
    colors.muted = QColor(0x59, 0x63, 0x6e);
    colors.surface = QColor(0xf3, 0xf5, 0xf7);
    return colors;
}

ExportPictures::ExportPictures() : ExportPictures(Options()) {}

ExportPictures::ExportPictures(const Options &options) : m_options(options)
{
    m_helpers = m_options.service.findHelpers ? HelperSet::find() : m_options.service.helpers;
    if (!m_options.service.findHelpers) return;
    m_options.service.helpers = m_helpers;
    m_options.service.findHelpers = false;
}

QString ExportPictures::key(int kind, const QString &text, bool display)
{
    return QString::number(kind) + (display ? u'd' : u'i') + text;
}

bool ExportPictures::draw(const md::Document &document, const std::function<bool()> &cancelled,
                          const std::function<void(int, int)> &progress)
{
    QList<Wanted> wanted;
    QSet<QString> seen;
    collect(document.root, wanted, seen, &ExportPictures::key);
    // Those already drawn are not drawn again.
    wanted.removeIf([this](const Wanted &w) { return m_pictures.contains(key(w.kind, w.text, w.display)); });
    if (wanted.isEmpty()) return !(cancelled && cancelled());

    RenderService service(m_options.service);
    QHash<quint64, Wanted> tickets;
    QEventLoop loop;
    int done = 0;
    const int total = int(wanted.size());
    QObject::connect(&service, &RenderService::finished, &loop, [&](quint64 ticket, const RenderResult &result) {
        const auto it = tickets.constFind(ticket);
        if (it == tickets.cend()) return;
        const Wanted w = *it;
        tickets.erase(it);
        Picture picture;
        picture.description = result.description.isEmpty() ? w.text.trimmed() : result.description;
        if (!result.ok()) {
            picture.diagnostic = result.diagnostic.isEmpty() ? QStringLiteral("it could not be drawn") : result.diagnostic;
        } else if (w.kind == Formula) {
            picture.drawn = result.format == QLatin1String("svg") && !result.data.isEmpty();
            picture.svg = result.data;
            const qreal scale = m_options.fontSize / std::max<qreal>(1, std::round(m_options.fontSize));
            picture.size = result.size * scale;
            picture.baseline = result.baseline * scale;
            if (!picture.drawn) picture.diagnostic = QStringLiteral("the math helper drew nothing usable");
        } else {
            const auto source = PictureData::read(result.data);
            const auto decoded = source.format == "png" ? PictureData::decode(source) : PictureData::Decoded{};
            if (!decoded.image.isNull()) {
                if (decoded.image.size() == decoded.naturalSize) picture.png = result.data;
                else {
                    QBuffer buffer(&picture.png);
                    if (!buffer.open(QIODevice::WriteOnly) || !decoded.image.save(&buffer, "PNG")) picture.png.clear();
                }
            }
            picture.drawn = !picture.png.isEmpty();
            picture.markup = result.svg;
            picture.size = result.size;
            // Vectors only when Qt draws the plain SVG as Merman drew the picture.
            if (!result.svg.isEmpty()) {
                const QByteArray plain = SvgPaper::plain(result.svg);
                if (!plain.isEmpty() && picture.drawn && SvgPaper::difference(plain, decoded.image) <= SvgPaper::kUnlike)
                    picture.svg = plain;
            }
            if (!picture.drawn) picture.diagnostic = QStringLiteral("the diagram helper drew nothing usable");
        }
        m_pictures.insert(key(w.kind, w.text, w.display), picture);
        ++done;
        if (progress) progress(done, total);
        if (tickets.isEmpty()) loop.quit();
    });
    // Asked every so often, so that a cancelled export stops while it waits.
    QTimer poll;
    poll.setInterval(50);
    bool stopped = false;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (cancelled && cancelled()) {
            stopped = true;
            loop.quit();
        }
    });
    for (const Wanted &w : std::as_const(wanted)) {
        RenderRequest request;
        request.kind = w.kind == Diagram ? RenderKind::Diagram : w.display ? RenderKind::DisplayMath : RenderKind::InlineMath;
        request.purpose = RenderPurpose::Print;
        request.source = w.text;
        request.colors = m_options.colors;
        // Drawn at a whole size, which the helpers take; a formula is scaled to the text after.
        request.fontSize = std::max<qreal>(1, std::round(m_options.fontSize));
        request.scale = 1;
        request.fontFamily = m_options.fontFamily;
        tickets.insert(service.render(request), w);
    }
    if (!tickets.isEmpty()) {
        poll.start();
        loop.exec();
    }
    if (stopped || (cancelled && cancelled())) {
        for (auto it = tickets.cbegin(); it != tickets.cend(); ++it) service.cancel(it.key());
        return false;
    }
    return true;
}

const ExportPictures::Picture *ExportPictures::formula(const QString &tex, bool display) const
{
    const auto it = m_pictures.constFind(key(Formula, tex, display));
    return it == m_pictures.cend() ? nullptr : &*it;
}

const ExportPictures::Picture *ExportPictures::diagram(const QString &source) const
{
    const auto it = m_pictures.constFind(key(Diagram, source, false));
    return it == m_pictures.cend() ? nullptr : &*it;
}

QJsonObject ExportPictures::helperReport() const
{
    QJsonObject report;
    const auto add = [&report](const QString &name, const QString &path) {
        if (path.isEmpty() || !QFileInfo(path).isExecutable()) return;
        report.insert(name, QJsonObject{{QStringLiteral("path"), path},
                                        {QStringLiteral("sha256"), HelperIdentity::of(path)}});
    };
    add(QStringLiteral("render-svg"), m_helpers.ratex);
    add(QStringLiteral("merman-cli"), m_helpers.merman);
    return report;
}

void ExportPictures::noteDrawingLimit(const md::Document &document, QStringList &limitations)
{
    for (const md::Diagnostic &issue : document.diagnostics) {
        if (issue.code == md::DiagnosticCode::HelperLimit) {
            limitations.append(issue.message);
            return;
        }
    }
}

ExportPictures::Counts ExportPictures::counts(const md::Document &document) const
{
    Counts counts;
    count(document.root, [&](const md::Node &node) {
        if (node.type == md::NodeType::MermaidBlock) {
            ++counts.diagrams;
            const Picture *picture = diagram(node.text);
            if (!node.resolved || !picture || !picture->drawn) ++counts.diagramsNotDrawn;
            else if (!picture->vector()) ++counts.diagramsAsPictures;
        } else {
            ++counts.formulas;
            const Picture *picture = formula(node.text, node.type == md::NodeType::MathBlock || node.display);
            if (!node.resolved || !picture || !picture->drawn) ++counts.formulasNotDrawn;
        }
    });
    return counts;
}

QString ExportPictures::missingMath() const
{
    if (!m_helpers.ratex.isEmpty() && QFileInfo(m_helpers.ratex).isExecutable()) return {};
    return QStringLiteral("the math helper (render-svg) is not installed");
}

QString ExportPictures::missingDiagrams() const
{
    if (!m_helpers.merman.isEmpty() && QFileInfo(m_helpers.merman).isExecutable()) return {};
    return QStringLiteral("the diagram helper (merman-cli) is not installed");
}
