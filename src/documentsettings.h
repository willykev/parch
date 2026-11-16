#pragma once

#include "apppaths.h"
#include "dialectchoice.h"

#include <QString>

// The Markdown settings parch keeps for documents and for library folders,
// never in the documents themselves: AppPaths::documentSettingsFile(), JSON.
//
//   {
//     "documents": {
//       "<document id>": {"profile": "standard",
//                         "extensions": {"math": true, "footnotes": false},
//                         "libraryRoot": "/full/path"}
//     },
//     "libraries": {
//       "/full/path/of/a/library/folder": {"profile": "writer",
//                                          "extensions": {"citations": false}}
//     }
//   }
//
// A document is known by its id, which AppPaths::registryFile() gives its
// canonical path, so its settings follow it when parch sees it renamed.
// Every key of an entry is optional. A library's entry applies to every
// document beneath its folder that has no setting of its own; a document in
// one of the library folders the window lists finds `/rooted` includes in
// that folder unless a setting says otherwise.
//
// Commands run from the terminal read these only when asked to, with
// --use-saved-settings (see dialectchoice.h).
namespace DocumentSettings {

struct Saved {
    Dialect::Layer document;
    Dialect::Layer library;
    // The library folder that holds the document, the nearest when several
    // do; empty when none does.
    QString libraryFolder;
};

// What is kept for the document at `canonicalPath`. Nothing kept is no
// error; a file that is there but cannot be read as settings is, with why.
bool read(const AppPaths &paths, const QString &canonicalPath, Saved *saved, QString *error);

} // namespace DocumentSettings
