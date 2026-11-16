#pragma once

#include "mdtree.h"

#include <QJsonArray>
#include <QJsonObject>

// A parsed document as JSON, for scripts: `parch render --to json-tree` and
// `--to outline`.
//
// A node is {"type": "heading", "file": 0, "range": [start, end], ...}: its
// type by md::typeName(), the index of its file in "files", and its range in
// that file as UTF-16 offsets, start included and end not. A block also has
// its first "line", from 1. Then whatever the node has: "open", "close" and
// "target" ranges, "level", "ordered", "tight", "number", "text", "dest",
// "title", "label", "align", "attributes", "children" and the rest of the
// fields of md::Node, each only where it is set; "resolved" only when it is
// false and "generated" only when it is true.
namespace TreeJson {

// {"files": [{"path", "directory", "includedFrom", "frontMatter"}],
//  "root": node, "incomplete": bool}
QJsonObject document(const md::Document &document);

// [{"level", "text", "slug", "label", "file", "line"}], in document order.
QJsonArray outline(const md::Document &document);

QJsonArray files(const md::Document &document);

// Front matter as plain JSON: mappings, lists, strings and nulls.
QJsonValue yaml(const md::YamlValue &value);

} // namespace TreeJson
