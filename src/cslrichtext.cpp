#include "cslrichtext.h"

#include <QRegularExpression>

CslText::CslText(const QString &plain)
{
    append(plain);
}

void CslText::append(const QString &text, const CslFormat &format)
{
    if (text.isEmpty()) return;
    if (!m_spans.isEmpty() && m_spans.last().format == format)
        m_spans.last().text += text;
    else
        m_spans.append({text, format});
}

void CslText::append(const CslText &other)
{
    for (const CslSpan &span : other.m_spans) append(span.text, span.format);
}

QString CslText::toPlainText() const
{
    QString text;
    for (const CslSpan &span : m_spans) text += span.text;
    return text;
}

QString bibliographyLink(const QString &address)
{
    // As a browser reads an address: without the spaces and control
    // characters around it, and without tabs and line breaks inside it.
    QString link;
    link.reserve(address.size());
    for (const QChar c : address)
        if (c.unicode() > 0x20 && c.unicode() != 0x7f) link += c;
    const QString lower = link.toLower();
    if (lower.startsWith(QLatin1StringView("https://")) || lower.startsWith(QLatin1StringView("http://"))
        || lower.startsWith(QLatin1StringView("mailto:")))
        return link;
    if (lower.startsWith(QLatin1StringView("doi:"))) {
        QString doi = link.mid(4);
        while (doi.startsWith(u'/')) doi.remove(0, 1);
        return doi.isEmpty() ? QString() : QStringLiteral("https://doi.org/") + doi;
    }
    return {};
}

namespace {

// The inline tags of a run, outermost first, so that runs sharing a format
// share its tags.
enum class Tag { Link, Bold, Italic, SmallCaps, Underline, Superscript, Subscript };

QList<Tag> tagsOf(const CslFormat &format)
{
    QList<Tag> tags;
    // A link that came by any other way than the engine is checked again here.
    if (!bibliographyLink(format.link).isEmpty()) tags.append(Tag::Link);
    if (format.bold) tags.append(Tag::Bold);
    if (format.italic) tags.append(Tag::Italic);
    if (format.smallCaps) tags.append(Tag::SmallCaps);
    if (format.underline) tags.append(Tag::Underline);
    if (format.position == CslFormat::Position::Superscript) tags.append(Tag::Superscript);
    if (format.position == CslFormat::Position::Subscript) tags.append(Tag::Subscript);
    return tags;
}

QString openTag(Tag tag, const CslFormat &format)
{
    switch (tag) {
    case Tag::Link:
        return QStringLiteral("<a href=\"%1\">").arg(CslMarkup::escapeHtml(bibliographyLink(format.link)));
    case Tag::Bold:
        return QStringLiteral("<b>");
    case Tag::Italic:
        return QStringLiteral("<i>");
    case Tag::SmallCaps:
        return QStringLiteral("<span style=\"font-variant:small-caps;\">");
    case Tag::Underline:
        return QStringLiteral("<u>");
    case Tag::Superscript:
        return QStringLiteral("<sup>");
    case Tag::Subscript:
        return QStringLiteral("<sub>");
    }
    return {};
}

QString closeTag(Tag tag)
{
    switch (tag) {
    case Tag::Link:
        return QStringLiteral("</a>");
    case Tag::Bold:
        return QStringLiteral("</b>");
    case Tag::Italic:
        return QStringLiteral("</i>");
    case Tag::SmallCaps:
        return QStringLiteral("</span>");
    case Tag::Underline:
        return QStringLiteral("</u>");
    case Tag::Superscript:
        return QStringLiteral("</sup>");
    case Tag::Subscript:
        return QStringLiteral("</sub>");
    }
    return {};
}

QString displayClass(CslFormat::Display display)
{
    switch (display) {
    case CslFormat::Display::Block:
        return QStringLiteral("csl-block");
    case CslFormat::Display::LeftMargin:
        return QStringLiteral("csl-left-margin");
    case CslFormat::Display::RightInline:
        return QStringLiteral("csl-right-inline");
    case CslFormat::Display::Indent:
        return QStringLiteral("csl-indent");
    case CslFormat::Display::Inline:
        break;
    }
    return {};
}

QString inlineHtml(const QList<CslSpan> &spans)
{
    QString html;
    QList<Tag> open;
    CslFormat openFormat;
    for (const CslSpan &span : spans) {
        const QList<Tag> wanted = tagsOf(span.format);
        // Keeps the open tags, outermost first, for as long as the run
        // still wants them (a link only with the same target), then opens
        // what it wants besides.
        qsizetype keep = 0;
        while (keep < open.size() && wanted.contains(open[keep])
               && (open[keep] != Tag::Link || openFormat.link == span.format.link))
            ++keep;
        while (open.size() > keep) html += closeTag(open.takeLast());
        for (const Tag tag : wanted) {
            if (open.contains(tag)) continue;
            html += openTag(tag, span.format);
            open.append(tag);
        }
        openFormat = span.format;
        html += CslMarkup::escapeHtml(span.text);
    }
    while (!open.isEmpty()) html += closeTag(open.takeLast());
    return html;
}

} // namespace

QString CslText::toHtml() const
{
    QString html;
    qsizetype start = 0;
    while (start < m_spans.size()) {
        const CslFormat::Display display = m_spans[start].format.display;
        qsizetype end = start;
        while (end < m_spans.size() && m_spans[end].format.display == display) ++end;
        const QString body = inlineHtml(m_spans.mid(start, end - start));
        if (display == CslFormat::Display::Inline)
            html += body;
        else
            html += QStringLiteral("<div class=\"%1\">%2</div>").arg(displayClass(display), body);
        start = end;
    }
    return html;
}

namespace {

// Applies one CSS declaration list, as CSL JSON and citeproc HTML write it.
void applyStyle(const QString &style, CslFormat &format)
{
    for (const QString &declaration : style.split(u';', Qt::SkipEmptyParts)) {
        const qsizetype colon = declaration.indexOf(u':');
        if (colon < 0) continue;
        const QString property = declaration.left(colon).trimmed().toLower();
        const QString value = declaration.mid(colon + 1).trimmed().toLower();
        if (property == u"font-style")
            format.italic = value == u"italic" || value == u"oblique";
        else if (property == u"font-weight")
            format.bold = value == u"bold";
        else if (property == u"font-variant")
            format.smallCaps = value == u"small-caps";
        else if (property == u"text-decoration")
            format.underline = value == u"underline";
        else if (property == u"vertical-align")
            format.position = value == u"sup" || value == u"super" ? CslFormat::Position::Superscript
                : value == u"sub"                                   ? CslFormat::Position::Subscript
                                                                    : CslFormat::Position::Baseline;
    }
}

} // namespace

CslText CslText::fromHtml(const QString &html)
{
    CslText text;
    struct Open {
        QString name;
        CslFormat format;
    };
    QList<Open> stack;
    auto current = [&stack] { return stack.isEmpty() ? CslFormat() : stack.last().format; };
    for (const CslMarkup::Token &token : CslMarkup::tokenize(html)) {
        if (token.type == CslMarkup::Token::Type::Text) {
            text.append(token.text, current());
        } else if (token.type == CslMarkup::Token::Type::Open) {
            if (token.text == u"br") {
                text.append(QStringLiteral("\n"), current());
                continue;
            }
            CslFormat format = current();
            const QString &name = token.text;
            if (name == u"i" || name == u"em")
                format.italic = true;
            else if (name == u"b" || name == u"strong")
                format.bold = true;
            else if (name == u"u")
                format.underline = true;
            else if (name == u"sup")
                format.position = CslFormat::Position::Superscript;
            else if (name == u"sub")
                format.position = CslFormat::Position::Subscript;
            else if (name == u"sc")
                format.smallCaps = true;
            else if (name == u"a")
                format.link = bibliographyLink(token.href);
            else if (name == u"div") {
                if (token.cssClass.contains(u"csl-left-margin"))
                    format.display = CslFormat::Display::LeftMargin;
                else if (token.cssClass.contains(u"csl-right-inline"))
                    format.display = CslFormat::Display::RightInline;
                else if (token.cssClass.contains(u"csl-indent"))
                    format.display = CslFormat::Display::Indent;
                else if (token.cssClass.contains(u"csl-block"))
                    format.display = CslFormat::Display::Block;
            } else if (name == u"span") {
                if (token.cssClass.split(u' ').contains(u"nodecor")) {
                    format.italic = format.bold = format.smallCaps = format.underline = false;
                    format.position = CslFormat::Position::Baseline;
                }
                applyStyle(token.style, format);
            }
            if (!token.selfClosing) {
                if (stack.size() >= CslMarkup::MaxNesting) return CslText(html);
                stack.append({name, format});
            }
        } else {
            for (qsizetype i = stack.size() - 1; i >= 0; --i) {
                if (stack[i].name == token.text) {
                    stack.resize(i);
                    break;
                }
            }
        }
    }
    return text;
}

QString CslMarkup::escapeHtml(const QString &text)
{
    QString escaped;
    escaped.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'&':
            escaped += QStringLiteral("&amp;");
            break;
        case u'<':
            escaped += QStringLiteral("&lt;");
            break;
        case u'>':
            escaped += QStringLiteral("&gt;");
            break;
        case u'"':
            escaped += QStringLiteral("&quot;");
            break;
        default:
            escaped += c;
        }
    }
    return escaped;
}

namespace {

QString decodeEntities(const QString &text)
{
    if (!text.contains(u'&')) return text;
    static const QRegularExpression entity(QStringLiteral("&(#[0-9]+|#[xX][0-9a-fA-F]+|[a-zA-Z]+);"));
    QString decoded;
    qsizetype last = 0;
    auto it = entity.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString name = match.captured(1);
        QString replacement;
        if (name.startsWith(u'#')) {
            bool ok = false;
            const uint code = name.size() > 1 && (name[1] == u'x' || name[1] == u'X') ? name.mid(2).toUInt(&ok, 16)
                                                                                       : name.mid(1).toUInt(&ok, 10);
            if (ok && code > 0 && code <= 0x10FFFF) {
                const char32_t character = code;
                replacement = QString::fromUcs4(&character, 1);
            }
        } else if (name == u"amp") {
            replacement = QStringLiteral("&");
        } else if (name == u"lt") {
            replacement = QStringLiteral("<");
        } else if (name == u"gt") {
            replacement = QStringLiteral(">");
        } else if (name == u"quot") {
            replacement = QStringLiteral("\"");
        } else if (name == u"apos") {
            replacement = QStringLiteral("'");
        } else if (name == u"nbsp") {
            replacement = QString(QChar(0x00A0));
        }
        if (replacement.isEmpty()) continue;
        decoded += text.mid(last, match.capturedStart() - last) + replacement;
        last = match.capturedEnd();
    }
    decoded += text.mid(last);
    return decoded;
}

QString attributeValue(const QString &attributes, const QString &name)
{
    const QRegularExpression pattern(QStringLiteral("(?:^|\\s)%1\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')").arg(name),
                                     QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(attributes);
    if (!match.hasMatch()) return {};
    return decodeEntities(match.captured(1).isNull() ? match.captured(2) : match.captured(1));
}

} // namespace

QList<CslMarkup::Token> CslMarkup::tokenize(const QString &markup)
{
    static const QRegularExpression tag(
        QStringLiteral("<(/?)(i|em|b|strong|u|sup|sub|sc|span|a|div|br)(\\s[^<>]*+|/?)>"),
        QRegularExpression::CaseInsensitiveOption);
    QList<Token> tokens;
    qsizetype last = 0;
    auto addText = [&tokens](const QString &text) {
        if (text.isEmpty()) return;
        const QString decoded = decodeEntities(text);
        if (!tokens.isEmpty() && tokens.last().type == Token::Type::Text)
            tokens.last().text += decoded;
        else
            tokens.append({Token::Type::Text, decoded, {}, {}, {}, false});
    };
    auto it = tag.globalMatch(markup);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        addText(markup.mid(last, match.capturedStart() - last));
        last = match.capturedEnd();
        Token token;
        token.text = match.captured(2).toLower();
        const bool closing = !match.captured(1).isEmpty();
        token.type = closing ? Token::Type::Close : Token::Type::Open;
        if (!closing) {
            const QString attributes = match.captured(3);
            if (!attributes.isEmpty()) {
                token.cssClass = attributeValue(attributes, QStringLiteral("class"));
                token.style = attributeValue(attributes, QStringLiteral("style"));
                token.href = attributeValue(attributes, QStringLiteral("href"));
            }
            token.selfClosing = attributes.endsWith(u'/') || token.text == u"br";
        }
        tokens.append(token);
    }
    addText(markup.mid(last));
    return tokens;
}
