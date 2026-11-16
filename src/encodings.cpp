#include "encodings.h"

#include "document.h"

#include <QCoreApplication>
#include <QSet>
#include <QStringConverter>
#include <QStringDecoder>
#include <QtConcurrent>
#include <algorithm>
#include <array>

namespace {

QString tr(const char *text) { return QCoreApplication::translate("Encodings", text); }

const QString windows1252 = QStringLiteral("windows-1252");

// What Windows-1252 has at 0x80 to 0x9F, where Latin-1 has control
// characters; the five places it leaves empty keep those, as browsers do.
constexpr std::array<char16_t, 32> windows1252High = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

// Read here rather than by the converter, so that it never depends on
// what the converter knows.
QString fromWindows1252(const QByteArray &bytes)
{
    QString text(bytes.size(), Qt::Uninitialized);
    QChar *out = text.data();
    for (const char byte : bytes) {
        const auto value = uchar(byte);
        *out++ = value >= 0x80 && value < 0xA0 ? QChar(windows1252High[value - 0x80]) : QChar(value);
    }
    return text;
}

// The common encodings, in the order offered, with how they are shown.
struct Common {
    const char *name;
    const char *label;
};
constexpr Common common[] = {
    {"windows-1252", "Western European (Windows-1252)"},
    {"ISO-8859-1", "Western European (ISO-8859-1, Latin-1)"},
    {"ISO-8859-15", "Western European (ISO-8859-15)"},
    {"macintosh", "Western European (Mac OS Roman)"},
    {"UTF-16LE", "Unicode (UTF-16, little-endian)"},
    {"UTF-16BE", "Unicode (UTF-16, big-endian)"},
    {"UTF-16", "Unicode (UTF-16, by its byte-order mark)"},
    {"UTF-32LE", "Unicode (UTF-32, little-endian)"},
    {"UTF-32BE", "Unicode (UTF-32, big-endian)"},
    {"windows-1250", "Central European (Windows-1250)"},
    {"ISO-8859-2", "Central European (ISO-8859-2)"},
    {"windows-1251", "Cyrillic (Windows-1251)"},
    {"KOI8-R", "Cyrillic (KOI8-R)"},
    {"KOI8-U", "Cyrillic (KOI8-U)"},
    {"ISO-8859-5", "Cyrillic (ISO-8859-5)"},
    {"windows-1253", "Greek (Windows-1253)"},
    {"ISO-8859-7", "Greek (ISO-8859-7)"},
    {"windows-1254", "Turkish (Windows-1254)"},
    {"ISO-8859-9", "Turkish (ISO-8859-9)"},
    {"windows-1255", "Hebrew (Windows-1255)"},
    {"windows-1256", "Arabic (Windows-1256)"},
    {"windows-1257", "Baltic (Windows-1257)"},
    {"windows-1258", "Vietnamese (Windows-1258)"},
    {"windows-874", "Thai (Windows-874)"},
    {"Shift_JIS", "Japanese (Shift_JIS)"},
    {"EUC-JP", "Japanese (EUC-JP)"},
    {"ISO-2022-JP", "Japanese (ISO-2022-JP)"},
    {"GB18030", "Chinese, simplified (GB18030)"},
    {"Big5", "Chinese, traditional (Big5)"},
    {"EUC-KR", "Korean (EUC-KR)"},
    {"IBM437", "DOS (code page 437)"},
};

// Always readable, whatever the converter knows.
bool builtIn(const QString &name)
{
    return name == windows1252 || name == u"ISO-8859-1" || name.startsWith(QStringLiteral("UTF-16"))
           || name.startsWith(QStringLiteral("UTF-32"));
}

// Names the converter lists that are not encodings a file is written in,
// or that read no differently from one offered already.
bool offeredAmongOthers(const QString &name)
{
    static const QSet<QString> left = {QStringLiteral("Locale"), QStringLiteral("UTF-8"), QStringLiteral("CESU-8"),
                                       QStringLiteral("IMAP-mailbox-name")};
    return !left.contains(name) && !name.contains(QStringLiteral("Endian")) && !name.contains(u',');
}

} // namespace

QList<Encodings::Encoding> Encodings::offered()
{
    QList<Encoding> encodings;
    // The converter's own names for those offered, so that none is offered twice under another name.
    QSet<QString> taken;
    for (const Common &encoding : common) {
        const QString name = QString::fromLatin1(encoding.name);
        QStringDecoder decoder(encoding.name);
        if (!builtIn(name) && !decoder.isValid()) continue;
        encodings.append({name, tr(encoding.label)});
        taken.insert(name.toLower());
        if (decoder.isValid()) taken.insert(QString::fromLatin1(decoder.name()).toLower());
    }
    QStringList others;
    for (const QString &name : QStringConverter::availableCodecs()) {
        if (!offeredAmongOthers(name) || taken.contains(name.toLower())) continue;
        QStringDecoder decoder(name.toLatin1().constData());
        if (!decoder.isValid() || taken.contains(QString::fromLatin1(decoder.name()).toLower())) continue;
        taken.insert(name.toLower());
        others.append(name);
    }
    std::sort(others.begin(), others.end(),
              [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    for (const QString &name : std::as_const(others)) encodings.append({name, name});
    return encodings;
}

QString Encodings::labelOf(const QString &name)
{
    for (const Common &encoding : common)
        if (name.compare(QLatin1String(encoding.name), Qt::CaseInsensitive) == 0) return tr(encoding.label);
    return name;
}

Encodings::Decoded Encodings::decode(const QByteArray &bytes, const QString &name)
{
    Decoded decoded;
    if (name == windows1252) {
        decoded.known = true;
        decoded.text = fromWindows1252(bytes);
    } else {
        // A trailing sequence left unfinished is a character not read, too.
        QStringDecoder decoder(name.toLatin1().constData(), QStringConverter::Flag::Stateless);
        if (!decoder.isValid()) return decoded;
        decoded.known = true;
        decoded.text = decoder(bytes);
    }
    decoded.failures = int(decoded.text.count(QChar::ReplacementCharacter));
    // CRLF when as many breaks use it as LF, as a file read as UTF-8 has it.
    qsizetype crlf = 0, bareLf = 0;
    for (qsizetype i = decoded.text.indexOf(u'\n'); i >= 0; i = decoded.text.indexOf(u'\n', i + 1)) {
        if (i > 0 && decoded.text.at(i - 1) == u'\r') ++crlf;
        else ++bareLf;
    }
    if (crlf > 0 && crlf >= bareLf) decoded.convention.newline = QStringLiteral("\r\n");
    return decoded;
}

QString Encodings::likely(const QByteArray &bytes)
{
    if (bytes.startsWith(QByteArray("\xff\xfe\x00\x00", 4))) return QStringLiteral("UTF-32LE");
    if (bytes.startsWith(QByteArray("\x00\x00\xfe\xff", 4))) return QStringLiteral("UTF-32BE");
    if (bytes.startsWith("\xff\xfe")) return QStringLiteral("UTF-16LE");
    if (bytes.startsWith("\xfe\xff")) return QStringLiteral("UTF-16BE");
    // Text mostly in the first 256 characters has a zero in every other byte.
    const qsizetype length = std::min<qsizetype>(bytes.size(), 1024) & ~qsizetype(1);
    qsizetype evenZeros = 0, oddZeros = 0;
    for (qsizetype i = 0; i < length; ++i)
        if (bytes.at(i) == 0) ++(i % 2 == 0 ? evenZeros : oddZeros);
    const qsizetype pairs = length / 2;
    if (pairs > 0 && oddZeros * 2 > pairs && evenZeros * 8 < pairs) return QStringLiteral("UTF-16LE");
    if (pairs > 0 && evenZeros * 2 > pairs && oddZeros * 8 < pairs) return QStringLiteral("UTF-16BE");
    return windows1252;
}

EncodingChoice::EncodingChoice(QObject *parent) : QObject(parent), m_encodings(Encodings::offered())
{
    connect(&m_reading, &QFutureWatcherBase::finished, this, [this] {
        const Read read = m_reading.result();
        m_read = true;
        if (!read.ok) {
            m_error = tr("The file could not be read again; it may have changed since it was opened.");
            emit changed();
            return;
        }
        m_bytes = read.bytes;
        const QString likely = Encodings::likely(m_bytes);
        m_chosen = 0;
        for (int i = 0; i < m_encodings.size(); ++i)
            if (m_encodings.at(i).name == likely) m_chosen = i;
        decodeChosen();
    });
    connect(&m_decoding, &QFutureWatcherBase::finished, this, [this] {
        m_decoded = m_decoding.result();
        emit changed();
    });
}

EncodingChoice::~EncodingChoice()
{
    // Neither job holds anything of this object's, so neither is waited for.
}

Document *EncodingChoice::document() const { return m_document; }

void EncodingChoice::setDocument(Document *document)
{
    if (document == m_document) return;
    m_document = document;
    m_read = false;
    m_bytes.clear();
    m_decoded.reset();
    m_error.clear();
    m_version = {};
    emit documentChanged();
    emit changed();
    if (!document) return;
    if (document->isDraft() || document->state() != Document::State::ReadOnly) {
        m_read = true;
        m_error = tr("Only a file that is open read-only, because it is not UTF-8, can be read in another encoding.");
        emit changed();
        return;
    }
    m_version = document->diskVersion();
    m_reading.setFuture(QtConcurrent::run([path = document->path(), version = m_version] {
        Read read;
        read.ok = DiskFile::readVersion(path, version, read.bytes);
        return read;
    }));
}

QStringList EncodingChoice::labels() const
{
    QStringList labels;
    for (const Encodings::Encoding &encoding : m_encodings) labels.append(encoding.label);
    return labels;
}

QStringList EncodingChoice::names() const
{
    QStringList names;
    for (const Encodings::Encoding &encoding : m_encodings) names.append(encoding.name);
    return names;
}

int EncodingChoice::chosen() const { return m_chosen; }

void EncodingChoice::setChosen(int index)
{
    if (index < 0 || index >= m_encodings.size() || index == m_chosen) return;
    m_chosen = index;
    if (m_read && m_error.isEmpty()) decodeChosen();
    else emit changed();
}

QString EncodingChoice::name() const { return m_encodings.value(m_chosen).name; }
bool EncodingChoice::ready() const { return m_read && m_error.isEmpty() && m_decoded.has_value(); }
QString EncodingChoice::preview() const { return m_decoded ? m_decoded->text.left(PreviewLength) : QString(); }
int EncodingChoice::failures() const { return m_decoded ? m_decoded->failures : 0; }
QString EncodingChoice::error() const { return m_error; }
Encodings::Decoded EncodingChoice::decoded() const { return m_decoded.value_or(Encodings::Decoded{}); }
DiskVersion EncodingChoice::version() const { return m_version; }

void EncodingChoice::decodeChosen()
{
    m_decoded.reset();
    emit changed();
    // A newer choice replaces the future watched, so an older result is never taken.
    m_decoding.setFuture(QtConcurrent::run([bytes = m_bytes, name = name()] { return Encodings::decode(bytes, name); }));
}
