#pragma once

#include <QString>

// Paths as text, as they come from a command line, a session or another
// launch. Nothing here asks the file system: what a path names is the
// kernel's to resolve, so paths are never tidied as text.
namespace PathText {
// The longest path accepted from outside, as Linux's PATH_MAX counts it.
constexpr int MaxLength = 4096;
// A path parch accepts from outside: absolute, without NUL, and not longer than MaxLength.
bool isFullPath(const QString &path);
// The folder part, taken as text, so that a path in a missing folder never
// turns into a different, existing folder.
QString folderOf(const QString &path);
QString fileName(const QString &path);
} // namespace PathText
