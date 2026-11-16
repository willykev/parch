#include "citations.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>

#include <optional>

namespace {

struct BundledStyle {
    const char *id;
    const char *title;
    const char *locale;
};

// Copied from the official CSL styles repository; see LICENSE.
const BundledStyle bundled[] = {
    {"apa", "APA Style 7th edition", "en-US"},
    {"chicago-author-date", "Chicago Manual of Style 18th edition (author-date)", "en-US"},
    {"chicago-notes-bibliography", "Chicago Manual of Style 18th edition (notes and bibliography)", "en-US"},
    {"ieee", "IEEE Reference Guide version 11.29.2023", "en-US"},
    {"modern-language-association", "MLA Handbook 9th edition (in-text citations)", "en-US"},
    {"harvard-cite-them-right", "Cite Them Right 12th edition (author-date/Harvard)", "en-GB"},
    {"nlm-citation-sequence", "NLM/Vancouver: Citing Medicine 2nd edition (citation-sequence)", "en-US"},
};

// The locales parch bundles, from the official CSL locales repository.
const char *const bundledLocales[] = {"en-US", "en-GB"};

std::shared_ptr<const CslLocale> bundledLocale(const QString &language)
{
    static QMutex mutex;
    static QHash<QString, std::shared_ptr<const CslLocale>> cache;
    bool known = false;
    for (const char *name : bundledLocales) known = known || language == QLatin1String(name);
    if (!known) return nullptr;
    QMutexLocker locker(&mutex);
    if (const auto it = cache.constFind(language); it != cache.constEnd()) return *it;
    QFile file(QStringLiteral(":/csl/locales-%1.xml").arg(language));
    std::shared_ptr<const CslLocale> loaded;
    if (file.open(QIODevice::ReadOnly)) loaded = CslLocale::fromXml(file.readAll(), nullptr);
    if (loaded) cache.insert(language, loaded);
    return loaded;
}

// Other names the bundled styles go by.
QString canonicalId(const QString &id)
{
    const QString lower = id.trimmed().toLower();
    if (lower == u"vancouver") return QStringLiteral("nlm-citation-sequence");
    if (lower == u"mla") return QStringLiteral("modern-language-association");
    if (lower == u"harvard") return QStringLiteral("harvard-cite-them-right");
    return lower;
}

QString normalizedKey(const QString &key)
{
    QString text = key.trimmed();
    if (text.startsWith(u'#') || text.startsWith(u'@')) text = text.mid(1);
    return text;
}

} // namespace

QList<CslCatalog::StyleInfo> CslCatalog::bundledStyles()
{
    QList<StyleInfo> styles;
    for (const BundledStyle &style : bundled)
        styles.append({QLatin1String(style.id), QString::fromUtf8(style.title), QLatin1String(style.locale)});
    return styles;
}

std::shared_ptr<const CslStyle> CslCatalog::bundledStyle(const QString &id, QString *error)
{
    static QMutex mutex;
    static QHash<QString, std::shared_ptr<const CslStyle>> cache;
    const QString canonical = canonicalId(id);
    bool known = false;
    for (const BundledStyle &style : bundled) known = known || canonical == QLatin1String(style.id);
    if (!known) {
        if (error) *error = QStringLiteral("parch has no bundled citation style called “%1”.").arg(id);
        return nullptr;
    }
    QMutexLocker locker(&mutex);
    if (const auto it = cache.constFind(canonical); it != cache.constEnd()) return *it;
    QFile file(QStringLiteral(":/csl/%1.csl").arg(canonical));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("The bundled citation style “%1” is missing.").arg(canonical);
        return nullptr;
    }
    std::shared_ptr<const CslStyle> style = CslStyle::fromXml(file.readAll(), error);
    if (style) cache.insert(canonical, style);
    return style;
}

std::shared_ptr<const CslStyle> CslCatalog::styleFromFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read the citation style “%1”: %2.")
                         .arg(QFileInfo(path).fileName(), file.errorString());
        return nullptr;
    }
    return CslStyle::fromXml(file.readAll(), error);
}

std::shared_ptr<const CslLocale> CslCatalog::defaultLocale()
{
    static const std::shared_ptr<const CslLocale> locale = [] {
        std::shared_ptr<const CslLocale> loaded = bundledLocale(QStringLiteral("en-US"));
        return loaded ? loaded : std::make_shared<const CslLocale>();
    }();
    return locale;
}

QString CslCatalog::localeFor(const CslStyle &style)
{
    if (!style.defaultLocale().isEmpty()) return style.defaultLocale();
    for (const BundledStyle &bundledStyle : bundled) {
        if (style.id() == QStringLiteral("http://www.zotero.org/styles/%1").arg(QLatin1String(bundledStyle.id)))
            return QLatin1String(bundledStyle.locale);
    }
    return QStringLiteral("en-US");
}

std::shared_ptr<const CslLocale> CslCatalog::localeFromFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read the citation locale “%1”: %2.")
                         .arg(QFileInfo(path).fileName(), file.errorString());
        return nullptr;
    }
    return CslLocale::fromXml(file.readAll(), error);
}

std::shared_ptr<const CslLocale> CslCatalog::locale(const QString &language, const QStringList &folders, QString *error)
{
    if (language.isEmpty() || language == u"en-US") return defaultLocale();
    if (std::shared_ptr<const CslLocale> bundledOne = bundledLocale(language)) return bundledOne;
    QStringList names{QStringLiteral("locales-%1.xml").arg(language)};
    for (const QString &folder : folders) {
        for (const QString &name : names) {
            const QString path = QDir(folder).filePath(name);
            if (!QFile::exists(path)) continue;
            QString message;
            std::shared_ptr<const CslLocale> loaded = localeFromFile(path, &message);
            if (loaded) return loaded;
            if (error) *error = message;
        }
    }
    if (error && error->isEmpty())
        *error = QStringLiteral("No citation locale for “%1” was found; English (US) is used.").arg(language);
    return defaultLocale();
}

CitationFormatter::CitationFormatter() = default;

CitationOutput CitationFormatter::format(const QList<CitationClusterSpec> &clusters,
                                         const QList<WriterDefinition> &definitions) const
{
    using Code = CitationDiagnostic::Code;
    using Severity = CitationDiagnostic::Severity;
    CitationOutput out;
    // Without a locale chosen, the one the style is written for.
    QString localeProblem;
    const std::shared_ptr<const CslLocale> locale =
        m_locale ? m_locale
        : m_style ? CslCatalog::locale(CslCatalog::localeFor(*m_style), {}, &localeProblem)
                  : CslCatalog::defaultLocale();
    if (!localeProblem.isEmpty())
        out.diagnostics.append({Code::Locale, Severity::Warning, localeProblem, {}, -1, -1, {}, 0});
    std::optional<CslEngine> engine;
    if (m_style) engine.emplace(m_style, *locale);
    const CslLocale &activeLocale = engine ? engine->locale() : *locale;
    out.noteStyle = m_style && m_style->isNoteStyle();

    // What each database reported when it was read.
    QSet<const BibliographyDatabase *> reported;
    auto reportDatabase = [&](const Database &database) {
        if (!database || reported.contains(database.get())) return;
        reported.insert(database.get());
        for (const BibliographyDiagnostic &d : database->diagnostics()) {
            out.diagnostics.append({Code::Database,
                                    d.severity == BibliographyDiagnostic::Severity::Error ? Severity::Error
                                                                                          : Severity::Warning,
                                    d.message, d.key, -1, -1, database->identity(), d.line});
        }
    };
    for (const Database &database : m_databases) reportDatabase(database);
    for (auto it = m_fileDatabases.constBegin(); it != m_fileDatabases.constEnd(); ++it)
        for (const Database &database : *it) reportDatabase(database);

    // Writer definitions, kept apart by the file that defines them.
    QHash<QString, QHash<QString, const WriterDefinition *>> byFile;
    for (const WriterDefinition &definition : definitions) {
        const QString key = normalizedKey(definition.key);
        QHash<QString, const WriterDefinition *> &keys = byFile[definition.sourceFile];
        if (keys.contains(key)) {
            out.diagnostics.append({Code::DuplicateDefinition, Severity::Warning,
                                    QStringLiteral("The citation “%1” is defined more than once; the first "
                                                   "definition is used.")
                                        .arg(key),
                                    key, -1, -1, definition.sourceFile, 0});
            continue;
        }
        keys.insert(key, &definition);
    }

    QHash<const WriterDefinition *, int> writerNumbers;
    QList<const WriterDefinition *> writerOrder;
    QHash<const CslReference *, QString> referenceSource;
    QSet<QString> ambiguous;
    QList<CslCitation> citations;
    // Per cluster, the Writer cites, rendered after the style's cites.
    QList<QList<QPair<const WriterDefinition *, CitationItemSpec>>> writerCites;
    bool anyStyleCite = false;
    for (qsizetype c = 0; c < clusters.size(); ++c) {
        const CitationClusterSpec &cluster = clusters[c];
        CslCitation citation;
        citation.noteIndex = cluster.noteIndex;
        QList<QPair<const WriterDefinition *, CitationItemSpec>> writers;
        for (qsizetype i = 0; i < cluster.items.size(); ++i) {
            const CitationItemSpec &item = cluster.items[i];
            const QString key = normalizedKey(item.key);
            // A definition in the citing file, then one in the root document.
            const WriterDefinition *definition = byFile.value(cluster.sourceFile).value(key);
            if (!definition && !cluster.sourceFile.isEmpty()) definition = byFile.value(QString()).value(key);
            if (definition) {
                if (!writerNumbers.contains(definition)) {
                    writerOrder.append(definition);
                    writerNumbers.insert(definition, int(writerOrder.size()));
                }
                writers.append({definition, item});
                continue;
            }
            anyStyleCite = true;
            const QList<Database> &databases = m_fileDatabases.contains(cluster.sourceFile)
                                                   ? m_fileDatabases[cluster.sourceFile]
                                                   : m_databases;
            const CslReference *found = nullptr;
            QString foundIn;
            QStringList others;
            for (const Database &database : databases) {
                if (!database) continue;
                if (const CslReference *reference = database->find(key)) {
                    if (!found) {
                        found = reference;
                        foundIn = database->identity();
                    } else {
                        others.append(QFileInfo(database->identity()).fileName());
                    }
                }
            }
            if (!others.isEmpty() && !ambiguous.contains(key)) {
                ambiguous.insert(key);
                out.diagnostics.append({Code::AmbiguousKey, Severity::Warning,
                                        QStringLiteral("The key “%1” is in more than one bibliography: %2 is used, "
                                                       "not %3.")
                                            .arg(key, QFileInfo(foundIn).fileName(), others.join(QStringLiteral(", "))),
                                        key, int(c), int(i), foundIn, 0});
            }
            CslCite cite;
            cite.reference = found;
            cite.prefix = item.prefix;
            cite.suffix = item.suffix;
            cite.mode = item.mode;
            if (!item.locator.trimmed().isEmpty()) {
                const CslEngine::Locator locator = CslEngine::parseLocator(item.locator, activeLocale);
                cite.locator = locator.locator;
                cite.label = locator.label;
                if (!locator.suffix.trimmed().isEmpty()) cite.suffix = locator.suffix + cite.suffix;
            }
            if (found) {
                referenceSource.insert(found, foundIn);
            } else {
                cite.missingKey = key;
                out.diagnostics.append({Code::MissingKey, Severity::Warning,
                                        databases.isEmpty() && byFile.isEmpty()
                                            ? QStringLiteral("The citation “%1” has no definition and no "
                                                             "bibliography is chosen.")
                                                  .arg(key)
                                            : QStringLiteral("The citation “%1” is not defined in the document or "
                                                             "its bibliography.")
                                                  .arg(key),
                                        key, int(c), int(i), cluster.sourceFile, 0});
            }
            citation.cites.append(cite);
        }
        citations.append(citation);
        writerCites.append(writers);
    }

    CslEngine::Result result;
    if (engine) {
        result = engine->process(citations);
    } else {
        for (const CslCitation &citation : citations) {
            CslText text;
            if (!citation.cites.isEmpty()) {
                QStringList keys;
                for (const CslCite &cite : citation.cites)
                    keys.append(cite.reference ? cite.reference->id : cite.missingKey);
                text.append(QStringLiteral("[") + keys.join(QStringLiteral("; ")) + u']');
            }
            result.citations.append(text);
        }
        if (anyStyleCite)
            out.diagnostics.append({Code::NoStyle, Severity::Warning,
                                    QStringLiteral("No citation style is chosen, so citations show their keys."), {},
                                    -1, -1, {}, 0});
    }
    if (m_style && m_style->usesCitationNumber() && anyStyleCite && !writerOrder.isEmpty())
        out.diagnostics.append({Code::SeparateNumbering, Severity::Warning,
                                QStringLiteral("Bibliography entries written in the document are numbered apart from "
                                               "the citation style's numbers."),
                                {}, -1, -1, {}, 0});

    // Writer cites: the definition's number, then the locator: "[1, p. 23]".
    for (qsizetype c = 0; c < clusters.size(); ++c) {
        CslText text = result.citations.value(c);
        const auto &writers = writerCites[c];
        if (!writers.isEmpty()) {
            QStringList parts;
            for (const auto &[definition, item] : writers) {
                QString part = QString::number(writerNumbers.value(definition));
                const QString raw = item.locator.trimmed();
                if (!raw.isEmpty()) {
                    const CslEngine::Locator locator = CslEngine::parseLocator(raw, activeLocale);
                    QString shown = raw;
                    if (!locator.locator.isEmpty()) {
                        static const QRegularExpression many(QStringLiteral("\\d\\s*[-–—,&]\\s*\\d"));
                        const bool plural = many.match(locator.locator).hasMatch();
                        QString label = activeLocale.termText(locator.label, QStringLiteral("short"), plural);
                        QString value = locator.locator;
                        value.replace(QRegularExpression(QStringLiteral("(\\d)\\s*-+\\s*(\\d)")), QStringLiteral("\\1–\\2"));
                        shown = (label.isEmpty() ? value : label + u' ' + value) + locator.suffix;
                    }
                    part += QStringLiteral(", ") + shown;
                }
                parts.append(part);
            }
            CslText writerText(QStringLiteral("[") + parts.join(QStringLiteral("; ")) + u']');
            if (!text.isEmpty()) text.append(QStringLiteral(" "));
            text.append(writerText);
        }
        out.clusters.append(text);
    }

    for (const CslBibliographyEntry &entry : std::as_const(result.bibliography))
        out.bibliography.append({entry.id, entry.text, 0, referenceSource.value(entry.reference)});
    out.layout = result.layout;
    // Writer definitions, numbered by first use and listed as written.
    for (const WriterDefinition *definition : std::as_const(writerOrder)) {
        const int number = writerNumbers.value(definition);
        CslText text;
        CslFormat margin;
        margin.display = CslFormat::Display::LeftMargin;
        text.append(QStringLiteral("[%1]").arg(number), margin);
        for (CslSpan span : definition->text.spans()) {
            span.format.display = CslFormat::Display::RightInline;
            text.append(span.text, span.format);
        }
        out.bibliography.append({normalizedKey(definition->key), text, number, definition->sourceFile});
    }
    return out;
}
