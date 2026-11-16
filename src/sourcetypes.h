#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>

using Revision = quint64;
struct SourceRange { int start; int length; }; // UTF-16, half-open range.
Q_DECLARE_METATYPE(SourceRange)
// One replacement of an edit that changes several places at once.
struct SourceEdit { SourceRange range; QString replacement; };
struct SourceSnapshot { Revision revision; QString text; };
