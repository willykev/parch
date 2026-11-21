#include "treejson.h"

#include "documentcheck.h"

#include <memory>

namespace TreeJson {
namespace {

QJsonArray range(md::Range range)
{
    return QJsonArray{range.start, range.end};
}

const char *alignName(md::Align align)
{
    switch (align) {
    case md::Align::None: return nullptr;
    case md::Align::Left: return "left";
    case md::Align::Center: return "center";
    case md::Align::Right: return "right";
    }
    return nullptr;
}

class Writer {
public:
    explicit Writer(const md::Document &document) : doc(document), lines(document.files.size()) {}

    int lineOf(int file, int offset)
    {
        if (file < 0 || size_t(file) >= lines.size()) return 0;
        std::unique_ptr<DocumentCheck::LineIndex> &index = lines[size_t(file)];
        if (!index) index = std::make_unique<DocumentCheck::LineIndex>(doc.files[size_t(file)].text);
        return index->at(offset).line;
    }

    QJsonObject node(const md::Node &node)
    {
        QJsonObject out{{QStringLiteral("type"), QString::fromLatin1(md::typeName(node.type))},
                        {QStringLiteral("file"), node.file},
                        {QStringLiteral("range"), range(node.range)}};
        if (node.isBlock()) out.insert(QStringLiteral("line"), lineOf(node.file, node.range.start));
        if (!node.open.isEmpty()) out.insert(QStringLiteral("open"), range(node.open));
        if (!node.close.isEmpty()) out.insert(QStringLiteral("close"), range(node.close));
        if (!node.target.isEmpty()) out.insert(QStringLiteral("target"), range(node.target));
        if (const char *align = alignName(node.align)) out.insert(QStringLiteral("align"), QString::fromLatin1(align));
        if (node.level) out.insert(QStringLiteral("level"), node.level);
        if (!node.marker.isNull()) out.insert(QStringLiteral("marker"), QString(node.marker));
        const std::pair<const char *, bool> flags[] = {
            {"tight", node.tight},   {"ordered", node.ordered}, {"task", node.task},
            {"checked", node.checked}, {"header", node.header}, {"fenced", node.fenced},
            {"setext", node.setext}, {"display", node.display}, {"generated", node.generated},
        };
        for (const auto &[name, on] : flags)
            if (on) out.insert(QLatin1StringView(name), true);
        if (!node.resolved) out.insert(QStringLiteral("resolved"), false);
        if (node.number) out.insert(QStringLiteral("number"), node.number);
        if (node.sourceNumber) out.insert(QStringLiteral("sourceNumber"), node.sourceNumber);
        if (node.columns) out.insert(QStringLiteral("columns"), node.columns);
        if (node.type == md::NodeType::TableCell) {
            out.insert(QStringLiteral("column"), node.column);
            out.insert(QStringLiteral("span"), node.span);
        }
        if (node.includedFile >= 0) out.insert(QStringLiteral("includedFile"), node.includedFile);
        const std::pair<const char *, const QString *> texts[] = {
            {"text", &node.text}, {"dest", &node.dest}, {"title", &node.title}, {"label", &node.label}};
        for (const auto &[name, value] : texts)
            if (!value->isEmpty()) out.insert(QLatin1StringView(name), *value);
        if (!node.attributes.empty()) {
            QJsonObject attributes;
            for (const md::Attribute &attribute : node.attributes) attributes.insert(attribute.name, attribute.value);
            out.insert(QStringLiteral("attributes"), attributes);
        }
        if (!node.markers.empty() && node.type != md::NodeType::CodeBlock) {
            QJsonArray markers;
            for (const md::Range &marker : node.markers) markers.append(range(marker));
            out.insert(QStringLiteral("markers"), markers);
        }
        if (!node.children.empty()) {
            QJsonArray children;
            for (const md::Node &child : node.children) children.append(this->node(child));
            out.insert(QStringLiteral("children"), children);
        }
        return out;
    }

    const md::Document &doc;
    std::vector<std::unique_ptr<DocumentCheck::LineIndex>> lines;
};

} // namespace

QJsonValue yaml(const md::YamlValue &value)
{
    switch (value.kind) {
    case md::YamlValue::Kind::Null: return QJsonValue();
    case md::YamlValue::Kind::Scalar: return value.scalar;
    case md::YamlValue::Kind::List: {
        QJsonArray list;
        for (const md::YamlValue &item : value.list) list.append(yaml(item));
        return list;
    }
    case md::YamlValue::Kind::Map: {
        QJsonObject map;
        // The first of a repeated key, as YamlValue::find() reads it.
        for (const auto &[key, item] : value.map)
            if (!map.contains(key)) map.insert(key, yaml(item));
        return map;
    }
    }
    return QJsonValue();
}

QJsonArray files(const md::Document &document)
{
    QJsonArray list;
    for (const md::SourceFile &file : document.files) {
        list.append(QJsonObject{
            {QStringLiteral("path"), file.path.isEmpty() ? QJsonValue() : QJsonValue(file.path)},
            {QStringLiteral("directory"), file.directory.isEmpty() ? QJsonValue() : QJsonValue(file.directory)},
            {QStringLiteral("includedFrom"), file.includedFrom},
            {QStringLiteral("frontMatter"), file.frontMatter.present ? yaml(file.frontMatter.data) : QJsonValue()},
        });
    }
    return list;
}

QJsonObject document(const md::Document &document)
{
    Writer writer(document);
    return QJsonObject{
        {QStringLiteral("files"), files(document)},
        {QStringLiteral("root"), writer.node(document.root)},
        {QStringLiteral("incomplete"), document.incomplete},
    };
}

QJsonArray outline(const md::Document &document)
{
    Writer writer(document);
    QJsonArray headings;
    for (const md::Heading &heading : document.headings) {
        headings.append(QJsonObject{
            {QStringLiteral("level"), heading.level},
            {QStringLiteral("text"), heading.text},
            {QStringLiteral("slug"), heading.slug},
            {QStringLiteral("label"), heading.label.isEmpty() ? QJsonValue() : QJsonValue(heading.label)},
            {QStringLiteral("file"), heading.file},
            {QStringLiteral("line"), writer.lineOf(heading.file, heading.range.start)},
        });
    }
    return headings;
}

} // namespace TreeJson
