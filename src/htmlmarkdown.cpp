#include "htmlmarkdown.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

namespace {

// Elements deeper than this are not made: their content joins the deepest
// element made, so that hostile nesting costs no more than its text.
constexpr int maxDepth = 256;
// The most columns a table keeps, and the most a cell spans.
constexpr int maxColumns = 64;
constexpr int maxSpan = 64;

bool isSpace(QChar c)
{
    // HTML's whitespace, and the no-break space, which pasted text uses for
    // spacing far more often than to keep words together.
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f' || c == QChar(0x00a0);
}

bool isAsciiLetter(QChar c)
{
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}

// ---------------------------------------------------------------------------
// Character references

// HTML 4's named references and a few later ones in common use. A name not
// here is left as text, as a browser leaves one it does not know.
const QHash<QString, char32_t> &namedReferences()
{
    static const QHash<QString, char32_t> table = [] {
        QHash<QString, char32_t> names;
        // U+00A0 to U+00FF, in order.
        static const char *const latin1[] = {
            "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy", "ordf", "laquo",
            "not", "shy", "reg", "macr", "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot",
            "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest", "Agrave", "Aacute", "Acirc",
            "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute",
            "Icirc", "Iuml", "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times", "Oslash",
            "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc", "atilde",
            "auml", "aring", "aelig", "ccedil", "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc",
            "iuml", "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave",
            "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml"};
        for (int i = 0; i < int(std::size(latin1)); ++i) names.insert(QLatin1String(latin1[i]), char32_t(0xa0 + i));
        // Capital Greek from U+0391 and small from U+03B1, U+03A2 unused.
        static const char *const greek[] = {"Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta", "Eta", "Theta",
                                            "Iota", "Kappa", "Lambda", "Mu", "Nu", "Xi", "Omicron", "Pi", "Rho",
                                            nullptr, "Sigma", "Tau", "Upsilon", "Phi", "Chi", "Psi", "Omega"};
        for (int i = 0; i < int(std::size(greek)); ++i) {
            if (!greek[i]) continue;
            QString name = QLatin1String(greek[i]);
            names.insert(name, char32_t(0x391 + i));
            name[0] = name[0].toLower();
            names.insert(name, char32_t(0x3b1 + i));
        }
        static const std::pair<const char *, char32_t> others[] = {
            {"quot", 34}, {"amp", 38}, {"apos", 39}, {"lt", 60}, {"gt", 62}, {"OElig", 338}, {"oelig", 339},
            {"Scaron", 352}, {"scaron", 353}, {"Yuml", 376}, {"fnof", 402}, {"circ", 710}, {"tilde", 732},
            {"sigmaf", 962}, {"thetasym", 977}, {"upsih", 978}, {"piv", 982}, {"ensp", 8194}, {"emsp", 8195},
            {"thinsp", 8201}, {"hairsp", 8202}, {"zwnj", 8204}, {"zwj", 8205}, {"lrm", 8206}, {"rlm", 8207},
            {"ndash", 8211}, {"mdash", 8212}, {"lsquo", 8216}, {"rsquo", 8217}, {"sbquo", 8218}, {"ldquo", 8220},
            {"rdquo", 8221}, {"bdquo", 8222}, {"dagger", 8224}, {"Dagger", 8225}, {"bull", 8226}, {"hellip", 8230},
            {"permil", 8240}, {"prime", 8242}, {"Prime", 8243}, {"lsaquo", 8249}, {"rsaquo", 8250}, {"oline", 8254},
            {"frasl", 8260}, {"euro", 8364}, {"image", 8465}, {"weierp", 8472}, {"real", 8476}, {"trade", 8482},
            {"alefsym", 8501}, {"larr", 8592}, {"uarr", 8593}, {"rarr", 8594}, {"darr", 8595}, {"harr", 8596},
            {"crarr", 8629}, {"lArr", 8656}, {"uArr", 8657}, {"rArr", 8658}, {"dArr", 8659}, {"hArr", 8660},
            {"forall", 8704}, {"part", 8706}, {"exist", 8707}, {"empty", 8709}, {"nabla", 8711}, {"isin", 8712},
            {"notin", 8713}, {"ni", 8715}, {"prod", 8719}, {"sum", 8721}, {"minus", 8722}, {"lowast", 8727},
            {"radic", 8730}, {"prop", 8733}, {"infin", 8734}, {"ang", 8736}, {"and", 8743}, {"or", 8744},
            {"cap", 8745}, {"cup", 8746}, {"int", 8747}, {"there4", 8756}, {"sim", 8764}, {"cong", 8773},
            {"asymp", 8776}, {"ne", 8800}, {"equiv", 8801}, {"le", 8804}, {"ge", 8805}, {"sub", 8834},
            {"sup", 8835}, {"nsub", 8836}, {"sube", 8838}, {"supe", 8839}, {"oplus", 8853}, {"otimes", 8855},
            {"perp", 8869}, {"sdot", 8901}, {"lceil", 8968}, {"rceil", 8969}, {"lfloor", 8970}, {"rfloor", 8971},
            {"lang", 0x27e8}, {"rang", 0x27e9}, {"loz", 9674}, {"spades", 9824}, {"clubs", 9827},
            {"hearts", 9829}, {"diams", 9830}, {"starf", 9733}, {"star", 9734}, {"check", 10003},
            {"cross", 10007}, {"ZeroWidthSpace", 8203}, {"NewLine", 10}, {"Tab", 9}};
        for (const auto &[name, code] : others) names.insert(QLatin1String(name), code);
        return names;
    }();
    return table;
}

// What a numeric reference stands for, as HTML reads one: nothing valid
// becomes U+FFFD, and the C1 range the Windows code page it came from.
char32_t numericReference(char32_t code)
{
    static const char16_t windows1252[32] = {0x20ac, 0x81,   0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
                                             0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8d,   0x017d, 0x8f,
                                             0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
                                             0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x9d,   0x017e, 0x0178};
    if (code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return 0xfffd;
    if (code >= 0x80 && code <= 0x9f) return windows1252[code - 0x80];
    return code;
}

// Characters a text keeps: controls and byte-order marks go, line breaks
// and tabs stay.
bool keepsCharacter(char32_t c)
{
    if (c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f') return true;
    if (c < 0x20 || c == 0x7f || (c >= 0x80 && c <= 0x9f)) return false;
    return c != 0xfeff;
}

void appendCode(QString &out, char32_t c)
{
    if (!keepsCharacter(c)) return;
    if (QChar::requiresSurrogates(c)) {
        out += QChar(QChar::highSurrogate(c));
        out += QChar(QChar::lowSurrogate(c));
    } else {
        out += QChar(char16_t(c));
    }
}

int digitValue(QChar c, bool hex)
{
    const char16_t u = c.unicode();
    if (u >= u'0' && u <= u'9') return u - u'0';
    if (hex && u >= u'a' && u <= u'f') return u - u'a' + 10;
    if (hex && u >= u'A' && u <= u'F') return u - u'A' + 10;
    return -1;
}

// Text with its character references decoded and its control characters gone.
QString decodeText(QStringView text)
{
    QString out;
    out.reserve(text.size());
    const qsizetype n = text.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = text[i];
        if (c != u'&') {
            if (keepsCharacter(c.unicode())) out += c;
            continue;
        }
        qsizetype k = i + 1;
        if (k < n && text[k] == u'#') {
            ++k;
            const bool hex = k < n && (text[k] == u'x' || text[k] == u'X');
            if (hex) ++k;
            const qsizetype digits = k;
            quint64 value = 0;
            for (int digit; k < n && (digit = digitValue(text[k], hex)) >= 0; ++k)
                if (value <= 0x10ffff) value = value * (hex ? 16 : 10) + quint64(digit);
            if (k == digits) {
                out += c;
                continue;
            }
            if (k < n && text[k] == u';') ++k;
            appendCode(out, numericReference(char32_t(std::min<quint64>(value, 0x110000))));
            i = k - 1;
            continue;
        }
        while (k < n && k - i <= 32 && (text[k].isLetterOrNumber() && text[k].unicode() < 128)) ++k;
        const QString name = text.mid(i + 1, k - i - 1).toString();
        const bool closed = k < n && text[k] == u';';
        const auto found = namedReferences().constFind(name);
        // Without the semicolon only the oldest references count, and only
        // where nothing that could continue a name follows.
        static const QSet<QString> bare = {QStringLiteral("amp"), QStringLiteral("lt"), QStringLiteral("gt"),
                                           QStringLiteral("quot"), QStringLiteral("nbsp"), QStringLiteral("copy"),
                                           QStringLiteral("reg")};
        if (found != namedReferences().cend() && (closed || bare.contains(name))) {
            appendCode(out, found.value());
            i = closed ? k : k - 1;
            continue;
        }
        out += c;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The tree

struct Node {
    // Lower case; empty for text.
    QString tag;
    QString text;
    std::vector<std::pair<QString, QString>> attributes;
    std::vector<Node *> children;
    Node *parent = nullptr;
    // Some element inside is a block, worked out once the tree is whole.
    bool holdsBlock = false;

    bool isText() const { return tag.isEmpty(); }
    QString attribute(QStringView name) const
    {
        for (const auto &[key, value] : attributes)
            if (key == name) return value;
        return {};
    }
    bool has(QStringView name) const
    {
        return std::any_of(attributes.cbegin(), attributes.cend(), [&](const auto &entry) { return entry.first == name; });
    }
};

bool isVoid(const QString &tag)
{
    static const QSet<QString> tags = {
        QStringLiteral("area"), QStringLiteral("base"),  QStringLiteral("br"),    QStringLiteral("col"),
        QStringLiteral("embed"), QStringLiteral("hr"),   QStringLiteral("img"),   QStringLiteral("input"),
        QStringLiteral("link"), QStringLiteral("meta"),  QStringLiteral("param"), QStringLiteral("source"),
        QStringLiteral("track"), QStringLiteral("wbr"),  QStringLiteral("keygen"), QStringLiteral("basefont"),
        QStringLiteral("bgsound"), QStringLiteral("frame")};
    return tags.contains(tag);
}

bool isBlock(const QString &tag)
{
    static const QSet<QString> tags = {
        QStringLiteral("address"), QStringLiteral("article"), QStringLiteral("aside"), QStringLiteral("blockquote"),
        QStringLiteral("body"), QStringLiteral("caption"), QStringLiteral("center"), QStringLiteral("dd"),
        QStringLiteral("details"), QStringLiteral("dialog"), QStringLiteral("dir"), QStringLiteral("div"),
        QStringLiteral("dl"), QStringLiteral("dt"), QStringLiteral("fieldset"), QStringLiteral("figcaption"),
        QStringLiteral("figure"), QStringLiteral("footer"), QStringLiteral("form"), QStringLiteral("h1"),
        QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"), QStringLiteral("h6"),
        QStringLiteral("header"), QStringLiteral("hgroup"), QStringLiteral("hr"), QStringLiteral("html"),
        QStringLiteral("legend"), QStringLiteral("li"), QStringLiteral("listing"), QStringLiteral("main"),
        QStringLiteral("menu"), QStringLiteral("nav"), QStringLiteral("ol"), QStringLiteral("p"),
        QStringLiteral("plaintext"), QStringLiteral("pre"), QStringLiteral("search"), QStringLiteral("section"),
        QStringLiteral("summary"), QStringLiteral("table"), QStringLiteral("tbody"), QStringLiteral("td"),
        QStringLiteral("tfoot"), QStringLiteral("th"), QStringLiteral("thead"), QStringLiteral("tr"),
        QStringLiteral("ul"), QStringLiteral("xmp"), QStringLiteral("#root")};
    return tags.contains(tag);
}

// Elements whose content is not text to keep at all.
bool isDroppedTag(const QString &tag)
{
    static const QSet<QString> tags = {
        QStringLiteral("head"), QStringLiteral("script"), QStringLiteral("style"), QStringLiteral("title"),
        QStringLiteral("meta"), QStringLiteral("link"), QStringLiteral("base"), QStringLiteral("template"),
        QStringLiteral("noscript"), QStringLiteral("iframe"), QStringLiteral("object"), QStringLiteral("embed"),
        QStringLiteral("applet"), QStringLiteral("svg"), QStringLiteral("canvas"), QStringLiteral("video"),
        QStringLiteral("audio"), QStringLiteral("map"), QStringLiteral("area"), QStringLiteral("select"),
        QStringLiteral("option"), QStringLiteral("optgroup"), QStringLiteral("datalist"), QStringLiteral("textarea"),
        QStringLiteral("frameset"), QStringLiteral("frame"), QStringLiteral("noframes"), QStringLiteral("noembed"),
        QStringLiteral("param"), QStringLiteral("source"), QStringLiteral("track"), QStringLiteral("colgroup"),
        QStringLiteral("col"), QStringLiteral("input"), QStringLiteral("button"), QStringLiteral("wbr")};
    return tags.contains(tag);
}

bool isHeading(QStringView tag)
{
    return tag.size() == 2 && tag[0] == u'h' && tag[1] >= u'1' && tag[1] <= u'6';
}

// A style attribute's declarations, by property, in lower case without spaces.
QHash<QString, QString> declarations(const Node *node)
{
    QHash<QString, QString> found;
    const QString style = node->attribute(u"style");
    if (style.isEmpty()) return found;
    for (const QString &declaration : style.split(u';', Qt::SkipEmptyParts)) {
        const qsizetype colon = declaration.indexOf(u':');
        if (colon < 0) continue;
        QString value = declaration.mid(colon + 1).trimmed().toLower();
        value.remove(QStringLiteral("!important"));
        found.insert(declaration.left(colon).trimmed().toLower(), value.trimmed());
    }
    return found;
}

// An element the page does not show: hidden, or styled not to display.
bool isHidden(const Node *node)
{
    if (node->has(u"hidden")) return true;
    if (!node->has(u"style")) return false;
    const QHash<QString, QString> style = declarations(node);
    return style.value(QStringLiteral("display")) == u"none" || style.value(QStringLiteral("visibility")) == u"hidden";
}

bool isDropped(const Node *node)
{
    return isDroppedTag(node->tag) || isHidden(node);
}

// Reads HTML into a tree as a browser would, tolerating anything: stray and
// missing end tags, unquoted and unterminated attributes, and markup that
// is no markup are all taken as a browser takes them, or as text.
class Parser {
public:
    explicit Parser(QStringView html) : m_html(html)
    {
        m_root = make(QStringLiteral("#root"));
        m_open.push_back(m_root);
    }

    Node *parse()
    {
        const qsizetype n = m_html.size();
        while (m_at < n) {
            if (m_html[m_at] != u'<') {
                qsizetype next = m_html.indexOf(u'<', m_at);
                if (next < 0) next = n;
                addText(decodeText(m_html.mid(m_at, next - m_at)));
                m_at = next;
                continue;
            }
            const QChar after = m_at + 1 < n ? m_html[m_at + 1] : QChar();
            if (after == u'!' && m_html.mid(m_at, 4) == u"<!--") {
                const qsizetype close = m_html.indexOf(u"-->", m_at + 4);
                m_at = close < 0 ? n : close + 3;
            } else if (after == u'!' || after == u'?') {
                // A doctype, a processing instruction, CDATA or a word
                // processor's conditional: never shown.
                skipPast(u'>', m_at + 2);
            } else if (after == u'/') {
                if (m_at + 2 < n && isAsciiLetter(m_html[m_at + 2])) {
                    qsizetype k = m_at + 2;
                    while (k < n && !isSpace(m_html[k]) && m_html[k] != u'/' && m_html[k] != u'>') ++k;
                    const QString name = m_html.mid(m_at + 2, k - m_at - 2).toString().toLower();
                    skipPast(u'>', k);
                    end(name);
                } else {
                    skipPast(u'>', m_at + 2);
                }
            } else if (isAsciiLetter(after)) {
                startTag();
            } else {
                addText(QStringLiteral("<"));
                ++m_at;
            }
        }
        // Which elements hold a block, worked out from the last made to the
        // first, since every element is made after the one that holds it.
        for (auto node = m_nodes.rbegin(); node != m_nodes.rend(); ++node)
            if (node->parent && !node->isText() && (node->holdsBlock || isBlock(node->tag))) node->parent->holdsBlock = true;
        return m_root;
    }

private:
    Node *make(const QString &tag)
    {
        m_nodes.emplace_back();
        Node *node = &m_nodes.back();
        node->tag = tag;
        return node;
    }

    void attach(Node *node)
    {
        Node *parent = m_open.back();
        node->parent = parent;
        parent->children.push_back(node);
    }

    void addText(const QString &text)
    {
        if (text.isEmpty()) return;
        Node *parent = m_open.back();
        if (!parent->children.empty() && parent->children.back()->isText()) {
            parent->children.back()->text += text;
            return;
        }
        Node *node = make(QString());
        node->text = text;
        attach(node);
    }

    void skipPast(QChar c, qsizetype from)
    {
        const qsizetype close = m_html.indexOf(c, from);
        m_at = close < 0 ? m_html.size() : close + 1;
    }

    void startTag()
    {
        const qsizetype n = m_html.size();
        qsizetype k = m_at + 1;
        while (k < n && !isSpace(m_html[k]) && m_html[k] != u'/' && m_html[k] != u'>') ++k;
        const QString name = m_html.mid(m_at + 1, k - m_at - 1).toString().toLower();
        std::vector<std::pair<QString, QString>> attributes;
        bool selfClosing = false;
        for (;;) {
            while (k < n && (isSpace(m_html[k]) || m_html[k] == u'/')) {
                selfClosing = m_html[k] == u'/' && k + 1 < n && m_html[k + 1] == u'>';
                ++k;
            }
            // A tag the input ends inside is not shown.
            if (k >= n) {
                m_at = n;
                return;
            }
            if (m_html[k] == u'>') {
                ++k;
                break;
            }
            const qsizetype nameStart = k++;
            while (k < n && !isSpace(m_html[k]) && m_html[k] != u'/' && m_html[k] != u'>' && m_html[k] != u'=') ++k;
            const QString key = m_html.mid(nameStart, k - nameStart).toString().toLower();
            while (k < n && isSpace(m_html[k])) ++k;
            QString value;
            if (k < n && m_html[k] == u'=') {
                ++k;
                while (k < n && isSpace(m_html[k])) ++k;
                if (k < n && (m_html[k] == u'"' || m_html[k] == u'\'')) {
                    qsizetype close = m_html.indexOf(m_html[k], k + 1);
                    if (close < 0) close = n;
                    value = decodeText(m_html.mid(k + 1, close - k - 1));
                    k = close < n ? close + 1 : n;
                } else {
                    const qsizetype valueStart = k;
                    while (k < n && !isSpace(m_html[k]) && m_html[k] != u'>') ++k;
                    value = decodeText(m_html.mid(valueStart, k - valueStart));
                }
            }
            const bool known = std::any_of(attributes.cbegin(), attributes.cend(),
                                           [&](const auto &entry) { return entry.first == key; });
            if (!known) attributes.emplace_back(key, value);
        }
        m_at = k;
        start(name, std::move(attributes), selfClosing);
    }

    // The text of an element whose content is never markup, up to its end
    // tag or the end of the input.
    QStringView rawText(const QString &name)
    {
        const qsizetype n = m_html.size();
        const qsizetype from = m_at;
        for (qsizetype k = m_html.indexOf(u"</", from); k >= 0; k = m_html.indexOf(u"</", k + 2)) {
            const qsizetype after = k + 2 + name.size();
            if (m_html.mid(k + 2, name.size()).compare(name, Qt::CaseInsensitive) != 0) continue;
            if (after < n && !isSpace(m_html[after]) && m_html[after] != u'/' && m_html[after] != u'>') continue;
            skipPast(u'>', after);
            return m_html.mid(from, k - from);
        }
        m_at = n;
        return m_html.mid(from);
    }

    // Pops the open elements down to and including the nearest named
    // `targets`, unless one named `stops` comes first. Returns whether it did.
    bool popTo(std::initializer_list<QStringView> targets, std::initializer_list<QStringView> stops)
    {
        for (size_t k = m_open.size(); k-- > 1;) {
            const QString &tag = m_open[k]->tag;
            if (std::find(targets.begin(), targets.end(), tag) != targets.end()) {
                m_open.resize(k);
                return true;
            }
            if (std::find(stops.begin(), stops.end(), tag) != stops.end()) return false;
        }
        return false;
    }

    // What a start tag closes before it opens, as a browser closes it.
    void closeImplied(const QString &name)
    {
        static const QSet<QString> closesParagraph = {
            QStringLiteral("address"), QStringLiteral("article"), QStringLiteral("aside"),
            QStringLiteral("blockquote"), QStringLiteral("center"), QStringLiteral("details"),
            QStringLiteral("dialog"), QStringLiteral("dir"), QStringLiteral("div"), QStringLiteral("dl"),
            QStringLiteral("dd"), QStringLiteral("dt"), QStringLiteral("fieldset"), QStringLiteral("figcaption"),
            QStringLiteral("figure"), QStringLiteral("footer"), QStringLiteral("form"), QStringLiteral("h1"),
            QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"),
            QStringLiteral("h6"), QStringLiteral("header"), QStringLiteral("hgroup"), QStringLiteral("hr"),
            QStringLiteral("li"), QStringLiteral("listing"), QStringLiteral("main"), QStringLiteral("menu"),
            QStringLiteral("nav"), QStringLiteral("ol"), QStringLiteral("p"), QStringLiteral("plaintext"),
            QStringLiteral("pre"), QStringLiteral("section"), QStringLiteral("summary"), QStringLiteral("table"),
            QStringLiteral("ul"), QStringLiteral("xmp")};
        if (closesParagraph.contains(name)) popTo({u"p"}, {u"table", u"td", u"th", u"caption", u"button"});
        if (name == u"li") popTo({u"li"}, {u"ul", u"ol", u"menu", u"dir", u"table", u"td", u"th"});
        if (name == u"dt" || name == u"dd") popTo({u"dt", u"dd"}, {u"dl", u"table", u"td", u"th"});
        if (isHeading(name) && isHeading(m_open.back()->tag)) m_open.pop_back();
        if (name == u"tr") popTo({u"tr"}, {u"table", u"tbody", u"thead", u"tfoot"});
        if (name == u"td" || name == u"th") popTo({u"td", u"th"}, {u"tr", u"table"});
        if (name == u"tbody" || name == u"thead" || name == u"tfoot")
            popTo({u"tbody", u"thead", u"tfoot"}, {u"table"});
        if (name == u"a") popTo({u"a"}, {u"table", u"td", u"th"});
    }

    void start(const QString &name, std::vector<std::pair<QString, QString>> attributes, bool selfClosing)
    {
        static const QSet<QString> rawTextTags = {
            QStringLiteral("script"), QStringLiteral("style"), QStringLiteral("noscript"), QStringLiteral("iframe"),
            QStringLiteral("noembed"), QStringLiteral("noframes"), QStringLiteral("title"),
            QStringLiteral("textarea"), QStringLiteral("xmp"), QStringLiteral("plaintext")};
        if (rawTextTags.contains(name) && !selfClosing) {
            // xmp and plaintext show their text as it is; the others hide it.
            const bool shown = name == u"xmp" || name == u"plaintext";
            if (name == u"plaintext") {
                closeImplied(name);
                Node *node = make(QStringLiteral("pre"));
                attach(node);
                m_open.push_back(node);
                addText(m_html.mid(m_at).toString());
                m_open.pop_back();
                m_at = m_html.size();
                return;
            }
            const QStringView raw = rawText(name);
            if (!shown) return;
            closeImplied(name);
            Node *node = make(QStringLiteral("pre"));
            attach(node);
            m_open.push_back(node);
            addText(raw.toString());
            m_open.pop_back();
            return;
        }
        closeImplied(name);
        const bool opens = !isVoid(name) && !selfClosing;
        if (opens && int(m_open.size()) > maxDepth) {
            ++m_skipped[name];
            return;
        }
        Node *node = make(name);
        node->attributes = std::move(attributes);
        attach(node);
        if (opens) m_open.push_back(node);
    }

    void end(const QString &name)
    {
        if (const auto skipped = m_skipped.find(name); skipped != m_skipped.end() && *skipped > 0) {
            --*skipped;
            return;
        }
        // A browser takes </br> as <br>.
        if (name == u"br") {
            start(name, {}, false);
            return;
        }
        if (name == u"td" || name == u"th" || name == u"tr" || name == u"tbody" || name == u"thead"
            || name == u"tfoot" || name == u"caption") {
            popTo({name}, {u"table"});
        } else if (name == u"table") {
            popTo({name}, {});
        } else if (name == u"li") {
            popTo({name}, {u"ul", u"ol", u"menu", u"dir", u"table", u"td", u"th"});
        } else if (isHeading(name)) {
            popTo({u"h1", u"h2", u"h3", u"h4", u"h5", u"h6"}, {u"table", u"td", u"th", u"caption"});
        } else {
            // An end tag never reaches out of a table cell it was not opened in.
            popTo({name}, {u"table", u"td", u"th", u"caption"});
        }
    }

    QStringView m_html;
    qsizetype m_at = 0;
    std::deque<Node> m_nodes;
    Node *m_root = nullptr;
    std::vector<Node *> m_open;
    // Start tags not made at the depth limit, whose end tags are passed over.
    QHash<QString, int> m_skipped;
};

// ---------------------------------------------------------------------------
// Inline text

// Formatting a run of text can have, in the order its markers nest, outside in.
enum Style : quint8 { Strike = 1, Strong = 2, Emphasis = 4, Mark = 8 };
using Styles = quint8;
constexpr Style styleOrder[] = {Strike, Strong, Emphasis, Mark};

QString marker(Style style)
{
    switch (style) {
    case Strike:
        return QStringLiteral("~~");
    case Strong:
        return QStringLiteral("**");
    case Emphasis:
        return QStringLiteral("*");
    case Mark:
        return QStringLiteral("==");
    }
    return {};
}

// How line breaks in a run are written: as Markdown hard breaks, as <br> in
// a table cell, or as spaces in a heading or a link's text.
enum class Breaks { Hard, Html, Space };

struct Segment {
    // A word is text to escape and never holds whitespace; raw is Markdown
    // made already, as a code span, a link or an image.
    enum Kind : quint8 { Word, Space, Raw, Break } kind;
    Styles styles = 0;
    QString text;
};

// Escapes one character of a word, knowing what is around it.
void escapeInto(QString &out, QStringView word, qsizetype i, QChar before, QChar after, bool lineStart,
                qsizetype listDelimiter)
{
    const QChar c = word[i];
    const QChar previous = i > 0 ? word[i - 1] : before;
    const QChar next = i + 1 < word.size() ? word[i + 1] : after;
    const bool first = lineStart && i == 0;
    bool escape = false;
    switch (c.unicode()) {
    case u'\\':
    case u'`':
    case u'*':
    case u'[':
    case u']':
    case u'~':
    case u'^':
        escape = true;
        break;
    case u'_':
        // Between letters or digits an underscore never starts or ends emphasis.
        escape = !(previous.isLetterOrNumber() && next.isLetterOrNumber());
        break;
    case u'<':
        // Only before what could start a tag, a comment or an autolink.
        escape = isAsciiLetter(next) || next == u'/' || next == u'!' || next == u'?';
        break;
    case u'&': {
        // Only what would read as a character reference.
        qsizetype k = i + 1;
        if (k < word.size() && word[k] == u'#') {
            ++k;
            if (k < word.size() && (word[k] == u'x' || word[k] == u'X')) ++k;
        }
        const qsizetype nameStart = k;
        while (k < word.size() && word[k].isLetterOrNumber() && word[k].unicode() < 128) ++k;
        escape = k > nameStart && k < word.size() && word[k] == u';';
        break;
    }
    case u'$':
        // Money, a dollar sign before a number after a space, never starts or
        // ends math; any other could.
        escape = !(previous.isSpace() && next.isDigit());
        break;
    case u'=':
        escape = first || previous == u'=' || next == u'=';
        break;
    case u'{':
        escape = next == u'{';
        break;
    case u'#':
    case u'>':
    case u'-':
    case u'+':
    case u':':
    case u'|':
        escape = first;
        break;
    case u'/':
        escape = first && next == u'/';
        break;
    default:
        break;
    }
    if (i == listDelimiter) escape = true;
    if (escape) out += u'\\';
    out += c;
}

// A paragraph's worth of inline content, or a heading's, a cell's or a
// link's, as segments that collapse whitespace as a browser does.
class Run {
public:
    void text(QStringView text, Styles styles)
    {
        qsizetype i = 0;
        const qsizetype n = text.size();
        while (i < n) {
            if (isSpace(text[i])) {
                while (i < n && isSpace(text[i])) ++i;
                space();
                continue;
            }
            const qsizetype start = i;
            while (i < n && !isSpace(text[i])) ++i;
            word(text.mid(start, i - start), styles);
        }
    }

    void word(QStringView text, Styles styles)
    {
        if (!m_segments.empty() && m_segments.back().kind == Segment::Word && m_segments.back().styles == styles) {
            m_segments.back().text += text;
            return;
        }
        m_segments.push_back({Segment::Word, styles, text.toString()});
    }

    void space()
    {
        if (!m_segments.empty() && (m_segments.back().kind == Segment::Space || m_segments.back().kind == Segment::Break))
            return;
        m_segments.push_back({Segment::Space, 0, {}});
    }

    void raw(const QString &markdown, Styles styles)
    {
        if (!markdown.isEmpty()) m_segments.push_back({Segment::Raw, styles, markdown});
    }

    void lineBreak()
    {
        while (!m_segments.empty() && m_segments.back().kind == Segment::Space) m_segments.pop_back();
        m_segments.push_back({Segment::Break, 0, {}});
    }

    // Whether the last thing shown is a line break, as a second <br> in a row asks.
    bool endsWithBreak() const
    {
        for (auto segment = m_segments.crbegin(); segment != m_segments.crend(); ++segment) {
            if (segment->kind == Segment::Space) continue;
            return segment->kind == Segment::Break;
        }
        return false;
    }

    void dropTrailingBreak()
    {
        while (!m_segments.empty() && m_segments.back().kind != Segment::Word && m_segments.back().kind != Segment::Raw)
            m_segments.pop_back();
    }

    bool hasContent() const
    {
        return std::any_of(m_segments.cbegin(), m_segments.cend(), [](const Segment &segment) {
            return segment.kind == Segment::Word || segment.kind == Segment::Raw;
        });
    }

    void clear() { m_segments.clear(); }

    // The Markdown, without whitespace at either end. A run that starts a
    // line escapes what would start a block there.
    QString render(Breaks breaks, bool startsLine) const
    {
        QString out;
        // Each style open, with the marker it was opened with.
        std::vector<std::pair<Style, QString>> open;
        enum class Pending { Nothing, Space, Break } pending = Pending::Nothing;
        qsizetype lineStart = startsLine ? 0 : -1;
        const auto close = [&](size_t keep) {
            while (open.size() > keep) {
                out += open.back().second;
                open.pop_back();
            }
        };
        for (size_t k = 0; k < m_segments.size(); ++k) {
            const Segment &segment = m_segments[k];
            if (segment.kind == Segment::Space) {
                if (!out.isEmpty() && pending == Pending::Nothing) pending = Pending::Space;
                continue;
            }
            if (segment.kind == Segment::Break) {
                if (!out.isEmpty()) pending = breaks == Breaks::Space ? Pending::Space : Pending::Break;
                continue;
            }
            std::vector<Style> wanted;
            for (Style style : styleOrder)
                if (segment.styles & style) wanted.push_back(style);
            size_t same = 0;
            while (same < open.size() && same < wanted.size() && open[same].first == wanted[same]) ++same;
            close(same);
            if (pending == Pending::Space) {
                out += u' ';
            } else if (pending == Pending::Break) {
                out += breaks == Breaks::Html ? QStringLiteral("<br>") : QStringLiteral("\\\n");
                if (breaks == Breaks::Hard && startsLine) lineStart = out.size();
            }
            pending = Pending::Nothing;
            const qsizetype openFrom = out.size();
            for (size_t s = same; s < wanted.size(); ++s) {
                // Asterisks right after ones that closed would join them in
                // one run, so emphasis opened there takes underscores.
                QString opening = marker(wanted[s]);
                if (out.size() == openFrom && !out.isEmpty() && out.back() == u'*'
                    && (wanted[s] == Strong || wanted[s] == Emphasis))
                    opening.fill(u'_');
                out += opening;
                open.emplace_back(wanted[s], opening);
            }
            if (segment.kind == Segment::Raw) {
                out += segment.text;
                continue;
            }
            const bool atLineStart = out.size() == lineStart;
            const QChar before = out.isEmpty() || atLineStart ? QChar(u' ') : out.back();
            // What follows the word: whitespace, or a marker when the next
            // piece is formatted otherwise.
            QChar after = u' ';
            if (k + 1 < m_segments.size()) {
                const Segment &next = m_segments[k + 1];
                if (next.kind == Segment::Word || next.kind == Segment::Raw)
                    after = next.styles == segment.styles && !next.text.isEmpty() ? next.text.front() : QChar(u'*');
            }
            appendWord(out, segment.text, before, after, atLineStart);
        }
        close(0);
        return out;
    }

    // Escapes a word into `out`; at the start of a line, what would make a
    // block there is escaped too.
    static void appendWord(QString &out, QStringView word, QChar before, QChar after, bool lineStart)
    {
        // An ordered list marker: up to nine digits, then . or ), then a space or nothing.
        qsizetype listDelimiter = -1;
        if (lineStart) {
            qsizetype digits = 0;
            while (digits < word.size() && digits < 10 && word[digits].isDigit() && word[digits].unicode() < 128)
                ++digits;
            if (digits > 0 && digits <= 9 && digits + 1 == word.size()
                && (word[digits] == u'.' || word[digits] == u')') && after.isSpace())
                listDelimiter = digits;
        }
        for (qsizetype i = 0; i < word.size(); ++i) escapeInto(out, word, i, before, after, lineStart, listDelimiter);
    }

private:
    std::vector<Segment> m_segments;
};

// ---------------------------------------------------------------------------
// Blocks

struct Block {
    enum Kind { Paragraph, Heading, List, Quote, Code, Table, Rule } kind;
    QString text;
    // For a list: whether it is ordered, and the marker character its items use.
    bool ordered = false;
    QChar marker = QChar();
};
using Blocks = QList<Block>;

QString joined(const Blocks &blocks)
{
    QStringList texts;
    for (const Block &block : blocks) texts << block.text;
    return texts.join(QStringLiteral("\n\n"));
}

QString prefixed(const QString &text, const QString &first, const QString &rest, const QString &blank)
{
    QStringList lines = text.split(u'\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (i == 0) lines[i] = first + lines[i];
        else lines[i] = lines[i].isEmpty() ? blank : rest + lines[i];
    }
    return lines.join(u'\n');
}

// Two lists in a row with the same markers would read as one, so the second
// takes the other marker: * for -, ) for .
void takeOtherMarker(Block &list)
{
    QStringList lines = list.text.split(u'\n');
    const QChar other = list.ordered ? (list.marker == u'.' ? u')' : u'.') : (list.marker == u'-' ? u'*' : u'-');
    for (QString &line : lines) {
        if (line.isEmpty() || line.front().isSpace()) continue;
        if (!list.ordered) {
            line[0] = other;
            continue;
        }
        qsizetype digits = 0;
        while (digits < line.size() && line[digits].isDigit()) ++digits;
        if (digits < line.size()) line[digits] = other;
    }
    list.text = lines.join(u'\n');
    list.marker = other;
}

void append(Blocks &out, Block block)
{
    if (block.text.isEmpty()) return;
    if (block.kind == Block::List && !out.isEmpty() && out.last().kind == Block::List
        && out.last().ordered == block.ordered && out.last().marker == block.marker)
        takeOtherMarker(block);
    out.append(std::move(block));
}

void append(Blocks &out, const Blocks &more)
{
    for (const Block &block : more) append(out, block);
}

QString collapsed(const QString &text)
{
    return text.simplified();
}

// The text an element shows, with whitespace collapsed.
void textContent(const Node *node, QString &out)
{
    for (const Node *child : node->children) {
        if (child->isText()) out += child->text;
        else if (child->tag == u"br") out += u' ';
        else if (!isDropped(child)) textContent(child, out);
    }
}

QString textOf(const Node *node)
{
    QString text;
    textContent(node, text);
    QString cleaned;
    cleaned.reserve(text.size());
    for (QChar c : std::as_const(text)) cleaned += isSpace(c) ? QChar(u' ') : c;
    return collapsed(cleaned);
}

// A code block's text, whitespace kept: a line break for <br>, and each
// block inside on lines of its own.
void preformattedText(const Node *node, QString &out)
{
    for (const Node *child : node->children) {
        if (child->isText()) {
            out += child->text;
            continue;
        }
        if (isDropped(child)) continue;
        if (child->tag == u"br") {
            out += u'\n';
            continue;
        }
        const bool block = isBlock(child->tag);
        if (block && !out.isEmpty() && !out.endsWith(u'\n')) out += u'\n';
        preformattedText(child, out);
        if (block && !out.isEmpty() && !out.endsWith(u'\n')) out += u'\n';
    }
}

QString languageOf(const Node *node)
{
    static const QString allowed = QStringLiteral("+-_.#");
    const auto clean = [](const QString &name) {
        QString language;
        for (QChar c : name) {
            if (c.unicode() >= 128 || !(c.isLetterOrNumber() || allowed.contains(c))) return QString();
            language += c;
        }
        return language.left(32);
    };
    const auto fromNode = [&](const Node *element) -> QString {
        if (!element || element->isText()) return {};
        for (const char *attribute : {"data-lang", "data-language"}) {
            const QString value = clean(element->attribute(QString::fromLatin1(attribute)).trimmed());
            if (!value.isEmpty()) return value;
        }
        for (const QString &name : element->attribute(u"class").split(u' ', Qt::SkipEmptyParts)) {
            for (const char *prefix : {"language-", "lang-", "highlight-source-"}) {
                if (!name.startsWith(QLatin1String(prefix))) continue;
                const QString value = clean(name.mid(qsizetype(qstrlen(prefix))));
                if (!value.isEmpty()) return value;
            }
        }
        return {};
    };
    QString language = fromNode(node);
    for (const Node *child : node->children)
        if (language.isEmpty() && !child->isText()) language = fromNode(child);
    if (language.isEmpty()) language = fromNode(node->parent);
    return language;
}

// A block element laid out as preformatted monospace text, as code editors
// copy code, rather than with <pre>.
bool showsAsCode(const Node *node)
{
    if (!node->has(u"style")) return false;
    const QHash<QString, QString> style = declarations(node);
    const QString whiteSpace = style.value(QStringLiteral("white-space"));
    if (whiteSpace != u"pre" && whiteSpace != u"pre-wrap") return false;
    const QString family = style.value(QStringLiteral("font-family"));
    return family.contains(QStringLiteral("mono")) || family.contains(QStringLiteral("courier"))
           || family.contains(QStringLiteral("consolas")) || family.contains(QStringLiteral("menlo"));
}

QString codeSpan(const QString &code)
{
    int longest = 0, run = 0;
    for (QChar c : code) {
        run = c == u'`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const QString fence(longest + 1, u'`');
    const bool pad = code.startsWith(u'`') || code.endsWith(u'`')
                     || (code.startsWith(u' ') && code.endsWith(u' ') && !code.trimmed().isEmpty());
    const QString space = pad ? QStringLiteral(" ") : QString();
    return fence + space + code + space + fence;
}

QString quotedTitle(const QString &title)
{
    const QString cleaned = collapsed(title);
    if (cleaned.isEmpty()) return {};
    QString escaped;
    for (QChar c : cleaned) {
        if (c == u'"' || c == u'\\') escaped += u'\\';
        escaped += c;
    }
    return QStringLiteral(" \"") + escaped + u'"';
}

bool hasScheme(const QString &url)
{
    const qsizetype colon = url.indexOf(u':');
    if (colon < 2) return false;
    for (qsizetype i = 0; i < colon; ++i) {
        const QChar c = url[i];
        if (!(isAsciiLetter(c) || (i > 0 && (c.isDigit() || c == u'+' || c == u'.' || c == u'-')))) return false;
    }
    return true;
}

bool isUnsafeUrl(const QString &url)
{
    const QString lower = url.toLower();
    return lower.startsWith(QStringLiteral("javascript:")) || lower.startsWith(QStringLiteral("vbscript:"))
           || lower.startsWith(QStringLiteral("data:"));
}

// A URL as HTML takes it from an attribute: without the whitespace around
// it, or tabs and line breaks in it.
QString cleanUrl(const QString &url)
{
    QString cleaned = url.trimmed();
    cleaned.remove(u'\t');
    cleaned.remove(u'\n');
    cleaned.remove(u'\r');
    return cleaned;
}

class Converter {
public:
    explicit Converter(const HtmlMarkdown::ImageSink &images) : m_images(images) {}

    Blocks flow(const Node *container, Styles styles)
    {
        Blocks out;
        Run run;
        walkChildren(container, run, &out, styles);
        flush(run, out);
        return out;
    }

private:
    void flush(Run &run, Blocks &out)
    {
        if (run.hasContent()) append(out, Block{Block::Paragraph, run.render(Breaks::Hard, true)});
        run.clear();
    }

    void walkChildren(const Node *node, Run &run, Blocks *out, Styles styles)
    {
        for (const Node *child : node->children) walk(child, run, out, styles);
    }

    // `out` is where blocks go; without it, as in a heading, a cell or a
    // link's text, blocks run on in the same line, apart.
    void walk(const Node *node, Run &run, Blocks *out, Styles styles)
    {
        if (node->isText()) {
            run.text(node->text, styles);
            return;
        }
        if (isDropped(node)) return;
        const QString &tag = node->tag;
        if (isBlock(tag)) {
            if (out) {
                flush(run, *out);
                block(node, *out, styles);
            } else {
                run.lineBreak();
                walkChildren(node, run, nullptr, styles);
                run.lineBreak();
            }
            return;
        }
        if (tag == u"br") {
            // Two breaks in a row show an empty line, which is a new paragraph.
            if (out && run.endsWithBreak()) {
                run.dropTrailingBreak();
                flush(run, *out);
            } else {
                run.lineBreak();
            }
            return;
        }
        if (tag == u"img") {
            image(node, run, styles);
            return;
        }
        if (!node->holdsBlock) {
            if (tag == u"a" && link(node, run, styles)) return;
            if (tag == u"code" || tag == u"kbd" || tag == u"samp" || tag == u"tt") {
                code(node, run, styles);
                return;
            }
            if (tag == u"sup" || tag == u"sub") {
                Run inner;
                walkChildren(node, inner, nullptr, 0);
                const QString text = inner.render(Breaks::Space, false);
                if (!text.isEmpty()) run.raw(u'<' + tag + u'>' + text + QStringLiteral("</") + tag + u'>', styles);
                return;
            }
            if (tag == u"q") {
                run.word(u"\"", styles);
                walkChildren(node, run, out, styles);
                run.word(u"\"", styles);
                return;
            }
        }
        walkChildren(node, run, out, styled(node, styles));
    }

    // The formatting an element adds to, or takes from, what holds it.
    static Styles styled(const Node *node, Styles styles)
    {
        const QString &tag = node->tag;
        if (tag == u"b" || tag == u"strong") styles |= Strong;
        if (tag == u"i" || tag == u"em" || tag == u"cite" || tag == u"dfn" || tag == u"var") styles |= Emphasis;
        if (tag == u"s" || tag == u"strike" || tag == u"del") styles |= Strike;
        if (tag == u"mark") styles |= Mark;
        if (!node->has(u"style")) return styles;
        const QHash<QString, QString> style = declarations(node);
        const QString weight = style.value(QStringLiteral("font-weight"));
        if (weight == u"bold" || weight == u"bolder" || weight.toInt() >= 600) styles |= Strong;
        else if (weight == u"normal" || weight == u"lighter" || (weight.toInt() > 0 && weight.toInt() < 600))
            styles &= ~Strong;
        const QString fontStyle = style.value(QStringLiteral("font-style"));
        if (fontStyle == u"italic" || fontStyle.startsWith(QStringLiteral("oblique"))) styles |= Emphasis;
        else if (fontStyle == u"normal") styles &= ~Emphasis;
        const QString decoration =
            style.value(QStringLiteral("text-decoration-line"), style.value(QStringLiteral("text-decoration")));
        if (decoration.contains(QStringLiteral("line-through"))) styles |= Strike;
        return styles;
    }

    void code(const Node *node, Run &run, Styles styles)
    {
        QString text;
        textContent(node, text);
        QString cleaned;
        for (QChar c : std::as_const(text)) cleaned += isSpace(c) ? QChar(u' ') : c;
        const bool leading = !cleaned.isEmpty() && cleaned.front() == u' ';
        const bool trailing = !cleaned.isEmpty() && cleaned.back() == u' ';
        cleaned = collapsed(cleaned);
        if (cleaned.isEmpty()) {
            if (leading || trailing) run.space();
            return;
        }
        if (leading) run.space();
        run.raw(codeSpan(cleaned), styles);
        if (trailing) run.space();
    }

    bool link(const Node *node, Run &run, Styles styles)
    {
        const QString href = cleanUrl(node->attribute(u"href"));
        // A link to nowhere, a script, or a place in the page it came from
        // keeps only its text.
        if (href.isEmpty() || href.startsWith(u'#') || isUnsafeUrl(href)) return false;
        Run inner;
        walkChildren(node, inner, nullptr, 0);
        const QString text = inner.render(Breaks::Space, false);
        const QString shown = textOf(node);
        const bool plain = hasScheme(href) && !href.contains(u' ') && !href.contains(u'<') && !href.contains(u'>');
        // A link that shows its own address is an autolink; an email
        // address is one without its mailto:.
        const bool mail = href.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive) && shown == href.mid(7)
                          && shown.contains(u'@') && !shown.contains(u'?');
        if (plain && (shown == href || mail) && !text.contains(QStringLiteral("!["))) {
            run.raw(u'<' + (mail ? shown : href) + u'>', styles);
            return true;
        }
        if (text.isEmpty()) {
            if (plain) run.raw(u'<' + href + u'>', styles);
            return true;
        }
        run.raw(u'[' + text + QStringLiteral("](") + HtmlMarkdown::encodeUrl(href) + quotedTitle(node->attribute(u"title"))
                    + u')',
                styles);
        return true;
    }

    void image(const Node *node, Run &run, Styles styles)
    {
        // A pixel that tracks the reader, not an image to keep.
        for (const char *dimension : {"width", "height"}) {
            const QString value = node->attribute(QString::fromLatin1(dimension)).trimmed();
            if (value == u"0" || value == u"1" || value == u"1px" || value == u"0px") return;
        }
        QString source = cleanUrl(node->attribute(u"src"));
        // Pages that load images late keep the real one aside and show a
        // placeholder until then.
        if (source.isEmpty() || source.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive)) {
            for (const char *lazy : {"data-src", "data-original", "data-lazy-src"}) {
                const QString real = cleanUrl(node->attribute(QString::fromLatin1(lazy)));
                if (real.startsWith(QStringLiteral("http"), Qt::CaseInsensitive)) {
                    source = real;
                    break;
                }
            }
        }
        if (source.isEmpty()) source = cleanUrl(node->attribute(u"srcset").section(u',', 0, 0).section(u' ', 0, 0));
        const QString alt = collapsed(node->attribute(u"alt"));
        const auto altText = [&] {
            if (!alt.isEmpty()) run.text(alt, styles);
        };
        QString destination;
        if (source.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive)) {
            const QString path = embedded(source);
            if (path.isEmpty()) {
                altText();
                return;
            }
            destination = HtmlMarkdown::encodePath(path);
        } else if (source.isEmpty() || isUnsafeUrl(source) || source.startsWith(QStringLiteral("blob:"))
                   || source.startsWith(QStringLiteral("cid:"))) {
            altText();
            return;
        } else {
            if (source.startsWith(QStringLiteral("//"))) source.prepend(QStringLiteral("https:"));
            destination = HtmlMarkdown::encodeUrl(source);
        }
        Run altRun;
        altRun.text(alt, 0);
        run.raw(QStringLiteral("![") + altRun.render(Breaks::Space, false) + QStringLiteral("](") + destination
                    + quotedTitle(node->attribute(u"title")) + u')',
                styles);
    }

    // Saves a data: image through the sink; nothing when it is no image or none is kept.
    QString embedded(const QString &source)
    {
        if (!m_images) return {};
        const qsizetype comma = source.indexOf(u',');
        if (comma < 0) return {};
        const QStringList header = source.mid(5, comma - 5).split(u';');
        const QString mimeType = header.value(0).trimmed().toLower();
        if (!mimeType.startsWith(QStringLiteral("image/"))) return {};
        const QByteArray payload = source.mid(comma + 1).toLatin1();
        const bool base64 = header.contains(QStringLiteral("base64"), Qt::CaseInsensitive);
        const QByteArray bytes = base64 ? QByteArray::fromBase64(payload) : QByteArray::fromPercentEncoding(payload);
        if (bytes.isEmpty()) return {};
        return m_images(bytes, mimeType);
    }

    void block(const Node *node, Blocks &out, Styles styles)
    {
        const QString &tag = node->tag;
        if (tag == u"pre" || tag == u"listing" || showsAsCode(node)) {
            codeBlock(node, out);
        } else if (isHeading(tag)) {
            heading(node, out, styles);
        } else if (tag == u"ul" || tag == u"ol" || tag == u"menu" || tag == u"dir") {
            list(node, out, styles);
        } else if (tag == u"li") {
            // An item outside any list is a list of its own.
            listOf({node}, false, 1, out, styles);
        } else if (tag == u"blockquote") {
            const QString inner = joined(flow(node, styles));
            if (!inner.isEmpty()) append(out, Block{Block::Quote, prefixed(inner, QStringLiteral("> "), QStringLiteral("> "), QStringLiteral(">"))});
        } else if (tag == u"table") {
            table(node, out, styles);
        } else if (tag == u"hr") {
            append(out, Block{Block::Rule, QStringLiteral("---")});
        } else {
            append(out, flow(node, styled(node, styles)));
        }
    }

    void codeBlock(const Node *node, Blocks &out)
    {
        QString text;
        preformattedText(node, text);
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(u'\r', u'\n');
        text.replace(u'\f', u'\n');
        text.replace(QChar(0x00a0), u' ');
        // A line break right after <pre> is not part of its text.
        if (text.startsWith(u'\n')) text.remove(0, 1);
        while (text.endsWith(u'\n')) text.chop(1);
        if (text.trimmed().isEmpty()) return;
        int longest = 0, run = 0;
        for (QChar c : std::as_const(text)) {
            run = c == u'`' ? run + 1 : 0;
            longest = std::max(longest, run);
        }
        const QString fence(std::max(3, longest + 1), u'`');
        append(out, Block{Block::Code, fence + languageOf(node) + u'\n' + text + u'\n' + fence});
    }

    void heading(const Node *node, Blocks &out, Styles styles)
    {
        Run run;
        walkChildren(node, run, nullptr, styles);
        QString text = run.render(Breaks::Space, false);
        if (text.isEmpty()) return;
        // A closing run of # after a space would be taken off the heading.
        qsizetype hashes = text.size();
        while (hashes > 0 && text[hashes - 1] == u'#') --hashes;
        if (hashes < text.size() && (hashes == 0 || text[hashes - 1] == u' ')) text.insert(hashes, u'\\');
        const int level = node->tag[1].digitValue();
        append(out, Block{Block::Heading, QString(level, u'#') + u' ' + text});
    }

    // The first thing an item shows, which makes it a task when it is a checkbox.
    static const Node *firstShown(const Node *node)
    {
        for (const Node *child : node->children) {
            if (child->isText()) {
                if (!collapsed(child->text).isEmpty()) return child;
                continue;
            }
            if (child->tag == u"input") return child;
            if (isDropped(child)) continue;
            if (child->tag == u"img" || child->tag == u"br" || child->tag == u"hr") return child;
            if (const Node *found = firstShown(child)) return found;
        }
        return nullptr;
    }

    void list(const Node *node, Blocks &out, Styles styles)
    {
        const bool ordered = node->tag == u"ol";
        bool started = false;
        const int start = node->attribute(u"start").trimmed().toInt(&started);
        std::vector<const Node *> items;
        for (const Node *child : node->children) {
            if (child->isText() && collapsed(child->text).isEmpty()) continue;
            if (!child->isText() && isDropped(child)) continue;
            items.push_back(child);
        }
        listOf(items, ordered, started ? start : 1, out, styles);
    }

    struct Item {
        Blocks blocks;
        int number = 0;
        bool task = false;
        bool checked = false;
    };

    void listOf(const std::vector<const Node *> &children, bool ordered, int number, Blocks &out, Styles styles)
    {
        QList<Item> items;
        for (const Node *child : children) {
            if (!child->isText() && child->tag == u"li") {
                Item item;
                bool valued = false;
                const int value = child->attribute(u"value").trimmed().toInt(&valued);
                item.number = valued ? value : number;
                number = item.number + 1;
                if (child->attribute(u"role") == u"checkbox") {
                    item.task = true;
                    item.checked = child->attribute(u"aria-checked") == u"true";
                } else if (const Node *first = firstShown(child);
                           first && first->tag == u"input" && first->attribute(u"type").toLower() == u"checkbox") {
                    item.task = true;
                    item.checked = first->has(u"checked");
                }
                item.blocks = flow(child, styles);
                items.append(std::move(item));
                continue;
            }
            // Anything else in a list, as a list put straight inside one,
            // belongs to the item before it.
            Blocks content;
            if (child->isText()) {
                Run run;
                run.text(child->text, styles);
                if (run.hasContent()) content.append({Block::Paragraph, run.render(Breaks::Hard, true)});
            } else if (isBlock(child->tag)) {
                block(child, content, styles);
            } else {
                Run run;
                walk(child, run, &content, styles);
                flush(run, content);
            }
            if (content.isEmpty()) continue;
            if (items.isEmpty()) {
                Item item;
                item.number = number++;
                items.append(item);
            }
            append(items.last().blocks, content);
        }
        if (items.isEmpty()) return;
        const QChar delimiter = ordered ? u'.' : u'-';
        QStringList texts;
        bool loose = false;
        for (const Item &item : std::as_const(items)) {
            const QString marker = ordered ? QString::number(item.number) + delimiter + u' ' : QStringLiteral("- ");
            const QString indent(marker.size(), u' ');
            const QString box = item.task ? (item.checked ? QStringLiteral("[x] ") : QStringLiteral("[ ] ")) : QString();
            QString text;
            for (qsizetype b = 0; b < item.blocks.size(); ++b) {
                const Block &block = item.blocks[b];
                if (b == 0) {
                    text = prefixed(block.text, marker + box, indent, QString());
                    continue;
                }
                // A list right after a paragraph belongs to it; anything else
                // after something needs a blank line between.
                const bool tight = block.kind == Block::List && item.blocks[b - 1].kind == Block::Paragraph;
                if (!tight) loose = true;
                text += (tight ? QStringLiteral("\n") : QStringLiteral("\n\n")) + prefixed(block.text, indent, indent, QString());
            }
            if (item.blocks.isEmpty()) text = (marker + box).trimmed();
            texts << text;
        }
        Block list{Block::List, texts.join(loose ? QStringLiteral("\n\n") : QStringLiteral("\n"))};
        list.ordered = ordered;
        list.marker = delimiter;
        append(out, list);
    }

    // A cell's Markdown: one line, its breaks as <br>, its pipes escaped.
    QString cell(const Node *node, Styles styles)
    {
        Run run;
        walkChildren(node, run, nullptr, styled(node, styles));
        QString text = run.render(Breaks::Html, false);
        text.replace(u'|', QStringLiteral("\\|"));
        return text;
    }

    void table(const Node *node, Blocks &out, Styles styles)
    {
        std::vector<const Node *> rows;
        std::vector<const Node *> headRows, bodyRows;
        const Node *caption = nullptr;
        for (const Node *child : node->children) {
            if (child->isText() || isDropped(child)) continue;
            if (child->tag == u"tr") bodyRows.push_back(child);
            else if (child->tag == u"caption" && !caption) caption = child;
            else if (child->tag == u"thead" || child->tag == u"tbody" || child->tag == u"tfoot")
                for (const Node *row : child->children)
                    if (!row->isText() && row->tag == u"tr" && !isDropped(row))
                        (child->tag == u"thead" ? headRows : bodyRows).push_back(row);
        }
        rows = headRows;
        rows.insert(rows.end(), bodyRows.begin(), bodyRows.end());
        if (caption) append(out, flow(caption, styles));
        // Every cell in its place, spans taken into account.
        std::vector<std::vector<const Node *>> grid(rows.size());
        std::vector<std::vector<bool>> taken(rows.size());
        int columns = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            int column = 0;
            for (const Node *child : rows[r]->children) {
                if (child->isText() || (child->tag != u"td" && child->tag != u"th") || isDropped(child)) continue;
                while (column < int(taken[r].size()) && taken[r][size_t(column)]) ++column;
                if (column >= maxColumns) break;
                const int across = std::clamp(child->attribute(u"colspan").trimmed().toInt(), 1, maxSpan);
                const int down = std::clamp(child->attribute(u"rowspan").trimmed().toInt(), 1, maxSpan);
                const int width = std::min(across, maxColumns - column);
                for (size_t d = r; d < std::min(rows.size(), r + size_t(down)); ++d) {
                    if (int(taken[d].size()) < column + width) {
                        taken[d].resize(size_t(column + width), false);
                        grid[d].resize(size_t(column + width), nullptr);
                    }
                    for (int c = column; c < column + width; ++c) taken[d][size_t(c)] = true;
                }
                grid[r][size_t(column)] = child;
                column += width;
                columns = std::max(columns, column);
            }
        }
        if (columns == 0) return;
        // A table of one column is laid out, not tabulated: its cells are shown as they are.
        if (columns == 1) {
            for (const auto &row : grid)
                for (const Node *cell : row)
                    if (cell) append(out, flow(cell, styles));
            return;
        }
        const auto line = [](const QStringList &cells) {
            return QStringLiteral("| ") + cells.join(QStringLiteral(" | ")) + QStringLiteral(" |");
        };
        QStringList lines;
        QStringList alignments;
        for (size_t r = 0; r < grid.size(); ++r) {
            QStringList cells;
            for (int c = 0; c < columns; ++c) {
                const Node *at = size_t(c) < grid[r].size() ? grid[r][size_t(c)] : nullptr;
                cells << (at ? cell(at, styles) : QString());
                if (r != 0) continue;
                QString align = at ? at->attribute(u"align").trimmed().toLower() : QString();
                if (at && align.isEmpty()) align = declarations(at).value(QStringLiteral("text-align"));
                alignments << (align == u"center" ? QStringLiteral(":---:")
                               : align == u"right" ? QStringLiteral("---:")
                               : align == u"left" ? QStringLiteral(":---")
                                                  : QStringLiteral("---"));
            }
            lines << line(cells);
            if (r == 0) lines << line(alignments);
        }
        append(out, Block{Block::Table, lines.join(u'\n')});
    }

    const HtmlMarkdown::ImageSink &m_images;
};

// Text before the first tag, as the clipboard of some Windows programs
// carries, is a header and not the page's.
QStringView withoutHeader(QStringView html)
{
    if (!html.startsWith(QLatin1String("Version:"))) return html;
    const qsizetype tag = html.indexOf(u'<');
    return tag < 0 ? QStringView() : html.mid(tag);
}

} // namespace

QString HtmlMarkdown::convert(const QString &html, const ImageSink &images, bool *inlineOnly)
{
    Parser parser(withoutHeader(html));
    const Node *root = parser.parse();
    Converter converter(images);
    const Blocks blocks = converter.flow(root, 0);
    if (inlineOnly) *inlineOnly = blocks.size() == 1 && blocks.first().kind == Block::Paragraph;
    return joined(blocks);
}

bool HtmlMarkdown::mayEmbedImages(const QString &html)
{
    return html.contains(QLatin1String("data:image/"), Qt::CaseInsensitive);
}

bool HtmlMarkdown::isLoneImage(const QString &html, QString *alt)
{
    Parser parser(withoutHeader(html));
    const Node *image = nullptr;
    bool more = false;
    // Every element and text shown, in order; one image and nothing else qualifies.
    std::vector<const Node *> pending{parser.parse()};
    while (!pending.empty() && !more) {
        const Node *node = pending.back();
        pending.pop_back();
        if (node->isText()) {
            more = !collapsed(node->text).isEmpty();
            continue;
        }
        if (isDropped(node)) continue;
        if (node->tag == u"img") {
            more = image != nullptr;
            image = node;
            continue;
        }
        if (node->tag == u"hr" || node->tag == u"input") more = true;
        for (auto child = node->children.crbegin(); child != node->children.crend(); ++child) pending.push_back(*child);
    }
    if (!image || more) return false;
    if (alt) *alt = collapsed(image->attribute(u"alt"));
    return true;
}

QString HtmlMarkdown::escapeText(const QString &text)
{
    Run run;
    run.text(text, 0);
    return run.render(Breaks::Space, false);
}

QString HtmlMarkdown::encodePath(const QString &path)
{
    // Unreserved characters, "/" between folders, and a few that are plain
    // in file names and mean nothing in a destination.
    static const QByteArray kept = QByteArrayLiteral("/!$'*+,;=@");
    QString encoded;
    for (char byte : path.toUtf8()) {
        const auto c = static_cast<unsigned char>(byte);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'
            || c == '_' || c == '~' || kept.contains(char(c))) {
            encoded += QLatin1Char(char(c));
            continue;
        }
        encoded += QStringLiteral("%%1").arg(int(c), 2, 16, QLatin1Char('0')).toUpper();
    }
    return encoded;
}

QString HtmlMarkdown::encodeUrl(const QString &url)
{
    // Parentheses nest in a destination; one without its pair is escaped.
    std::vector<bool> unpaired(size_t(url.size()), false);
    std::vector<qsizetype> openings;
    for (qsizetype i = 0; i < url.size(); ++i) {
        if (url[i] == u'(') {
            openings.push_back(i);
        } else if (url[i] == u')') {
            if (openings.empty()) unpaired[size_t(i)] = true;
            else openings.pop_back();
        }
    }
    for (qsizetype i : openings) unpaired[size_t(i)] = true;
    QString encoded;
    for (qsizetype i = 0; i < url.size(); ++i) {
        const QChar c = url[i];
        if (c == u' ') encoded += QStringLiteral("%20");
        else if (c == u'<') encoded += QStringLiteral("%3C");
        else if (c == u'>') encoded += QStringLiteral("%3E");
        else if (c == u'\\') encoded += QStringLiteral("%5C");
        else if (c.unicode() < 0x20 || c.unicode() == 0x7f) continue;
        else if (unpaired[size_t(i)]) encoded += QStringLiteral("\\") + c;
        else encoded += c;
    }
    return encoded;
}
