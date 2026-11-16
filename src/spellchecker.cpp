#include "spellchecker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <hunspell.hxx>
#include <vector>

namespace {

bool isHyphen(QChar c) { return c == u'-' || c == QChar(0x2010) || c == QChar(0x2011); }

// A word worth asking about: two letters or more, at least one of them
// Latin, and no digit, nor a dot or colon inside, as a domain, a file name
// or "e.g" has.
bool checkable(QStringView word)
{
    int letters = 0;
    bool latin = false;
    for (const QChar c : word) {
        if (c.isDigit() || c == u'.' || c == u':') return false;
        if (c.isLetter()) {
            ++letters;
            latin = latin || c.script() == QChar::Script_Latin;
        }
    }
    return letters >= 2 && latin;
}

// The words of one run of prose, at `start` in the text.
void wordsIn(QStringView text, int start, int end, QList<SourceRange> &out)
{
    const QStringView piece = text.sliced(start, end - start);
    struct Token {
        int start, end;
    };
    std::vector<Token> tokens;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, piece);
    int previous = 0;
    for (qsizetype boundary = finder.toNextBoundary(); boundary >= 0; boundary = finder.toNextBoundary()) {
        const int segmentEnd = int(boundary);
        // Unicode keeps an underscore inside a word; Markdown's is emphasis or a name's.
        int part = previous;
        for (int i = previous; i <= segmentEnd; ++i) {
            if (i < segmentEnd && piece[i] != u'_') continue;
            const QStringView found = piece.sliced(part, i - part);
            if (std::any_of(found.begin(), found.end(), [](QChar c) { return c.isLetter(); }))
                tokens.push_back({part, i});
            part = i + 1;
        }
        previous = segmentEnd;
    }
    std::vector<Token> words;
    for (const Token &token : tokens) {
        // Words joined by a hyphen are one word, as Hunspell checks them;
        // joined by a dot, they are a name such as a file's, not checked.
        if (!words.empty() && words.back().end + 1 == token.start
            && (isHyphen(piece[words.back().end]) || piece[words.back().end] == u'.'))
            words.back().end = token.end;
        else
            words.push_back(token);
    }
    for (const Token &word : words)
        if (checkable(piece.sliced(word.start, word.end - word.start))) out.append({start + word.start, word.end - word.start});
}

// The words of one run of prose, but for those of a URL written as text,
// which the engine leaves as text when it is no autolink.
void proseWords(QStringView text, int start, int end, QList<SourceRange> &out)
{
    static const QRegularExpression url(QStringLiteral(R"((?<![\w/])(?:[A-Za-z][A-Za-z0-9+.-]*://|mailto:)\S+)"));
    int at = start;
    for (auto match = url.globalMatchView(text.sliced(start, end - start)); match.hasNext();) {
        const QRegularExpressionMatch found = match.next();
        const int urlStart = start + int(found.capturedStart());
        if (urlStart > at) wordsIn(text, at, urlStart, out);
        at = start + int(found.capturedEnd());
    }
    if (end > at) wordsIn(text, at, end, out);
}

} // namespace

std::vector<md::Range> spellingProse(const QString &text)
{
    std::vector<md::Range> prose;
    md::parse(text).forEachProseRange(0, [&](md::Range range) {
        if (!prose.empty() && prose.back().end >= range.start) prose.back().end = std::max(prose.back().end, range.end);
        else prose.push_back(range);
    });
    return prose;
}

QList<SourceRange> spellingWords(QStringView text, int from, int to, const std::vector<md::Range> &prose)
{
    QList<SourceRange> words;
    from = qBound(0, from, int(text.size()));
    to = qBound(from, to, int(text.size()));
    auto next =
        std::partition_point(prose.begin(), prose.end(), [from](const md::Range &range) { return range.end <= from; });
    for (; next != prose.end() && next->start < to; ++next) {
        const int start = qMax(from, next->start);
        int end = next->end;
        // Ranges that touch are one run of prose.
        while (next + 1 != prose.end() && (next + 1)->start <= end) end = qMax(end, (++next)->end);
        end = qMin(end, to);
        if (end > start) proseWords(text, start, end, words);
    }
    return words;
}

SpellEngine::SpellEngine() = default;
SpellEngine::~SpellEngine() = default;

bool SpellEngine::load(const QString &affix, const QString &dictionary, QString *error)
{
    for (const QString &file : {affix, dictionary}) {
        QFile readable(file);
        if (!readable.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("%1: %2").arg(file, readable.errorString());
            return false;
        }
    }
    auto hunspell = std::make_unique<Hunspell>(QFile::encodeName(affix).constData(),
                                               QFile::encodeName(dictionary).constData());
    const QString encoding = QString::fromStdString(hunspell->get_dict_encoding()).toUpper().remove(u'-');
    QMutexLocker lock(&m_mutex);
    m_hunspell = std::move(hunspell);
    m_latin1 = encoding == u"ISO88591" || encoding == u"LATIN1";
    for (const QString &word : std::as_const(m_personal)) {
        const QByteArray encoded = encode(word);
        if (!encoded.isEmpty()) m_hunspell->add(encoded.toStdString());
    }
    m_answers.clear();
    return true;
}

bool SpellEngine::loaded() const
{
    QMutexLocker lock(&m_mutex);
    return m_hunspell != nullptr;
}

QByteArray SpellEngine::encode(const QString &word) const
{
    QString plain = word.normalized(QString::NormalizationForm_C);
    plain.replace(QChar(0x2019), u'\'');
    if (!m_latin1) return plain.toUtf8();
    if (std::any_of(plain.cbegin(), plain.cend(), [](QChar c) { return c.unicode() > 0xff; })) return {};
    return plain.toLatin1();
}

QString SpellEngine::decode(const std::string &word) const
{
    return m_latin1 ? QString::fromLatin1(word) : QString::fromStdString(word);
}

bool SpellEngine::check(const QString &word)
{
    QMutexLocker lock(&m_mutex);
    if (m_personal.contains(word) || m_ignored.contains(word)) return true;
    // Without a dictionary nothing is wrong.
    if (!m_hunspell) return true;
    if (const auto found = m_answers.constFind(word); found != m_answers.cend()) return *found;
    const QByteArray encoded = encode(word);
    // A word the dictionary's encoding cannot hold is not its to judge.
    const bool right = encoded.isEmpty() || m_hunspell->spell(encoded.toStdString());
    // Bounded, should a long session meet very many words.
    if (m_answers.size() >= 100000) m_answers.clear();
    m_answers.insert(word, right);
    return right;
}

QStringList SpellEngine::suggest(const QString &word, int count)
{
    QMutexLocker lock(&m_mutex);
    if (!m_hunspell) return {};
    const QByteArray encoded = encode(word);
    if (encoded.isEmpty()) return {};
    QStringList suggestions;
    for (const std::string &found : m_hunspell->suggest(encoded.toStdString())) {
        QString suggestion = decode(found);
        // The writer's own apostrophe.
        if (word.contains(QChar(0x2019))) suggestion.replace(u'\'', QChar(0x2019));
        if (!suggestions.contains(suggestion)) suggestions.append(suggestion);
        if (suggestions.size() == count) break;
    }
    return suggestions;
}

void SpellEngine::addPersonal(const QString &word)
{
    QMutexLocker lock(&m_mutex);
    m_personal.insert(word);
    if (m_hunspell) {
        const QByteArray encoded = encode(word);
        if (!encoded.isEmpty()) m_hunspell->add(encoded.toStdString());
    }
    // The dictionary now takes other forms of it too, as its capitalised one.
    m_answers.clear();
}

void SpellEngine::ignore(const QString &word)
{
    QMutexLocker lock(&m_mutex);
    m_ignored.insert(word);
    m_answers.remove(word);
}

bool SpellEngine::accepted(const QString &word) const
{
    QMutexLocker lock(&m_mutex);
    return m_personal.contains(word) || m_ignored.contains(word);
}

SpellChecker::SpellChecker(const QString &personalFile, QObject *parent)
    : QObject(parent), m_personalFile(personalFile), m_folders(systemDictionaryFolders()),
      m_engine(std::make_shared<SpellEngine>()), m_pool(std::make_unique<QThreadPool>())
{
    // One worker: Hunspell answers one question at a time anyway.
    m_pool->setMaxThreadCount(1);
}

SpellChecker::~SpellChecker()
{
    // Whatever still runs holds the engine; it only waits for its worker.
    m_pool->waitForDone();
}

QStringList SpellChecker::systemDictionaryFolders()
{
    QString home = qEnvironmentVariable("XDG_DATA_HOME");
    if (home.isEmpty()) home = QDir::homePath() + QStringLiteral("/.local/share");
    QString shared = qEnvironmentVariable("XDG_DATA_DIRS");
    if (shared.isEmpty()) shared = QStringLiteral("/usr/local/share:/usr/share");
    QStringList folders{home + QStringLiteral("/hunspell")};
    for (const QString &folder : shared.split(u':', Qt::SkipEmptyParts)) folders << folder + QStringLiteral("/hunspell");
    folders << QStringLiteral("/usr/share/myspell/dicts");
    folders.removeDuplicates();
    return folders;
}

void SpellChecker::setDictionaryFolders(const QStringList &folders) { m_folders = folders; }

void SpellChecker::load()
{
    if (m_loading || m_settled) return;
    QFile file(m_personalFile);
    if (file.exists()) {
        if (file.open(QIODevice::ReadOnly)) {
            for (const QString &line : QString::fromUtf8(file.readAll()).split(u'\n')) {
                const QString word = line.trimmed();
                if (word.isEmpty() || m_personalWords.contains(word)) continue;
                m_personalWords.append(word);
                m_engine->addPersonal(word);
            }
        } else {
            // Never written over: what it holds is the writer's.
            m_personalUnreadable = true;
            emit problem(tr("The personal dictionary cannot be read"),
                         tr("%1: %2").arg(m_personalFile, file.errorString()));
        }
    }
    const QString language = QString::fromLatin1(Language);
    QString affix, dictionary;
    for (const QString &folder : std::as_const(m_folders)) {
        const QString base = folder + u'/' + language;
        if (QFileInfo(base + QStringLiteral(".aff")).isFile() && QFileInfo(base + QStringLiteral(".dic")).isFile()) {
            affix = base + QStringLiteral(".aff");
            dictionary = base + QStringLiteral(".dic");
            break;
        }
    }
    m_found = !affix.isEmpty();
    if (!m_found) {
        finishLoading(false, {});
        return;
    }
    m_loading = true;
    emit availabilityChanged();
    // Reading en_US takes a moment, which no window waits for.
    m_pool->start([engine = m_engine, affix, dictionary, self = QPointer<SpellChecker>(this)] {
        QString error;
        const bool read = engine->load(affix, dictionary, &error);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, read, error] {
                if (self) self->finishLoading(read, error);
            },
            Qt::QueuedConnection);
    });
}

void SpellChecker::finishLoading(bool available, const QString &error)
{
    const bool wasActive = active();
    m_loading = false;
    m_settled = true;
    m_available = available;
    if (!error.isEmpty()) emit problem(tr("The English dictionary cannot be read"), error);
    emit availabilityChanged();
    if (active() != wasActive) emit activeChanged();
}

void SpellChecker::setWanted(bool wanted)
{
    if (wanted == m_wanted) return;
    const bool wasActive = active();
    m_wanted = wanted;
    emit wantedChanged();
    if (active() != wasActive) emit activeChanged();
}

QStringList SpellChecker::suggestions(const QString &word) const
{
    return m_engine->suggest(word, MaxSuggestions);
}

bool SpellChecker::addToDictionary(const QString &given)
{
    const QString word = given.trimmed();
    if (word.isEmpty() || std::any_of(word.cbegin(), word.cend(), [](QChar c) { return c.isSpace(); })) return false;
    // At once, in every view, whatever becomes of the file.
    m_engine->addPersonal(word);
    if (!m_personalWords.contains(word)) m_personalWords.append(word);
    emit wordAccepted(word);
    QString error;
    if (m_personalUnreadable) {
        error = tr("it could not be read, so it is left as it is");
    } else if (!QDir().mkpath(QFileInfo(m_personalFile).absolutePath())) {
        error = tr("its folder cannot be made");
    } else {
        QSaveFile file(m_personalFile);
        if (!file.open(QIODevice::WriteOnly) || file.write((m_personalWords.join(u'\n') + u'\n').toUtf8()) < 0
            || !file.commit())
            error = file.errorString();
    }
    if (error.isEmpty()) return true;
    emit problem(tr("“%1” is not kept in the personal dictionary").arg(word),
                 tr("It is taken until parch ends, but %1 could not be written: %2").arg(m_personalFile, error));
    return false;
}

void SpellChecker::ignore(const QString &word)
{
    if (word.isEmpty()) return;
    m_engine->ignore(word);
    emit wordAccepted(word);
}
