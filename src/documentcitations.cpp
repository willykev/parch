#include "documentcitations.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMutex>

#include <functional>

namespace DocumentCitations {
namespace {

// A file read once for as long as it is unchanged: its size and time say so.
struct Stamp {
    qint64 size = -1;
    QDateTime modified;
    bool operator==(const Stamp &) const = default;
};

Stamp stampOf(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) return {};
    return {info.size(), info.lastModified()};
}

std::shared_ptr<const BibliographyDatabase> database(const QString &path)
{
    static QMutex mutex;
    static QHash<QString, std::pair<Stamp, std::shared_ptr<const BibliographyDatabase>>> cache;
    const Stamp stamp = stampOf(path);
    {
        QMutexLocker locker(&mutex);
        const auto known = cache.constFind(path);
        if (known != cache.constEnd() && stamp.size >= 0 && known->first == stamp) return known->second;
    }
    auto read = std::make_shared<const BibliographyDatabase>(BibliographyDatabase::fromFile(path));
    QMutexLocker locker(&mutex);
    if (stamp.size >= 0) cache.insert(path, {stamp, read});
    else cache.remove(path);
    return read;
}

std::shared_ptr<const CslStyle> styleFile(const QString &path, QString *error)
{
    static QMutex mutex;
    static QHash<QString, std::tuple<Stamp, std::shared_ptr<const CslStyle>, QString>> cache;
    const Stamp stamp = stampOf(path);
    {
        QMutexLocker locker(&mutex);
        const auto known = cache.constFind(path);
        if (known != cache.constEnd() && stamp.size >= 0 && std::get<0>(*known) == stamp) {
            *error = std::get<2>(*known);
            return std::get<1>(*known);
        }
    }
    QString why;
    std::shared_ptr<const CslStyle> style = CslCatalog::styleFromFile(path, &why);
    if (!style && why.isEmpty()) why = QStringLiteral("The citation style “%1” cannot be used.").arg(QFileInfo(path).fileName());
    QMutexLocker locker(&mutex);
    if (stamp.size >= 0) cache.insert(path, {stamp, style, why});
    *error = style ? QString() : why;
    return style;
}

// The problems a bibliography file has, said once however often it is used.
void report(const Database &db, QSet<QString> &reported, QList<Problem> &problems)
{
    if (!db || reported.contains(db->identity())) return;
    reported.insert(db->identity());
    for (const BibliographyDiagnostic &d : db->diagnostics()) {
        Problem problem;
        switch (d.kind) {
        case BibliographyDiagnostic::Kind::Unreadable: problem.code = QStringLiteral("unreadable-bibliography"); break;
        case BibliographyDiagnostic::Kind::DuplicateKey: problem.code = QStringLiteral("duplicate-citation-key"); break;
        case BibliographyDiagnostic::Kind::Other: problem.code = QStringLiteral("bibliography"); break;
        }
        problem.severity = d.severity == BibliographyDiagnostic::Severity::Error ? md::Severity::Error : md::Severity::Warning;
        problem.message = d.message;
        problem.key = d.key;
        problem.source = db->identity();
        problem.line = d.line;
        problems.append(problem);
    }
}

QList<Database> databasesOf(const QStringList &files, Setup &setup, QSet<QString> &reported)
{
    QList<Database> list;
    for (const QString &file : files) {
        Database db = database(file);
        report(db, reported, setup.problems);
        list.append(db);
    }
    return list;
}

QSet<QString> reportedOf(const Setup &setup)
{
    QSet<QString> reported;
    for (const Problem &problem : setup.problems)
        if (!problem.source.isEmpty()) reported.insert(problem.source);
    for (const Database &db : setup.databases)
        if (db) reported.insert(db->identity());
    for (const QList<Database> &list : setup.fileDatabases)
        for (const Database &db : list)
            if (db) reported.insert(db->identity());
    return reported;
}

quint64 placeKey(int file, int start)
{
    return (quint64(quint32(file)) << 32) | quint32(start);
}

bool safeLink(const QString &dest)
{
    const QString lower = dest.trimmed().toLower();
    return lower.startsWith(QLatin1StringView("https://")) || lower.startsWith(QLatin1StringView("http://"))
           || lower.startsWith(QLatin1StringView("mailto:"));
}

void inlineText(const md::Document *document, const md::Node &node, CslFormat format, CslText &out)
{
    using T = md::NodeType;
    switch (node.type) {
    case T::Text:
    case T::Escape:
    case T::Entity:
    case T::Code:
    case T::MathInline:
        out.append(node.text, format);
        return;
    case T::SoftBreak:
    case T::HardBreak:
        out.append(QStringLiteral(" "), format);
        return;
    case T::Variable:
        if (node.resolved) out.append(node.text, format);
        else if (document) out.append(document->source(node).toString(), format);
        return;
    case T::Emphasis:
        format.italic = !format.italic;
        break;
    case T::Strong:
        format.bold = true;
        break;
    case T::Superscript:
        format.position = CslFormat::Position::Superscript;
        break;
    case T::Subscript:
        format.position = CslFormat::Position::Subscript;
        break;
    case T::Link:
    case T::Autolink:
        if (safeLink(node.dest)) format.link = node.dest.trimmed();
        break;
    case T::Citation:
    case T::FootnoteReference:
    case T::InlineNote:
    case T::HtmlComment:
    case T::HtmlInline:
    case T::CommentLine:
    case T::HeadingLabel:
    case T::TaskMarker:
    case T::Image:
        return;
    default:
        break;
    }
    for (const md::Node &child : node.children) inlineText(document, child, format, out);
}

void definitionTextOf(const md::Document *document, const md::Node &definition, CslText &out)
{
    bool first = true;
    for (const md::Node &child : definition.children) {
        if (child.isInline()) {
            inlineText(document, child, {}, out);
            continue;
        }
        if (child.type == md::NodeType::CommentLine || child.type == md::NodeType::LinkDefinition) continue;
        CslText block;
        if (child.children.empty() || child.children.front().isInline()) {
            for (const md::Node &inner : child.children) inlineText(document, inner, {}, block);
        } else {
            definitionTextOf(document, child, block);
        }
        if (block.isEmpty()) continue;
        if (!first) out.append(QStringLiteral(" "));
        out.append(block);
        first = false;
    }
}

QString attribute(const QString &value)
{
    return md::escapeHtml(value);
}

} // namespace

QSet<QString> Setup::keys() const
{
    QSet<QString> all;
    const auto add = [&all](const QList<Database> &list) {
        for (const Database &db : list)
            if (db)
                for (const CslReference &reference : db->references()) all.insert(reference.id);
    };
    add(databases);
    for (const QList<Database> &list : fileDatabases) add(list);
    return all;
}

Setup prepare(const Dialect::Choice &dialect, bool linkCitations)
{
    Setup setup;
    if (!dialect.options.citations) return setup;
    setup.linkCitations = linkCitations;
    setup.chosen = !dialect.citationStyle.isEmpty() || !dialect.bibliography.isEmpty();
    setup.styleName = dialect.citationStyle;
    if (!dialect.citationStyle.isEmpty()) {
        QString error;
        if (Dialect::isStyleFile(dialect.citationStyle)) {
            setup.style = styleFile(dialect.citationStyle, &error);
        } else {
            setup.style = CslCatalog::bundledStyle(dialect.citationStyle, &error);
            if (!setup.style && error.isEmpty())
                error = QStringLiteral("The citation style “%1” cannot be used.").arg(dialect.citationStyle);
        }
        if (!setup.style) {
            Problem problem;
            problem.code = QStringLiteral("citation-style");
            problem.severity = md::Severity::Error;
            problem.message = error;
            if (Dialect::isStyleFile(dialect.citationStyle)) problem.source = dialect.citationStyle;
            setup.problems.append(problem);
        }
    }
    QSet<QString> reported;
    setup.databases = databasesOf(dialect.bibliography, setup, reported);
    return setup;
}

bool addIncludedBibliographies(Setup &setup, const md::Document &document)
{
    const QSet<QString> before = setup.keys();
    QSet<QString> reported = reportedOf(setup);
    for (size_t i = 1; i < document.files.size(); ++i) {
        const md::SourceFile &file = document.files[i];
        if (!file.frontMatter.present || file.path.isEmpty() || setup.fileDatabases.contains(file.path)) continue;
        const Dialect::Layer layer = Dialect::frontMatterLayer(file.frontMatter, file.directory);
        if (!layer.bibliography) continue;
        setup.fileDatabases.insert(file.path, databasesOf(*layer.bibliography, setup, reported));
        setup.chosen = true;
    }
    for (const QString &key : setup.keys())
        if (!before.contains(key)) return true;
    return false;
}

const Formatted::Citation *Formatted::find(const md::Node &node) const
{
    const auto it = m_byNode.constFind(&node);
    if (it != m_byNode.constEnd()) return &citations[*it];
    return find(node.file, node.range.start);
}

const Formatted::Citation *Formatted::find(int file, int start) const
{
    const auto it = m_index.constFind(placeKey(file, start));
    return it == m_index.constEnd() ? nullptr : &citations[*it];
}

Formatted format(const md::Document &document, const Setup &setup)
{
    Formatted out;
    out.problems = setup.problems;
    out.linkCitations = setup.linkCitations;
    out.noteStyle = setup.noteStyle();
    if (!setup.chosen || !document.options.citations) return out;
    out.active = true;

    // The citations that are notes of their own, in a note style.
    QHash<const md::Node *, int> citationNotes;
    for (const md::Footnote &note : document.footnotes)
        if (note.content && note.content->type == md::NodeType::Citation) citationNotes.insert(note.content, note.number);
    const auto fileOf = [&document](int file) {
        return file <= 0 || size_t(file) >= document.files.size() ? QString() : document.files[size_t(file)].path;
    };

    // The citations in the order a reader meets them: a note's where the
    // note is first referred to.
    QList<CitationClusterSpec> clusters;
    QList<QList<const md::Node *>> items;
    QSet<int> notesRead;
    std::function<void(const md::Node &, int)> walk = [&](const md::Node &node, int note) {
        switch (node.type) {
        case md::NodeType::FootnoteDefinition:
        case md::NodeType::CitationDefinition:
            return;
        case md::NodeType::FootnoteReference: {
            if (node.number <= 0 || size_t(node.number) > document.footnotes.size() || notesRead.contains(node.number))
                return;
            notesRead.insert(node.number);
            const md::Node *content = document.footnotes[size_t(node.number - 1)].content;
            if (content && content->type == md::NodeType::FootnoteDefinition)
                for (const md::Node &child : content->children) walk(child, node.number);
            return;
        }
        case md::NodeType::InlineNote:
            for (const md::Node &child : node.children) walk(child, node.number);
            return;
        case md::NodeType::Citation: {
            CitationClusterSpec cluster;
            cluster.noteIndex = citationNotes.value(&node, note);
            cluster.sourceFile = fileOf(node.file);
            QList<const md::Node *> parts;
            if (node.children.empty()) parts.append(&node);
            for (const md::Node &child : node.children)
                if (child.type == md::NodeType::Citation) parts.append(&child);
            Formatted::Citation citation;
            citation.node = &node;
            citation.file = node.file;
            citation.range = node.range;
            citation.note = cluster.noteIndex;
            for (const md::Node *part : std::as_const(parts)) {
                CitationItemSpec item;
                item.key = part->label;
                item.locator = part->text;
                item.mode = part->marker == u'-' ? CslCite::Mode::SuppressAuthor : CslCite::Mode::Normal;
                cluster.items.append(item);
                citation.keys.append(part->label);
            }
            const int index = int(out.citations.size());
            out.citations.append(citation);
            out.m_byNode.insert(&node, index);
            if (!out.m_index.contains(placeKey(node.file, node.range.start)))
                out.m_index.insert(placeKey(node.file, node.range.start), index);
            clusters.append(cluster);
            items.append(parts);
            return;
        }
        default:
            for (const md::Node &child : node.children) walk(child, note);
            return;
        }
    };
    walk(document.root, 0);

    // The Writer bibliography written in the text, wherever it stands.
    QList<WriterDefinition> definitions;
    std::function<void(const md::Node &)> collect = [&](const md::Node &node) {
        if (node.type == md::NodeType::CitationDefinition) {
            CslText text;
            definitionTextOf(&document, node, text);
            definitions.append({node.label, text, fileOf(node.file)});
            return;
        }
        if (node.isInline()) return;
        for (const md::Node &child : node.children) collect(child);
    };
    collect(document.root);

    CitationFormatter formatter;
    formatter.setStyle(setup.style);
    formatter.setDatabases(setup.databases);
    for (auto it = setup.fileDatabases.cbegin(); it != setup.fileDatabases.cend(); ++it)
        formatter.setDatabasesFor(it.key(), it.value());
    const CitationOutput output = formatter.format(clusters, definitions);
    for (qsizetype i = 0; i < out.citations.size() && i < output.clusters.size(); ++i)
        out.citations[i].text = output.clusters[i];
    out.bibliography = output.bibliography;
    out.layout = output.layout;

    using Code = CitationDiagnostic::Code;
    for (const CitationDiagnostic &d : output.diagnostics) {
        Problem problem;
        problem.severity = d.severity == CitationDiagnostic::Severity::Error ? md::Severity::Error : md::Severity::Warning;
        problem.message = d.message;
        problem.key = d.key;
        const md::Node *part = d.cluster >= 0 && d.cluster < items.size() && d.item >= 0 && d.item < items[d.cluster].size()
                                   ? items[d.cluster][d.item]
                                   : nullptr;
        if (part) {
            problem.file = part->file;
            problem.range = part->range;
        }
        switch (d.code) {
        case Code::MissingKey:
            // The engine reports a key it could not resolve; this is a key a
            // bibliography has, but not the one this part of the document uses.
            if (!part || !part->resolved) continue;
            problem.code = QStringLiteral("missing-citation");
            break;
        case Code::DuplicateDefinition:
        case Code::Database:
            // The engine reports the one, prepare() the other.
            continue;
        case Code::AmbiguousKey:
            problem.code = QStringLiteral("duplicate-citation-key");
            break;
        case Code::NoStyle:
            // A style that was chosen but cannot be used is reported already.
            if (!setup.styleName.isEmpty()) continue;
            problem.code = QStringLiteral("no-citation-style");
            problem.message = QStringLiteral("No citation style is chosen, so citations of the bibliography show "
                                             "their keys. Choose one in Document settings, or with csl: in the "
                                             "front matter.");
            break;
        case Code::SeparateNumbering:
            problem.code = QStringLiteral("separate-numbering");
            break;
        case Code::Locale:
            problem.code = QStringLiteral("citation-locale");
            break;
        }
        out.problems.append(problem);
    }
    return out;
}

QString entryId(const QString &key)
{
    QString id = key.simplified();
    id.replace(u' ', u'-');
    return QStringLiteral("ref-") + id;
}

std::optional<md::CitationHtml> html(const Formatted &formatted)
{
    if (!formatted.active) return std::nullopt;
    QSet<QString> entries;
    for (const CitationBibliographyEntry &entry : formatted.bibliography) entries.insert(entry.key);
    QHash<const md::Node *, QString> texts;
    for (const Formatted::Citation &citation : formatted.citations) {
        QString inner = citation.text.toHtml();
        bool linked = false;
        for (const CslSpan &span : citation.text.spans()) linked = linked || !span.format.link.isEmpty();
        if (formatted.linkCitations && !linked && citation.keys.size() == 1) {
            QString key = citation.keys.first().trimmed();
            if (key.startsWith(u'#') || key.startsWith(u'@')) key = key.mid(1);
            if (entries.contains(key))
                inner = QStringLiteral("<a href=\"#%1\">%2</a>").arg(attribute(entryId(key)), inner);
        }
        texts.insert(citation.node, QStringLiteral("<span class=\"citation\" data-cites=\"%1\">%2</span>")
                                        .arg(attribute(citation.keys.join(u' ')), inner));
    }
    md::CitationHtml html;
    html.citation = [texts](const md::Node &node) { return texts.value(&node); };
    if (!formatted.bibliography.isEmpty()) {
        QString section = QStringLiteral("<section class=\"bibliography csl-bib-body%1\" role=\"doc-bibliography\">\n")
                              .arg(formatted.layout.hangingIndent ? QStringLiteral(" hanging-indent") : QString());
        for (const CitationBibliographyEntry &entry : formatted.bibliography)
            section += QStringLiteral("<div class=\"csl-entry\" id=\"%1\">%2</div>\n")
                           .arg(attribute(entryId(entry.key)), entry.text.toHtml());
        section += QStringLiteral("</section>");
        html.bibliography = section;
    }
    return html;
}

CslText definitionText(const md::Node &definition)
{
    CslText text;
    definitionTextOf(nullptr, definition, text);
    return text;
}

} // namespace DocumentCitations
