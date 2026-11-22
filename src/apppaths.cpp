#include "apppaths.h"

#include "privatefiles.h"

#include <QFile>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QUuid>
#include <sys/un.h>

bool AppPaths::isDocumentId(const QString &id)
{
    return !id.isEmpty() && QUuid::fromString(id).toString(QUuid::WithoutBraces) == id;
}

AppPaths AppPaths::standard()
{
    const QString name = QStringLiteral("/parch");
    AppPaths paths;
    paths.data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + name;
    paths.state = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + name;
    paths.config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + name;
    paths.runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (paths.runtime.isEmpty()) {
        paths.runtime = paths.state + QStringLiteral("/runtime");
        if (paths.socketPathFits()) {
            PrivateFiles::makeFolder(paths.runtime);
        } else {
            // One private folder for this process, removed at exit.
            static const QTemporaryDir temporary;
            paths.runtime = temporary.path();
            paths.singleInstance = false;
        }
    }
    return paths;
}

QString AppPaths::socketPath() const { return runtime + QStringLiteral("/parch.sock"); }

bool AppPaths::socketPathFits() const
{
    return QFile::encodeName(socketPath()).size() < qsizetype(sizeof(sockaddr_un::sun_path));
}

AppPaths AppPaths::under(const QString &root)
{
    return AppPaths{root + QStringLiteral("/data"), root + QStringLiteral("/state"), root + QStringLiteral("/config"),
                    root + QStringLiteral("/runtime")};
}

// Drafts, history and the identity registry are the person's writing or
// point at it, so they live with data. Recovery, the session and recent files
// describe the last run and live with state.
QString AppPaths::drafts() const { return data + QStringLiteral("/drafts"); }
QString AppPaths::history() const { return data + QStringLiteral("/history"); }
QString AppPaths::registryFile() const { return data + QStringLiteral("/documents.json"); }
QString AppPaths::documentSettingsFile() const { return data + QStringLiteral("/document-settings.json"); }
QString AppPaths::recovery() const { return state + QStringLiteral("/recovery"); }
QString AppPaths::sessionFile() const { return state + QStringLiteral("/session.json"); }
QString AppPaths::recentFile() const { return state + QStringLiteral("/recent.json"); }
QString AppPaths::positionsFile() const { return state + QStringLiteral("/positions.json"); }
QString AppPaths::settingsFile() const { return config + QStringLiteral("/settings.json"); }
// The writer's own words, kept with their writing.
QString AppPaths::personalDictionaryFile() const { return data + QStringLiteral("/dictionary.txt"); }
