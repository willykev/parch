#include "treejson.h"

#include "documentcheck.h"

#include <QJsonDocument>
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

    QJsonObject fields(const md::Node &node, const md::Node *contentsAt)
    {
        const md::Node &place = contentsAt ? *contentsAt : node;
        QJsonObject out{{QStringLiteral("type"), QString::fromLatin1(md::typeName(node.type))},
                        {QStringLiteral("file"), place.file},
                        {QStringLiteral("range"), range(place.range)}};
        if (node.isBlock()) out.insert(QStringLiteral("line"), lineOf(place.file, place.range.start));
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
        return out;
    }

    QJsonObject node(const md::Node &root)
    {
        struct Frame {
            const md::Node *node;
            const md::Node *place;
            QJsonArray children;
            size_t next = 0;
        };
        std::vector<Frame> stack{{&root, nullptr, {}}};
        for (;;) {
            Frame &frame = stack.back();
            const auto &children = frame.node->contentsChildren();
            if (frame.next < children.size()) {
                const auto *place = frame.node->contentsOwner ? frame.node : frame.place;
                stack.push_back({&children[frame.next++], place, {}});
                continue;
            }
            QJsonObject value = fields(*frame.node, frame.place);
            if (!frame.children.isEmpty()) value.insert(QStringLiteral("children"), std::move(frame.children));
            stack.pop_back();
            if (stack.empty()) return value;
            stack.back().children.append(std::move(value));
        }
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

// Use Qt's JSON quoting for leaves. The explicit stack and bounded indentation
// keep both the working stack and the bytes per field independent of depth.
QByteArray encode(const QJsonObject &root)
{
    struct Frame {
        QJsonObject object;
        QJsonArray array;
        QStringList keys;
        qsizetype next = 0;
        bool list = false;
    };
    QByteArray bytes;
    std::vector<Frame> stack;
    const auto leaf = [&](const QJsonValue &value) {
        const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
        bytes.append(wrapped.constData() + 1, wrapped.size() - 2);
    };
    const auto open = [&](const QJsonValue &value) {
        if (value.isObject()) {
            const auto object = value.toObject();
            bytes += '{';
            stack.push_back({object, {}, object.keys(), 0, false});
        } else if (value.isArray()) {
            bytes += '[';
            stack.push_back({{}, value.toArray(), {}, 0, true});
        } else leaf(value);
    };
    const auto newline = [&](size_t depth) {
        bytes += '\n';
        bytes.append(qsizetype(std::min<size_t>(depth, 16) * 4), ' ');
    };
    open(root);
    while (!stack.empty()) {
        Frame &frame = stack.back();
        const auto count = frame.list ? frame.array.size() : frame.keys.size();
        if (frame.next == count) {
            newline(stack.size() - 1);
            bytes += frame.list ? ']' : '}';
            stack.pop_back();
            continue;
        }
        if (frame.next) bytes += ',';
        newline(stack.size());
        const auto i = frame.next++;
        if (frame.list) open(frame.array.at(i));
        else {
            const QString &key = frame.keys.at(i);
            leaf(key);
            bytes += ": ";
            open(frame.object.value(key));
        }
    }
    bytes += '\n';
    return bytes;
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
