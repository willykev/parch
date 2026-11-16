#include "cslreference.h"

#include "bibtex.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <yaml.h>

namespace {

const QSet<QString> &nameVariables()
{
    static const QSet<QString> variables{
        QStringLiteral("author"),           QStringLiteral("chair"),
        QStringLiteral("collection-editor"), QStringLiteral("compiler"),
        QStringLiteral("composer"),         QStringLiteral("container-author"),
        QStringLiteral("contributor"),      QStringLiteral("curator"),
        QStringLiteral("director"),         QStringLiteral("editor"),
        QStringLiteral("editorial-director"), QStringLiteral("editor-translator"),
        QStringLiteral("executive-producer"), QStringLiteral("guest"),
        QStringLiteral("host"),             QStringLiteral("illustrator"),
        QStringLiteral("interviewer"),      QStringLiteral("narrator"),
        QStringLiteral("organizer"),        QStringLiteral("original-author"),
        QStringLiteral("performer"),        QStringLiteral("producer"),
        QStringLiteral("recipient"),        QStringLiteral("reviewed-author"),
        QStringLiteral("script-writer"),    QStringLiteral("series-creator"),
        QStringLiteral("translator"),
    };
    return variables;
}

const QSet<QString> &dateVariables()
{
    static const QSet<QString> variables{
        QStringLiteral("accessed"), QStringLiteral("available-date"), QStringLiteral("event-date"),
        QStringLiteral("issued"),   QStringLiteral("original-date"),  QStringLiteral("submitted"),
    };
    return variables;
}

const QSet<QString> &numberVariables()
{
    static const QSet<QString> variables{
        QStringLiteral("chapter-number"), QStringLiteral("citation-number"),
        QStringLiteral("collection-number"), QStringLiteral("edition"),
        QStringLiteral("first-reference-note-number"), QStringLiteral("issue"),
        QStringLiteral("locator"),        QStringLiteral("number"),
        QStringLiteral("number-of-pages"), QStringLiteral("number-of-volumes"),
        QStringLiteral("page"),           QStringLiteral("page-first"),
        QStringLiteral("part-number"),    QStringLiteral("printing-number"),
        QStringLiteral("section"),        QStringLiteral("supplement-number"),
        QStringLiteral("version"),        QStringLiteral("volume"),
    };
    return variables;
}

} // namespace

bool CslReference::isNameVariable(const QString &variable)
{
    return nameVariables().contains(variable);
}

bool CslReference::isDateVariable(const QString &variable)
{
    return dateVariables().contains(variable);
}

bool CslReference::isNumberVariable(const QString &variable)
{
    return numberVariables().contains(variable);
}

bool CslReference::hasVariable(const QString &variable) const
{
    if (const auto it = names.constFind(variable); it != names.constEnd()) return !it->isEmpty();
    if (const auto it = dates.constFind(variable); it != dates.constEnd()) return !it->isEmpty();
    return !fields.value(variable).isEmpty();
}

namespace {

// Reads one end of an EDTF date: [-]YYYY[-MM[-DD]], where a month of 21 to
// 24 is a season.
bool readPoint(const QString &text, CslDatePoint &point)
{
    static const QRegularExpression pattern(QStringLiteral("^(-?\\d{1,4})(?:-(\\d{1,2})(?:-(\\d{1,2}))?)?$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) return false;
    point = {};
    point.year = match.captured(1).toInt();
    point.parts = 1;
    if (!match.captured(2).isEmpty()) {
        int month = match.captured(2).toInt();
        if (month >= 21 && month <= 24) month -= 8;
        if (month < 1 || month > 16) return false;
        point.month = month;
        point.parts = 2;
        if (!match.captured(3).isEmpty()) {
            const int day = match.captured(3).toInt();
            if (day < 1 || day > 31 || month > 12) return false;
            point.day = day;
            point.parts = 3;
        }
    }
    // The year 0 is 1 BC in EDTF; CSL has no year zero, so it stays as given.
    return true;
}

int intOf(const QJsonValue &value, bool *ok)
{
    if (value.isDouble()) {
        *ok = true;
        return value.toInt();
    }
    const QString text = value.toString().trimmed();
    return text.toInt(ok);
}

CslDatePoint pointFromParts(const QJsonArray &parts)
{
    CslDatePoint point;
    for (int i = 0; i < parts.size() && i < 3; ++i) {
        bool ok = false;
        const int value = intOf(parts[i], &ok);
        if (!ok) break;
        if (i == 0) point.year = value;
        if (i == 1) {
            if (value >= 21 && value <= 24)
                point.month = value - 8;
            else if (value >= 1 && value <= 16)
                point.month = value;
            else
                break;
        }
        if (i == 2) {
            if (value < 1 || value > 31) break;
            point.day = value;
        }
        point.parts = i + 1;
    }
    return point;
}

bool truthy(const QJsonValue &value)
{
    if (value.isBool()) return value.toBool();
    if (value.isDouble()) return value.toDouble() != 0;
    const QString text = value.toString().trimmed().toLower();
    return text == u"true" || text == u"1" || text == u"yes";
}

QString scalarText(const QJsonValue &value)
{
    if (value.isString()) return value.toString();
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (number == double(qint64(number))) return QString::number(qint64(number));
        return QString::number(number);
    }
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return {};
}

bool isLowerWord(const QString &word)
{
    for (const QChar c : word) {
        if (c.isLetter()) return c.isLower();
    }
    return false;
}

// Moves particles written into the family or given name into their own
// parts, as CSL processors do for names without explicit particles:
// "van Gogh" has the non-dropping particle "van", and "d'Alembert" "d'".
void parseParticles(CslName &name)
{
    if (!name.nonDroppingParticle.isEmpty() || !name.droppingParticle.isEmpty()) return;
    // A quoted family name is taken as it is: "\"Van Dyke\"".
    if (name.family.size() > 1 && name.family.startsWith(u'"') && name.family.endsWith(u'"')) {
        name.family = name.family.mid(1, name.family.size() - 2);
        return;
    }
    static const QRegularExpression apostrophe(QStringLiteral("^((?:\\p{Ll}+\\s+)*\\p{Ll}+['’-])(\\p{Lu}.*)$"));
    if (const QRegularExpressionMatch match = apostrophe.match(name.family); match.hasMatch()) {
        name.nonDroppingParticle = match.captured(1);
        name.family = match.captured(2);
    } else {
        QStringList words = name.family.split(u' ', Qt::SkipEmptyParts);
        QStringList particles;
        while (words.size() > 1 && isLowerWord(words.first())) particles.append(words.takeFirst());
        if (!particles.isEmpty()) {
            name.nonDroppingParticle = particles.join(u' ');
            name.family = words.join(u' ');
        }
    }
    // "John, Jr." holds a suffix; "John,! Jr." one written after a comma.
    if (const qsizetype comma = name.given.indexOf(u','); comma > 0 && name.suffix.isEmpty()) {
        QString suffix = name.given.mid(comma + 1);
        if (suffix.startsWith(u'!')) {
            suffix = suffix.mid(1);
            name.commaSuffix = true;
        }
        name.suffix = suffix.trimmed();
        name.given = name.given.left(comma).trimmed();
        if (name.suffix.startsWith(u'!')) {
            name.suffix = name.suffix.mid(1).trimmed();
            name.commaSuffix = true;
        }
    }
    QStringList given = name.given.split(u' ', Qt::SkipEmptyParts);
    QStringList trailing;
    while (given.size() > 1 && isLowerWord(given.last())) trailing.prepend(given.takeLast());
    if (!trailing.isEmpty()) {
        name.droppingParticle = trailing.join(u' ');
        name.given = given.join(u' ');
    }
}

} // namespace

CslDate CslDate::fromText(const QString &input)
{
    CslDate date;
    QString text = input.trimmed();
    if (text.isEmpty()) return date;
    // A time of day says nothing CSL can show.
    static const QRegularExpression time(QStringLiteral("T\\d{2}:\\d{2}.*$"));
    text.remove(time);
    while (!text.isEmpty() && (text.endsWith(u'?') || text.endsWith(u'~') || text.endsWith(u'%'))) {
        date.circa = true;
        text.chop(1);
    }
    const qsizetype slash = text.indexOf(u'/');
    QString startText = slash < 0 ? text : text.left(slash);
    QString endText = slash < 0 ? QString() : text.mid(slash + 1);
    auto stripMarks = [&date](QString &part) {
        while (!part.isEmpty() && (part.endsWith(u'?') || part.endsWith(u'~') || part.endsWith(u'%'))) {
            date.circa = true;
            part.chop(1);
        }
    };
    stripMarks(startText);
    stripMarks(endText);
    if (!readPoint(startText, date.start)) {
        CslDate literal;
        literal.literal = input.trimmed();
        return literal;
    }
    if (slash >= 0) {
        date.isRange = true;
        if (!endText.isEmpty() && endText != u".." && !readPoint(endText, date.end)) {
            CslDate literal;
            literal.literal = input.trimmed();
            return literal;
        }
    }
    return date;
}

QList<CslName> CslJson::names(const QJsonValue &value)
{
    QList<CslName> names;
    const QJsonArray list = value.isArray() ? value.toArray() : QJsonArray{value};
    for (const QJsonValue &entry : list) {
        CslName name;
        if (entry.isString()) {
            name.literal = entry.toString().trimmed();
        } else if (entry.isObject()) {
            const QJsonObject object = entry.toObject();
            name.family = scalarText(object.value(QStringLiteral("family"))).trimmed();
            name.given = scalarText(object.value(QStringLiteral("given"))).trimmed();
            name.droppingParticle = scalarText(object.value(QStringLiteral("dropping-particle"))).trimmed();
            name.nonDroppingParticle = scalarText(object.value(QStringLiteral("non-dropping-particle"))).trimmed();
            name.suffix = scalarText(object.value(QStringLiteral("suffix"))).trimmed();
            name.literal = scalarText(object.value(QStringLiteral("literal"))).trimmed();
            name.commaSuffix = truthy(object.value(QStringLiteral("comma-suffix")));
            name.staticOrdering = truthy(object.value(QStringLiteral("static-ordering")));
            const QJsonValue parse = object.value(QStringLiteral("parse-names"));
            if (name.literal.isEmpty() && (parse.isUndefined() || truthy(parse))) parseParticles(name);
        }
        if (!name.isEmpty()) names.append(name);
    }
    return names;
}

CslDate CslJson::date(const QJsonValue &value)
{
    if (value.isString() || value.isDouble()) return CslDate::fromText(scalarText(value));
    CslDate date;
    if (value.isArray()) {
        // The older YAML form: a list of one or two {year, month, day} maps.
        const QJsonArray list = value.toArray();
        QList<CslDatePoint> points;
        for (const QJsonValue &entry : list) {
            const QJsonObject object = entry.toObject();
            QJsonArray parts;
            for (const char *key : {"year", "month", "day"}) {
                const QJsonValue part = object.value(QLatin1String(key));
                if (part.isUndefined()) break;
                parts.append(part);
            }
            points.append(pointFromParts(parts));
        }
        if (!points.isEmpty()) date.start = points[0];
        if (points.size() > 1) {
            date.isRange = true;
            date.end = points[1];
        }
        return date;
    }
    const QJsonObject object = value.toObject();
    if (object.contains(QStringLiteral("year"))) {
        QJsonArray parts;
        for (const char *key : {"year", "month", "day"}) {
            const QJsonValue part = object.value(QLatin1String(key));
            if (part.isUndefined()) break;
            parts.append(part);
        }
        date.start = pointFromParts(parts);
    }
    const QJsonArray dateParts = object.value(QStringLiteral("date-parts")).toArray();
    bool ok = false;
    if (!dateParts.isEmpty()) {
        date.start = pointFromParts(dateParts[0].toArray());
        if (dateParts.size() > 1) {
            CslDatePoint end = pointFromParts(dateParts[1].toArray());
            // An end of [0] or [] leaves the range open, as in "1987–".
            if (end.parts > 0 && end.year == 0) end = {};
            if (end.parts > 0 || dateParts[1].toArray().isEmpty() || scalarText(dateParts[1].toArray().first()).isEmpty()
                || intOf(dateParts[1].toArray().first(), &ok) == 0) {
                date.isRange = true;
                date.end = end;
                // An end equal to the start is not a range.
                if (end == date.start) {
                    date.isRange = false;
                    date.end = {};
                }
            }
        }
    }
    if (const QJsonValue season = object.value(QStringLiteral("season")); !season.isUndefined()) {
        date.season = scalarText(season).trimmed();
        bool ok = false;
        const int number = date.season.toInt(&ok);
        if (ok && number >= 1 && number <= 4 && date.start.parts == 1) {
            date.start.month = number + 12;
            date.start.parts = 2;
            date.season.clear();
        }
    }
    date.circa = truthy(object.value(QStringLiteral("circa")));
    if (date.start.parts == 0) {
        const QString raw = scalarText(object.value(QStringLiteral("raw")));
        if (!raw.isEmpty()) {
            const bool circa = date.circa;
            date = CslDate::fromText(raw);
            date.circa = date.circa || circa;
        }
        if (date.start.parts == 0) {
            const QString literal = scalarText(object.value(QStringLiteral("literal")));
            if (!literal.isEmpty()) date.literal = literal;
        }
    }
    return date;
}

CslReference CslJson::reference(const QJsonObject &item, QString *error)
{
    CslReference reference;
    reference.id = scalarText(item.value(QStringLiteral("id"))).trimmed();
    reference.type = scalarText(item.value(QStringLiteral("type"))).trimmed();
    if (reference.id.isEmpty() && error) *error = QStringLiteral("An item has no id.");
    for (auto it = item.constBegin(); it != item.constEnd(); ++it) {
        const QString key = it.key();
        if (key == u"id" || key == u"type") continue;
        if (CslReference::isNameVariable(key)) {
            const QList<CslName> list = CslJson::names(it.value());
            if (!list.isEmpty()) reference.names.insert(key, list);
        } else if (CslReference::isDateVariable(key)) {
            const CslDate date = CslJson::date(it.value());
            if (!date.isEmpty()) reference.dates.insert(key, date);
        } else {
            const QString text = scalarText(it.value());
            if (!text.isEmpty()) reference.fields.insert(key, text);
        }
    }
    if (reference.type.isEmpty()) reference.type = QStringLiteral("document");
    // The names older CSL JSON and Zotero use for the short forms.
    const QList<QPair<const char *, const char *>> aliases{{"journalAbbreviation", "container-title-short"},
                                                          {"shortTitle", "title-short"}};
    for (const auto &[legacy, current] : aliases) {
        const QString value = reference.fields.value(QLatin1String(legacy));
        if (!value.isEmpty() && !reference.fields.contains(QLatin1String(current)))
            reference.fields.insert(QLatin1String(current), value);
    }
    return reference;
}

namespace {

// Builds JSON from libyaml's events.
class YamlReader {
public:
    explicit YamlReader(const QByteArray &data)
    {
        yaml_parser_initialize(&m_parser);
        yaml_parser_set_input_string(&m_parser, reinterpret_cast<const unsigned char *>(data.constData()),
                                     size_t(data.size()));
    }
    ~YamlReader()
    {
        if (m_pending) yaml_event_delete(&m_peeked);
        yaml_parser_delete(&m_parser);
    }

    QJsonValue read(QString *error, int *errorLine)
    {
        QJsonValue result;
        bool haveDocument = false;
        for (;;) {
            yaml_event_t event;
            if (!next(event, error, errorLine)) return {};
            const yaml_event_type_t type = event.type;
            if (type == YAML_STREAM_END_EVENT) {
                yaml_event_delete(&event);
                break;
            }
            if (type == YAML_DOCUMENT_START_EVENT) {
                yaml_event_delete(&event);
                QJsonValue document;
                if (!value(document, error, errorLine)) return {};
                // The first document with content is the bibliography, as
                // in Markdown front matter.
                if (!haveDocument && !document.isNull()) {
                    result = document;
                    haveDocument = true;
                }
                continue;
            }
            yaml_event_delete(&event);
        }
        return result;
    }

private:
    bool next(yaml_event_t &event, QString *error, int *errorLine)
    {
        if (m_pending) {
            event = m_peeked;
            m_pending = false;
            return true;
        }
        if (yaml_parser_parse(&m_parser, &event)) return true;
        if (error) {
            *error = QString::fromUtf8(m_parser.problem ? m_parser.problem : "invalid YAML");
            if (m_parser.context) *error = QString::fromUtf8(m_parser.context) + u", " + *error;
        }
        if (errorLine) *errorLine = int(m_parser.problem_mark.line) + 1;
        return false;
    }

    // Reads the value starting at the next event, up to its end; a
    // document's end gives null.
    bool value(QJsonValue &out, QString *error, int *errorLine)
    {
        yaml_event_t event;
        if (!next(event, error, errorLine)) return false;
        bool ok = true;
        switch (event.type) {
        case YAML_DOCUMENT_END_EVENT:
            out = QJsonValue();
            break;
        case YAML_SCALAR_EVENT: {
            const QString text = QString::fromUtf8(reinterpret_cast<const char *>(event.data.scalar.value),
                                                   qsizetype(event.data.scalar.length));
            const bool plain = event.data.scalar.style == YAML_PLAIN_SCALAR_STYLE;
            out = plain && (text == u"~" || text == u"null" || text.isEmpty()) ? QJsonValue() : QJsonValue(text);
            remember(event.data.scalar.anchor, out);
            break;
        }
        case YAML_ALIAS_EVENT:
            out = m_anchors.value(QString::fromUtf8(reinterpret_cast<const char *>(event.data.alias.anchor)));
            break;
        case YAML_SEQUENCE_START_EVENT: {
            const QString anchor = anchorName(event.data.sequence_start.anchor);
            QJsonArray array;
            for (;;) {
                if (peekEnd(YAML_SEQUENCE_END_EVENT, ok, error, errorLine)) break;
                if (!ok) break;
                QJsonValue item;
                if (!value(item, error, errorLine)) {
                    ok = false;
                    break;
                }
                array.append(item);
            }
            out = array;
            if (!anchor.isEmpty()) m_anchors.insert(anchor, out);
            break;
        }
        case YAML_MAPPING_START_EVENT: {
            const QString anchor = anchorName(event.data.mapping_start.anchor);
            QJsonObject object;
            for (;;) {
                if (peekEnd(YAML_MAPPING_END_EVENT, ok, error, errorLine)) break;
                if (!ok) break;
                QJsonValue key;
                QJsonValue item;
                if (!value(key, error, errorLine) || !value(item, error, errorLine)) {
                    ok = false;
                    break;
                }
                object.insert(key.toString(), item);
            }
            out = object;
            if (!anchor.isEmpty()) m_anchors.insert(anchor, out);
            break;
        }
        default:
            out = QJsonValue();
            break;
        }
        yaml_event_delete(&event);
        return ok;
    }

    // Takes the next event if it ends the current collection; otherwise
    // keeps it for value() to read.
    bool peekEnd(yaml_event_type_t endType, bool &ok, QString *error, int *errorLine)
    {
        if (!m_pending) {
            if (!next(m_peeked, error, errorLine)) {
                ok = false;
                return false;
            }
            m_pending = true;
        }
        if (m_peeked.type == endType) {
            yaml_event_delete(&m_peeked);
            m_pending = false;
            return true;
        }
        return false;
    }

    static QString anchorName(const yaml_char_t *anchor)
    {
        return anchor ? QString::fromUtf8(reinterpret_cast<const char *>(anchor)) : QString();
    }

    void remember(const yaml_char_t *anchor, const QJsonValue &value)
    {
        if (anchor) m_anchors.insert(anchorName(anchor), value);
    }

    yaml_parser_t m_parser;
    // An event read ahead to see whether a collection ends, which the next
    // read takes.
    yaml_event_t m_peeked;
    bool m_pending = false;
    QHash<QString, QJsonValue> m_anchors;
};

} // namespace

QJsonValue CslYaml::toJson(const QByteArray &yaml, QString *error, int *errorLine)
{
    YamlReader reader(yaml);
    return reader.read(error, errorLine);
}

BibliographyDatabase::Format BibliographyDatabase::formatForPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == u"json") return Format::CslJson;
    if (suffix == u"yaml" || suffix == u"yml") return Format::CslYaml;
    return Format::BibLaTeX;
}

BibliographyDatabase BibliographyDatabase::fromFile(const QString &path, Format format)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        BibliographyDatabase database;
        database.m_identity = path;
        database.m_diagnostics.append({BibliographyDiagnostic::Severity::Error,
                                       QStringLiteral("Cannot read the bibliography “%1”: %2.")
                                           .arg(QFileInfo(path).fileName(), file.errorString()),
                                       {}, 0});
        return database;
    }
    return fromData(file.readAll(), format == Format::Auto ? formatForPath(path) : format, path);
}

namespace {

int lineAtOffset(const QByteArray &data, qsizetype offset)
{
    return int(data.left(offset).count('\n')) + 1;
}

} // namespace

const CslReference *BibliographyDatabase::find(const QString &key) const
{
    const auto it = m_index.constFind(key);
    return it == m_index.constEnd() ? nullptr : &m_references[*it];
}

void BibliographyDatabase::add(CslReference reference, int line)
{
    if (reference.id.isEmpty()) {
        m_diagnostics.append({BibliographyDiagnostic::Severity::Warning, QStringLiteral("An entry without a key is ignored."),
                              {}, line});
        return;
    }
    if (m_index.contains(reference.id)) {
        m_diagnostics.append({BibliographyDiagnostic::Severity::Warning,
                              QStringLiteral("The key “%1” is defined more than once; the first entry is used.")
                                  .arg(reference.id),
                              reference.id, line});
        return;
    }
    m_index.insert(reference.id, m_references.size());
    m_references.append(std::move(reference));
}

BibliographyDatabase BibliographyDatabase::fromData(const QByteArray &data, Format format, const QString &identity)
{
    BibliographyDatabase database;
    database.m_identity = identity;
    auto error = [&database](const QString &message, int line) {
        database.m_diagnostics.append({BibliographyDiagnostic::Severity::Error, message, {}, line});
    };
    if (format == Format::Auto) format = formatForPath(identity);
    if (format == Format::BibLaTeX) {
        const BibTeX::Result result = BibTeX::parse(QString::fromUtf8(data));
        database.m_valid = true;
        database.m_diagnostics = result.diagnostics;
        for (const BibTeX::Entry &entry : result.entries) database.add(entry.reference, entry.line);
        return database;
    }
    QJsonValue root;
    if (format == Format::CslJson) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            error(QStringLiteral("The CSL JSON is not valid: %1.").arg(parseError.errorString()),
                  lineAtOffset(data, parseError.offset));
            return database;
        }
        root = document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
    } else {
        QString message;
        int line = 0;
        root = CslYaml::toJson(data, &message, &line);
        if (!message.isEmpty()) {
            error(QStringLiteral("The CSL YAML is not valid: %1.").arg(message), line);
            return database;
        }
    }
    QJsonArray items;
    if (root.isArray()) {
        items = root.toArray();
    } else if (root.isObject()) {
        const QJsonObject object = root.toObject();
        if (object.value(QStringLiteral("references")).isArray())
            items = object.value(QStringLiteral("references")).toArray();
        else if (object.value(QStringLiteral("items")).isArray())
            items = object.value(QStringLiteral("items")).toArray();
        else if (object.contains(QStringLiteral("id")))
            items.append(object);
    } else if (!root.isNull()) {
        error(QStringLiteral("The bibliography holds no list of references."), 0);
        return database;
    }
    database.m_valid = true;
    for (qsizetype i = 0; i < items.size(); ++i) {
        if (!items[i].isObject()) {
            database.m_diagnostics.append({BibliographyDiagnostic::Severity::Warning,
                                           QStringLiteral("Item %1 is not an object and is ignored.").arg(i + 1), {}, 0});
            continue;
        }
        database.add(CslJson::reference(items[i].toObject()), 0);
    }
    return database;
}
