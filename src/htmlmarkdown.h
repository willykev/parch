#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

// Rich text, as the clipboard or a drop offers it in HTML, turned into clean
// Markdown that means what the HTML showed: headings, paragraphs, emphasis,
// strikethrough, code, links, images, lists and task lists, quotes, pipe
// tables and line breaks. Scripts, styles, comments, hidden elements and
// presentational markup are dropped, whitespace collapses as a browser
// collapses it, and text is escaped wherever it would otherwise turn into
// Markdown of its own. Nothing is fetched: remote images stay links to where
// they are. The HTML is read by a small, tolerant tokenizer that never fails,
// keeps its tree at most a few hundred elements deep and takes time in
// proportion to the input, so hostile input costs no more than its size.
namespace HtmlMarkdown {

// Saves the bytes of an image the HTML carried inside a data: URL and returns
// the path the Markdown refers to it by, relative to the document; or nothing,
// which leaves the image out and keeps its alt text.
using ImageSink = std::function<QString(const QByteArray &bytes, const QString &mimeType)>;

inline constexpr qsizetype MaxInputBytes = 16 * 1024 * 1024;
inline constexpr qsizetype SynchronousChars = 64 * 1024;
bool fitsInput(QStringView html);

// The Markdown for the HTML, without a final line break; nothing when the
// HTML shows nothing. Line breaks are LF. `inlineOnly`, when given, says
// whether the Markdown is one paragraph, which can go inside a line.
QString convert(const QString &html, const ImageSink &images = {}, bool *inlineOnly = nullptr,
                bool mergedCells = false, QStringList *imageErrors = nullptr,
                qsizetype maxOutput = MaxInputBytes, const QString &longestImagePath = {});

struct TableCells {
    QList<QStringList> rows;
    bool hasGeometry = false; // spans or more than one table
    bool tooLarge = false;
};

// The first table's visible cell text. Spanned positions are empty except
// at the origin, and the rows hold the same number of columns.
std::optional<TableCells> firstTableCells(const QString &html);

// Whether the HTML may carry images in data: URLs, which only a sink keeps.
bool mayEmbedImages(const QString &html);

// Whether all the HTML shows is one image, as a browser's Copy Image puts
// on the clipboard beside the image itself; `alt` is its alt text.
bool isLoneImage(const QString &html, QString *alt = nullptr);

// Text as Markdown shows it literally inside a line, as a link's text.
QString escapeText(const QString &text);

// A URL as a link destination, as little changed as Markdown allows: spaces
// and angle brackets percent-encoded, unbalanced parentheses escaped.
QString encodeUrl(const QString &url);

} // namespace HtmlMarkdown
