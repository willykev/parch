#pragma once

#include "mdtree.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

// Which Markdown dialect a document is read in: the profile, the
// extensions, the library folder that `/rooted` includes are found in, the
// bibliography files and the citation style, each with where it came from.
// The choices come in layers, and a higher layer wins over a lower one:
//
//   flag          what the command line says: --profile, --extension,
//                 --no-extension, --library, --bibliography, --csl
//   document      what parch keeps for the document (documentsettings.h);
//                 from the terminal only with --use-saved-settings
//   front-matter  the document's own front matter: `bibliography:` and
//                 `csl:` only, since the profile is never read from the text
//   library       what parch keeps for the library folder that holds the
//                 document; from the terminal only with --use-saved-settings
//   application   what parch keeps for every document (Preferences); from
//                 the terminal only with --use-saved-settings
//   default       the stable defaults: the Writer profile with all of its
//                 extensions, no library, no bibliography and no style
//
// The profile is that of the highest layer that names one. The extensions
// start as that profile's own, and each layer's switches apply on top, from
// the lowest layer to the highest. The library root, the bibliography and the
// style are each that of the highest layer that names one; a bibliography is
// a list as a whole, never merged with a lower layer's.
namespace Dialect {

// What one layer chooses; anything it leaves out is left to the layers below.
struct Layer {
    std::optional<md::Profile> profile;
    // Extension names and whether each is on, in the order they apply.
    QList<std::pair<QString, bool>> extensions;
    QString libraryRoot;
    // The bibliography files, full paths (a front matter's may stay
    // relative when its document has no folder). Set, even to an empty
    // list, the layer chooses them; unset, it leaves them to the layers below.
    std::optional<QStringList> bibliography;
    // A bundled style's id ("apa") or the full path of a .csl file; empty
    // leaves the style to the layers below.
    QString citationStyle;

    bool isEmpty() const
    {
        return !profile && extensions.isEmpty() && libraryRoot.isEmpty() && !bibliography && citationStyle.isEmpty();
    }
    bool operator==(const Layer &) const = default;
};

// Every layer but the defaults, highest first.
struct Layers {
    Layer flags;
    Layer document;
    Layer frontMatter;
    Layer library;
    Layer application;

    bool operator==(const Layers &) const = default;
};

struct Choice {
    md::Options options;
    QString libraryRoot;       // empty when there is none
    QStringList bibliography;  // the files, in the order they are searched
    QString citationStyle;     // empty when none is chosen
    // Where each came from: "flag", "document", "front-matter", "library",
    // "application" or "default".
    QString profileFrom = QStringLiteral("default");
    QString libraryRootFrom = QStringLiteral("default");
    QString bibliographyFrom = QStringLiteral("default");
    QString citationStyleFrom = QStringLiteral("default");
    // The extensions a layer turned on or off against the profile's own, and
    // the layer that did.
    QHash<QString, QString> extensionsFrom;
    // A layer parch keeps (document, library or application) chose something.
    bool savedSettings = false;

    // For JSON results:
    // {"version": 1, "profile": "writer", "extensions": [...on...],
    //  "libraryRoot": path or null, "bibliography": [paths],
    //  "citationStyle": id or path or null, "savedSettings": false,
    //  "from": {"profile": "default", "libraryRoot": "default",
    //           "bibliography": "front-matter", "citationStyle": "flag",
    //           "extensions": {"math": "flag", ...}}}
    QJsonObject toJson() const;
    // One line for people: "writer profile", "standard profile, with math".
    QString describe() const;
};

// The choice the layers make together.
Choice effective(const Layers &layers);
// Highest layer first in precedence: flags over document over library.
Choice choose(const Layer &flags, const Layer &document = {}, const Layer &library = {});

// "writer" or "standard", in any case; nothing for another name.
std::optional<md::Profile> profileNamed(const QString &name);
QStringList profileNames();
// The names of the layers, highest first, as Choice gives them.
QStringList layerNames();

// What a document's front matter chooses: `bibliography:` (one file or a
// list of them) and `csl:` (a .csl file, or the id of a bundled style).
// Relative paths are found from `folder`, the document's own; without one
// they stay as written. Every other key is left alone.
Layer frontMatterLayer(const md::FrontMatter &frontMatter, const QString &folder);
// The front matter's `link-citations: true`: citations link to their
// entries in the bibliography.
bool linkCitations(const md::FrontMatter &frontMatter);

// A citation style as written in front matter or on the command line: the
// id of a bundled style ("apa", or "apa.csl" when no such file is there),
// or else the full path of a .csl file, found from `folder`.
QString styleReference(const QString &written, const QString &folder);
// Whether a style reference names a file rather than a bundled style.
bool isStyleFile(const QString &style);

} // namespace Dialect
