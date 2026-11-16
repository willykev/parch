// Adapted from sprawl's src/portalfilepicker.cpp (MIT), which follows Omacut's src/portalfilepicker.cpp (MIT, David Heinemeier Hansson).
#pragma once

#include "filepicker.h"

#include <QList>
#include <QString>
#include <QUrl>
#include <QVariantMap>

class QDBusPendingCallWatcher;

// Open and save dialogs through xdg-desktop-portal's FileChooser, so that
// they are the desktop's own. One request is open at a time: a newer one
// closes it and takes its place, and a late answer to it is ignored.
class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void openDocuments() override;
    void openFolder() override;
    void saveDocument(const QUrl &suggested) override;
    void openFiles(const QString &title, const QString &filterName, const QStringList &patterns, bool multiple) override;

    // What the portal's answer to a request means: the files or folder
    // chosen; nothing, for a dialog cancelled or closed through its window;
    // or a failure with its reason, for an answer that cannot be used.
    struct Answer {
        enum class Kind { Chosen, Nothing, Failed };
        Kind kind = Kind::Nothing;
        QList<QUrl> urls;
        QString failure;
    };
    static Answer read(uint response, const QVariantMap &results);

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    enum class Action { None, Documents, Folder, Save, Files };

    void request(const QString &method, const QString &title, QVariantMap options, Action action);
    bool listen(const QString &path);
    void stopListening();
    void clearPending();
    // Asks the portal to close the request still open, if there is one.
    void closePending();

    QString m_pendingPath;
    Action m_pendingAction = Action::None;
    // One more with each request, so that a late reply to a replaced one is ignored.
    int m_generation = 0;
    QDBusPendingCallWatcher *m_pendingWatcher = nullptr;
};
