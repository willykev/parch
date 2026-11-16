#include "printportal.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QRandomGenerator>
#include <QTemporaryFile>

namespace {
const QString portalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString portalPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString printInterface = QStringLiteral("org.freedesktop.portal.Print");
const QString requestInterface = QStringLiteral("org.freedesktop.portal.Request");
} // namespace

Printer::Page Printer::pageOf(const QVariantMap &setup)
{
    Page page;
    const double width = setup.value(QStringLiteral("Width")).toDouble();
    const double height = setup.value(QStringLiteral("Height")).toDouble();
    if (width > 0 && height > 0) page.size = QSizeF(std::min(width, height), std::max(width, height));
    const QString orientation = setup.value(QStringLiteral("Orientation")).toString().toLower();
    page.landscape = orientation.contains(QLatin1String("landscape"));
    for (const char *side : {"MarginTop", "MarginBottom", "MarginLeft", "MarginRight"})
        page.margin = std::max(page.margin, setup.value(QString::fromLatin1(side)).toDouble());
    return page;
}

PortalPrinter::PortalPrinter(QObject *parent) : Printer(parent) {}

PortalPrinter::~PortalPrinter() { clear(); }

void PortalPrinter::prepare(const QString &title)
{
    const QString token = QStringLiteral("parch_%1").arg(QRandomGenerator::global()->generate());
    QVariantMap options{{QStringLiteral("handle_token"), token}, {QStringLiteral("modal"), true}};
    request(QStringLiteral("PreparePrint"), {QString(), title, QVariantMap(), QVariantMap(), options}, token, Step::Prepare);
}

void PortalPrinter::print(const QString &title, const QByteArray &pdf, quint32 token)
{
    // The portal reads the job from a file that is already gone from the
    // file system: only its descriptor is handed over.
    auto *file = new QTemporaryFile(QDir::tempPath() + QStringLiteral("/parch-print-XXXXXX.pdf"), this);
    if (!file->open() || file->write(pdf) != pdf.size() || !file->flush() || !file->seek(0)) {
        delete file;
        emit failed(tr("The document could not be handed to the printer."));
        return;
    }
    const QString handle = QStringLiteral("parch_%1").arg(QRandomGenerator::global()->generate());
    QVariantMap options{{QStringLiteral("handle_token"), handle}, {QStringLiteral("token"), token},
                        {QStringLiteral("modal"), true}};
    const QDBusUnixFileDescriptor descriptor(file->handle());
    file->remove();
    request(QStringLiteral("Print"), {QString(), title, QVariant::fromValue(descriptor), options}, handle, Step::Print);
    m_file = file;
}

void PortalPrinter::request(const QString &method, const QList<QVariant> &arguments, const QString &token, Step step)
{
    clear();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit failed(tr("The desktop's print dialog (xdg-desktop-portal) is not available."));
        return;
    }
    QString sender = bus.baseService().mid(1);
    sender.replace(u'.', u'_');
    m_path = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    m_step = step;
    if (!bus.connect(portalService, m_path, requestInterface, QStringLiteral("Response"), this,
                     SLOT(handleResponse(uint,QVariantMap)))) {
        clear();
        emit failed(tr("The print dialog's answer cannot be listened for."));
        return;
    }
    const int generation = ++m_generation;
    QDBusMessage call = QDBusMessage::createMethodCall(portalService, portalPath, printInterface, method);
    call.setArguments(arguments);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (generation != m_generation || m_step == Step::None) return;
        if (reply.isError()) {
            clear();
            emit failed(tr("Printing failed: %1").arg(reply.error().message()));
            return;
        }
        // An older portal names the request otherwise than the token says.
        const QString actual = reply.value().path();
        if (actual == m_path) return;
        const Step step = m_step;
        stopListening();
        m_path = actual;
        m_step = step;
        QDBusConnection::sessionBus().connect(portalService, m_path, requestInterface, QStringLiteral("Response"), this,
                                              SLOT(handleResponse(uint,QVariantMap)));
    });
}

void PortalPrinter::handleResponse(uint response, const QVariantMap &results)
{
    const Step step = m_step;
    clear();
    // 1 is the dialog cancelled and 2 ended some other way: nothing to say.
    if (response == 1 || response == 2) return;
    if (response != 0) {
        emit failed(tr("The print dialog ended without printing."));
        return;
    }
    if (step == Step::Prepare) {
        const QVariantMap setup = qdbus_cast<QVariantMap>(results.value(QStringLiteral("page-setup")));
        emit prepared(results.value(QStringLiteral("token")).toUInt(), setup);
    } else if (step == Step::Print) {
        emit printed();
    }
}

void PortalPrinter::stopListening()
{
    if (!m_path.isEmpty())
        QDBusConnection::sessionBus().disconnect(portalService, m_path, requestInterface, QStringLiteral("Response"),
                                                 this, SLOT(handleResponse(uint,QVariantMap)));
    m_path.clear();
    m_step = Step::None;
}

void PortalPrinter::clear()
{
    stopListening();
    if (m_file) {
        m_file->deleteLater();
        m_file = nullptr;
    }
}
