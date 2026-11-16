#include "pdfoutline.h"

#include <QHash>
#include <QRegularExpression>

#include <functional>

namespace PdfOutline {

namespace {

// The objects of a PDF by number, where the cross-reference table puts them.
struct Layout {
    qsizetype xref = -1;
    int size = 0;
    int root = 0;
    QByteArray info; // "/Info 1 0 R", as written
    QByteArray id;   // "/ID [<…> <…>]", as written
    QHash<int, qsizetype> offsets;
};

bool fail(QString *error, const QString &why)
{
    if (error) *error = why;
    return false;
}

bool readLayout(const QByteArray &pdf, Layout &layout, QString *error)
{
    const qsizetype start = pdf.lastIndexOf("startxref");
    if (start < 0) return fail(error, QStringLiteral("the PDF has no cross-reference table"));
    static const QRegularExpression number(QStringLiteral("startxref\\s+(\\d+)"));
    const QRegularExpressionMatch found = number.match(QString::fromLatin1(pdf.mid(start, 40)));
    if (!found.hasMatch()) return fail(error, QStringLiteral("the PDF has no cross-reference table"));
    layout.xref = found.captured(1).toLongLong();
    if (layout.xref <= 0 || layout.xref >= pdf.size() || !pdf.mid(layout.xref, 4).startsWith("xref"))
        return fail(error, QStringLiteral("the PDF's cross-reference table is not where it says"));
    const qsizetype trailer = pdf.indexOf("trailer", layout.xref);
    if (trailer < 0) return fail(error, QStringLiteral("the PDF has no trailer"));
    // Every subsection of the table: its first number and count, then its entries.
    const QList<QByteArray> lines = pdf.mid(layout.xref + 4, trailer - layout.xref - 4).split('\n');
    int next = 0;
    for (QByteArray line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() == 2) {
            next = fields[0].toInt();
            continue;
        }
        if (fields.size() >= 3 && fields[2] == "n") layout.offsets.insert(next, fields[0].toLongLong());
        ++next;
    }
    const QByteArray tail = pdf.mid(trailer, start - trailer);
    static const QRegularExpression size(QStringLiteral("/Size\\s+(\\d+)"));
    static const QRegularExpression root(QStringLiteral("/Root\\s+(\\d+)\\s+0\\s+R"));
    static const QRegularExpression info(QStringLiteral("/Info\\s+\\d+\\s+0\\s+R"));
    static const QRegularExpression id(QStringLiteral("/ID\\s*\\[[^\\]]*\\]"));
    const QString text = QString::fromLatin1(tail);
    layout.size = size.match(text).captured(1).toInt();
    layout.root = root.match(text).captured(1).toInt();
    layout.info = info.match(text).captured(0).toLatin1();
    layout.id = id.match(text).captured(0).toLatin1();
    if (layout.size <= 0 || layout.root <= 0 || !layout.offsets.contains(layout.root))
        return fail(error, QStringLiteral("the PDF's trailer is not as expected"));
    return true;
}

// The dictionary of an object, without its << and >>.
QByteArray dictionary(const QByteArray &pdf, const Layout &layout, int object)
{
    const qsizetype at = layout.offsets.value(object, -1);
    if (at < 0) return {};
    const qsizetype end = pdf.indexOf("endobj", at);
    const qsizetype open = pdf.indexOf("<<", at);
    if (end < 0 || open < 0 || open > end) return {};
    const qsizetype close = pdf.lastIndexOf(">>", end);
    if (close < open) return {};
    return pdf.mid(open + 2, close - open - 2);
}

QList<int> references(const QByteArray &text)
{
    QList<int> refs;
    static const QRegularExpression ref(QStringLiteral("(\\d+)\\s+0\\s+R"));
    for (auto match = ref.globalMatch(QString::fromLatin1(text)); match.hasNext();) refs.append(match.next().captured(1).toInt());
    return refs;
}

// A text string as PDF writes any text: UTF-16 with its mark, in hex.
QByteArray textString(const QString &text)
{
    QByteArray bytes("<FEFF");
    for (const QChar c : text) bytes += QByteArray::number(c.unicode(), 16).rightJustified(4, '0').toUpper();
    return bytes + '>';
}

QString readTextString(const QByteArray &hex)
{
    QByteArray digits = hex;
    digits.replace(' ', "");
    if (digits.startsWith("FEFF") || digits.startsWith("feff")) digits = digits.mid(4);
    QString text;
    for (qsizetype i = 0; i + 4 <= digits.size(); i += 4) text.append(QChar(char16_t(digits.mid(i, 4).toUInt(nullptr, 16))));
    return text;
}

QByteArray real(qreal value) { return QByteArray::number(value, 'f', 2); }

} // namespace

bool add(QByteArray &pdf, const QList<Entry> &entries, QString *error)
{
    if (entries.isEmpty()) return true;
    Layout layout;
    if (!readLayout(pdf, layout, error)) return false;
    const QByteArray catalog = dictionary(pdf, layout, layout.root);
    if (catalog.isEmpty() || catalog.contains("/Outlines")) return fail(error, QStringLiteral("the PDF's catalog is not as expected"));
    static const QRegularExpression pagesRef(QStringLiteral("/Pages\\s+(\\d+)\\s+0\\s+R"));
    const int pagesObject = pagesRef.match(QString::fromLatin1(catalog)).captured(1).toInt();
    const QByteArray pagesTree = dictionary(pdf, layout, pagesObject);
    const qsizetype kids = pagesTree.indexOf("/Kids");
    if (kids < 0) return fail(error, QStringLiteral("the PDF has no pages"));
    const QList<int> pages = references(pagesTree.mid(kids, pagesTree.indexOf(']', kids) - kids));
    QHash<int, qreal> heights;
    static const QRegularExpression mediaBox(QStringLiteral("/MediaBox\\s*\\[\\s*[-0-9.]+\\s+[-0-9.]+\\s+[-0-9.]+\\s+([-0-9.]+)"));
    for (const int page : pages) {
        const QByteArray object = dictionary(pdf, layout, page);
        if (object.contains("/Type /Pages")) return fail(error, QStringLiteral("the PDF's pages are nested"));
        QRegularExpressionMatch box = mediaBox.match(QString::fromLatin1(object));
        if (!box.hasMatch()) box = mediaBox.match(QString::fromLatin1(pagesTree));
        heights.insert(page, box.hasMatch() ? box.captured(1).toDouble() : 842);
    }
    if (pages.isEmpty()) return fail(error, QStringLiteral("the PDF has no pages"));

    // The tree of entries: each under the nearest entry before it of a lower level.
    const int count = int(entries.size());
    const int first = layout.size; // the outline's own object; the entries follow
    std::vector<int> parent(count, -1), firstChild(count, -1), lastChild(count, -1), previous(count, -1),
        next(count, -1), descendants(count, 0);
    int rootFirst = -1, rootLast = -1;
    std::vector<int> stack;
    for (int i = 0; i < count; ++i) {
        while (!stack.empty() && entries[stack.back()].level >= entries[i].level) stack.pop_back();
        const int up = stack.empty() ? -1 : stack.back();
        parent[i] = up;
        int &head = up < 0 ? rootFirst : firstChild[up];
        int &tail = up < 0 ? rootLast : lastChild[up];
        if (head < 0) head = i;
        if (tail >= 0) {
            next[tail] = i;
            previous[i] = tail;
        }
        tail = i;
        for (int a = up; a >= 0; a = parent[a]) ++descendants[a];
        stack.push_back(i);
    }
    const auto objectOf = [first](int entry) { return QByteArray::number(first + 1 + entry); };

    QByteArray update = "\n";
    QList<std::pair<int, qsizetype>> written;
    const auto object = [&](int number, const QByteArray &body) {
        written.append({number, pdf.size() + update.size()});
        update += QByteArray::number(number) + " 0 obj\n<<\n" + body + ">>\nendobj\n";
    };
    object(layout.root, catalog.trimmed() + "\n/Outlines " + QByteArray::number(first) + " 0 R\n/PageMode /UseOutlines\n");
    object(first, "/Type /Outlines\n/First " + objectOf(rootFirst) + " 0 R\n/Last " + objectOf(rootLast) +
                      " 0 R\n/Count " + QByteArray::number(count) + "\n");
    for (int i = 0; i < count; ++i) {
        const Entry &entry = entries[i];
        const int page = pages.value(std::clamp(entry.page, 0, int(pages.size()) - 1));
        QByteArray body = "/Title " + textString(entry.title.simplified()) + "\n";
        body += "/Parent " + (parent[i] < 0 ? QByteArray::number(first) : objectOf(parent[i])) + " 0 R\n";
        if (previous[i] >= 0) body += "/Prev " + objectOf(previous[i]) + " 0 R\n";
        if (next[i] >= 0) body += "/Next " + objectOf(next[i]) + " 0 R\n";
        if (firstChild[i] >= 0) {
            body += "/First " + objectOf(firstChild[i]) + " 0 R\n/Last " + objectOf(lastChild[i]) + " 0 R\n";
            body += "/Count " + QByteArray::number(descendants[i]) + "\n";
        }
        body += "/Dest [" + QByteArray::number(page) + " 0 R /XYZ 0 " + real(heights.value(page) - entry.top) + " null]\n";
        object(first + 1 + i, body);
    }
    std::sort(written.begin(), written.end());
    const qsizetype xref = pdf.size() + update.size();
    update += "xref\n";
    for (qsizetype i = 0; i < written.size();) {
        qsizetype j = i;
        while (j + 1 < written.size() && written[j + 1].first == written[j].first + 1) ++j;
        update += QByteArray::number(written[i].first) + ' ' + QByteArray::number(j - i + 1) + '\n';
        for (qsizetype k = i; k <= j; ++k)
            update += QByteArray::number(written[k].second).rightJustified(10, '0') + " 00000 n \n";
        i = j + 1;
    }
    update += "trailer\n<<\n/Size " + QByteArray::number(first + 1 + count) + "\n/Root " +
              QByteArray::number(layout.root) + " 0 R\n";
    if (!layout.info.isEmpty()) update += layout.info + '\n';
    if (!layout.id.isEmpty()) update += layout.id + '\n';
    update += "/Prev " + QByteArray::number(layout.xref) + "\n>>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    pdf += update;
    return true;
}

QList<std::pair<int, QString>> read(const QByteArray &pdf)
{
    QList<std::pair<int, QString>> outline;
    Layout layout;
    if (!readLayout(pdf, layout, nullptr)) return outline;
    // The newest objects win: read every object of the file, the last of each number.
    QHash<int, QByteArray> objects;
    static const QRegularExpression header(QStringLiteral("(?:^|\\n)(\\d+) 0 obj\\n"));
    const QString text = QString::fromLatin1(pdf);
    for (auto match = header.globalMatch(text); match.hasNext();) {
        const QRegularExpressionMatch found = match.next();
        const qsizetype end = pdf.indexOf("endobj", found.capturedEnd());
        if (end > 0) objects.insert(found.captured(1).toInt(), pdf.mid(found.capturedEnd(), end - found.capturedEnd()));
    }
    static const QRegularExpression outlines(QStringLiteral("/Outlines\\s+(\\d+)\\s+0\\s+R"));
    const int root = outlines.match(QString::fromLatin1(objects.value(layout.root))).captured(1).toInt();
    static const QRegularExpression firstRef(QStringLiteral("/First\\s+(\\d+)\\s+0\\s+R"));
    static const QRegularExpression nextRef(QStringLiteral("/Next\\s+(\\d+)\\s+0\\s+R"));
    static const QRegularExpression title(QStringLiteral("/Title\\s*<([0-9A-Fa-f\\s]*)>"));
    std::function<void(int, int)> walk = [&](int object, int level) {
        for (int item = firstRef.match(QString::fromLatin1(objects.value(object))).captured(1).toInt(); item > 0;) {
            const QString body = QString::fromLatin1(objects.value(item));
            outline.append({level, readTextString(title.match(body).captured(1).toLatin1())});
            if (outline.size() > 10000) return;
            walk(item, level + 1);
            item = nextRef.match(body).captured(1).toInt();
        }
    };
    if (root > 0) walk(root, 1);
    return outline;
}

} // namespace PdfOutline
