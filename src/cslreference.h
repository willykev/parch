#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

// One personal or institutional name, in the parts CSL distinguishes.
struct CslName {
    QString family;
    QString given;
    QString droppingParticle;
    QString nonDroppingParticle;
    QString suffix;
    // A name that is never split or initialised, such as an institution.
    QString literal;
    bool commaSuffix = false;
    bool staticOrdering = false;

    bool isEmpty() const { return family.isEmpty() && given.isEmpty() && literal.isEmpty(); }
    bool operator==(const CslName &other) const = default;
};

// A point in a date: a year, optionally a month (13 to 16 are the seasons
// spring to winter) and a day. A count of zero means no point at all.
struct CslDatePoint {
    int year = 0;
    int month = 0;
    int day = 0;
    // How many of year, month and day are set.
    int parts = 0;

    bool operator==(const CslDatePoint &other) const = default;
};

struct CslDate {
    CslDatePoint start;
    // Set for a range; a range with an open end has isRange and no end parts.
    CslDatePoint end;
    bool isRange = false;
    // An uncertain or approximate date, which CSL calls circa.
    bool circa = false;
    // A season given apart from the month, 1 to 4, or a season's name.
    QString season;
    // Text that could not be read as a date, shown as it is.
    QString literal;

    bool isEmpty() const { return start.parts == 0 && literal.isEmpty(); }
    bool operator==(const CslDate &other) const = default;
    // Reads an EDTF-like date: "2006", "2006-05", "2006-05-01", "2006/2007",
    // "2006-05-01/2006-05-03", "2006/", "2006?" and "2006~" (uncertain).
    // Text that is not such a date becomes a literal date.
    static CslDate fromText(const QString &text);
};

// One bibliographic item in CSL's model. Ordinary and number variables are
// strings, which may hold the inline markup CSL JSON allows (<i>, <b>,
// <sup>, <sub>, small-caps and nocase spans).
struct CslReference {
    QString id;
    QString type;
    QHash<QString, QString> fields;
    QHash<QString, QList<CslName>> names;
    QHash<QString, CslDate> dates;

    QString field(const QString &variable) const { return fields.value(variable); }
    bool hasVariable(const QString &variable) const;

    static bool isNameVariable(const QString &variable);
    static bool isDateVariable(const QString &variable);
    static bool isNumberVariable(const QString &variable);
};

struct BibliographyDiagnostic {
    enum class Severity { Warning, Error };
    Severity severity = Severity::Warning;
    QString message;
    // The entry the diagnostic is about, if any.
    QString key;
    // A line in the source file, from 1, or 0 when unknown.
    int line = 0;
};

// One bibliography database, read from a BibTeX/BibLaTeX, CSL JSON or CSL
// YAML file. Its identity keeps its keys apart from those of every other
// database: two databases may hold the same key for different works.
class BibliographyDatabase {
public:
    enum class Format { Auto, BibLaTeX, CslJson, CslYaml };

    BibliographyDatabase() = default;
    // Reads a file; the format comes from its extension when Auto.
    static BibliographyDatabase fromFile(const QString &path, Format format = Format::Auto);
    static BibliographyDatabase fromData(const QByteArray &data, Format format, const QString &identity);
    static Format formatForPath(const QString &path);

    QString identity() const { return m_identity; }
    // False when the file could not be read at all.
    bool isValid() const { return m_valid; }
    const QList<CslReference> &references() const { return m_references; }
    const CslReference *find(const QString &key) const;
    const QList<BibliographyDiagnostic> &diagnostics() const { return m_diagnostics; }

private:
    void add(CslReference reference, int line);

    QString m_identity;
    bool m_valid = false;
    QList<CslReference> m_references;
    QHash<QString, qsizetype> m_index;
    QList<BibliographyDiagnostic> m_diagnostics;
};

namespace CslJson {
// Reads one CSL JSON item. Names, dates and numbers in the forms CSL JSON
// and its YAML variant use are accepted; unknown variables are kept.
CslReference reference(const QJsonObject &item, QString *error = nullptr);
QList<CslName> names(const QJsonValue &value);
CslDate date(const QJsonValue &value);
} // namespace CslJson

namespace CslYaml {
// Converts a YAML document to JSON with libyaml. Scalars become strings,
// mappings objects and sequences arrays; anchors and aliases are followed.
QJsonValue toJson(const QByteArray &yaml, QString *error, int *errorLine = nullptr);
} // namespace CslYaml
