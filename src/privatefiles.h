#pragma once

#include "apppaths.h"

#include <QFileDevice>
#include <QString>
#include <QStringList>

// parch's own folders and the files in them, which hold copies of what the
// writer writes (drafts, history, recovery) and what they were doing (the
// session, recent files, settings), are their owner's alone whatever the
// umask: folders 0700, files 0600. The writer's documents and exports are
// not among them and keep the usual permissions.
namespace PrivateFiles {

// Makes the folder and those above it that are missing, each 0700. False,
// with the system's reason, when one cannot be made.
bool makeFolder(const QString &path, QString *reason = nullptr);
// Makes an open file, such as a QSaveFile before it is committed, its
// owner's alone.
bool restrict(QFileDevice &file);
// parch's own folders beneath `paths`, and any others given, such as the
// cache of drawings.
QStringList folders(const AppPaths &paths, const QStringList &others = {});
// Takes the group's and others' permissions away from each folder and from
// everything in it, never through a link, saying nothing of what it cannot
// change. For the files earlier versions made; it reads every folder, so it
// runs off the UI thread.
void tighten(const QStringList &folders);

} // namespace PrivateFiles
