#pragma once

#include "previewstyle.h"

#include <QList>
#include <QString>

// The colouring of code, from KSyntaxHighlighting's definitions: which runs
// of a text are keywords, strings, comments and so on, each named by the
// ink a PreviewStyle draws it in, so that the colours are the theme's and
// not a highlighting theme's. A language is named as a fence's info string
// names it ("python", "c++", "js", "sh"); code in a language nobody knows
// has no runs and is drawn as plain code.
//
// Safe from any thread. Results are kept, so the same code is read once:
// a preview's worker reads the code of a document before the UI thread
// builds it.
namespace CodeHighlight {

struct Span {
    int start = 0;
    int length = 0;
    PreviewStyle::Ink ink = PreviewStyle::Code;
};

// The runs of `code` that are not plain, in order. Code longer than
// MaxLength is not read.
inline constexpr qsizetype MaxLength = 400000;
QList<Span> spans(const QString &language, const QString &code);
// Whether a language is one that can be coloured.
bool knows(const QString &language);
// The first word of a fence's info string, in lower case.
QString languageOf(const QString &info);
// Loads the list of languages, which belongs to the thread the application
// object lives on: called there, before code is read on any other thread.
// Code read on another thread before then is left plain.
void prepare();
// Lets go of the list of languages, as the application's end does, so that
// a test starts as a launch does.
void unload();

} // namespace CodeHighlight
