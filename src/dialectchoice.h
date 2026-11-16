#pragma once

#include "mdtree.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

// Which Markdown dialect a command run from the terminal reads a document
// in: the profile, the extensions, and the library folder that `/rooted`
// includes are found in, each with where it came from. The choices come in
// layers, and a higher layer wins over a lower one:
//
//   flag      what the command line says: --profile, --extension,
//             --no-extension, --library
//   document  with --use-saved-settings only: what parch keeps for the
//             document (see documentsettings.h)
//   library   with --use-saved-settings only: what parch keeps for the
//             library folder that holds the document
//   default   the stable defaults: the Writer profile with all of its
//             extensions, and no library
//
// The profile is that of the highest layer that names one. The extensions
// start as that profile's own, and each layer's switches apply on top, from
// the lowest layer to the highest. Nothing the window was last set to is
// ever read, so a command gives the same result wherever it runs.
namespace Dialect {

// What one layer chooses; anything it leaves out is left to the layers below.
struct Layer {
    std::optional<md::Profile> profile;
    // Extension names and whether each is on, in the order they apply.
    QList<std::pair<QString, bool>> extensions;
    QString libraryRoot;

    bool isEmpty() const { return !profile && extensions.isEmpty() && libraryRoot.isEmpty(); }
};

struct Choice {
    md::Options options;
    QString libraryRoot; // empty when there is none
    // Where each came from: "flag", "document", "library" or "default".
    QString profileFrom = QStringLiteral("default");
    QString libraryRootFrom = QStringLiteral("default");
    // The extensions a layer turned on or off against the profile's own, and
    // the layer that did.
    QHash<QString, QString> extensionsFrom;
    bool savedSettings = false;

    // For JSON results:
    // {"version": 1, "profile": "writer", "extensions": [...on...],
    //  "libraryRoot": path or null, "savedSettings": false,
    //  "from": {"profile": "default", "libraryRoot": "default",
    //           "extensions": {"math": "flag", ...}}}
    QJsonObject toJson() const;
    // One line for people: "writer profile", "standard profile, with math".
    QString describe() const;
};

// Highest layer first in precedence: flags over document over library.
Choice choose(const Layer &flags, const Layer &document = {}, const Layer &library = {});

// "writer" or "standard", in any case; nothing for another name.
std::optional<md::Profile> profileNamed(const QString &name);
QStringList profileNames();

} // namespace Dialect
