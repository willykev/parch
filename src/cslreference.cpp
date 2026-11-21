#include "cslreference.h"

#include "bibtex.h"
#include "saferead.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <yaml.h>

#include <algorithm>

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

namespace {
// Only anchored values and the paths leading to them need identities.
// Reusing a name list or a date in YAML shares its converted CSL value too.
struct YamlSharing {
    struct Shape {
        QHash<QString, int> fields;
        QHash<qsizetype, int> elements;
    };
    std::vector<Shape> shapes;
    int root = -1;
    QHash<int, QList<CslName>> names;
    QHash<int, CslDate> dates;
    QHash<int, QString> text;

    int field(int shape, const QString &key) const
    {
        return shape < 0 ? -1 : shapes[size_t(shape)].fields.value(key, -1);
    }
};

template <typename T>
T converted(QHash<int, T> &cache, int id, const QJsonValue &value, T (*convert)(const QJsonValue &))
{
    if (id < 0) return convert(value);
    const auto found = cache.constFind(id);
    if (found != cache.cend()) return found.value();
    return cache.insert(id, convert(value)).value();
}

CslReference readReference(const QJsonObject &item, QString *error, YamlSharing &shared, int shape)
{
    CslReference reference;
    reference.id = scalarText(item.value(QStringLiteral("id"))).trimmed();
    reference.type = scalarText(item.value(QStringLiteral("type"))).trimmed();
    if (reference.id.isEmpty() && error) *error = QStringLiteral("An item has no id.");
    for (auto it = item.constBegin(); it != item.constEnd(); ++it) {
        const QString key = it.key();
        if (key == u"id" || key == u"type") continue;
        const int id = shared.field(shape, key);
        if (CslReference::isNameVariable(key)) {
            const auto list = converted(shared.names, id, it.value(), CslJson::names);
            if (!list.isEmpty()) reference.names.insert(key, list);
        } else if (CslReference::isDateVariable(key)) {
            const auto date = converted(shared.dates, id, it.value(), CslJson::date);
            if (!date.isEmpty()) reference.dates.insert(key, date);
        } else {
            const auto text = converted(shared.text, id, it.value(), scalarText);
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

} // namespace

CslReference CslJson::reference(const QJsonObject &item, QString *error)
{
    YamlSharing sharing;
    return readReference(item, error, sharing, -1);
}

namespace {

// Builds JSON from libyaml's events.
// No bibliography nests its lists and maps more than a few levels deep.
// Reading one recurses once per level, on a worker's stack, and so does
// every walk over the values read, so deeper nesting refuses the file.
constexpr int MaxNesting = 256;

class YamlReader {
public:
    explicit YamlReader(const QByteArray &data, YamlSharing *sharing = nullptr) : m_sharing(sharing)
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
                int shape = -1;
                if (!value(document, error, errorLine, 0, shape)) return {};
                // The first document with content is the bibliography, as
                // in Markdown front matter.
                if (!haveDocument && !document.isNull()) {
                    result = document;
                    if (m_sharing) m_sharing->root = shape;
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
    bool value(QJsonValue &out, QString *error, int *errorLine, int depth, int &shape)
    {
        yaml_event_t event;
        if (!next(event, error, errorLine)) return false;
        bool ok = true;
        if ((event.type == YAML_SEQUENCE_START_EVENT || event.type == YAML_MAPPING_START_EVENT) && depth >= MaxNesting) {
            if (error) *error = QStringLiteral("its lists and maps are nested more than %1 deep").arg(MaxNesting);
            if (errorLine) *errorLine = int(event.start_mark.line) + 1;
            yaml_event_delete(&event);
            return false;
        }
        switch (event.type) {
        case YAML_DOCUMENT_END_EVENT:
            out = QJsonValue();
            break;
        case YAML_SCALAR_EVENT: {
            const QString text = QString::fromUtf8(reinterpret_cast<const char *>(event.data.scalar.value),
                                                   qsizetype(event.data.scalar.length));
            const bool plain = event.data.scalar.style == YAML_PLAIN_SCALAR_STYLE;
            out = plain && (text == u"~" || text == u"null" || text.isEmpty()) ? QJsonValue() : QJsonValue(text);
            if (event.data.scalar.anchor) shape = save(anchorName(event.data.scalar.anchor), out, {});
            break;
        }
        case YAML_ALIAS_EVENT: {
            const auto anchor = m_anchors.value(anchorName(event.data.alias.anchor));
            out = anchor.value;
            shape = anchor.shape;
            break;
        }
        case YAML_SEQUENCE_START_EVENT: {
            const QString anchor = anchorName(event.data.sequence_start.anchor);
            QJsonArray array;
            YamlSharing::Shape members;
            for (;;) {
                if (peekEnd(YAML_SEQUENCE_END_EVENT, ok, error, errorLine)) break;
                if (!ok) break;
                QJsonValue item;
                int child = -1;
                if (!value(item, error, errorLine, depth + 1, child)) {
                    ok = false;
                    break;
                }
                if (child >= 0) members.elements.insert(array.size(), child);
                array.append(item);
            }
            out = array;
            shape = save(anchor, out, std::move(members));
            break;
        }
        case YAML_MAPPING_START_EVENT: {
            const QString anchor = anchorName(event.data.mapping_start.anchor);
            QMap<QString, QJsonValue> fields;
            YamlSharing::Shape members;
            for (;;) {
                if (peekEnd(YAML_MAPPING_END_EVENT, ok, error, errorLine)) break;
                if (!ok) break;
                QJsonValue key;
                QJsonValue item;
                int ignored = -1, child = -1;
                if (!value(key, error, errorLine, depth + 1, ignored) || !value(item, error, errorLine, depth + 1, child)) {
                    ok = false;
                    break;
                }
                const auto name = key.toString();
                if (child >= 0) members.fields.insert(name, child);
                else members.fields.remove(name);
                fields.insert(name, item);
            }
            // QJsonObject stores its keys in order. Appending them in that
            // order avoids moving every earlier field in a reverse-sorted map.
            QJsonObject object;
            for (auto it = fields.cbegin(); it != fields.cend(); ++it) object.insert(it.key(), it.value());
            out = object;
            shape = save(anchor, out, std::move(members));
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

    yaml_parser_t m_parser;
    // An event read ahead to see whether a collection ends, which the next
    // read takes.
    yaml_event_t m_peeked;
    bool m_pending = false;
    struct Anchor { QJsonValue value; int shape = -1; };
    QHash<QString, Anchor> m_anchors;
    YamlSharing *m_sharing;
    int save(const QString &anchor, const QJsonValue &value, YamlSharing::Shape members)
    {
        int shape = -1;
        if (m_sharing && (!anchor.isEmpty() || !members.fields.isEmpty() || !members.elements.isEmpty())) {
            shape = int(m_sharing->shapes.size());
            m_sharing->shapes.push_back(std::move(members));
        }
        if (!anchor.isEmpty()) m_anchors.insert(anchor, {value, shape});
        return shape;
    }
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

BibliographyDatabase BibliographyDatabase::fromFile(const QString &path, Format format, const QStringList *folders)
{
    const auto read = folders ? SafeRead::readWithin(path, SafeRead::ReferenceBytes, *folders)
                              : SafeRead::read(path, SafeRead::ReferenceBytes);
    if (!read.ok) {
        BibliographyDatabase database;
        database.m_identity = path;
        database.m_diagnostics.append({BibliographyDiagnostic::Severity::Error,
                                       QStringLiteral("Cannot read the bibliography “%1”: %2.")
                                           .arg(QFileInfo(path).fileName(), read.error),
                                       {}, 0, BibliographyDiagnostic::Kind::Unreadable});
        return database;
    }
    return fromData(read.bytes, format == Format::Auto ? formatForPath(path) : format, path);
}

namespace {

int lineAtOffset(const QByteArray &data, qsizetype offset)
{
    return int(data.left(offset).count('\n')) + 1;
}

// How deep a JSON value's arrays and objects are nested, found without
// recursion; QJsonDocument already refuses more than 1,024 levels.
int nestingOf(const QJsonValue &root)
{
    int deepest = 0;
    std::vector<std::pair<QJsonValue, int>> pending{{root, 0}};
    while (!pending.empty()) {
        const auto [value, depth] = std::move(pending.back());
        pending.pop_back();
        if (value.isArray()) {
            deepest = std::max(deepest, depth + 1);
            for (const QJsonValue &item : value.toArray()) pending.emplace_back(item, depth + 1);
        } else if (value.isObject()) {
            deepest = std::max(deepest, depth + 1);
            for (const QJsonValue &item : value.toObject()) pending.emplace_back(item, depth + 1);
        }
    }
    return deepest;
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
                              reference.id, line, BibliographyDiagnostic::Kind::DuplicateKey});
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
    const auto checkEntries = [&] {
        if (!database.m_references.isEmpty()) return;
        database.m_valid = std::none_of(database.m_diagnostics.cbegin(), database.m_diagnostics.cend(), [](const auto &d) {
            return d.severity == BibliographyDiagnostic::Severity::Error;
        });
        if (database.m_valid)
            database.m_diagnostics.append({BibliographyDiagnostic::Severity::Warning,
                                           QStringLiteral("The bibliography holds no entries yet."), {}, 0});
    };
    if (format == Format::BibLaTeX) {
        const BibTeX::Result result = BibTeX::parse(QString::fromUtf8(data));
        database.m_valid = true;
        database.m_diagnostics = result.diagnostics;
        for (const BibTeX::Entry &entry : result.entries) database.add(entry.reference, entry.line);
        checkEntries();
        return database;
    }
    QJsonValue root;
    YamlSharing sharing;
    if (format == Format::CslJson) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            error(QStringLiteral("The CSL JSON is not valid: %1.").arg(parseError.errorString()),
                  lineAtOffset(data, parseError.offset));
            return database;
        }
        root = document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
        if (nestingOf(root) > MaxNesting) {
            error(QStringLiteral("The CSL JSON is not read: its arrays and objects are nested more than %1 deep.")
                      .arg(MaxNesting),
                  0);
            return database;
        }
    } else {
        QString message;
        int line = 0;
        root = YamlReader(data, &sharing).read(&message, &line);
        if (!message.isEmpty()) {
            error(QStringLiteral("The CSL YAML is not valid: %1.").arg(message), line);
            return database;
        }
    }
    QJsonArray items;
    int sequence = sharing.root;
    bool single = false;
    if (root.isArray()) {
        items = root.toArray();
    } else if (root.isObject()) {
        const QJsonObject object = root.toObject();
        if (object.value(QStringLiteral("references")).isArray()) {
            items = object.value(QStringLiteral("references")).toArray();
            sequence = sharing.field(sharing.root, QStringLiteral("references"));
        } else if (object.value(QStringLiteral("items")).isArray()) {
            items = object.value(QStringLiteral("items")).toArray();
            sequence = sharing.field(sharing.root, QStringLiteral("items"));
        } else if (object.contains(QStringLiteral("id"))) {
            items.append(object);
            single = true;
        }
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
        const auto item = items.at(i).toObject();
        CslReference key;
        key.id = scalarText(item.value(QStringLiteral("id"))).trimmed();
        if (key.id.isEmpty() || database.find(key.id)) database.add(std::move(key), 0);
        else {
            const int shape = sequence < 0 || single ? sequence : sharing.shapes[size_t(sequence)].elements.value(i, -1);
            database.add(readReference(item, nullptr, sharing, shape), 0);
        }
    }
    checkEntries();
    return database;
}
