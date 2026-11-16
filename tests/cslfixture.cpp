#include "cslfixture.h"

#include "citations.h"
#include "cslengine.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <deque>

namespace {

QHash<QString, QString> sectionsOf(const QString &text)
{
    static const QRegularExpression section(
        QStringLiteral(">>=+\\s*([A-Z-]+)\\s*=+>>\\r?\\n(.*?)\\r?\\n?<<=+\\s*\\1\\s*=+<<"),
        QRegularExpression::DotMatchesEverythingOption);
    QHash<QString, QString> sections;
    auto it = section.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        sections.insert(match.captured(1), match.captured(2));
    }
    return sections;
}

// HTML with runs of spaces as one space; line breaks stay, since a style
// can put cites on lines of their own.
QString canonical(const CslText &text)
{
    // The suite's expectations come from a processor that does not link
    // URLs and DOIs.
    CslText unlinked;
    for (CslSpan span : text.spans()) {
        span.format.link.clear();
        unlinked.append(span.text, span.format);
    }
    QStringList lines = unlinked.toHtml().split(u'\n');
    for (QString &line : lines) line = line.simplified();
    return lines.join(u'\n');
}

// The entries of a bibliography written as citeproc-js writes it.
QStringList expectedEntries(const QString &html)
{
    QStringList entries;
    static const QRegularExpression open(QStringLiteral("<div class=\"csl-entry\"[^>]*>"));
    qsizetype at = 0;
    for (;;) {
        const QRegularExpressionMatch match = open.match(html, at);
        if (!match.hasMatch()) break;
        // The entry ends at the </div> that balances its opening.
        qsizetype pos = match.capturedEnd();
        int depth = 1;
        static const QRegularExpression tag(QStringLiteral("<(/?)div\\b[^>]*>"));
        qsizetype end = html.size();
        while (depth > 0) {
            const QRegularExpressionMatch t = tag.match(html, pos);
            if (!t.hasMatch()) break;
            depth += t.captured(1).isEmpty() ? 1 : -1;
            pos = t.capturedEnd();
            if (depth == 0) end = t.capturedStart();
        }
        QString inner = html.mid(match.capturedEnd(), end - match.capturedEnd());
        // Layout whitespace between the divisions of an entry.
        static const QRegularExpression between(QStringLiteral(">\\s*\\n\\s*<"));
        inner.replace(between, QStringLiteral("><"));
        entries.append(canonical(CslText::fromHtml(inner.trimmed())).replace(u'\n', u' ').simplified());
        at = pos;
    }
    return entries;
}

CslCite citeFrom(const QJsonObject &object, const QHash<QString, const CslReference *> &references)
{
    CslCite cite;
    const QString id = object.value(QStringLiteral("id")).isDouble()
                           ? QString::number(object.value(QStringLiteral("id")).toInt())
                           : object.value(QStringLiteral("id")).toString();
    cite.reference = references.value(id);
    cite.missingKey = id;
    const QJsonValue locator = object.value(QStringLiteral("locator"));
    cite.locator = locator.isDouble() ? QString::number(locator.toInt()) : locator.toString();
    cite.label = object.value(QStringLiteral("label")).toString();
    cite.prefix = object.value(QStringLiteral("prefix")).toString();
    cite.suffix = object.value(QStringLiteral("suffix")).toString();
    if (object.value(QStringLiteral("suppress-author")).toBool()) cite.mode = CslCite::Mode::SuppressAuthor;
    if (object.value(QStringLiteral("author-only")).toBool()) cite.mode = CslCite::Mode::AuthorOnly;
    return cite;
}

} // namespace

CslFixture::Outcome CslFixture::run(const QString &path)
{
    Outcome outcome;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        outcome.error = QStringLiteral("cannot read the fixture");
        return outcome;
    }
    const QHash<QString, QString> sections = sectionsOf(QString::fromUtf8(file.readAll()));
    const QString mode = sections.value(QStringLiteral("MODE")).trimmed();
    if (mode != u"citation" && mode != u"bibliography") {
        outcome.error = QStringLiteral("mode %1 is not run").arg(mode);
        return outcome;
    }
    if (sections.contains(QStringLiteral("BIBENTRIES")) || sections.contains(QStringLiteral("BIBSECTION"))
        || sections.contains(QStringLiteral("ABBREVIATIONS"))) {
        outcome.error = QStringLiteral("bibliography sections and abbreviations are not run");
        return outcome;
    }
    QString error;
    const std::shared_ptr<const CslStyle> style = CslStyle::fromXml(sections.value(QStringLiteral("CSL")).toUtf8(), &error);
    if (!style) {
        outcome.error = error;
        return outcome;
    }
    const QJsonArray input = QJsonDocument::fromJson(sections.value(QStringLiteral("INPUT")).toUtf8()).array();
    std::deque<CslReference> store;
    QHash<QString, const CslReference *> references;
    QList<const CslReference *> all;
    for (const QJsonValue &value : input) {
        store.push_back(CslJson::reference(value.toObject()));
        references.insert(store.back().id, &store.back());
        all.append(&store.back());
    }
    QList<CslCitation> citations;
    if (sections.contains(QStringLiteral("CITATIONS"))) {
        // Calls of an integration: each puts one citation among others,
        // given by id with their notes. The last call's order is the document's.
        const QJsonArray calls = QJsonDocument::fromJson(sections.value(QStringLiteral("CITATIONS")).toUtf8()).array();
        QHash<QString, CslCitation> byId;
        QStringList order;
        for (const QJsonValue &callValue : calls) {
            const QJsonArray call = callValue.toArray();
            const QJsonObject citation = call.at(0).toObject();
            const QString id = citation.value(QStringLiteral("citationID")).toString();
            CslCitation cluster;
            cluster.noteIndex = citation.value(QStringLiteral("properties")).toObject().value(QStringLiteral("noteIndex")).toInt();
            for (const QJsonValue &item : citation.value(QStringLiteral("citationItems")).toArray())
                cluster.cites.append(citeFrom(item.toObject(), references));
            byId.insert(id, cluster);
            QStringList now;
            for (const QJsonValue &pair : call.at(1).toArray()) {
                const QString otherId = pair.toArray().at(0).toString();
                now.append(otherId);
                if (byId.contains(otherId)) byId[otherId].noteIndex = pair.toArray().at(1).toInt();
            }
            now.append(id);
            for (const QJsonValue &pair : call.at(2).toArray()) {
                const QString otherId = pair.toArray().at(0).toString();
                now.append(otherId);
                if (byId.contains(otherId)) byId[otherId].noteIndex = pair.toArray().at(1).toInt();
            }
            order = now;
        }
        for (const QString &id : std::as_const(order)) citations.append(byId.value(id));
    } else if (sections.contains(QStringLiteral("CITATION-ITEMS"))) {
        const QJsonArray list = QJsonDocument::fromJson(sections.value(QStringLiteral("CITATION-ITEMS")).toUtf8()).array();
        int note = 0;
        for (const QJsonValue &value : list) {
            CslCitation cluster;
            cluster.noteIndex = ++note;
            for (const QJsonValue &item : value.toArray()) cluster.cites.append(citeFrom(item.toObject(), references));
            citations.append(cluster);
        }
    } else if (mode == u"citation") {
        CslCitation cluster;
        cluster.noteIndex = 1;
        for (const CslReference *reference : std::as_const(all)) {
            CslCite cite;
            cite.reference = reference;
            cluster.cites.append(cite);
        }
        citations.append(cluster);
    }

    const CslEngine engine(style, *CslCatalog::defaultLocale());
    // A bibliography lists every item; citations know only those cited.
    const CslEngine::Result result = engine.process(citations, mode == u"bibliography" ? all : QList<const CslReference *>());
    const QString expected = sections.value(QStringLiteral("RESULT"));
    if (mode == u"bibliography") {
        outcome.expected = expectedEntries(expected).join(u'\n');
        QStringList actual;
        for (const CslBibliographyEntry &entry : result.bibliography)
            actual.append(canonical(entry.text).replace(u'\n', u' ').simplified());
        outcome.actual = actual.join(u'\n');
    } else {
        QStringList expectedLines;
        static const QRegularExpression marker(QStringLiteral("^(?:>>|\\.\\.)\\[\\d+\\]\\s?"));
        for (QString line : expected.split(u'\n')) {
            if (sections.contains(QStringLiteral("CITATIONS"))) line.remove(marker);
            expectedLines.append(canonical(CslText::fromHtml(line)));
        }
        outcome.expected = expectedLines.join(u'\n');
        QStringList actual;
        for (const CslText &text : result.citations) actual.append(canonical(text));
        outcome.actual = actual.join(u'\n');
    }
    outcome.passed = outcome.expected == outcome.actual;
    return outcome;
}
