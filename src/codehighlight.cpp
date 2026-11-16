#include "codehighlight.h"

#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>

#include <QCache>
#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <memory>

namespace {
using KSyntaxHighlighting::Theme;

PreviewStyle::Ink inkOf(Theme::TextStyle style)
{
    switch (style) {
    case Theme::Keyword:
    case Theme::ControlFlow:
        return PreviewStyle::Keyword;
    case Theme::Function:
    case Theme::BuiltIn:
    case Theme::Extension:
        return PreviewStyle::Function;
    case Theme::Variable:
    case Theme::Attribute:
        return PreviewStyle::Variable;
    case Theme::Operator:
        return PreviewStyle::Operator;
    case Theme::Preprocessor:
    case Theme::Import:
    case Theme::RegionMarker:
        return PreviewStyle::Preprocessor;
    case Theme::Char:
    case Theme::SpecialChar:
    case Theme::String:
    case Theme::VerbatimString:
    case Theme::SpecialString:
        return PreviewStyle::String;
    case Theme::DataType:
        return PreviewStyle::Type;
    case Theme::DecVal:
    case Theme::BaseN:
    case Theme::Float:
        return PreviewStyle::Number;
    case Theme::Constant:
        return PreviewStyle::Constant;
    case Theme::Comment:
    case Theme::Documentation:
    case Theme::Annotation:
    case Theme::CommentVar:
    case Theme::Information:
        return PreviewStyle::Comment;
    case Theme::Warning:
    case Theme::Alert:
    case Theme::Error:
        return PreviewStyle::Danger;
    case Theme::Normal:
    case Theme::Others:
        break;
    }
    return PreviewStyle::Code;
}

// Reads a text line by line and keeps the runs that are not plain.
class Reader : public KSyntaxHighlighting::AbstractHighlighter {
public:
    QList<CodeHighlight::Span> read(const QString &code)
    {
        KSyntaxHighlighting::State state;
        qsizetype start = 0;
        while (start <= code.size()) {
            qsizetype end = code.indexOf(u'\n', start);
            if (end < 0) end = code.size();
            m_lineStart = int(start);
            state = highlightLine(QStringView(code).sliced(start, end - start), state);
            start = end + 1;
        }
        return std::move(m_spans);
    }

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override
    {
        if (length <= 0) return;
        const PreviewStyle::Ink ink = inkOf(format.textStyle());
        if (ink == PreviewStyle::Code) return;
        // A run that goes on in the same ink is one run.
        if (!m_spans.isEmpty() && m_spans.last().ink == ink
            && m_spans.last().start + m_spans.last().length == m_lineStart + offset) {
            m_spans.last().length += length;
            return;
        }
        m_spans.append({m_lineStart + offset, length, ink});
    }

private:
    QList<CodeHighlight::Span> m_spans;
    int m_lineStart = 0;
};

struct Shared {
    QMutex mutex;
    // The list of languages, loaded by prepare() and gone with the application.
    std::unique_ptr<KSyntaxHighlighting::Repository> repository;
    QHash<QString, KSyntaxHighlighting::Definition> definitions;
    QCache<QString, QList<CodeHighlight::Span>> cache{200000};
};

Shared &shared()
{
    static Shared instance;
    return instance;
}

void forget()
{
    Shared &s = shared();
    QMutexLocker lock(&s.mutex);
    s.definitions.clear();
    s.repository.reset();
}

// With the mutex held. The repository watches the application for a change
// of palette, so it is made on the application's thread, and goes before
// the application does.
bool load(Shared &s)
{
    if (s.repository) return true;
    QCoreApplication *application = QCoreApplication::instance();
    if (!application || QThread::currentThread() != application->thread()) return false;
    s.repository = std::make_unique<KSyntaxHighlighting::Repository>();
    qAddPostRoutine(forget);
    return true;
}

// With the mutex held.
KSyntaxHighlighting::Definition definitionFor(Shared &s, const QString &language)
{
    if (language.isEmpty()) return {};
    if (const auto found = s.definitions.constFind(language); found != s.definitions.cend()) return *found;
    // On any other thread than the application's, the languages must have
    // been loaded already; until then no code is coloured, and nothing is
    // remembered about a language.
    if (!load(s)) return {};
    // Names people write on fences that are neither a definition's name
    // nor a file extension it claims.
    static const QHash<QString, QString> aliases{
        {QStringLiteral("shell"), QStringLiteral("Bash")},      {QStringLiteral("console"), QStringLiteral("Bash")},
        {QStringLiteral("zsh"), QStringLiteral("Zsh")},         {QStringLiteral("c++"), QStringLiteral("C++")},
        {QStringLiteral("golang"), QStringLiteral("Go")},       {QStringLiteral("jsonc"), QStringLiteral("JSON")},
        {QStringLiteral("dockerfile"), QStringLiteral("Dockerfile")},
        {QStringLiteral("make"), QStringLiteral("Makefile")},   {QStringLiteral("tex"), QStringLiteral("LaTeX")},
        {QStringLiteral("objc"), QStringLiteral("Objective-C")}, {QStringLiteral("text"), QString()},
        {QStringLiteral("plain"), QString()},                   {QStringLiteral("plaintext"), QString()},
        {QStringLiteral("txt"), QString()},                     {QStringLiteral("none"), QString()},
    };
    KSyntaxHighlighting::Definition definition;
    if (const auto alias = aliases.constFind(language); alias != aliases.cend()) {
        if (!alias->isEmpty()) definition = s.repository->definitionForName(*alias);
    } else {
        definition = s.repository->definitionForName(language);
        if (!definition.isValid()) definition = s.repository->definitionForFileName(QStringLiteral("file.") + language);
    }
    s.definitions.insert(language, definition);
    return definition;
}
} // namespace

QString CodeHighlight::languageOf(const QString &info)
{
    const QStringView trimmed = QStringView(info).trimmed();
    qsizetype end = 0;
    while (end < trimmed.size() && !trimmed[end].isSpace()) ++end;
    QString language = trimmed.first(end).toString().toLower();
    for (const QLatin1StringView prefix : {QLatin1StringView("language-"), QLatin1StringView("lang-")})
        if (language.startsWith(prefix)) language.remove(0, prefix.size());
    // "{.python}" and "python,linenos", as some tools write them.
    while (!language.isEmpty() && (language.front() == u'{' || language.front() == u'.')) language.remove(0, 1);
    for (qsizetype i = 0; i < language.size(); ++i) {
        const QChar c = language.at(i);
        if (c == u',' || c == u'}' || c == u'{' || c == u':') {
            language.truncate(i);
            break;
        }
    }
    return language;
}

void CodeHighlight::prepare()
{
    Shared &s = shared();
    QMutexLocker lock(&s.mutex);
    load(s);
}

void CodeHighlight::unload()
{
    Shared &s = shared();
    QMutexLocker lock(&s.mutex);
    s.definitions.clear();
    s.cache.clear();
    s.repository.reset();
}

bool CodeHighlight::knows(const QString &language)
{
    Shared &s = shared();
    QMutexLocker lock(&s.mutex);
    return definitionFor(s, language.toLower()).isValid();
}

QList<CodeHighlight::Span> CodeHighlight::spans(const QString &language, const QString &code)
{
    if (language.isEmpty() || code.isEmpty() || code.size() > MaxLength) return {};
    Shared &s = shared();
    QMutexLocker lock(&s.mutex);
    const QString name = language.toLower();
    const KSyntaxHighlighting::Definition definition = definitionFor(s, name);
    if (!definition.isValid()) return {};
    const QString key = name + u'\n' + QString::number(code.size()) + u'\n' + QString::number(qHash(code, 0x51ed270b));
    if (const QList<Span> *kept = s.cache.object(key)) return *kept;
    Reader reader;
    reader.setDefinition(definition);
    QList<Span> found = reader.read(code);
    s.cache.insert(key, new QList<Span>(found), int(std::max<qsizetype>(1, found.size())));
    return found;
}
