#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

// CSL 1.0.2 styles and locales, read with QXmlStreamReader into a tree
// that the processor (CslEngine) renders. Reading checks what rendering
// relies on, so that a malformed style fails here with a clear message.

enum class CslElement : quint8 {
    Layout,
    Text,
    Number,
    Date,
    DatePart,
    Names,
    Name,
    NamePart,
    EtAl,
    Label,
    Substitute,
    Group,
    Choose,
    If,
    ElseIf,
    Else,
};

enum class CslTextCase : quint8 { None, Lowercase, Uppercase, CapitalizeFirst, CapitalizeAll, Sentence, Title };
enum class CslDisplay : quint8 { None, Block, LeftMargin, RightInline, Indent };

// The formatting attributes of a rendering element. Unset font
// attributes are -1, so that an element changes only what it names.
struct CslFormatting {
    QString prefix;
    QString suffix;
    qint8 italic = -1;
    qint8 bold = -1;
    qint8 smallCaps = -1;
    qint8 underline = -1;
    // 0 baseline, 1 superscript, 2 subscript.
    qint8 verticalAlign = -1;
    bool quotes = false;
    bool stripPeriods = false;
    CslTextCase textCase = CslTextCase::None;
    CslDisplay display = CslDisplay::None;
};

// Reads the formatting attributes of an element.
CslFormatting cslFormatting(const QHash<QString, QString> &attributes);

struct CslNode {
    CslElement element = CslElement::Group;
    QHash<QString, QString> attributes;
    CslFormatting formatting;
    QString delimiter;
    bool hasDelimiter = false;
    std::vector<CslNode> children;
    int line = 0;

    QString attribute(const QString &name) const { return attributes.value(name); }
    bool has(const QString &name) const { return attributes.contains(name); }
};

class CslLocale {
public:
    struct Term {
        QString single;
        QString multiple;
        QString gender;
        // For ordinals: the gender of the noun the ordinal goes with.
        QString genderForm;
        // For ordinals: last-digit, last-two-digits or whole-number.
        QString match;
    };

    // Reads a locales-xx-XX.xml file.
    static std::shared_ptr<CslLocale> fromXml(const QByteArray &xml, QString *error);

    QString language() const { return m_language; }
    bool punctuationInQuote() const { return m_punctuationInQuote; }
    bool limitDayOrdinalsToDay1() const { return m_limitDayOrdinals; }
    // A term in a form, falling back as CSL says: verb-short to verb to
    // long, symbol to short to long, short to long.
    std::optional<Term> term(const QString &name, const QString &form = QStringLiteral("long"),
                             const QString &genderForm = {}) const;
    QString termText(const QString &name, const QString &form = QStringLiteral("long"), bool plural = false) const;
    // "1st", "2nd"; "first" for long ordinals up to ten.
    QString ordinal(int number, const QString &gender = {}) const;
    QString longOrdinal(int number, const QString &gender = {}) const;
    // The localised date format, "text" or "numeric".
    const CslNode *dateFormat(const QString &form) const;
    // Applies a style's own locale over this one: its terms, dates and
    // options replace these.
    void merge(const CslLocale &overrides);

private:
    friend class CslReader;
    QString m_language;
    bool m_punctuationInQuote = false;
    bool m_limitDayOrdinals = false;
    bool m_hasPunctuationOption = false;
    bool m_hasDayOption = false;
    bool m_replacesOrdinals = false;
    // Keyed by name, '/', form and, for ordinals, '/', gender form.
    QHash<QString, Term> m_terms;
    QHash<QString, CslNode> m_dates;
};

class CslStyle {
public:
    struct SortKey {
        QString variable;
        QString macro;
        bool descending = false;
        int namesMin = -1;
        int namesUseFirst = -1;
        int namesUseLast = -1;
    };
    struct Section {
        CslNode layout;
        std::vector<SortKey> sort;
        // Options and inheritable name options set on the element.
        QHash<QString, QString> options;
    };

    // Reads a style. On failure returns null and sets error to a message
    // that names the problem and, where it has one, its line.
    static std::shared_ptr<const CslStyle> fromXml(const QByteArray &xml, QString *error, int maxExpandedNodes = 100000);

    QString id() const { return m_id; }
    QString title() const { return m_title; }
    bool isNoteStyle() const { return m_noteStyle; }
    QString defaultLocale() const { return m_defaultLocale; }
    const QHash<QString, QString> &options() const { return m_options; }
    const Section &citation() const { return m_citation; }
    const Section *bibliography() const { return m_hasBibliography ? &m_bibliography : nullptr; }
    const CslNode *macro(const QString &name) const;
    // The style's own locale elements, those for every language first.
    const std::vector<std::pair<QString, CslLocale>> &locales() const { return m_locales; }
    // Whether the style places the year-suffix itself; otherwise the
    // processor adds it after the first year it renders.
    bool rendersYearSuffix() const { return m_rendersYearSuffix; }
    bool usesCitationNumber() const { return m_usesCitationNumber; }

private:
    friend class CslReader;
    QString m_id;
    QString m_title;
    bool m_noteStyle = false;
    QString m_defaultLocale;
    QHash<QString, QString> m_options;
    Section m_citation;
    Section m_bibliography;
    bool m_hasBibliography = false;
    QHash<QString, CslNode> m_macros;
    std::vector<std::pair<QString, CslLocale>> m_locales;
    bool m_rendersYearSuffix = false;
    bool m_usesCitationNumber = false;
};
