#pragma once

#include "dialectchoice.h"
#include "documentcitations.h"
#include "mdtree.h"

#include <memory>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// The formats parch exports to, behind one interface, so that the command
// line and the window write each one the same way. An exporter is given a
// document already parsed, its includes assembled, in the dialect chosen for
// it, and writes it to a file; it never reads the Markdown again, and never
// writes to the document or to a file the document includes.
//
// To add a format, write an Exporter and list it in Exporters::all(). The
// command line takes its name for --to, its suffixes for -o, and its
// options (ExportOption) as options of `parch export`, and lists them in
// `parch help export`.

// An option of one format, as the command line spells it without dashes.
struct ExportOption {
    QString name;
    QString description;
    QString valueName; // empty for a switch
};

// What an export was asked to do, kept so that it can be done again the
// same way: the document, the dialect it was read in, the format, its
// options and where it went.
struct ExportRecipe {
    QString source; // canonical path of the document
    QString format;
    QString output; // absolute path of the file written
    Dialect::Choice dialect;
    // The format's options that were given; a switch has an empty value.
    QHash<QString, QString> options;
    // The document's citations as its style formats them, read with the
    // document; none leaves the engine's own numbering.
    std::shared_ptr<const DocumentCitations::Formatted> citations;

    QJsonObject toJson() const;
};

struct ExportResult {
    bool ok = false;
    QString error; // why nothing was written
    // The output could not be written, as against the export failing.
    bool ioFailure = false;
    QStringList written; // every file written, the output first
    // What the format could not carry over as the document has it, for the
    // writer to know: "Math is written as TeX source (3 formulas)".
    QStringList limitations;
    // What drew the output: renderer and helper names with their versions.
    QJsonObject renderers;
    QJsonObject helpers;
};

class Exporter {
public:
    virtual ~Exporter() = default;

    // The name --to takes: "html".
    virtual QString format() const = 0;
    // As people know it: "HTML".
    virtual QString title() const = 0;
    // The suffixes of its files, the usual one first: {"html", "htm"}.
    virtual QStringList suffixes() const = 0;
    virtual QList<ExportOption> options() const { return {}; }
    // Whether this build can write the format, and why not when it cannot.
    virtual bool available(QString *why = nullptr) const
    {
        Q_UNUSED(why);
        return true;
    }
    // Writes the document to recipe.output, replacing whatever is there in
    // one step, so that a failed export leaves the old file as it was.
    // Safe to run on any thread.
    virtual ExportResult write(const md::Document &document, const ExportRecipe &recipe) const = 0;
};

namespace Exporters {

// Every format, in the order they are listed.
QList<const Exporter *> all();
QStringList formats();
const Exporter *find(const QString &format);
// The format a file name's suffix stands for, if any: "notes.html" is html.
const Exporter *forFile(const QString &path);

// Whether `path` is the document or one of the files it includes, which no
// export may write.
bool isSource(const md::Document &document, const QString &path);

// Writes a whole file in one step (a temporary file renamed over it).
bool writeFile(const QString &path, const QByteArray &bytes, QString *error);

// A local file as a link or image in a file written to `outputFolder`
// gives it: relative to that folder.
QString relativeTo(const QString &outputFolder, const QString &absolutePath);

} // namespace Exporters
