#pragma once
#include <QStringView>
#include <QTextBoundaryFinder>
#include <optional>
#include "sourcetypes.h"
bool sourceGraphemeBoundary(QStringView source, int position);
bool sourceGraphemeRange(QStringView source, SourceRange range);
int previousSourceGrapheme(QStringView source, int position);
int nextSourceGrapheme(QStringView source, int position);
SourceRange sourceParagraphRange(QStringView source, int position);

// A walk over an immutable source. Unicode segmentation is shared by all
// steps in the same paragraph; the source must outlive the walk.
class SourceGraphemes {
public:
    explicit SourceGraphemes(QStringView source) : m_source(source) {}
    int step(int position, bool forward) const;

private:
    QStringView m_source;
    mutable int m_start = 0;
    mutable int m_end = 0;
    mutable std::optional<QTextBoundaryFinder> m_finder;
};
