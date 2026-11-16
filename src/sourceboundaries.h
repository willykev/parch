#pragma once
#include <QStringView>
#include "sourcetypes.h"
bool sourceGraphemeBoundary(QStringView source, int position);
bool sourceGraphemeRange(QStringView source, SourceRange range);
int previousSourceGrapheme(QStringView source, int position);
int nextSourceGrapheme(QStringView source, int position);
SourceRange sourceParagraphRange(QStringView source, int position);
