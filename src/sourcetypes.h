#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>

using Revision = quint64;
struct SourceRange { int start; int length; }; // UTF-16, half-open range.
Q_DECLARE_METATYPE(SourceRange)
struct SourceSnapshot { Revision revision; QString text; };
