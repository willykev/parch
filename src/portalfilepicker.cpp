// Adapted from sprawl's src/portalfilepicker.cpp (MIT), which follows Omacut's src/portalfilepicker.cpp (MIT, David Heinemeier Hansson).
#include "portalfilepicker.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

namespace {
const QString portalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString portalPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString chooserInterface = QStringLiteral("org.freedesktop.portal.FileChooser");
const QString requestInterface = QStringLiteral("org.freedesktop.portal.Request");

struct PortalFilterRule {
    uint type;
    QString pattern;
};

using PortalFilterRules = QList<PortalFilterRule>;

struct PortalFileFilter {
    QString name;
    PortalFilterRules rules;
};

using PortalFileFilters = QList<PortalFileFilter>;

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFilterRule &rule)
{
    argument.beginStructure();
    argument << rule.type << rule.pattern;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFilterRule &rule)
{
    argument.beginStructure();
    argument >> rule.type >> rule.pattern;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFileFilter &filter)
{
    argument.beginStructure();
    argument << filter.name << filter.rules;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFileFilter &filter)
{
    argument.beginStructure();
    argument >> filter.name >> filter.rules;
    argument.endStructure();
    return argument;
}

void registerPortalFilterTypes()
{
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalFilterRule>();
        qDBusRegisterMetaType<PortalFilterRules>();
        qDBusRegisterMetaType<PortalFileFilter>();
        qDBusRegisterMetaType<PortalFileFilters>();
        return true;
    }();
    Q_UNUSED(registered);
}

// Rule type 0 is a glob pattern.
PortalFileFilter textFilter()
{
    return {QCoreApplication::translate("PortalFilePicker", "Markdown and text"),
            {{0, QStringLiteral("*.md")}, {0, QStringLiteral("*.markdown")}, {0, QStringLiteral("*.txt")}}};
}

PortalFileFilter allFilter()
{
    return {QCoreApplication::translate("PortalFilePicker", "All files"), {{0, QStringLiteral("*")}}};
}

// A token of this request's own, from which the portal names it.
QString handleToken()
{
    return QStringLiteral("parch_%1").arg(QRandomGenerator::global()->generate());
}

// The portal takes a folder as bytes ending in NUL.
QByteArray portalPathBytes(const QString &path)
{
    QByteArray bytes = QFile::encodeName(path);
    bytes.append('\0');
    return bytes;
}
} // namespace

PortalFilePicker::PortalFilePicker(QObject *parent) : FilePicker(parent)
{
    registerPortalFilterTypes();
}

void PortalFilePicker::openDocuments()
{
    QVariantMap options;
    options.insert(QStringLiteral("accept_label"), tr("Open"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("multiple"), true);
    options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{textFilter(), allFilter()}));
    options.insert(QStringLiteral("current_filter"), QVariant::fromValue(textFilter()));
    if (!startFolder().isEmpty()) options.insert(QStringLiteral("current_folder"), portalPathBytes(startFolder()));
    request(QStringLiteral("OpenFile"), tr("Open"), options, Action::Documents);
}

void PortalFilePicker::openFolder()
{
    QVariantMap options;
    options.insert(QStringLiteral("accept_label"), tr("Add to Library"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("directory"), true);
    if (!startFolder().isEmpty()) options.insert(QStringLiteral("current_folder"), portalPathBytes(startFolder()));
    request(QStringLiteral("OpenFile"), tr("Open Folder"), options, Action::Folder);
}

void PortalFilePicker::saveDocument(const QUrl &suggested)
{
    const QFileInfo target(suggested.toLocalFile());
    QVariantMap options;
    options.insert(QStringLiteral("accept_label"), tr("Save"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("current_name"), target.fileName());
    options.insert(QStringLiteral("current_folder"), portalPathBytes(target.absolutePath()));
    request(QStringLiteral("SaveFile"), tr("Save"), options, Action::Save);
}

bool PortalFilePicker::listen(const QString &path)
{
    m_pendingPath = path;
    return QDBusConnection::sessionBus().connect(portalService, m_pendingPath, requestInterface,
                                                 QStringLiteral("Response"), this,
                                                 SLOT(handleResponse(uint,QVariantMap)));
}

void PortalFilePicker::stopListening()
{
    if (m_pendingPath.isEmpty()) return;
    QDBusConnection::sessionBus().disconnect(portalService, m_pendingPath, requestInterface, QStringLiteral("Response"),
                                             this, SLOT(handleResponse(uint,QVariantMap)));
}

void PortalFilePicker::closePending()
{
    if (m_pendingPath.isEmpty()) return;
    // Sent without waiting for an answer, as every call here is.
    QDBusConnection::sessionBus().send(
        QDBusMessage::createMethodCall(portalService, m_pendingPath, requestInterface, QStringLiteral("Close")));
}

void PortalFilePicker::request(const QString &method, const QString &title, QVariantMap options, Action action)
{
    // A request whose answer never comes, as when the portal ended with its
    // dialog open, must not hold up every later one: it is closed and replaced.
    if (m_pendingAction != Action::None) {
        closePending();
        clearPending();
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    // Nothing here waits on the portal: a call is sent and its reply comes
    // later, so a portal that starts slowly never holds up the window. A
    // portal that is not there answers with an error.
    if (!bus.isConnected()) {
        emit failed(tr("The desktop's file dialog (xdg-desktop-portal) is not available."));
        return;
    }
    // The answer comes on the path the portal makes from the token, so it is
    // listened for before the call, and cannot slip past while that is set up.
    const QString token = handleToken();
    options.insert(QStringLiteral("handle_token"), token);
    QString sender = bus.baseService().mid(1);
    sender.replace(u'.', u'_');
    m_pendingAction = action;
    if (!listen(QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token))) {
        clearPending();
        emit failed(tr("The file dialog's answer cannot be listened for."));
        return;
    }
    const int generation = ++m_generation;
    QDBusMessage call = QDBusMessage::createMethodCall(portalService, portalPath, chooserInterface, method);
    call << QString() << title << options;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    m_pendingWatcher = watcher;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (m_pendingWatcher == watcher) m_pendingWatcher = nullptr;
        // Replaced by a newer request, or answered already.
        if (generation != m_generation || m_pendingAction == Action::None) return;
        if (reply.isError()) {
            clearPending();
            emit failed(tr("The file dialog failed: %1").arg(reply.error().message()));
            return;
        }
        // An older portal can name the request otherwise than the token says.
        const QString actual = reply.value().path();
        if (actual == m_pendingPath) return;
        stopListening();
        if (!listen(actual)) {
            clearPending();
            emit failed(tr("The file dialog's answer cannot be listened for."));
        }
    });
}

PortalFilePicker::Answer PortalFilePicker::read(uint response, const QVariantMap &results)
{
    Answer answer;
    const auto failed = [&answer](const QString &reason) {
        answer.kind = Answer::Kind::Failed;
        answer.failure = reason;
        return answer;
    };
    // 1 is the dialog cancelled, and 2 the dialog ended some other way, as
    // when its window is closed: neither chooses anything, and neither is a
    // failure to report.
    if (response == 1 || response == 2) return answer;
    if (response != 0) return failed(tr("The file dialog ended without a choice."));
    for (const QString &uri : results.value(QStringLiteral("uris")).toStringList()) {
        const QUrl url(uri);
        // Only a local file is a document's place.
        if (!url.isLocalFile()) return failed(tr("%1 is not a local file.").arg(url.toDisplayString()));
        answer.urls.append(url);
    }
    if (answer.urls.isEmpty()) return failed(tr("The file dialog chose nothing."));
    answer.kind = Answer::Kind::Chosen;
    return answer;
}

void PortalFilePicker::handleResponse(uint response, const QVariantMap &results)
{
    const Action action = m_pendingAction;
    clearPending();
    if (action == Action::None) return;
    const Answer answer = read(response, results);
    switch (answer.kind) {
    case Answer::Kind::Nothing:
        return;
    case Answer::Kind::Failed:
        emit failed(answer.failure);
        return;
    case Answer::Kind::Chosen:
        break;
    }
    switch (action) {
    case Action::Documents:
        emit documentsSelected(answer.urls);
        break;
    case Action::Folder:
        emit folderSelected(answer.urls.first());
        break;
    case Action::Save:
        emit saveSelected(answer.urls.first());
        break;
    case Action::None:
        break;
    }
}

void PortalFilePicker::clearPending()
{
    // A watcher whose reply never comes goes with its request.
    if (m_pendingWatcher) {
        m_pendingWatcher->deleteLater();
        m_pendingWatcher = nullptr;
    }
    stopListening();
    m_pendingPath.clear();
    m_pendingAction = Action::None;
}
