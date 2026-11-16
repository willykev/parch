#pragma once

#include "sourcetypes.h"

#include <QList>
#include <QStringView>

// The parts of a Markdown source that are not prose, which spelling leaves
// alone: front matter; fenced and indented code; display and inline math;
// code spans; HTML comments and tags; Writer `//` comment lines; URLs,
// autolinks and email addresses; link destinations and reference
// definitions; and the labels of references, footnotes, citations,
// variables and `{{TOC}}`. In order and apart, as source ranges.
//
// A small classifier of its own, line by line, until the Markdown parser's
// prose ranges take its place: this one function is what spelling asks.
// What it cannot tell apart it leaves to be checked, so that prose is never
// skipped; code spans and math are found within one line.
QList<SourceRange> spellingExclusions(QStringView text);
