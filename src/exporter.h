#pragma once

#include "dialectchoice.h"
#include "documentcitations.h"
#include "mdtree.h"
#include "renderservice.h"

#include <functional>
#include <memory>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class ExportPictures;

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
// The window's Export dialog shows the same options: a check box for a
// switch, a list for an option with choices, and a field otherwise.
struct ExportOption {
    QString name;
    QString description;
    QString valueName; // empty for a switch
    // What the dialog calls it: "Page size".
    QString label;
    // The values it takes, when it takes one of a few.
    QStringList choices;
    // The value when none is given.
    QString defaultValue;
    // A number, from `minimum` to `maximum`.
    bool number = false;
    double minimum = 0;
    double maximum = 0;
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
    // The output is a symbolic link the writer chose to replace: the link
    // itself is replaced by the export, and what it leads to is left as it
    // is. Otherwise a link at the output is refused (Exporters::writeFile).
    bool replaceLink = false;

    QJsonObject toJson() const;
};

struct ExportResult {
    bool ok = false;
    QString error; // why nothing was written
    // The output could not be written, as against the export failing.
    bool ioFailure = false;
    // It stopped because it was asked to; nothing was written.
    bool cancelled = false;
    QStringList written; // every file written, the output first
    // What the format could not carry over as the document has it, for the
    // writer to know: "Math is written as TeX source (3 formulas)".
    QStringList limitations;
    // What drew the output: renderer and helper names with their versions.
    QJsonObject renderers;
    QJsonObject helpers;
};

// What an export runs with besides what it was asked to do: when to stop,
// where to say how far it has come, and how its formulas and diagrams are
// drawn. None of it is part of the recipe.
struct ExportRun {
    // Asked between steps; true stops the export, which then writes nothing.
    std::function<bool()> cancelled;
    // How far it has come, from 0 to 1, and what it is doing.
    std::function<void(qreal done, const QString &step)> progress;
    // Where the math and diagram helpers are and where their drawings are
    // kept; found as the application finds them unless set.
    RenderService::Options render;
    // Without pictures, formulas and diagrams stay their source.
    bool pictures = true;

    bool stopped() const { return cancelled && cancelled(); }
    void report(qreal done, const QString &step) const
    {
        if (progress) progress(done, step);
    }
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
    // What the format will not carry over as the document has it, said
    // before anything is written: what write() then reports, as far as it
    // can be known without drawing anything. `helpers` are the math and
    // diagram helpers the export would draw with.
    virtual QStringList limitations(const md::Document &document, const ExportRecipe &recipe,
                                    const HelperSet &helpers) const
    {
        Q_UNUSED(document);
        Q_UNUSED(recipe);
        Q_UNUSED(helpers);
        return {};
    }
    // Writes the document to recipe.output, replacing whatever is there in
    // one step, so that a failed or cancelled export leaves the old file as
    // it was. Safe to run on any thread; the thread waits for the helpers
    // in an event loop of its own while they draw.
    virtual ExportResult write(const md::Document &document, const ExportRecipe &recipe,
                               const ExportRun &run = ExportRun()) const = 0;

    // The value of one of its options in a recipe: as given, else its default.
    QString option(const ExportRecipe &recipe, const QString &name) const;
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

// Writes a whole file in one step: a temporary file beside it, made anew,
// renamed over it. Never through a symbolic link: a link at `path` is
// refused, saying where it leads (linkRefusal()), unless `replaceLink`,
// when the link itself is replaced and what it leads to is left as it is.
// A file replaced keeps its permissions.
bool writeFile(const QString &path, const QByteArray &bytes, QString *error, bool replaceLink = false);
// Why an export does not write `path`: it is a symbolic link, "X is a link
// to Y; parch never writes through a link". Empty when it is not one.
QString linkRefusal(const QString &path);

// writeFile() in two steps, for an export that writes other files between
// them: write() puts the bytes in the temporary file beside `path`, and
// commit() puts it in place. A file never committed is removed.
class PendingFile {
public:
    PendingFile(const QString &path, bool replaceLink = false);
    ~PendingFile();
    PendingFile(const PendingFile &) = delete;
    PendingFile &operator=(const PendingFile &) = delete;
    bool write(const QByteArray &bytes, QString *error);
    bool commit(QString *error);

private:
    bool fail(const QString &why, QString *error);

    QString m_path;
    bool m_replaceLink = false;
    int m_folder = -1;
    QByteArray m_name;
    QByteArray m_temporary;
};

// A local file as a link or image in a file written to `outputFolder`
// gives it: relative to that folder.
QString relativeTo(const QString &outputFolder, const QString &absolutePath);

// "1 formula", "3 formulas".
QString count(int n, const QString &one, const QString &many);

// Draws the formulas and diagrams of the document for an export, at a body
// size in logical pixels, telling the run how far it has come up to
// `share` of the whole. Null when there is nothing to draw or the run wants
// no pictures; also null, with `cancelled` set, when the run was stopped.
std::unique_ptr<ExportPictures> drawPictures(const md::Document &document, const ExportRun &run, qreal fontSize,
                                             qreal share, bool *cancelled);
// The result of an export that was stopped.
ExportResult cancelled();

// The document's title, as exports name it: the recipe's `title` option,
// else the front matter's, else the first heading, else the file's name.
QString title(const md::Document &document, const ExportRecipe &recipe);
// A front matter value as one line of text; empty when it has none.
QString frontMatterText(const md::Document &document, const QString &key);
// The front matter's `lang` or `language`.
QString language(const md::Document &document);

} // namespace Exporters
