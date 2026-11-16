#include "previewresources.h"

#include "remotefetcher.h"
#include "renderservice.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QSvgRenderer>
#include <QUrl>
#include <QtConcurrent>
#include <cmath>

namespace {

QString tr(const char *text) { return QCoreApplication::translate("Preview", text); }

QString digest(const QString &text)
{
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex().left(24));
}

struct Loaded {
    QImage image;
    QString error;
};

Loaded readPicture(QImageReader &reader)
{
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead()) return {{}, tr("it is not a picture parch can read")};
    const QSize size = reader.size();
    if (size.isValid() && std::max(size.width(), size.height()) > PreviewResources::MaxSide)
        reader.setScaledSize(size.scaled(PreviewResources::MaxSide, PreviewResources::MaxSide, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull()) return {{}, reader.errorString()};
    if (std::max(image.width(), image.height()) > PreviewResources::MaxSide)
        image = image.scaled(PreviewResources::MaxSide, PreviewResources::MaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image.setDevicePixelRatio(1);
    return {image, {}};
}

Loaded readFile(const QString &path)
{
    const QFileInfo file(path);
    if (!file.exists()) return {{}, tr("there is no such file")};
    if (!file.isFile()) return {{}, tr("it is not a file")};
    QImageReader reader(path);
    return readPicture(reader);
}

Loaded readBytes(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    return readPicture(reader);
}

// The bytes a data: URL carries.
QByteArray inlineBytes(const QString &url)
{
    const qsizetype comma = url.indexOf(u',');
    if (comma < 0) return {};
    const QStringView header = QStringView(url).first(comma);
    const QByteArray body = QStringView(url).sliced(comma + 1).toLatin1();
    return header.endsWith(QLatin1String(";base64"), Qt::CaseInsensitive) ? QByteArray::fromBase64(body)
                                                                           : QByteArray::fromPercentEncoding(body);
}

} // namespace

PreviewResources::PreviewResources(QObject *parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(2);
}

PreviewResources::~PreviewResources()
{
    ++m_generation;
    if (m_service)
        for (auto it = m_tickets.cbegin(); it != m_tickets.cend(); ++it) m_service->cancel(it.key());
    m_pool.clear();
    m_pool.waitForDone();
}

void PreviewResources::setRenderService(RenderService *service)
{
    if (service == m_service) return;
    if (m_service) disconnect(m_service, nullptr, this, nullptr);
    m_service = service;
    if (m_service)
        connect(m_service, &RenderService::finished, this,
                [this](quint64 ticket, const RenderResult &result) { rendered(ticket, result); });
}

void PreviewResources::setFetcher(RemoteFetcher *fetcher) { m_fetcher = fetcher; }
void PreviewResources::setRemoteAllowed(bool allowed) { m_remoteAllowed = allowed; }
void PreviewResources::setColors(const RenderColors &colors) { m_colors = colors; }
void PreviewResources::setScale(qreal scale) { m_scale = std::max<qreal>(1, scale); }
void PreviewResources::setLabelFamily(const QString &family) { m_labelFamily = family; }

QString PreviewResources::renderIdentity(qreal fontSize) const
{
    return m_colors.identity() + u'|' + QString::number(m_scale) + u'|' + QString::number(fontSize) + u'|' + m_labelFamily;
}

preview::Picture PreviewResources::drawn(const QString &key, const RenderRequest &request)
{
    Entry &entry = m_entries[key];
    entry.picture.key = key;
    if (!m_service) return entry.picture;
    const QString identity = renderIdentity(request.fontSize);
    const bool current = entry.drawnWith == identity;
    if (!current && !(entry.waiting && entry.askedWith == identity)) {
        if (entry.ticket) {
            m_service->cancel(entry.ticket);
            m_tickets.remove(entry.ticket);
        }
        entry.askedWith = identity;
        entry.waiting = true;
        entry.ticket = m_service->render(request, key);
        m_tickets.insert(entry.ticket, key);
    }
    return entry.picture;
}

preview::Picture PreviewResources::math(const QString &tex, bool display, qreal fontSize)
{
    const int size = std::max(1, int(std::lround(fontSize)));
    const QString key = QStringLiteral("parch:math/%1/%2/%3").arg(display ? u'd' : u'i').arg(size).arg(digest(tex));
    RenderRequest request;
    request.kind = display ? RenderKind::DisplayMath : RenderKind::InlineMath;
    request.source = tex;
    request.colors = m_colors;
    request.fontSize = size;
    request.scale = m_scale;
    request.fontFamily = m_labelFamily;
    return drawn(key, request);
}

preview::Picture PreviewResources::diagram(const QString &source)
{
    const QString key = QStringLiteral("parch:diagram/%1").arg(digest(source));
    RenderRequest request;
    request.kind = RenderKind::Diagram;
    request.source = source;
    request.colors = m_colors;
    request.fontSize = 16;
    request.scale = m_scale;
    request.fontFamily = m_labelFamily;
    return drawn(key, request);
}

void PreviewResources::rendered(quint64 ticket, const RenderResult &result)
{
    const auto found = m_tickets.constFind(ticket);
    if (found == m_tickets.cend()) return;
    const QString key = *found;
    m_tickets.erase(found);
    const auto at = m_entries.find(key);
    if (at == m_entries.end() || at->ticket != ticket) return;
    Entry &entry = *at;
    entry.ticket = 0;
    entry.waiting = false;
    entry.drawnWith = entry.askedWith;
    preview::Picture &picture = entry.picture;
    picture.description = result.description;
    picture.diagnostic.clear();
    QImage image;
    if (result.ok()) {
        if (result.format == QLatin1String("svg")) {
            QSvgRenderer renderer(result.data);
            const QSize pixels = (result.size * m_scale).toSize().expandedTo(QSize(1, 1));
            if (renderer.isValid() && pixels.width() <= 16384 && pixels.height() <= 16384) {
                image = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                renderer.render(&painter);
            }
        } else {
            image.loadFromData(result.data);
        }
    }
    if (!image.isNull()) {
        picture.state = preview::Picture::Ready;
        picture.image = image;
        picture.size = result.size;
        picture.baseline = result.baseline;
    } else {
        picture.state = preview::Picture::Failed;
        picture.image = QImage();
        picture.diagnostic = result.ok() ? tr("The picture that was drawn cannot be shown.") : result.diagnostic;
    }
    emit changed(key);
}

preview::Picture PreviewResources::image(const md::Destination &destination, const QString &written)
{
    using Kind = md::Destination::Kind;
    const bool remote = destination.kind == Kind::Remote;
    const bool embedded = destination.kind == Kind::OtherScheme && destination.scheme == QLatin1String("data");
    const QString source = destination.kind == Kind::LocalFile ? destination.path : written;
    const QString key = QStringLiteral("parch:image/%1").arg(digest(source));
    // Whether pictures may be fetched is asked each time one is shown, and
    // one that was fetched before is not shown while they may not be.
    if (remote && !m_remoteAllowed) {
        preview::Picture refused;
        refused.key = key;
        refused.state = preview::Picture::Failed;
        refused.diagnostic = tr("pictures from the network are turned off");
        return refused;
    }
    const auto known = m_entries.constFind(key);
    if (known != m_entries.cend() && (known->waiting || known->drawnWith == QLatin1String("done"))) return known->picture;
    Entry &entry = m_entries[key];
    entry.picture.key = key;
    const auto fail = [&](const QString &why) {
        entry.picture.state = preview::Picture::Failed;
        entry.picture.diagnostic = why;
        return entry.picture;
    };
    const quint64 generation = m_generation;
    const auto take = [this, key, generation](const Loaded &loaded) { this->loaded(key, generation, loaded.image, loaded.error); };
    if (destination.kind == Kind::LocalFile) {
        if (!destination.absolute) return fail(tr("the document has no folder yet"));
        entry.waiting = true;
        entry.picture.state = preview::Picture::Pending;
        QtConcurrent::run(&m_pool, readFile, destination.path).then(this, take);
    } else if (embedded) {
        if (written.size() > MaxInlineBytes * 2) return fail(tr("it is too large"));
        entry.waiting = true;
        entry.picture.state = preview::Picture::Pending;
        QtConcurrent::run(&m_pool, [written] { return readBytes(inlineBytes(written)); }).then(this, take);
    } else if (remote) {
        if (!m_fetcher) return fail(tr("pictures from the network are not available"));
        const QUrl url(written);
        if (!url.isValid()) return fail(tr("its address is not valid"));
        entry.waiting = true;
        entry.picture.state = preview::Picture::Pending;
        m_fetcher->fetch(url, this, [this, key, generation, take](const QByteArray &data, const QString &error) {
            if (!error.isEmpty() || data.isEmpty()) {
                loaded(key, generation, QImage(), error.isEmpty() ? tr("nothing came back") : error);
                return;
            }
            QtConcurrent::run(&m_pool, readBytes, data).then(this, take);
        });
    } else {
        return fail(tr("its address is not one parch shows pictures from"));
    }
    return m_entries[key].picture;
}

void PreviewResources::loaded(const QString &key, quint64 generation, const QImage &image, const QString &error)
{
    if (generation != m_generation) return;
    const auto at = m_entries.find(key);
    if (at == m_entries.end()) return;
    Entry &entry = *at;
    entry.waiting = false;
    entry.drawnWith = QStringLiteral("done");
    if (image.isNull()) {
        entry.picture.state = preview::Picture::Failed;
        entry.picture.diagnostic = error;
    } else {
        entry.picture.state = preview::Picture::Ready;
        entry.picture.image = image;
        entry.picture.size = image.size();
        entry.picture.diagnostic.clear();
    }
    emit changed(key);
}

void PreviewResources::keepOnly(const QSet<QString> &keys)
{
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (keys.contains(it.key())) {
            ++it;
            continue;
        }
        if (it->ticket) {
            if (m_service) m_service->cancel(it->ticket);
            m_tickets.remove(it->ticket);
        }
        it = m_entries.erase(it);
    }
}

void PreviewResources::clear()
{
    ++m_generation;
    keepOnly({});
}

int PreviewResources::pending() const
{
    int count = 0;
    for (const Entry &entry : m_entries) count += entry.waiting ? 1 : 0;
    return count;
}
