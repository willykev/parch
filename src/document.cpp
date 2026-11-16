#include "document.h"

#include "documentparse.h"
#include "sourceboundaries.h"
#include "sourcebuffer.h"

namespace {
constexpr qsizetype titleLimit = 60;

bool isLineBreak(QChar c)
{
    return c == u'\n' || c == u'\r' || c == QChar(0x2028) || c == QChar(0x2029);
}

// The first line with text, without the heading marks and spaces around it.
// Blank lines and a line of only marks are passed over. Only as much of the
// line as the title can hold is read, so a long first line costs nothing
// extra on each keystroke.
QString firstWords(const QString &text)
{
    qsizetype begin = 0;
    while (begin < text.size() && (text.at(begin).isSpace() || text.at(begin) == u'#')) ++begin;
    qsizetype end = begin;
    while (end < text.size() && end - begin <= titleLimit && !isLineBreak(text.at(end))) ++end;
    QString title = text.mid(begin, end - begin);
    if (title.size() > titleLimit) {
        // The cut falls between characters, never inside one; a single
        // character longer than the limit is kept whole.
        int cut = int(titleLimit);
        if (!sourceGraphemeBoundary(title, cut)) cut = previousSourceGrapheme(title, cut);
        if (cut <= 0) cut = nextSourceGrapheme(title, 0);
        title.truncate(cut);
    }
    return title.trimmed();
}

// The text of the first ATX heading with text, outside fenced code, or
// nothing. The text is read one line at a time, without copying it.
QString firstHeading(const QString &text)
{
    QString fence;
    for (qsizetype start = 0; start <= text.size();) {
        qsizetype end = start;
        while (end < text.size() && !isLineBreak(text.at(end))) ++end;
        const QStringView line = QStringView(text).mid(start, end - start);
        // CR LF ends one line.
        start = end + (end + 1 < text.size() && text.at(end) == u'\r' && text.at(end + 1) == u'\n' ? 2 : 1);
        qsizetype indent = 0;
        while (indent < line.size() && indent < 4 && line.at(indent) == u' ') ++indent;
        if (indent > 3) continue;
        const QStringView body = line.mid(indent);
        // A fence opens with three or more backticks or tildes and closes
        // with at least as many of the same.
        if (body.startsWith(u"```") || body.startsWith(u"~~~")) {
            qsizetype run = 0;
            while (run < body.size() && body.at(run) == body.at(0)) ++run;
            if (fence.isEmpty()) fence = body.left(run).toString();
            else if (body.at(0) == fence.at(0) && run >= fence.size()) fence.clear();
            continue;
        }
        if (!fence.isEmpty()) continue;
        qsizetype hashes = 0;
        while (hashes < body.size() && body.at(hashes) == u'#') ++hashes;
        if (hashes < 1 || hashes > 6 || (hashes < body.size() && body.at(hashes) != u' ' && body.at(hashes) != u'\t'))
            continue;
        QString heading = body.mid(hashes).trimmed().toString();
        // A closing run of # goes, when a space comes before it.
        qsizetype closing = heading.size();
        while (closing > 0 && heading.at(closing - 1) == u'#') --closing;
        if (closing == 0) heading.clear();
        else if (closing < heading.size() && heading.at(closing - 1).isSpace()) heading.truncate(closing);
        heading = heading.trimmed();
        if (!heading.isEmpty()) return heading;
    }
    return {};
}

// At most titleLimit characters, cut where a word ends, or between
// characters when one word is longer than that.
QString cutAtWord(const QString &words)
{
    if (words.size() <= titleLimit) return words;
    for (qsizetype cut = titleLimit; cut > 0; --cut)
        if (words.at(cut).isSpace()) return words.left(cut).trimmed();
    int cut = int(titleLimit);
    if (!sourceGraphemeBoundary(words, cut)) cut = previousSourceGrapheme(words, cut);
    if (cut <= 0) cut = nextSourceGrapheme(words, 0);
    return words.left(cut);
}

// Marks that reorder the text around them, which in a file name could make
// it read as another name.
bool isDirectionMark(QChar c)
{
    const char16_t u = c.unicode();
    return u == 0x200e || u == 0x200f || u == 0x061c || (u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069);
}

// The words as one file name in one folder.
QString safeFileName(const QString &words)
{
    QString safe;
    safe.reserve(words.size());
    for (qsizetype i = 0; i < words.size(); ++i) {
        const QChar c = words.at(i);
        if (c.isHighSurrogate() && i + 1 < words.size() && words.at(i + 1).isLowSurrogate()) {
            safe += c;
            safe += words.at(++i);
            continue;
        }
        // A half of a pair has no name in a file system's encoding.
        if (c.isSurrogate() || isDirectionMark(c)) continue;
        // A control character, a tab among them, parts words as a space does.
        safe += c.category() == QChar::Other_Control ? u' ' : c == u'/' ? u'-' : c;
    }
    // A leading dot hides the file, and "." and ".." name folders.
    for (;;) {
        safe = safe.simplified();
        if (!safe.startsWith(u'.')) break;
        while (safe.startsWith(u'.')) safe.remove(0, 1);
    }
    return safe;
}
} // namespace

Document::Document(const QString &id, const QString &path, const QString &text, const TextConvention &convention,
                   const DiskVersion &version, bool readOnly, QObject *parent)
    : QObject(parent), m_id(id), m_buffer(new SourceBuffer(text, this)), m_path(path), m_convention(convention),
      m_diskVersion(version), m_readOnly(readOnly), m_diskUnkept(!path.isEmpty() && version.exists),
      m_parse(new DocumentParse(this))
{
    connect(m_buffer, &SourceBuffer::modifiedChanged, this, &Document::refresh);
    // Only a draft's title follows its text.
    connect(m_buffer, &SourceBuffer::sourceChanged, this, [this] {
        if (isDraft()) refresh();
    });
    m_shown = shown();
}

void Document::setSavedDialect(const Dialect::Layers &layers)
{
    if (layers == m_dialect) return;
    m_dialect = layers;
    m_choice = Dialect::effective(layers);
    emit dialectChanged();
}

QString Document::id() const { return m_id; }
SourceBuffer *Document::buffer() const { return m_buffer; }
DocumentParse *Document::parse() const { return m_parse; }
QString Document::path() const { return m_path; }
bool Document::isDraft() const { return m_path.isEmpty(); }
QString Document::lastError() const { return m_lastError; }
bool Document::everSaved() const { return !isDraft() || !m_diskVersion.canonicalPath.isEmpty(); }
QByteArray Document::bytes() const { return DiskFile::encode(m_buffer->text(), m_convention); }
QString Document::newline() const { return m_convention.newline; }
DiskVersion Document::diskVersion() const { return m_diskVersion; }
bool Document::fileMissing() const { return m_fileMissing && m_problem == Problem::Conflict; }
bool Document::fileUnreadable() const { return m_fileUnreadable && m_problem == Problem::Conflict; }
QString Document::leadsTo() const { return m_problem == Problem::Conflict ? m_leadsTo : QString(); }
bool Document::nameTaken() const { return m_nameTaken && m_problem == Problem::Conflict; }
bool Document::isSaving() const { return m_saving; }
bool Document::isTextless() const { return m_textless; }
bool Document::isScratch() const { return m_scratch; }
QString Document::scratchFolder() const { return m_scratch ? m_scratchFolder : QString(); }
QString Document::convertedFrom() const { return m_convertedFrom; }
TextConvention Document::convention() const { return m_convention; }

QString Document::title() const
{
    if (m_scratch) return m_scratchTitle;
    if (!isDraft()) return m_path.mid(m_path.lastIndexOf(u'/') + 1);
    return draftTitle(m_buffer->text());
}

QString Document::draftTitle(const QString &text)
{
    const QString title = firstWords(text);
    return title.isEmpty() ? tr("Untitled") : title;
}

QString Document::suggestedFileName(const QString &text)
{
    QString words = firstHeading(text);
    if (words.isEmpty()) {
        // As much of the first line as can be cut between words.
        qsizetype begin = 0;
        while (begin < text.size() && (text.at(begin).isSpace() || text.at(begin) == u'#')) ++begin;
        qsizetype end = begin;
        while (end < text.size() && end - begin <= titleLimit && !isLineBreak(text.at(end))) ++end;
        words = text.mid(begin, end - begin);
    }
    const QString name = safeFileName(cutAtWord(words.trimmed()));
    return (name.isEmpty() ? tr("Untitled") : name) + QStringLiteral(".md");
}

bool Document::differsFromFile() const
{
    return m_buffer->modified() || m_diskAhead;
}

Document::State Document::state() const
{
    // A conflict outranks a running save because only a naming runs during
    // one, and the conflict waits on until it lands; a running save
    // outranks an earlier failure because it is the retry.
    if (m_readOnly) return State::ReadOnly;
    if (m_problem == Problem::Conflict) return State::Conflict;
    if (m_saving) return State::Saving;
    if (m_problem == Problem::Failed) return State::SaveFailed;
    if (m_buffer->modified() || m_diskAhead) return State::Modified;
    return State::Clean;
}

QString Document::statusText() const
{
    // parch's own text, which nothing keeps, until Save as writes a copy.
    if (m_scratch && (state() == State::Clean || state() == State::Modified)) return tr("Not saved");
    switch (state()) {
    case State::Clean:
        // Nothing has been written yet, so nothing is saved.
        if (isDraft() && m_buffer->length() == 0) return tr("Empty draft");
        return tr("Saved");
    case State::Modified:
        return tr("Edited");
    case State::Saving:
        return tr("Saving…");
    case State::SaveFailed:
        return tr("Save failed");
    case State::Conflict:
        // A file that went, or cannot be read, is a change too, but one with words of its own.
        if (fileMissing()) return tr("File missing");
        return fileUnreadable() ? tr("File unreadable") : tr("Changed on disk");
    case State::ReadOnly:
        return tr("Read-only");
    }
    return {};
}

Document::Shown Document::shown() const
{
    return Shown{state(),       statusText(),     title(),   m_path,      m_lastError,
                 fileMissing(), fileUnreadable(), leadsTo(), nameTaken(), m_textless, m_convertedFrom};
}

void Document::refresh()
{
    const Shown now = shown();
    if (now == m_shown) return;
    m_shown = now;
    emit changed();
}
