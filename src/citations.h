#pragma once

#include "cslengine.h"

#include <QHash>
#include <QList>
#include <QString>

#include <memory>

// Citations as the Markdown parser hands them over, and what they become:
// formatted citations, a bibliography and diagnostics. Writer citations
// ("[p. 23][#Doe:2006]") and Pandoc citations ("[see @doe, p. 23]") are the
// same data here. Everything is plain data, so formatting can run on a
// worker thread with a snapshot of the document's citations.

struct CitationItemSpec {
    // The key, with or without its leading '#' or '@'.
    QString key;
    // The locator as written, with its label: "p. 23", "chap. 2", "23-25".
    QString locator;
    QString prefix;
    QString suffix;
    CslCite::Mode mode = CslCite::Mode::Normal;
};

struct CitationClusterSpec {
    QList<CitationItemSpec> items;
    // The footnote the citation is in, from 1, or 0 in running text.
    int noteIndex = 0;
    // The file the citation is in, for documents assembled from several;
    // empty for the root document.
    QString sourceFile;
};

// A Writer bibliography definition, "[#Doe:2006]: John Doe. *A Sample
// Book*. Example Press, 2006.", with its text already formatted.
struct WriterDefinition {
    QString key;
    CslText text;
    QString sourceFile;
};

struct CitationDiagnostic {
    enum class Code {
        MissingKey,
        DuplicateDefinition,
        AmbiguousKey,
        Database,
        NoStyle,
        SeparateNumbering,
        Locale,
        FormattingLimit,
    };
    enum class Severity { Warning, Error };
    Code code = Code::MissingKey;
    Severity severity = Severity::Warning;
    QString message;
    QString key;
    // The citation and the item in it, or -1.
    int cluster = -1;
    int item = -1;
    // A database's identity or a document file.
    QString source;
    int line = 0;
};

struct CitationBibliographyEntry {
    QString key;
    CslText text;
    // A Writer definition's number, by first use; 0 for an entry the style formats.
    int number = 0;
    // The database or file that defines the entry.
    QString source;
};

struct CitationOutput {
    // One per citation given, in order. In a note style each is a note's text.
    QList<CslText> clusters;
    QList<CitationBibliographyEntry> bibliography;
    CslBibliographyLayout layout;
    bool noteStyle = false;
    QList<CitationDiagnostic> diagnostics;
};

class CitationFormatter {
public:
    using Database = std::shared_ptr<const BibliographyDatabase>;

    CitationFormatter();

    // The CSL style, explicitly chosen; without one, database citations show
    // their keys and a diagnostic says why.
    void setStyle(std::shared_ptr<const CslStyle> style) { m_style = std::move(style); }
    // The locale; the bundled en-US unless set.
    void setLocale(std::shared_ptr<const CslLocale> locale) { m_locale = std::move(locale); }
    // The root document's databases, searched in order.
    void setDatabases(QList<Database> databases) { m_databases = std::move(databases); }
    // The databases of one included file; a file without its own uses the root's.
    void setDatabasesFor(const QString &sourceFile, QList<Database> databases)
    {
        m_fileDatabases.insert(sourceFile, std::move(databases));
    }

    CitationOutput format(const QList<CitationClusterSpec> &clusters,
                          const QList<WriterDefinition> &definitions = {}) const;

private:
    std::shared_ptr<const CslStyle> m_style;
    std::shared_ptr<const CslLocale> m_locale;
    QList<Database> m_databases;
    QHash<QString, QList<Database>> m_fileDatabases;
};

// The styles and locale parch bundles, and the reading of others.
namespace CslCatalog {

struct StyleInfo {
    // The short identifier, as a document's settings or front matter name it ("apa").
    QString id;
    QString title;
    // The locale the style is written for: "en-GB" for Cite Them Right,
    // whose quotation marks and dates are British; "en-US" otherwise.
    QString locale;
};

QList<StyleInfo> bundledStyles();
// The id of the bundled style a name stands for, its other names
// ("vancouver", "mla", "harvard") and any case included; empty for none.
QString bundledId(const QString &name);
std::shared_ptr<const CslStyle> bundledStyle(const QString &id, QString *error = nullptr);
// A style chosen by the writer, from a .csl file.
std::shared_ptr<const CslStyle> styleFromFile(const QString &path, QString *error = nullptr);
// The bundled en-US locale.
std::shared_ptr<const CslLocale> defaultLocale();
std::shared_ptr<const CslLocale> localeFromFile(const QString &path, QString *error = nullptr);
// A locale for a language such as "de-DE": a bundled one (en-US, en-GB),
// else locales-de-DE.xml from the folders given, in order, else en-US, with
// an error that says so.
std::shared_ptr<const CslLocale> locale(const QString &language, const QStringList &folders = {},
                                        QString *error = nullptr);
// The locale a style asks for: its default-locale, else the one a bundled
// style is written for, else en-US.
QString localeFor(const CslStyle &style);

} // namespace CslCatalog
