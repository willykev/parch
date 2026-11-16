#include "cslstyle.h"

#include <QSet>
#include <QXmlStreamReader>

#include <functional>

namespace {

// A plain element tree, read first so that the CSL reading below can look
// at an element's children in any order.
struct XmlElement {
    QString name;
    QHash<QString, QString> attributes;
    QString text;
    std::vector<XmlElement> children;
    int line = 0;
};

bool readXml(const QByteArray &xml, XmlElement &root, QString *error)
{
    QXmlStreamReader reader(xml);
    std::vector<XmlElement *> stack;
    bool haveRoot = false;
    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            XmlElement element;
            element.name = reader.name().toString();
            element.line = int(reader.lineNumber());
            for (const QXmlStreamAttribute &attribute : reader.attributes())
                element.attributes.insert(attribute.qualifiedName().toString(), attribute.value().toString());
            if (stack.empty()) {
                if (haveRoot) break;
                root = std::move(element);
                haveRoot = true;
                stack.push_back(&root);
            } else {
                stack.back()->children.push_back(std::move(element));
                stack.push_back(&stack.back()->children.back());
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (!stack.empty()) stack.pop_back();
        } else if (token == QXmlStreamReader::Characters && !stack.empty()) {
            stack.back()->text += reader.text();
        }
    }
    if (reader.hasError()) {
        if (error)
            *error = QStringLiteral("The file is not well-formed XML: %1 (line %2, column %3)")
                         .arg(reader.errorString())
                         .arg(reader.lineNumber())
                         .arg(reader.columnNumber());
        return false;
    }
    if (!haveRoot) {
        if (error) *error = QStringLiteral("The file is empty");
        return false;
    }
    return true;
}

const XmlElement *child(const XmlElement &element, const QString &name)
{
    for (const XmlElement &c : element.children) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

QString termKey(const QString &name, const QString &form, const QString &genderForm = {})
{
    return name + u'/' + form + u'/' + genderForm;
}

} // namespace

class CslReader {
public:
    QString error;

    bool fail(const QString &message, int line = 0)
    {
        if (error.isEmpty()) error = line > 0 ? QStringLiteral("%1 (line %2)").arg(message).arg(line) : message;
        return false;
    }

    static CslTextCase textCase(const QString &value)
    {
        if (value == u"lowercase") return CslTextCase::Lowercase;
        if (value == u"uppercase") return CslTextCase::Uppercase;
        if (value == u"capitalize-first") return CslTextCase::CapitalizeFirst;
        if (value == u"capitalize-all") return CslTextCase::CapitalizeAll;
        if (value == u"sentence") return CslTextCase::Sentence;
        if (value == u"title") return CslTextCase::Title;
        return CslTextCase::None;
    }

    static CslFormatting formatting(const QHash<QString, QString> &a)
    {
        CslFormatting f;
        f.prefix = a.value(QStringLiteral("prefix"));
        f.suffix = a.value(QStringLiteral("suffix"));
        if (const QString v = a.value(QStringLiteral("font-style")); !v.isEmpty())
            f.italic = v == u"italic" || v == u"oblique" ? 1 : 0;
        if (const QString v = a.value(QStringLiteral("font-weight")); !v.isEmpty()) f.bold = v == u"bold" ? 1 : 0;
        if (const QString v = a.value(QStringLiteral("font-variant")); !v.isEmpty())
            f.smallCaps = v == u"small-caps" ? 1 : 0;
        if (const QString v = a.value(QStringLiteral("text-decoration")); !v.isEmpty())
            f.underline = v == u"underline" ? 1 : 0;
        if (const QString v = a.value(QStringLiteral("vertical-align")); !v.isEmpty())
            f.verticalAlign = v == u"sup" ? 1 : v == u"sub" ? 2 : 0;
        f.quotes = a.value(QStringLiteral("quotes")) == u"true";
        f.stripPeriods = a.value(QStringLiteral("strip-periods")) == u"true";
        f.textCase = textCase(a.value(QStringLiteral("text-case")));
        const QString display = a.value(QStringLiteral("display"));
        f.display = display == u"block"          ? CslDisplay::Block
                    : display == u"left-margin"  ? CslDisplay::LeftMargin
                    : display == u"right-inline" ? CslDisplay::RightInline
                    : display == u"indent"       ? CslDisplay::Indent
                                                 : CslDisplay::None;
        return f;
    }

    // Reads a rendering element and its children.
    bool node(const XmlElement &x, CslNode &out, bool inNames = false)
    {
        static const QHash<QString, CslElement> elements{
            {QStringLiteral("layout"), CslElement::Layout},       {QStringLiteral("text"), CslElement::Text},
            {QStringLiteral("number"), CslElement::Number},       {QStringLiteral("date"), CslElement::Date},
            {QStringLiteral("date-part"), CslElement::DatePart},  {QStringLiteral("names"), CslElement::Names},
            {QStringLiteral("name"), CslElement::Name},           {QStringLiteral("name-part"), CslElement::NamePart},
            {QStringLiteral("et-al"), CslElement::EtAl},          {QStringLiteral("label"), CslElement::Label},
            {QStringLiteral("substitute"), CslElement::Substitute}, {QStringLiteral("group"), CslElement::Group},
            {QStringLiteral("choose"), CslElement::Choose},       {QStringLiteral("if"), CslElement::If},
            {QStringLiteral("else-if"), CslElement::ElseIf},      {QStringLiteral("else"), CslElement::Else},
        };
        const auto it = elements.constFind(x.name);
        if (it == elements.constEnd())
            return fail(QStringLiteral("The style uses an element CSL does not define, <%1>").arg(x.name), x.line);
        out.element = *it;
        out.attributes = x.attributes;
        out.line = x.line;
        out.formatting = formatting(x.attributes);
        out.hasDelimiter = x.attributes.contains(QStringLiteral("delimiter"));
        out.delimiter = x.attributes.value(QStringLiteral("delimiter"));

        switch (out.element) {
        case CslElement::Text:
            if (!x.attributes.contains(QStringLiteral("variable")) && !x.attributes.contains(QStringLiteral("macro"))
                && !x.attributes.contains(QStringLiteral("term")) && !x.attributes.contains(QStringLiteral("value")))
                return fail(QStringLiteral("A <text> element needs a variable, macro, term or value"), x.line);
            if (x.attributes.contains(QStringLiteral("macro"))) m_calledMacros.append({x.attributes.value(QStringLiteral("macro")), x.line});
            if (x.attributes.value(QStringLiteral("variable")) == u"year-suffix") m_yearSuffix = true;
            if (x.attributes.value(QStringLiteral("variable")) == u"citation-number") m_citationNumber = true;
            break;
        case CslElement::Number:
        case CslElement::Date:
            // A locale's date formats name no variable.
            if (!x.attributes.contains(QStringLiteral("variable")) && !m_inLocale)
                return fail(QStringLiteral("A <%1> element needs a variable").arg(x.name), x.line);
            break;
        case CslElement::Names:
            if (!x.attributes.contains(QStringLiteral("variable")) && !inNames)
                return fail(QStringLiteral("A <names> element needs a variable"), x.line);
            break;
        case CslElement::Choose:
            if (x.children.empty() || x.children.front().name != u"if")
                return fail(QStringLiteral("A <choose> element must start with <if>"), x.line);
            for (std::size_t i = 0; i < x.children.size(); ++i) {
                const QString &name = x.children[i].name;
                if (name != u"if" && name != u"else-if" && name != u"else")
                    return fail(QStringLiteral("A <choose> element may hold only <if>, <else-if> and <else>"),
                                x.children[i].line);
                if ((name == u"if") != (i == 0))
                    return fail(QStringLiteral("A <choose> element has <if> only first"), x.children[i].line);
                if (name == u"else" && i + 1 != x.children.size())
                    return fail(QStringLiteral("A <choose> element has <else> only last"), x.children[i].line);
            }
            break;
        case CslElement::If:
        case CslElement::ElseIf: {
            static const QStringList conditions{QStringLiteral("type"),       QStringLiteral("variable"),
                                                QStringLiteral("is-numeric"), QStringLiteral("is-uncertain-date"),
                                                QStringLiteral("locator"),    QStringLiteral("position"),
                                                QStringLiteral("disambiguate")};
            bool any = false;
            for (const QString &condition : conditions) any = any || x.attributes.contains(condition);
            if (!any) return fail(QStringLiteral("A <%1> element tests nothing").arg(x.name), x.line);
            break;
        }
        default:
            break;
        }
        const bool names = out.element == CslElement::Names || (inNames && out.element == CslElement::Substitute);
        for (const XmlElement &c : x.children) {
            CslNode n;
            if (!node(c, n, names || inNames)) return false;
            out.children.push_back(std::move(n));
        }
        return true;
    }

    bool locale(const XmlElement &x, CslLocale &out)
    {
        out.m_language = x.attributes.value(QStringLiteral("xml:lang"));
        if (const XmlElement *options = child(x, QStringLiteral("style-options"))) {
            if (options->attributes.contains(QStringLiteral("punctuation-in-quote"))) {
                out.m_hasPunctuationOption = true;
                out.m_punctuationInQuote = options->attributes.value(QStringLiteral("punctuation-in-quote")) == u"true";
            }
            if (options->attributes.contains(QStringLiteral("limit-day-ordinals-to-day-1"))) {
                out.m_hasDayOption = true;
                out.m_limitDayOrdinals =
                    options->attributes.value(QStringLiteral("limit-day-ordinals-to-day-1")) == u"true";
            }
        }
        for (const XmlElement &c : x.children) {
            if (c.name == u"date") {
                CslNode date;
                m_inLocale = true;
                const bool ok = node(c, date);
                m_inLocale = false;
                if (!ok) return false;
                out.m_dates.insert(c.attributes.value(QStringLiteral("form")), date);
            } else if (c.name == u"terms") {
                for (const XmlElement &t : c.children) {
                    if (t.name != u"term") continue;
                    CslLocale::Term term;
                    const XmlElement *single = child(t, QStringLiteral("single"));
                    const XmlElement *multiple = child(t, QStringLiteral("multiple"));
                    if (single || multiple) {
                        term.single = single ? single->text : QString();
                        term.multiple = multiple ? multiple->text : term.single;
                    } else {
                        term.single = term.multiple = t.text;
                    }
                    term.gender = t.attributes.value(QStringLiteral("gender"));
                    term.genderForm = t.attributes.value(QStringLiteral("gender-form"));
                    term.match = t.attributes.value(QStringLiteral("match"));
                    const QString name = t.attributes.value(QStringLiteral("name"));
                    if (name.startsWith(u"ordinal")) out.m_replacesOrdinals = true;
                    QString form = t.attributes.value(QStringLiteral("form"));
                    if (form.isEmpty()) form = QStringLiteral("long");
                    out.m_terms.insert(termKey(name, form, term.genderForm), term);
                }
            }
        }
        return true;
    }

    std::vector<CslStyle::SortKey> sort(const XmlElement &x)
    {
        std::vector<CslStyle::SortKey> keys;
        for (const XmlElement &k : x.children) {
            if (k.name != u"key") continue;
            CslStyle::SortKey key;
            key.variable = k.attributes.value(QStringLiteral("variable"));
            key.macro = k.attributes.value(QStringLiteral("macro"));
            if (key.variable.isEmpty() && key.macro.isEmpty()) {
                fail(QStringLiteral("A sort <key> needs a variable or a macro"), k.line);
                continue;
            }
            if (!key.macro.isEmpty()) m_calledMacros.append({key.macro, k.line});
            key.descending = k.attributes.value(QStringLiteral("sort")) == u"descending";
            bool ok = false;
            if (const int v = k.attributes.value(QStringLiteral("names-min")).toInt(&ok); ok) key.namesMin = v;
            if (const int v = k.attributes.value(QStringLiteral("names-use-first")).toInt(&ok); ok) key.namesUseFirst = v;
            if (k.attributes.contains(QStringLiteral("names-use-last")))
                key.namesUseLast = k.attributes.value(QStringLiteral("names-use-last")) == u"true" ? 1 : 0;
            keys.push_back(key);
        }
        return keys;
    }

    bool section(const XmlElement &x, CslStyle::Section &out, const QString &what)
    {
        out.options = x.attributes;
        const XmlElement *layout = child(x, QStringLiteral("layout"));
        if (!layout) return fail(QStringLiteral("The <%1> element has no <layout>").arg(what), x.line);
        if (!node(*layout, out.layout)) return false;
        if (const XmlElement *sortElement = child(x, QStringLiteral("sort"))) out.sort = sort(*sortElement);
        return error.isEmpty();
    }

    // Finds a macro that calls itself, directly or through others.
    bool macroCycle(const CslStyle &style)
    {
        QHash<QString, int> state;
        std::function<bool(const QString &)> visit;
        std::function<bool(const CslNode &)> walk = [&](const CslNode &n) {
            if (n.element == CslElement::Text && n.has(QStringLiteral("macro")))
                if (!visit(n.attribute(QStringLiteral("macro")))) return false;
            for (const CslNode &c : n.children)
                if (!walk(c)) return false;
            return true;
        };
        visit = [&](const QString &name) {
            const int s = state.value(name);
            if (s == 1) return fail(QStringLiteral("The macro “%1” calls itself").arg(name));
            if (s == 2) return true;
            state.insert(name, 1);
            if (const CslNode *m = style.macro(name))
                if (!walk(*m)) return false;
            state.insert(name, 2);
            return true;
        };
        // In name order, so that a cycle is always reported the same way.
        QStringList names = style.m_macros.keys();
        names.sort();
        for (const QString &name : std::as_const(names))
            if (!visit(name)) return true;
        return false;
    }

    std::shared_ptr<const CslStyle> style(const QByteArray &xml)
    {
        XmlElement root;
        if (!readXml(xml, root, &error)) return nullptr;
        if (root.name != u"style") {
            fail(QStringLiteral("The file is not a CSL style: its root element is <%1>, not <style>").arg(root.name),
                 root.line);
            return nullptr;
        }
        auto style = std::make_shared<CslStyle>();
        const QString cls = root.attributes.value(QStringLiteral("class"));
        if (cls != u"in-text" && cls != u"note") {
            fail(QStringLiteral("The style's class must be “in-text” or “note”"), root.line);
            return nullptr;
        }
        style->m_noteStyle = cls == u"note";
        style->m_defaultLocale = root.attributes.value(QStringLiteral("default-locale"));
        style->m_options = root.attributes;
        QString parent;
        if (const XmlElement *info = child(root, QStringLiteral("info"))) {
            if (const XmlElement *title = child(*info, QStringLiteral("title"))) style->m_title = title->text.trimmed();
            if (const XmlElement *id = child(*info, QStringLiteral("id"))) style->m_id = id->text.trimmed();
            for (const XmlElement &link : info->children) {
                if (link.name == u"link" && link.attributes.value(QStringLiteral("rel")) == u"independent-parent")
                    parent = link.attributes.value(QStringLiteral("href"));
            }
        }
        const XmlElement *citation = child(root, QStringLiteral("citation"));
        if (!citation) {
            if (!parent.isEmpty())
                fail(QStringLiteral("This is a dependent style, which only names its parent style (%1); choose the "
                                    "parent style instead")
                         .arg(parent));
            else
                fail(QStringLiteral("The style has no <citation> element"));
            return nullptr;
        }
        for (const XmlElement &c : root.children) {
            if (c.name == u"macro") {
                const QString name = c.attributes.value(QStringLiteral("name"));
                if (name.isEmpty()) {
                    fail(QStringLiteral("A <macro> has no name"), c.line);
                    return nullptr;
                }
                CslNode macro;
                macro.element = CslElement::Group;
                macro.line = c.line;
                for (const XmlElement &m : c.children) {
                    CslNode n;
                    if (!node(m, n)) return nullptr;
                    macro.children.push_back(std::move(n));
                }
                style->m_macros.insert(name, std::move(macro));
            } else if (c.name == u"locale") {
                CslLocale locale;
                if (!this->locale(c, locale)) return nullptr;
                style->m_locales.emplace_back(locale.m_language, std::move(locale));
            }
        }
        // Locales for every language come first, so that a language's own replaces them.
        std::stable_sort(style->m_locales.begin(), style->m_locales.end(),
                         [](const auto &a, const auto &b) { return a.first.isEmpty() && !b.first.isEmpty(); });
        if (!section(*citation, style->m_citation, QStringLiteral("citation"))) return nullptr;
        if (const XmlElement *bibliography = child(root, QStringLiteral("bibliography"))) {
            if (!section(*bibliography, style->m_bibliography, QStringLiteral("bibliography"))) return nullptr;
            style->m_hasBibliography = true;
        }
        if (!error.isEmpty()) return nullptr;
        for (const auto &[name, line] : m_calledMacros) {
            if (!style->m_macros.contains(name)) {
                fail(QStringLiteral("The style calls the macro “%1”, which it does not define").arg(name), line);
                return nullptr;
            }
        }
        if (macroCycle(*style)) return nullptr;
        style->m_rendersYearSuffix = m_yearSuffix;
        style->m_usesCitationNumber = m_citationNumber;
        return style;
    }

private:
    bool m_inLocale = false;
    QList<QPair<QString, int>> m_calledMacros;
    bool m_yearSuffix = false;
    bool m_citationNumber = false;
};

CslFormatting cslFormatting(const QHash<QString, QString> &attributes)
{
    return CslReader::formatting(attributes);
}

std::shared_ptr<CslLocale> CslLocale::fromXml(const QByteArray &xml, QString *error)
{
    XmlElement root;
    QString message;
    if (!readXml(xml, root, &message)) {
        if (error) *error = QStringLiteral("The citation locale cannot be used. ") + message + u'.';
        return nullptr;
    }
    if (root.name != u"locale") {
        if (error) *error = QStringLiteral("The file is not a CSL locale: its root element is <%1>.").arg(root.name);
        return nullptr;
    }
    CslReader reader;
    auto locale = std::make_shared<CslLocale>();
    if (!reader.locale(root, *locale)) {
        if (error) *error = reader.error;
        return nullptr;
    }
    return locale;
}

std::optional<CslLocale::Term> CslLocale::term(const QString &name, const QString &form, const QString &genderForm) const
{
    QStringList forms{form};
    if (form == u"verb-short") forms << QStringLiteral("verb") << QStringLiteral("long");
    else if (form == u"symbol")
        forms << QStringLiteral("short") << QStringLiteral("long");
    else if (form == u"short")
        forms << QStringLiteral("long");
    else if (form == u"verb")
        forms << QStringLiteral("long");
    else if (form != u"long")
        forms << QStringLiteral("long");
    for (const QString &f : forms) {
        if (!genderForm.isEmpty()) {
            if (const auto it = m_terms.constFind(termKey(name, f, genderForm)); it != m_terms.constEnd()) return *it;
        }
        if (const auto it = m_terms.constFind(termKey(name, f)); it != m_terms.constEnd()) return *it;
    }
    return std::nullopt;
}

QString CslLocale::termText(const QString &name, const QString &form, bool plural) const
{
    const std::optional<Term> t = term(name, form);
    if (!t) return {};
    return plural ? t->multiple : t->single;
}

QString CslLocale::ordinal(int number, const QString &gender) const
{
    const int value = qAbs(number);
    auto matches = [value](int n, const QString &match, bool twoDigits) {
        const QString m = match.isEmpty() ? (twoDigits ? QStringLiteral("last-two-digits") : QStringLiteral("last-digit"))
                                          : match;
        if (m == u"whole-number") return value == n;
        if (m == u"last-two-digits") return value % 100 == n;
        return value % 10 == n;
    };
    auto find = [&](const QString &genderForm) -> std::optional<QString> {
        for (int n = 10; n <= 99; ++n) {
            const auto it = m_terms.constFind(termKey(QStringLiteral("ordinal-%1").arg(n, 2, 10, QLatin1Char('0')),
                                                      QStringLiteral("long"), genderForm));
            if (it != m_terms.constEnd() && matches(n, it->match, true)) return it->single;
        }
        for (int n = 0; n <= 9; ++n) {
            const auto it = m_terms.constFind(termKey(QStringLiteral("ordinal-%1").arg(n, 2, 10, QLatin1Char('0')),
                                                      QStringLiteral("long"), genderForm));
            if (it != m_terms.constEnd() && matches(n, it->match, false)) return it->single;
        }
        return std::nullopt;
    };
    std::optional<QString> suffix;
    if (!gender.isEmpty()) suffix = find(gender);
    if (!suffix) suffix = find({});
    if (!suffix) {
        const std::optional<Term> fallback = term(QStringLiteral("ordinal"), QStringLiteral("long"), gender);
        suffix = fallback ? fallback->single : QString();
    }
    return QString::number(number) + *suffix;
}

QString CslLocale::longOrdinal(int number, const QString &gender) const
{
    if (number >= 1 && number <= 10) {
        const std::optional<Term> t =
            term(QStringLiteral("long-ordinal-%1").arg(number, 2, 10, QLatin1Char('0')), QStringLiteral("long"), gender);
        if (t) return t->single;
    }
    return ordinal(number, gender);
}

const CslNode *CslLocale::dateFormat(const QString &form) const
{
    const auto it = m_dates.constFind(form);
    return it == m_dates.constEnd() ? nullptr : &*it;
}

void CslLocale::merge(const CslLocale &overrides)
{
    if (overrides.m_replacesOrdinals) {
        for (auto it = m_terms.begin(); it != m_terms.end();) {
            if (it.key().startsWith(u"ordinal") || it.key().startsWith(u"long-ordinal"))
                it = m_terms.erase(it);
            else
                ++it;
        }
    }
    for (auto it = overrides.m_terms.constBegin(); it != overrides.m_terms.constEnd(); ++it) m_terms.insert(it.key(), *it);
    for (auto it = overrides.m_dates.constBegin(); it != overrides.m_dates.constEnd(); ++it) m_dates.insert(it.key(), *it);
    if (overrides.m_hasPunctuationOption) {
        m_punctuationInQuote = overrides.m_punctuationInQuote;
        m_hasPunctuationOption = true;
    }
    if (overrides.m_hasDayOption) {
        m_limitDayOrdinals = overrides.m_limitDayOrdinals;
        m_hasDayOption = true;
    }
}

std::shared_ptr<const CslStyle> CslStyle::fromXml(const QByteArray &xml, QString *error)
{
    CslReader reader;
    std::shared_ptr<const CslStyle> style = reader.style(xml);
    if (!style && error) *error = QStringLiteral("The citation style cannot be used. ") + reader.error + u'.';
    if (error && style) error->clear();
    return style;
}

const CslNode *CslStyle::macro(const QString &name) const
{
    const auto it = m_macros.constFind(name);
    return it == m_macros.constEnd() ? nullptr : &*it;
}
