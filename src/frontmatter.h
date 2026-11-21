#pragma once

#include <QString>
#include <QStringView>

#include <optional>
#include <utility>
#include <vector>

// Front matter: the YAML block between `---` lines at the very start of a
// document (after an optional byte order mark). The block is found and read
// here; the text of the file is never changed, so unknown keys survive.
//
// The YAML understood is the ordinary data subset: block mappings and
// sequences nested by indentation, flow sequences `[a, b]` and mappings
// `{a: b}`, plain, single-quoted and double-quoted scalars, literal `|` and
// folded `>` block scalars, and `#` comments. Every scalar is kept as text.
// Tags (`!name`, `!!type`) are never acted on: the tag is dropped, its value
// is read as plain data, and an issue records it. Anchors and aliases are
// read as plain text.
namespace md {

struct YamlValue {
    enum class Kind { Null, Scalar, List, Map };
    Kind kind = Kind::Null;
    QString scalar;                                    // Kind::Scalar
    std::vector<YamlValue> list;                       // Kind::List
    std::vector<std::pair<QString, YamlValue>> map;    // Kind::Map, in source order
    // Offsets into the input, plus its base offset. A scalar can have a
    // starting offset without a single exact span, such as a folded value.
    int sourceOffset = -1;
    int sourceLength = 0;                              // includes quotes when present
    bool inFlow = false;

    bool isScalar() const { return kind == Kind::Scalar; }
    bool isList() const { return kind == Kind::List; }
    bool isMap() const { return kind == Kind::Map; }
    // The value of a key of a mapping, the first that matches; null when
    // this is not a mapping or has no such key.
    const YamlValue *find(QStringView key, Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive) const;
};

struct FrontMatterIssue {
    int offset = 0; // UTF-16 offset into the text that was parsed
    QString message;
};

struct FrontMatter {
    bool present = false;
    // UTF-16 offsets into the text given to parseFrontMatter(). The block
    // runs from the opening `---` to the end of the closing line, without
    // that line's terminator; the content is the YAML between the two.
    int start = 0;
    int end = 0;
    int contentStart = 0;
    int contentEnd = 0;
    // Where the document proper begins: after the closing line's terminator.
    int bodyStart = 0;
    QString source;   // the YAML exactly as written
    YamlValue data;   // a mapping when the YAML is one, otherwise what it is
    std::vector<FrontMatterIssue> issues;

    // A top-level scalar by key, compared without regard to case. Nothing
    // when the key is missing or its value is a list or a mapping.
    std::optional<QString> scalar(QStringView key) const;
};

// Finds and reads the front matter of a document. `present` is false when the
// text does not begin with a `---` line or that block is never closed by a
// `---` or `...` line; such text is ordinary document content.
FrontMatter parseFrontMatter(QStringView text);

// Reads YAML text on its own. Offsets in `issues` are relative to `yaml`
// plus `baseOffset`.
YamlValue parseYaml(QStringView yaml, std::vector<FrontMatterIssue> *issues = nullptr, int baseOffset = 0);

} // namespace md
