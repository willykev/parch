#pragma once

#include <QList>
#include <QString>

// The formatting of one run of citation or bibliography text. It covers what
// CSL can ask for, so that the native preview and every exporter can render
// the same result in its own markup.
struct CslFormat {
    enum class Position : quint8 { Baseline, Superscript, Subscript };
    // How CSL's display attribute places a run within a bibliography entry.
    enum class Display : quint8 { Inline, Block, LeftMargin, RightInline, Indent };

    bool italic = false;
    bool bold = false;
    bool smallCaps = false;
    bool underline = false;
    Position position = Position::Baseline;
    Display display = Display::Inline;
    // The target when the run is a link, such as a URL or a DOI.
    QString link;

    bool operator==(const CslFormat &other) const = default;
};

struct CslSpan {
    QString text;
    CslFormat format;

    bool operator==(const CslSpan &other) const = default;
};

// Formatted citation text: runs of text, each with one format. Adjacent runs
// always differ in format and no run is empty, so that two equal texts have
// equal runs.
class CslText {
public:
    CslText() = default;
    explicit CslText(const QString &plain);

    void append(const QString &text, const CslFormat &format = {});
    void append(const CslText &other);
    const QList<CslSpan> &spans() const { return m_spans; }
    bool isEmpty() const { return m_spans.isEmpty(); }
    QString toPlainText() const;
    // HTML that is safe to embed: every character of the text is escaped,
    // and only <i>, <b>, <u>, <sup>, <sub>, <a href>, small-caps spans and
    // csl-* display divisions appear.
    QString toHtml() const;
    // Reads that HTML, and the markup CSL JSON allows in its fields
    // (<i>, <b>, <sup>, <sub>, small-caps and nocase spans), back into runs.
    static CslText fromHtml(const QString &html);

    bool operator==(const CslText &other) const = default;

private:
    QList<CslSpan> m_spans;
};

namespace CslMarkup {
// One piece of a run of HTML-like markup: text, an opening tag or a closing tag.
struct Token {
    enum class Type { Text, Open, Close };
    Type type = Type::Text;
    // The text, with entities decoded, or the lowercase tag name.
    QString text;
    // For an opening tag: its class and style attributes, and an href.
    QString cssClass;
    QString style;
    QString href;
    bool selfClosing = false;
};
// Splits text into tokens. Only well-formed tags of the HTML subset in
// CslText::fromHtml become tags; any other '<' stays text.
QList<Token> tokenize(const QString &markup);
QString escapeHtml(const QString &text);
} // namespace CslMarkup
