#include "apppaths.h"

#include <QDir>
#include <QStandardPaths>
#include <QUuid>
#include <unistd.h>

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
    // Without a session runtime folder the temporary folder stands in. Its
    // name carries the account's numeric id so that two accounts never share
    // one. Since any account can create a name there first, whoever uses it
    // must create it private to its owner and refuse one that another made.
    if (paths.runtime.isEmpty()) paths.runtime = QDir::tempPath() + QStringLiteral("/parch-%1").arg(::getuid());
    return paths;
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
