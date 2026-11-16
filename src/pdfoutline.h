#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// The outline of a PDF, the bookmarks a reader lists beside the pages, which
// Qt's PDF writer does not make. It is added to what the writer wrote as an
// incremental update, which the PDF format provides for: the file as it
// was, then the new objects, a cross-reference section for them and a
// trailer that leads back to the old one. Nothing the writer wrote changes.
namespace PdfOutline {

struct Entry {
    // 1 for a top-level heading; deeper levels nest under the one before.
    int level = 1;
    QString title;
    // From 0.
    int page = 0;
    // From the top of the page, in points.
    qreal top = 0;
};

// Adds the entries as the PDF's outline and has readers show it. False,
// with why, when the PDF is not laid out as Qt writes it; it is then left
// as it was.
bool add(QByteArray &pdf, const QList<Entry> &entries, QString *error = nullptr);

// The outline of a PDF as add() writes it, read back for tests: each entry's
// level and title, in order.
QList<std::pair<int, QString>> read(const QByteArray &pdf);

} // namespace PdfOutline
